/*
 * jacklite.cc
 *
 * an in-process JACK: the subset of the JACK API norns uses (include/jack/), for android
 * where there is no jack server. clients, ports and connections live in this process; each
 * cycle (jacklite_process, called by the audio driver) fills system:capture_N from the
 * device, runs the clients' process callbacks in dependency order with every input port
 * holding the sum of the outputs connected to it, and sums the ports connected to
 * system:playback_N into the device's output.
 *
 * graph changes (register, connect, activate) take the graph lock; so does a cycle. a
 * process callback must not change the graph (as with real jack, where it must not block).
 */

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <regex>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

#include <jack/jack.h>
#include <jack/ringbuffer.h>

struct _jack_port {
    std::string name; // client:short
    std::string short_name;
    std::string type;
    unsigned long flags = 0;
    jack_client_t *client = nullptr;
    std::vector<float> buf;
    std::vector<_jack_port *> sources; // input ports: the outputs connected to it
};

struct _jack_client {
    std::string name;
    bool active = false;
    bool system = false;
    JackProcessCallback process = nullptr;
    void *process_arg = nullptr;
    JackShutdownCallback shutdown = nullptr;
    void *shutdown_arg = nullptr;
    JackXRunCallback xrun = nullptr;
    void *xrun_arg = nullptr;
    JackBufferSizeCallback bufsize = nullptr;
    void *bufsize_arg = nullptr;
    JackThreadInitCallback thread_init = nullptr;
    void *thread_init_arg = nullptr;
    std::vector<_jack_port *> ports;
};

namespace {

std::recursive_mutex graph;
std::vector<std::unique_ptr<_jack_client>> clients;
std::vector<std::unique_ptr<_jack_port>> ports;
std::vector<_jack_client *> order; // active clients, sources before sinks
bool order_dirty = true;

jack_client_t *system_client = nullptr;
std::vector<_jack_port *> capture;  // system:capture_N (outputs into the graph)
std::vector<_jack_port *> playback; // system:playback_N (inputs from the graph)

std::atomic<jack_nframes_t> sample_rate{48000};
jack_nframes_t max_block = 4096;
std::atomic<jack_nframes_t> block{256};

// cycle timing: frame count at the start of the current cycle and when it started (usec)
std::atomic<uint64_t> cycle_frames{0};
std::atomic<uint64_t> cycle_usecs{0};
std::atomic<float> cpu_load{0.f};
std::once_flag init_once;

jack_time_t now_usecs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (jack_time_t)ts.tv_sec * 1000000ULL + (jack_time_t)ts.tv_nsec / 1000ULL;
}

_jack_port *find_port(const char *name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (auto &p : ports) {
        if (p->name == name) {
            return p.get();
        }
    }
    return nullptr;
}

_jack_port *add_port(jack_client_t *c, const std::string &short_name, const char *type, unsigned long flags) {
    auto p = std::make_unique<_jack_port>();
    p->short_name = short_name;
    p->name = c->name + ":" + short_name;
    p->type = type ? type : JACK_DEFAULT_AUDIO_TYPE;
    p->flags = flags;
    p->client = c;
    p->buf.assign(max_block, 0.f);
    _jack_port *raw = p.get();
    ports.push_back(std::move(p));
    c->ports.push_back(raw);
    order_dirty = true;
    return raw;
}

void make_system(int num_in, int num_out) {
    auto c = std::make_unique<_jack_client>();
    c->name = "system";
    c->system = true;
    c->active = true;
    system_client = c.get();
    clients.push_back(std::move(c));
    for (int i = 0; i < num_in; i++) {
        capture.push_back(add_port(system_client, "capture_" + std::to_string(i + 1), JACK_DEFAULT_AUDIO_TYPE,
                                   JackPortIsOutput | JackPortIsPhysical | JackPortIsTerminal));
    }
    for (int i = 0; i < num_out; i++) {
        playback.push_back(add_port(system_client, "playback_" + std::to_string(i + 1), JACK_DEFAULT_AUDIO_TYPE,
                                    JackPortIsInput | JackPortIsPhysical | JackPortIsTerminal));
    }
}

void ensure_init() {
    std::call_once(init_once, [] {
        std::lock_guard<std::recursive_mutex> g(graph);
        if (system_client == nullptr) {
            make_system(2, 2);
        }
    });
}

