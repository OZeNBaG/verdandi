/*
 * device_android.cc
 *
 * android replacements for the linux device layer: device_monitor.cc (udev) and
 * device_hid.cc (evdev). usb midi, grids and hid devices reach android apps through java
 * apis (android.media.midi, UsbManager, InputDevice); until those are wired up, no devices
 * are found and none can be opened. the virtual midi device still works (device_midi).
 */

#include <stdio.h>

#include "device.h"
#include "device_hid.h"
#include "device_monitor.h"

void dev_monitor_init(void) {
}

void dev_monitor_deinit(void) {
}

int dev_monitor_scan(void) {
    return 0;
}

int dev_hid_init(void *self) {
    (void)self;
    fprintf(stderr, "dev_hid_init: no hid devices on android yet\n");
    return -1;
}

void *dev_hid_start(void *self) {
    (void)self;
    return NULL;
}

void dev_hid_deinit(void *self) {
    (void)self;
}
