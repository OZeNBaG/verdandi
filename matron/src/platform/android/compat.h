/*
 * compat.h
 *
 * force-included (-include) into every matron/sidecar translation unit on Android.
 * fills the gaps between glibc linux and bionic so upstream sources build unchanged.
 */

#pragma once

#ifdef __ANDROID__

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

//--- pthread_cancel / pthread_testcancel: bionic has neither.
// norns cancels metro threads (sleeping in clock_nanosleep), device reader threads (blocked
// in read) and a few pollers. emulation: a cancel marks the thread and interrupts it with
// NORNS_CANCEL_SIGNAL (its handler does nothing, so the blocking call returns EINTR);
// pthread_testcancel() and the reader loops then leave the thread. the table lives in
// android_compat.c.
#define NORNS_CANCEL_SIGNAL SIGUSR2
extern int norns_pthread_cancel(pthread_t t);
extern void norns_pthread_testcancel(void);
extern int norns_pthread_cancel_requested(void);
#define pthread_cancel(t) norns_pthread_cancel(t)
#define pthread_testcancel() norns_pthread_testcancel()
#ifndef PTHREAD_CANCEL_ENABLE
#define PTHREAD_CANCEL_ENABLE 0
#define PTHREAD_CANCEL_DISABLE 1
#define PTHREAD_CANCEL_DEFERRED 0
#define PTHREAD_CANCEL_ASYNCHRONOUS 1
static inline int pthread_setcancelstate(int s, int *old) {
    (void)s;
    if (old) {
        *old = PTHREAD_CANCEL_ENABLE;
    }
    return 0;
}
static inline int pthread_setcanceltype(int t, int *old) {
    (void)t;
    if (old) {
        *old = PTHREAD_CANCEL_DEFERRED;
    }
    return 0;
}
#endif

// the shell: android has no /bin/sh
#define NORNS_SHELL "/system/bin/sh"

#ifdef __cplusplus
}
#endif

//--- lua: matron's C++ sources include the lua headers without extern "C" (on norns they
// link against a C++-built lua). here lua is built as C, so give every declaration C linkage;
// the include guards make the later #include <lua.h> in matron a no-op.
#ifdef __cplusplus
extern "C" {
#endif
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#ifdef __cplusplus
}
#endif

#endif // __ANDROID__