// active non-system clients, each after every client feeding it (kahn; a cycle is broken
// by taking the remaining clients in registration order: one block of delay there)
void sort_clients() {
    order.clear();
    std::vector<_jack_client *> todo;
    for (auto &c : clients) {
        if (c->active && !c->system) {
            todo.push_back(c.get());
        }
    }
    while (!todo.empty()) {
        bool progressed = false;
        for (size_t i = 0; i < todo.size(); i++) {
            _jack_client *c = todo[i];
            bool ready = true;
            for (auto *p : c->ports) {
                for (auto *s : p->sources) {
                    _jack_client *from = s->client;
                    if (from != c && std::find(todo.begin(), todo.end(), from) != todo.end()) {
                        ready = false;
                    }
                }
            }
            if (ready) {
                order.push_back(c);
                todo.erase(todo.begin() + i);
                progressed = true;
                break;
            }
        }
        if (!progressed) {
            order.push_back(todo.front());
            todo.erase(todo.begin());
        }
    }
    order_dirty = false;
}

void mix_inputs(_jack_port *p, jack_nframes_t n) {
    float *dst = p->buf.data();
    if (p->sources.empty()) {
        memset(dst, 0, sizeof(float) * n);
        return;
    }
    memcpy(dst, p->sources[0]->buf.data(), sizeof(float) * n);
    for (size_t k = 1; k < p->sources.size(); k++) {
        const float *src = p->sources[k]->buf.data();
        for (jack_nframes_t i = 0; i < n; i++) {
            dst[i] += src[i];
        }
    }
}

const char **port_list(const std::vector<std::string> &names) {
    if (names.empty()) {
        return nullptr;
    }
    // one allocation (jack_free frees it): the pointers, then the strings
    size_t bytes = sizeof(char *) * (names.size() + 1);
    for (auto &n : names) {
        bytes += n.size() + 1;
    }
    char **list = (char **)malloc(bytes);
    char *s = (char *)(list + names.size() + 1);
    for (size_t i = 0; i < names.size(); i++) {
        list[i] = s;
        memcpy(s, names[i].c_str(), names[i].size() + 1);
        s += names[i].size() + 1;
    }
    list[names.size()] = nullptr;
    return (const char **)list;
}

bool connected(_jack_port *src, _jack_port *dst) {
    return std::find(dst->sources.begin(), dst->sources.end(), src) != dst->sources.end();
}

// src/dst by name, either order as long as one is an output and the other an input
bool resolve(const char *a, const char *b, _jack_port **src, _jack_port **dst) {
    _jack_port *pa = find_port(a), *pb = find_port(b);
    if (pa == nullptr || pb == nullptr) {
        return false;
    }
    if ((pa->flags & JackPortIsOutput) && (pb->flags & JackPortIsInput)) {
        *src = pa;
        *dst = pb;
        return true;
    }
    return false;
}

} // namespace

