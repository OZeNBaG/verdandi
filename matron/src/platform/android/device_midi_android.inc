// -*- c++ -*-
// device_midi_android.inc
//
// android backend for device_midi.cc, included in place of the ALSA functions so the byte
// parser above (running status, sysex chunks, midi clock) stays shared.
//
// input: whoever feeds a device (the java side, later) writes raw midi bytes into the
// device's pipe; dev_midi_start reads the pipe on the device thread like snd_rawmidi_read.
// for now only the "virtual" device exists: what scripts send to it goes nowhere.

#include <fcntl.h>
#include <unistd.h>

static int midi_pipe_open(struct dev_midi *midi) {
    int fds[2];
    if (pipe(fds) < 0) {
        perror("dev_midi: pipe");
        return -1;
    }
    fcntl(fds[1], F_SETFL, O_NONBLOCK);
    midi->pipe_rd = fds[0];
    midi->pipe_wr = fds[1];
    return 0;
}

unsigned int dev_midi_port_count(const char *path) {
    (void)path;
    return 1;
}

int dev_midi_init(void *self, unsigned int port_index, bool multiport_device) {
    (void)port_index;
    (void)multiport_device;
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;
    midi->port_id = -1;
    midi->pipe_rd = midi->pipe_wr = -1;
    if (midi_pipe_open(midi) < 0) {
        return -1;
    }
    base->start = &dev_midi_start;
    base->deinit = &dev_midi_deinit;
    midi->clock_enabled = true;
    return 0;
}

int dev_midi_virtual_init(void *self) {
    return dev_midi_init(self, 0, false);
}

void dev_midi_deinit(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    if (midi->pipe_wr >= 0) {
        close(midi->pipe_wr);
    }
    if (midi->pipe_rd >= 0) {
        close(midi->pipe_rd);
    }
}

void *dev_midi_start(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;
    midi_input_state_t state = {};
    ssize_t n;

    if (midi->pipe_rd < 0) {
        return NULL;
    }
    while ((n = read(midi->pipe_rd, state.buffer, DEV_MIDI_INPUT_BUFFER_SIZE)) > 0) {
        if (dev_midi_consume_buffer(&state, n, midi) != n) {
            fprintf(stderr, "midi inconsistency for device: %s\n", base->name);
        }
    }
    return NULL;
}

ssize_t dev_midi_send(void *self, uint8_t *data, size_t n) {
    (void)self;
    (void)data;
    return (ssize_t)n;
}
