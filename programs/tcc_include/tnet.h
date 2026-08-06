/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyOS userspace networking helper — Cycle 8 N4.
 *
 * Sockets themselves are the standard BSD API (<sys/socket.h> etc.); the only
 * piece the C library can't provide yet is name resolution (no UDP sockets for
 * musl's resolver), so this thin header exposes the kernel DNS resolver through
 * a private syscall.  Plain C, so on-device TinyCC accepts it. */
#ifndef TNET_H
#define TNET_H

#include <stdint.h>
#include <unistd.h>          /* musl syscall() */

#define NET_SYS_RESOLVE 0x4E455400L      /* must match kernel/arch/arch64.c */
#define NET_SYS_PING    0x4E455401L      /* must match kernel/arch/arch64.c */
#define NET_SYS_IFCONFIG 0x4E455402L     /* must match kernel/arch/arch64.c */
#define NET_SYS_SETCFG   0x4E455403L     /* must match kernel/arch/arch64.c */

/* Interface configuration (all IPv4 fields in host byte order). */
struct tnet_ifcfg {
    uint32_t ip, mask, gw, dns;
    unsigned char mac[6];
    unsigned char up;
    unsigned char _pad;
};

/* Resolve a hostname to an IPv4 address in host byte order; 0 on failure. */
static uint32_t net_resolve(const char *name) {
    long r = syscall(NET_SYS_RESOLVE, (long)name);
    return r < 0 ? 0u : (uint32_t)r;
}

/* Send one ICMP echo to an IPv4 address (host order); round-trip microseconds, or <0 on timeout. */
static long net_ping(uint32_t ip) {
    return syscall(NET_SYS_PING, (long)ip);
}

/* Fetch the current interface configuration; 0 on success. */
static int net_ifconfig(struct tnet_ifcfg *c) {
    return (int)syscall(NET_SYS_IFCONFIG, (long)c);
}

/* Apply a static IPv4 configuration (all args host byte order); 0 on success. */
static int net_setcfg(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    return (int)syscall(NET_SYS_SETCFG, (long)ip, (long)mask, (long)gw, (long)dns);
}

#endif /* TNET_H */