extern "C" {

//--- driver side

void jacklite_init(jack_nframes_t sr, jack_nframes_t maxb) {
    std::lock_guard<std::recursive_mutex> g(graph);
    sample_rate = sr;
    if (maxb > max_block) {
        max_block = maxb;
        for (auto &p : ports) {
            p->buf.assign(max_block, 0.f);
        }
    }
    ensure_init();
}

jack_nframes_t jacklite_sample_rate(void) {
    return sample_rate;
}

void jacklite_xrun(void) {
    std::unique_lock<std::recursive_mutex> g(graph, std::try_to_lock);
    if (!g.owns_lock()) {
        return;
    }
    for (auto &c : clients) {
        if (c->active && c->xrun) {
            c->xrun(c->xrun_arg);
        }
    }
}

void jacklite_process(const float *const *in, int num_in, float *const *out, int num_out, jack_nframes_t n) {
    ensure_init();
    jack_time_t t0 = now_usecs();
    std::lock_guard<std::recursive_mutex> g(graph);
    if (n > max_block) {
        n = max_block;
    }
    if (n != block) {
        block = n;
        for (auto &c : clients) {
            if (c->bufsize) {
                c->bufsize(n, c->bufsize_arg);
            }
        }
    }
    cycle_usecs = t0;
    if (order_dirty) {
        sort_clients();
    }
    for (size_t i = 0; i < capture.size(); i++) {
        float *dst = capture[i]->buf.data();
        if ((int)i < num_in && in != nullptr && in[i] != nullptr) {
            memcpy(dst, in[i], sizeof(float) * n);
        } else {
            memset(dst, 0, sizeof(float) * n);
        }
    }
    for (auto *c : order) {
        for (auto *p : c->ports) {
            if (p->flags & JackPortIsInput) {
                mix_inputs(p, n);
            }
        }
        if (c->process) {
            c->process(n, c->process_arg);
        }
    }
    for (size_t i = 0; i < playback.size(); i++) {
        mix_inputs(playback[i], n);
        if ((int)i < num_out && out != nullptr && out[i] != nullptr) {
            memcpy(out[i], playback[i]->buf.data(), sizeof(float) * n);
        }
    }
    for (int i = (int)playback.size(); i < num_out; i++) {
        if (out != nullptr && out[i] != nullptr) {
            memset(out[i], 0, sizeof(float) * n);
        }
    }
    cycle_frames += n;
    double period = 1e6 * n / (double)sample_rate;
    float load = (float)((now_usecs() - t0) / period);
    cpu_load = cpu_load * 0.9f + load * 10.f; // percent, smoothed
}

static void *null_driver(void *p) {
    jack_nframes_t n = (jack_nframes_t)(uintptr_t)p;
    std::vector<float> l(n), r(n);
    float *out[2] = {l.data(), r.data()};
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        jacklite_process(nullptr, 0, out, 2, n);
        uint64_t ns = (uint64_t)n * 1000000000ULL / sample_rate;
        next.tv_nsec += (long)ns;
        while (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

void jacklite_start_null_driver(jack_nframes_t n) {
    ensure_init();
    pthread_t t;
    pthread_create(&t, NULL, null_driver, (void *)(uintptr_t)n);
    pthread_setname_np(t, "jacklite_null");
    pthread_detach(t);
}

//--- clients

jack_client_t *jack_client_open(const char *client_name, jack_options_t options, jack_status_t *status, ...) {
    ensure_init();
    std::lock_guard<std::recursive_mutex> g(graph);
    std::string name = client_name ? client_name : "client";
    int st = 0;
    auto taken = [](const std::string &n) {
        for (auto &c : clients) {
            if (c->name == n) {
                return true;
            }
        }
        return false;
    };
    if (taken(name)) {
        if (options & JackUseExactName) {
            if (status) {
                *status = (jack_status_t)(JackFailure | JackNameNotUnique);
            }
            return nullptr;
        }
        int k = 2;
        while (taken(name + "-" + std::to_string(k))) {
            k++;
        }
        name += "-" + std::to_string(k);
        st |= JackNameNotUnique;
    }
    auto c = std::make_unique<_jack_client>();
    c->name = name;
    jack_client_t *raw = c.get();
    clients.push_back(std::move(c));
    if (status) {
        *status = (jack_status_t)st;
    }
    return raw;
}

int jack_client_close(jack_client_t *client) {
    if (client == nullptr) {
        return -1;
    }
    std::lock_guard<std::recursive_mutex> g(graph);
    while (!client->ports.empty()) {
        jack_port_unregister(client, client->ports.back());
    }
    for (auto it = clients.begin(); it != clients.end(); ++it) {
        if (it->get() == client) {
            clients.erase(it);
            break;
        }
    }
    order_dirty = true;
    return 0;
}

char *jack_get_client_name(jack_client_t *client) {
    return client ? (char *)client->name.c_str() : nullptr;
}

int jack_set_process_callback(jack_client_t *client, JackProcessCallback cb, void *arg) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->process = cb;
    client->process_arg = arg;
    return 0;
}

void jack_on_shutdown(jack_client_t *client, JackShutdownCallback cb, void *arg) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->shutdown = cb;
    client->shutdown_arg = arg;
}

int jack_set_xrun_callback(jack_client_t *client, JackXRunCallback cb, void *arg) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->xrun = cb;
    client->xrun_arg = arg;
    return 0;
}

int jack_set_buffer_size_callback(jack_client_t *client, JackBufferSizeCallback cb, void *arg) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->bufsize = cb;
    client->bufsize_arg = arg;
    return 0;
}

