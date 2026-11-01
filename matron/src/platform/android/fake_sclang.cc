/*
 * fake_sclang.cc
 *
 * stand-in for SuperCollider until scsynth runs on android. it answers matron's startup and
 * engine handshake on sclang's port (57120) the way sc/core/Crone.sc does, so scripts load
 * and run (silently):
 *  - /ready                -> /crone/ready
 *  - /report/engines       -> the Engine_*.sc files found under norns/sc and dust
 *  - /engine/load/name     -> the commands (addCommand) and polls (addPoll) read from that
 *                             engine's .sc file, plus the system polls of CroneAudioContext
 *  - /report/commands|polls, /engine/free: as Crone.sc
 * commands sent to the engine are dropped.
 */

#include <dirent.h>
#include <pthread.h>
#include <regex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

#include <lo/lo.h>

#include "platform/android/fake_sclang.h"

namespace {

lo_server_thread st = NULL;
lo_address matron = NULL;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

struct engine_desc {
    std::vector<std::pair<std::string, std::string>> commands; // name, format
    std::vector<std::pair<std::string, int>> polls;            // name, type (0 value, 1 data)
};
engine_desc current;

// CroneAudioContext.initPolls: always registered, before any engine's
const char *system_polls[] = {"cpu_peak", "cpu_avg", "amp_in_l", "amp_in_r", "amp_out_l",
                              "amp_out_r", "pitch_in_l", "pitch_in_r"};

void find_engines(const std::string &dir, int depth, std::vector<std::pair<std::string, std::string>> &out) {
    if (depth > 6) {
        return;
    }
    DIR *d = opendir(dir.c_str());
    if (d == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
            continue;
        }
        std::string path = dir + "/" + e->d_name;
        struct stat sb;
        if (stat(path.c_str(), &sb) != 0) {
            continue;
        }
        if (S_ISDIR(sb.st_mode)) {
            find_engines(path, depth + 1, out);
        } else {
            std::string n = e->d_name;
            if (n.size() > 10 && n.compare(n.size() - 3, 3, ".sc") == 0) {
                // a file can hold several engines: Engine_Foo : CroneEngine
                FILE *f = fopen(path.c_str(), "r");
                if (f == NULL) {
                    continue;
                }
                std::string text;
                char buf[4096];
                size_t r;
                while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
                    text.append(buf, r);
                }
                fclose(f);
                static const std::regex cls("(^|\\n)\\s*Engine_(\\w+)\\s*:\\s*\\w+");
                for (std::sregex_iterator it(text.begin(), text.end(), cls), end; it != end; ++it) {
                    out.push_back({(*it)[2].str(), path});
                }
            }
        }
    }
    closedir(d);
}

std::vector<std::pair<std::string, std::string>> all_engines() {
    std::vector<std::pair<std::string, std::string>> out;
    const char *home = getenv("HOME");
    std::string h = home ? home : ".";
    find_engines(h + "/norns/sc", 0, out);
    find_engines(h + "/dust", 0, out);
    return out;
}

// the body of class Engine_<name> in text: from its declaration to the next top-level class
std::string class_body(const std::string &text, const std::string &name) {
    std::regex decl("(^|\\n)\\s*Engine_" + name + "\\s*:");
    std::smatch m;
    if (!std::regex_search(text, m, decl)) {
        return text;
    }
    size_t start = m.position(0) + m.length(0);
    size_t open = text.find('{', start);
    if (open == std::string::npos) {
        return text.substr(start);
    }
    int depth = 0;
    for (size_t i = open; i < text.size(); i++) {
        if (text[i] == '{') {
            depth++;
        } else if (text[i] == '}') {
            if (--depth == 0) {
                return text.substr(open, i - open + 1);
            }
        }
    }
    return text.substr(open);
}

