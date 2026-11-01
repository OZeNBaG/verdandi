/*
 * android_compat.c
 *
 * pthread_cancel emulation for bionic (see compat.h).
 */

#include <pthread.h>
#include <signal.h>
#include <string.h>

#define MAX_CANCELLED 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t cancelled[MAX_CANCELLED];
static int num_cancelled = 0;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void on_cancel_signal(int sig) {
    (void)sig;
}

static void install_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_cancel_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // no SA_RESTART: blocking calls return EINTR
    sigaction(SIGUSR2, &sa, NULL);
}

static int find(pthread_t t) {
    for (int i = 0; i < num_cancelled; i++) {
        if (pthread_equal(cancelled[i], t)) {
            return i;
        }
    }
    return -1;
}

int norns_pthread_cancel(pthread_t t) {
    pthread_once(&once, install_handler);
    pthread_mutex_lock(&lock);
    if (find(t) < 0) {
        if (num_cancelled == MAX_CANCELLED) {
            // oldest entries belong to threads that have long exited
            memmove(&cancelled[0], &cancelled[1], sizeof(pthread_t) * (MAX_CANCELLED - 1));
            num_cancelled--;
        }
        cancelled[num_cancelled++] = t;
    }
    pthread_mutex_unlock(&lock);
    int r = pthread_kill(t, SIGUSR2);
    return r;
}

int norns_pthread_cancel_requested(void) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock);
    int i = find(self);
    pthread_mutex_unlock(&lock);
    return i >= 0;
}

void norns_pthread_testcancel(void) {
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock);
    int i = find(self);
    if (i >= 0) {
        // the id can be reused by a new thread: forget it on the way out
        memmove(&cancelled[i], &cancelled[i + 1], sizeof(pthread_t) * (num_cancelled - i - 1));
        num_cancelled--;
    }
    pthread_mutex_unlock(&lock);
    if (i >= 0) {
        pthread_exit(NULL);
    }
}