int jack_set_sample_rate_callback(jack_client_t *client, JackSampleRateCallback cb, void *arg) {
    (void)client;
    if (cb) {
        cb(sample_rate, arg);
    }
    return 0;
}

int jack_set_thread_init_callback(jack_client_t *client, JackThreadInitCallback cb, void *arg) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->thread_init = cb;
    client->thread_init_arg = arg;
    return 0;
}

int jack_activate(jack_client_t *client) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->active = true;
    order_dirty = true;
    return 0;
}

int jack_deactivate(jack_client_t *client) {
    std::lock_guard<std::recursive_mutex> g(graph);
    client->active = false;
    order_dirty = true;
    return 0;
}

jack_nframes_t jack_get_sample_rate(jack_client_t *client) {
    (void)client;
    return sample_rate;
}

jack_nframes_t jack_get_buffer_size(jack_client_t *client) {
    (void)client;
    return block;
}

float jack_cpu_load(jack_client_t *client) {
    (void)client;
    return cpu_load;
}

//--- ports

jack_port_t *jack_port_register(jack_client_t *client, const char *port_name, const char *port_type,
                                unsigned long flags, unsigned long buffer_size) {
    (void)buffer_size;
    if (client == nullptr || port_name == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::recursive_mutex> g(graph);
    if (find_port((client->name + ":" + port_name).c_str()) != nullptr) {
        return nullptr;
    }
    return add_port(client, port_name, port_type, flags);
}

int jack_port_unregister(jack_client_t *client, jack_port_t *port) {
    std::lock_guard<std::recursive_mutex> g(graph);
    for (auto &p : ports) {
        auto &s = p->sources;
        s.erase(std::remove(s.begin(), s.end(), port), s.end());
    }
    auto &cp = client->ports;
    cp.erase(std::remove(cp.begin(), cp.end(), port), cp.end());
    for (auto it = ports.begin(); it != ports.end(); ++it) {
        if (it->get() == port) {
            ports.erase(it);
            break;
        }
    }
    order_dirty = true;
    return 0;
}

void *jack_port_get_buffer(jack_port_t *port, jack_nframes_t nframes) {
    (void)nframes;
    return port ? port->buf.data() : nullptr;
}

const char *jack_port_name(const jack_port_t *port) {
    return port ? port->name.c_str() : nullptr;
}

const char *jack_port_short_name(const jack_port_t *port) {
    return port ? port->short_name.c_str() : nullptr;
}

int jack_port_flags(const jack_port_t *port) {
    return port ? (int)port->flags : 0;
}

const char *jack_port_type(const jack_port_t *port) {
    return port ? port->type.c_str() : nullptr;
}

jack_port_t *jack_port_by_name(jack_client_t *client, const char *port_name) {
    (void)client;
    std::lock_guard<std::recursive_mutex> g(graph);
    return find_port(port_name);
}

static std::vector<std::string> connections_of(const jack_port_t *port) {
    std::vector<std::string> out;
    if (port == nullptr) {
        return out;
    }
    if (port->flags & JackPortIsInput) {
        for (auto *s : port->sources) {
            out.push_back(s->name);
        }
    } else {
        for (auto &p : ports) {
            if (connected((_jack_port *)port, p.get())) {
                out.push_back(p->name);
            }
        }
    }
    return out;
}

int jack_port_connected(const jack_port_t *port) {
    std::lock_guard<std::recursive_mutex> g(graph);
    return (int)connections_of(port).size();
}

const char **jack_port_get_connections(const jack_port_t *port) {
    std::lock_guard<std::recursive_mutex> g(graph);
    return port_list(connections_of(port));
}

const char **jack_port_get_all_connections(const jack_client_t *client, const jack_port_t *port) {
    (void)client;
    return jack_port_get_connections(port);
}

const char **jack_get_ports(jack_client_t *client, const char *port_name_pattern, const char *type_name_pattern,
                            unsigned long flags) {
    (void)client;
    ensure_init();
    std::lock_guard<std::recursive_mutex> g(graph);
    std::vector<std::string> names;
    std::unique_ptr<std::regex> name_re, type_re;
    try {
        if (port_name_pattern && *port_name_pattern) {
            name_re = std::make_unique<std::regex>(port_name_pattern);
        }
        if (type_name_pattern && *type_name_pattern) {
            type_re = std::make_unique<std::regex>(type_name_pattern);
        }
    } catch (...) {
        return nullptr;
    }
    // physical ports first, as jackd lists them
    for (int pass = 0; pass < 2; pass++) {
        for (auto &p : ports) {
            bool physical = (p->flags & JackPortIsPhysical) != 0;
            if (physical != (pass == 0)) {
                continue;
            }
            if ((p->flags & flags) != flags) {
                continue;
            }
            if (name_re && !std::regex_search(p->name, *name_re)) {
                continue;
            }
            if (type_re && !std::regex_search(p->type, *type_re)) {
                continue;
            }
            names.push_back(p->name);
        }
    }
    return port_list(names);
}

int jack_connect(jack_client_t *client, const char *source_port, const char *destination_port) {
    (void)client;
    std::lock_guard<std::recursive_mutex> g(graph);
    _jack_port *src, *dst;
    if (!resolve(source_port, destination_port, &src, &dst)) {
        fprintf(stderr, "jacklite: cannot connect %s -> %s\n", source_port, destination_port);
        return -1;
    }
    if (connected(src, dst)) {
        return 17; // EEXIST, as jack
    }
    dst->sources.push_back(src);
    order_dirty = true;
    return 0;
}

int jack_disconnect(jack_client_t *client, const char *source_port, const char *destination_port) {
    (void)client;
    std::lock_guard<std::recursive_mutex> g(graph);
    _jack_port *src, *dst;
    if (!resolve(source_port, destination_port, &src, &dst) || !connected(src, dst)) {
        return -1;
    }
    dst->sources.erase(std::remove(dst->sources.begin(), dst->sources.end(), src), dst->sources.end());
    order_dirty = true;
    return 0;
}

int jack_port_disconnect(jack_client_t *client, jack_port_t *port) {
    (void)client;
    std::lock_guard<std::recursive_mutex> g(graph);
    port->sources.clear();
    for (auto &p : ports) {
        auto &s = p->sources;
        s.erase(std::remove(s.begin(), s.end(), port), s.end());
    }
    order_dirty = true;
    return 0;
}

void jack_free(void *ptr) {
    free(ptr);
}

//--- time

jack_time_t jack_get_time(void) {
    return now_usecs();
}

jack_nframes_t jack_last_frame_time(const jack_client_t *client) {
    (void)client;
    return (jack_nframes_t)cycle_frames.load();
}

jack_nframes_t jack_frames_since_cycle_start(const jack_client_t *client) {
    (void)client;
    uint64_t since = now_usecs() - cycle_usecs.load();
    uint64_t f = since * sample_rate / 1000000ULL;
    jack_nframes_t b = block;
    return (jack_nframes_t)(f < b ? f : b);
}

jack_nframes_t jack_frame_time(const jack_client_t *client) {
    return jack_last_frame_time(client) + jack_frames_since_cycle_start(client);
}

int jack_get_cycle_times(const jack_client_t *client, jack_nframes_t *current_frames, jack_time_t *current_usecs,
                         jack_time_t *next_usecs, float *period_usecs) {
    (void)client;
    float period = 1e6f * block / (float)sample_rate;
    if (current_frames) {
        *current_frames = (jack_nframes_t)cycle_frames.load();
    }
    if (current_usecs) {
        *current_usecs = cycle_usecs.load();
    }
    if (next_usecs) {
        *next_usecs = cycle_usecs.load() + (jack_time_t)period;
    }
    if (period_usecs) {
        *period_usecs = period;
    }
    return 0;
}

//--- ring buffer (power-of-two size, one byte kept free, as jack's)

jack_ringbuffer_t *jack_ringbuffer_create(size_t sz) {
    size_t power = 1;
    while (power < sz + 1) {
        power <<= 1;
    }
    jack_ringbuffer_t *rb = (jack_ringbuffer_t *)calloc(1, sizeof(jack_ringbuffer_t));
    if (rb == nullptr) {
        return nullptr;
    }
    rb->size = power;
    rb->size_mask = power - 1;
    rb->buf = (char *)malloc(power);
    if (rb->buf == nullptr) {
        free(rb);
        return nullptr;
    }
    return rb;
}

void jack_ringbuffer_free(jack_ringbuffer_t *rb) {
    if (rb) {
        free(rb->buf);
        free(rb);
    }
}

int jack_ringbuffer_mlock(jack_ringbuffer_t *rb) {
    (void)rb;
    return 0;
}

void jack_ringbuffer_reset(jack_ringbuffer_t *rb) {
    rb->read_ptr = 0;
    rb->write_ptr = 0;
}

size_t jack_ringbuffer_read_space(const jack_ringbuffer_t *rb) {
    size_t w = __atomic_load_n(&rb->write_ptr, __ATOMIC_ACQUIRE);
    size_t r = __atomic_load_n(&rb->read_ptr, __ATOMIC_ACQUIRE);
    return (w - r) & rb->size_mask;
}

size_t jack_ringbuffer_write_space(const jack_ringbuffer_t *rb) {
    size_t w = __atomic_load_n(&rb->write_ptr, __ATOMIC_ACQUIRE);
    size_t r = __atomic_load_n(&rb->read_ptr, __ATOMIC_ACQUIRE);
    return ((r - w - 1) & rb->size_mask);
}

void jack_ringbuffer_get_read_vector(const jack_ringbuffer_t *rb, jack_ringbuffer_data_t *vec) {
    size_t free_cnt = jack_ringbuffer_read_space(rb);
    size_t r = rb->read_ptr;
    size_t cnt2 = r + free_cnt;
    if (cnt2 > rb->size) {
        vec[0].buf = &rb->buf[r];
        vec[0].len = rb->size - r;
        vec[1].buf = rb->buf;
        vec[1].len = cnt2 & rb->size_mask;
    } else {
        vec[0].buf = &rb->buf[r];
        vec[0].len = free_cnt;
        vec[1].len = 0;
    }
}

void jack_ringbuffer_get_write_vector(const jack_ringbuffer_t *rb, jack_ringbuffer_data_t *vec) {
    size_t free_cnt = jack_ringbuffer_write_space(rb);
    size_t w = rb->write_ptr;
    size_t cnt2 = w + free_cnt;
    if (cnt2 > rb->size) {
        vec[0].buf = &rb->buf[w];
        vec[0].len = rb->size - w;
        vec[1].buf = rb->buf;
        vec[1].len = cnt2 & rb->size_mask;
    } else {
        vec[0].buf = &rb->buf[w];
        vec[0].len = free_cnt;
        vec[1].len = 0;
    }
}

size_t jack_ringbuffer_peek(jack_ringbuffer_t *rb, char *dest, size_t cnt) {
    size_t avail = jack_ringbuffer_read_space(rb);
    if (cnt > avail) {
        cnt = avail;
    }
    size_t r = rb->read_ptr;
    size_t first = std::min(cnt, rb->size - r);
    memcpy(dest, &rb->buf[r], first);
    if (cnt > first) {
        memcpy(dest + first, rb->buf, cnt - first);
    }
    return cnt;
}

void jack_ringbuffer_read_advance(jack_ringbuffer_t *rb, size_t cnt) {
    size_t r = (rb->read_ptr + cnt) & rb->size_mask;
    __atomic_store_n(&rb->read_ptr, r, __ATOMIC_RELEASE);
}

size_t jack_ringbuffer_read(jack_ringbuffer_t *rb, char *dest, size_t cnt) {
    cnt = jack_ringbuffer_peek(rb, dest, cnt);
    jack_ringbuffer_read_advance(rb, cnt);
    return cnt;
}

void jack_ringbuffer_write_advance(jack_ringbuffer_t *rb, size_t cnt) {
    size_t w = (rb->write_ptr + cnt) & rb->size_mask;
    __atomic_store_n(&rb->write_ptr, w, __ATOMIC_RELEASE);
}

size_t jack_ringbuffer_write(jack_ringbuffer_t *rb, const char *src, size_t cnt) {
    size_t avail = jack_ringbuffer_write_space(rb);
    if (cnt > avail) {
        cnt = avail;
    }
    size_t w = rb->write_ptr;
    size_t first = std::min(cnt, rb->size - w);
    memcpy(&rb->buf[w], src, first);
    if (cnt > first) {
        memcpy(rb->buf, src + first, cnt - first);
    }
    jack_ringbuffer_write_advance(rb, cnt);
    return cnt;
}

} // extern "C"
