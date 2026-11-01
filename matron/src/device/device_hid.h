#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "device_common.h"
#if !defined(__APPLE__) && !defined(__ANDROID__)
#include <libevdev/libevdev.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DEV_GUID_LEN 33

typedef uint8_t dev_vid_t;
typedef uint8_t dev_pid_t;
typedef uint16_t dev_code_t;

struct dev_hid {
    struct dev_common base;
#ifdef __APPLE__
    // IOHIDDeviceRef (retained by the device monitor), see platform/macos/device_hid_iohid.cc
    void *dev;
    // primary usage of the device (keyboard, mouse, joystick, gamepad); selects button codes
    uint32_t app_usage;
#elif defined(__ANDROID__)
    // the java side's InputDevice id; events arrive through android_hid_event()
    void *dev;
#else
    struct libevdev *dev;
#endif
    // identifiers
    dev_vid_t vid;
    dev_pid_t pid;
    char guid[DEV_GUID_LEN];
    // count of supported event types
    int num_types;
    // array of supported event types
    uint8_t *types;
    // count of supported event codes per event type
    int *num_codes;
    // arrays of supported event codes per event type
    dev_code_t **codes;
};

extern int dev_hid_init(void *self);
extern void *dev_hid_start(void *self);
extern void dev_hid_deinit(void *self);

#ifdef __cplusplus
}
#endif
