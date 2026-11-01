/*
 * jack/types.h (jacklite)
 *
 * the part of the JACK API that norns uses, implemented in-process by jacklite.cc: on android
 * there is no jack server, so crone, matron's clock and (later) scsynth share one process
 * graph driven by the phone's audio device.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t jack_nframes_t;
typedef uint64_t jack_time_t;
typedef float jack_default_audio_sample_t;
typedef uint64_t jack_port_id_t;

typedef struct _jack_client jack_client_t;
typedef struct _jack_port jack_port_t;

#define JACK_DEFAULT_AUDIO_TYPE "32 bit float mono audio"
#define JACK_DEFAULT_MIDI_TYPE "8 bit raw midi"
#define JACK_MAX_FRAMES (4294967295U)

enum JackOptions {
    JackNullOption = 0x00,
    JackNoStartServer = 0x01,
    JackUseExactName = 0x02,
    JackServerName = 0x04,
    JackLoadName = 0x08,
    JackLoadInit = 0x10,
    JackSessionID = 0x20
};
typedef enum JackOptions jack_options_t;

enum JackStatus {
    JackFailure = 0x01,
    JackInvalidOption = 0x02,
    JackNameNotUnique = 0x04,
    JackServerStarted = 0x08,
    JackServerFailed = 0x10,
    JackServerError = 0x20,
    JackNoSuchClient = 0x40,
    JackLoadFailure = 0x80,
    JackInitFailure = 0x100,
    JackShmFailure = 0x200,
    JackVersionError = 0x400,
    JackBackendError = 0x800,
    JackClientZombie = 0x1000
};
typedef enum JackStatus jack_status_t;

enum JackPortFlags {
    JackPortIsInput = 0x1,
    JackPortIsOutput = 0x2,
    JackPortIsPhysical = 0x4,
    JackPortCanMonitor = 0x8,
    JackPortIsTerminal = 0x10
};

typedef int (*JackProcessCallback)(jack_nframes_t nframes, void *arg);
typedef void (*JackShutdownCallback)(void *arg);
typedef int (*JackXRunCallback)(void *arg);
typedef int (*JackBufferSizeCallback)(jack_nframes_t nframes, void *arg);
typedef int (*JackSampleRateCallback)(jack_nframes_t nframes, void *arg);
typedef void (*JackThreadInitCallback)(void *arg);

#ifdef __cplusplus
}
#endif
