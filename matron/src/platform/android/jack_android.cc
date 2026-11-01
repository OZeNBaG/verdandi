/*
 * jack_android.cc
 *
 * android replacement for jack_client.cpp. there is no jack server on android: matron only
 * needs the sample rate, a monotonic audio-ish clock and the (empty) port lists.
 */

#include <stdlib.h>
#include <time.h>

#include "jack_client.h"

double jack_sample_rate = 48000.0;

int jack_client_init() {
    return 0;
}

void jack_client_deinit() {
}

float jack_client_get_cpu_load() {
    return 0.f;
}

uint32_t jack_client_get_xrun_count() {
    return 0;
}

double jack_client_get_current_time() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char **empty_list() {
    const char **l = (const char **)calloc(1, sizeof(char *));
    return l;
}

const char **jack_client_get_input_ports() {
    return empty_list();
}

const char **jack_client_get_output_ports() {
    return empty_list();
}

const char **jack_client_get_port_connections(const char *port_name) {
    (void)port_name;
    return empty_list();
}

void jack_client_free_port_list(const char **list) {
    free((void *)list);
}

bool jack_client_connect(const char *source_name, const char *destination_name) {
    (void)source_name;
    (void)destination_name;
    return false;
}

bool jack_client_disconnect(const char *source_name, const char *destination_name) {
    (void)source_name;
    (void)destination_name;
    return false;
}
