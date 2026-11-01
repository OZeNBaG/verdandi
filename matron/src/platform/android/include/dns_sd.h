/*
 * dns_sd.h (Android stand-in)
 *
 * android has no DNS-SD C API (bonjour goes through NsdManager in java). osc.cc only
 * announces matron's OSC port; on android that is a no-op.
 */

#pragma once

#include <stdint.h>

typedef void *DNSServiceRef;
typedef int32_t DNSServiceErrorType;

static inline DNSServiceErrorType DNSServiceRegister(DNSServiceRef *ref, uint32_t flags, uint32_t iface,
                                                     const char *name, const char *regtype, const char *domain,
                                                     const char *host, uint16_t port, uint16_t txtlen,
                                                     const void *txt, void *callback, void *context) {
    (void)flags;
    (void)iface;
    (void)name;
    (void)regtype;
    (void)domain;
    (void)host;
    (void)port;
    (void)txtlen;
    (void)txt;
    (void)callback;
    (void)context;
    *ref = 0;
    return 0;
}

static inline void DNSServiceRefDeallocate(DNSServiceRef ref) {
    (void)ref;
}
