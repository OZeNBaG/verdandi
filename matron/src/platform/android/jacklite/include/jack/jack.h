/*
 * jack/jack.h (jacklite): see types.h
 */

#pragma once

#include <stddef.h>

#include <jack/types.h>

#ifdef __cplusplus
extern "C" {
#endif

jack_client_t *jack_client_open(const char *client_name, jack_options_t options, jack_status_t *status, ...);
int jack_client_close(jack_client_t *client);
char *jack_get_client_name(jack_client_t *client);

int jack_set_process_callback(jack_client_t *client, JackProcessCallback cb, void *arg);
void jack_on_shutdown(jack_client_t *client, JackShutdownCallback cb, void *arg);
int jack_set_xrun_callback(jack_client_t *client, JackXRunCallback cb, void *arg);
int jack_set_buffer_size_callback(jack_client_t *client, JackBufferSizeCallback cb, void *arg);
int jack_set_sample_rate_callback(jack_client_t *client, JackSampleRateCallback cb, void *arg);
int jack_set_thread_init_callback(jack_client_t *client, JackThreadInitCallback cb, void *arg);

int jack_activate(jack_client_t *client);
int jack_deactivate(jack_client_t *client);

jack_nframes_t jack_get_sample_rate(jack_client_t *client);
jack_nframes_t jack_get_buffer_size(jack_client_t *client);
float jack_cpu_load(jack_client_t *client);

jack_port_t *jack_port_register(jack_client_t *client, const char *port_name, const char *port_type,
                                unsigned long flags, unsigned long buffer_size);
int jack_port_unregister(jack_client_t *client, jack_port_t *port);
void *jack_port_get_buffer(jack_port_t *port, jack_nframes_t nframes);
const char *jack_port_name(const jack_port_t *port);
const char *jack_port_short_name(const jack_port_t *port);
int jack_port_flags(const jack_port_t *port);
const char *jack_port_type(const jack_port_t *port);
jack_port_t *jack_port_by_name(jack_client_t *client, const char *port_name);
int jack_port_connected(const jack_port_t *port);
const char **jack_port_get_connections(const jack_port_t *port);
const char **jack_port_get_all_connections(const jack_client_t *client, const jack_port_t *port);
const char **jack_get_ports(jack_client_t *client, const char *port_name_pattern, const char *type_name_pattern,
                            unsigned long flags);
int jack_connect(jack_client_t *client, const char *source_port, const char *destination_port);
int jack_disconnect(jack_client_t *client, const char *source_port, const char *destination_port);
int jack_port_disconnect(jack_client_t *client, jack_port_t *port);

void jack_free(void *ptr);

jack_nframes_t jack_frame_time(const jack_client_t *client);
jack_nframes_t jack_frames_since_cycle_start(const jack_client_t *client);
jack_nframes_t jack_last_frame_time(const jack_client_t *client);
jack_time_t jack_get_time(void);
int jack_get_cycle_times(const jack_client_t *client, jack_nframes_t *current_frames, jack_time_t *current_usecs,
                         jack_time_t *next_usecs, float *period_usecs);

//--- jacklite: the driver side (platform/android)

// start the graph at this rate; processing is driven by jacklite_process()
void jacklite_init(jack_nframes_t sample_rate, jack_nframes_t max_block);
jack_nframes_t jacklite_sample_rate(void);
// one cycle: in/out are deinterleaved channel pointers (system:capture_N / system:playback_N)
void jacklite_process(const float *const *in, int num_in, float *const *out, int num_out, jack_nframes_t nframes);
void jacklite_xrun(void);
// no audio device: a thread runs the graph in real time, output discarded
void jacklite_start_null_driver(jack_nframes_t block);

#ifdef __cplusplus
}
#endif