std::string strip_comments(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') {
                i++;
            }
            out += '\n';
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/')) {
                i++;
            }
            i++;
        } else if (s[i] == '"') {
            // keep strings whole (they may hold //)
            out += s[i++];
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    out += s[i++];
                }
                out += s[i++];
            }
            if (i < s.size()) {
                out += s[i];
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

void read_engine(const std::string &name, engine_desc &desc) {
    desc.commands.clear();
    desc.polls.clear();
    for (const char *p : system_polls) {
        desc.polls.push_back({p, 0});
    }
    for (auto &e : all_engines()) {
        if (e.first != name) {
            continue;
        }
        FILE *f = fopen(e.second.c_str(), "r");
        if (f == NULL) {
            break;
        }
        std::string text;
        char buf[4096];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, f)) > 0) {
            text.append(buf, r);
        }
        fclose(f);
        std::string body = class_body(strip_comments(text), name);
        // this.addCommand("hz", "f", ...) / addCommand(\hz, "f", ...) / addCommand('hz', 'f', ...)
        static const std::regex cmd(
            "addCommand\\s*\\(\\s*(?:\"([^\"]+)\"|\\\\(\\w+)|'([^']+)')\\s*,\\s*(?:\"([^\"]*)\"|'([^']*)'|\\\\(\\w+))");
        for (std::sregex_iterator it(body.begin(), body.end(), cmd), end; it != end; ++it) {
            const std::smatch &m = *it;
            std::string n = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
            std::string fmt = m[4].matched ? m[4].str() : m[5].matched ? m[5].str() : m[6].str();
            bool dup = false;
            for (auto &c : desc.commands) {
                dup = dup || c.first == n;
            }
            if (!dup) {
                desc.commands.push_back({n, fmt});
            }
        }
        static const std::regex poll("addPoll\\s*\\(\\s*(?:\"([^\"]+)\"|\\\\(\\w+)|'([^']+)')([^;]*periodic\\s*:\\s*false)?");
        for (std::sregex_iterator it(body.begin(), body.end(), poll), end; it != end; ++it) {
            const std::smatch &m = *it;
            std::string n = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
            desc.polls.push_back({n, 0});
        }
        fprintf(stderr, "fake_sclang: engine %s: %zu commands, %zu polls (from %s)\n", name.c_str(),
                desc.commands.size(), desc.polls.size(), e.second.c_str());
        return;
    }
    fprintf(stderr, "fake_sclang: engine %s not found, no commands\n", name.c_str());
}

void report_commands() {
    lo_send(matron, "/report/commands/start", "i", (int)current.commands.size());
    for (size_t i = 0; i < current.commands.size(); i++) {
        lo_send(matron, "/report/commands/entry", "iss", (int)i, current.commands[i].first.c_str(),
                current.commands[i].second.c_str());
    }
    lo_send(matron, "/report/commands/end", "");
}

void report_polls() {
    lo_send(matron, "/report/polls/start", "i", (int)current.polls.size());
    for (size_t i = 0; i < current.polls.size(); i++) {
        lo_send(matron, "/report/polls/entry", "isi", (int)i, current.polls[i].first.c_str(), current.polls[i].second);
    }
    lo_send(matron, "/report/polls/end", "");
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"

int on_ready(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    lo_send(matron, "/crone/ready", "");
    return 0;
}

int on_report_engines(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    // matron and lua accept any engine name when none are reported (lua/core/engine.lua): a
    // script whose engine is not installed still runs. the names are only printed.
    auto engines = all_engines();
    fprintf(stderr, "fake_sclang: %zu engines installed\n", engines.size());
    lo_send(matron, "/report/engines/start", "i", 0);
    lo_send(matron, "/report/engines/end", "");
    return 0;
}

int on_report_commands(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    pthread_mutex_lock(&lock);
    report_commands();
    pthread_mutex_unlock(&lock);
    return 0;
}

int on_report_polls(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    pthread_mutex_lock(&lock);
    report_polls();
    pthread_mutex_unlock(&lock);
    return 0;
}

int on_load(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    pthread_mutex_lock(&lock);
    read_engine(&argv[0]->s, current);
    report_commands();
    report_polls();
    pthread_mutex_unlock(&lock);
    return 0;
}

int on_free(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    pthread_mutex_lock(&lock);
    current.commands.clear();
    current.polls.clear();
    pthread_mutex_unlock(&lock);
    return 0;
}

int on_other(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *ud) {
    return 0;
}

#pragma GCC diagnostic pop

void on_error(int num, const char *m, const char *path) {
    fprintf(stderr, "fake_sclang: liblo error %d: %s (%s)\n", num, m, path ? path : "");
}

} // namespace

void fake_sclang_start(void) {
    if (st != NULL) {
        return;
    }
    st = lo_server_thread_new("57120", on_error);
    if (st == NULL) {
        fprintf(stderr, "fake_sclang: cannot listen on 57120\n");
        return;
    }
    matron = lo_address_new("127.0.0.1", "8888");
    lo_server_thread_add_method(st, "/ready", NULL, on_ready, NULL);
    lo_server_thread_add_method(st, "/report/engines", NULL, on_report_engines, NULL);
    lo_server_thread_add_method(st, "/report/commands", NULL, on_report_commands, NULL);
    lo_server_thread_add_method(st, "/report/polls", NULL, on_report_polls, NULL);
    lo_server_thread_add_method(st, "/engine/load/name", "s", on_load, NULL);
    lo_server_thread_add_method(st, "/engine/free", NULL, on_free, NULL);
    lo_server_thread_add_method(st, NULL, NULL, on_other, NULL);
    lo_server_thread_start(st);
    fprintf(stderr, "fake_sclang: listening on 57120 (no SuperCollider yet)\n");
}
