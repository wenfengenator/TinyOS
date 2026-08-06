/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 minimal TCP/IP stack — Cycle 8 Phase N2.
 *
 * A compact, dependency-free network stack over the virtio-net frame transport
 * (kernel/drivers/vnet64.c): ARP, IPv4 + checksum, ICMP echo (ping), UDP, and a
 * small DNS resolver.  It is polled and synchronous — request/reply loops spin
 * on net_poll() until the expected packet arrives or a guard elapses — which
 * fits the kernel's interrupt-free driver model.
 *
 * Addressing uses the QEMU SLIRP layout (10.0.2.15, gw .2, dns .3).  All on-wire
 * multi-byte fields are big-endian; IP addresses are kept host-order and
 * serialized big-endian on the wire.  TCP and a DHCP client come later. */
#include "boot64.h"

/* ---- serialization ---- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* ---- ones-complement Internet checksum ---- */
static uint32_t sum16(const void *data, int len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { sum += get16(p); p += 2; len -= 2; }
    if (len) sum += (uint16_t)(p[0] << 8);
    return sum;
}
static uint16_t fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- configuration (SLIRP defaults) ---- */
#define IP4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
static uint8_t  g_mac[6];
static uint32_t g_ip   = IP4(10,0,2,15);
static uint32_t g_gw   = IP4(10,0,2,2);
static uint32_t g_mask = IP4(255,255,255,0);
static uint32_t g_dns  = IP4(10,0,2,3);
static int      g_up;

/* ---- NIC backend: virtio-net (QEMU) or netvsc (Hyper-V), picked at init ---- */
static int g_nic;                                /* 0 none, 1 virtio, 2 netvsc */
int nic_init(void) {
    if (vnet_init())   { g_nic = 1; return 1; }
    if (netvsc_init()) { g_nic = 2; return 1; }
    g_nic = 0; return 0;
}
static void nic_mac(uint8_t out[6]) { if (g_nic == 2) netvsc_mac(out); else vnet_mac(out); }
static int  nic_tx(const void *f, uint32_t l) { return g_nic == 2 ? netvsc_tx(f, l) : vnet_tx(f, l); }
static int  nic_rx(void *b, uint32_t m)       { return g_nic == 2 ? netvsc_rx(b, m) : vnet_rx(b, m); }

static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* ---- ARP cache ---- */
struct arpent { uint32_t ip; uint8_t mac[6]; int valid; };
static struct arpent g_arp[8];

static const uint8_t *arp_lookup(uint32_t ip) {
    for (int i = 0; i < 8; i++) if (g_arp[i].valid && g_arp[i].ip == ip) return g_arp[i].mac;
    return 0;
}
static void arp_insert(uint32_t ip, const uint8_t *mac) {
    int slot = -1;
    for (int i = 0; i < 8; i++) { if (g_arp[i].valid && g_arp[i].ip == ip) { slot = i; break; } if (slot < 0 && !g_arp[i].valid) slot = i; }
    if (slot < 0) slot = 0;
    g_arp[slot].ip = ip; for (int i = 0; i < 6; i++) g_arp[slot].mac[i] = mac[i]; g_arp[slot].valid = 1;
}

/* ---- shared TX/RX scratch (single-threaded, synchronous) ---- */
static uint8_t g_tx[1600];
static uint8_t g_rx[1600];

/* loopback queue: IP packets addressed to ourselves are delivered internally
 * (drained by net_poll) rather than hitting the wire, so two local TCP endpoints
 * can talk for the self-test with no external host. */
#define LO_SLOTS 8
static uint8_t g_lo[LO_SLOTS][1600];
static int     g_lolen[LO_SLOTS], g_lohead, g_lotail;
static void lo_enqueue(const uint8_t *ip, int len) {
    int nt = (g_lotail + 1) % LO_SLOTS;
    if (nt == g_lohead || len > 1600) return;
    memcpy(g_lo[g_lotail], ip, len); g_lolen[g_lotail] = len; g_lotail = nt;
}
uint32_t net_local_ip(void) { return g_ip; }

/* ---- ARP ---- */
static void arp_send(uint16_t op, uint32_t tip, const uint8_t *tmac) {
    uint8_t a[28];
    put16(a + 0, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, op);
    for (int i = 0; i < 6; i++) a[8 + i] = g_mac[i];
    put32(a + 14, g_ip);
    for (int i = 0; i < 6; i++) a[18 + i] = tmac ? tmac[i] : 0;
    put32(a + 24, tip);
    const uint8_t *dst = (op == 1) ? BCAST : tmac;
    for (int i = 0; i < 6; i++) { g_tx[i] = dst[i]; g_tx[6 + i] = g_mac[i]; }
    put16(g_tx + 12, 0x0806);
    memcpy(g_tx + 14, a, 28);
    nic_tx(g_tx, 14 + 28);
}
static void arp_input(const uint8_t *a, int len) {
    if (len < 28) return;
    uint16_t op = get16(a + 6);
    uint32_t spa = get32(a + 14);
    arp_insert(spa, a + 8);
    if (op == 1 && get32(a + 24) == g_ip) arp_send(2, spa, a + 8);
}

/* ---- ICMP echo state ---- */
static volatile int g_ping_got;
static uint16_t     g_ping_id, g_ping_seq;
static void icmp_input(uint32_t src, const uint8_t *ic, int len) {
    if (len < 8) return;
    if (ic[0] == 0) {                                /* echo reply: match our outstanding ping */
        if (get16(ic + 4) == g_ping_id && get16(ic + 6) == g_ping_seq) g_ping_got = 1;
        return;
    }
    if (ic[0] == 8) {                                /* echo request: answer it (also drives loopback ping) */
        static uint8_t rep[1500];
        if (len > (int)sizeof rep) return;
        memcpy(rep, ic, len);
        rep[0] = 0; put16(rep + 2, 0);
        put16(rep + 2, fold(sum16(rep, len, 0)));
        net_ip_send(src, 1, rep, len);
    }
}

/* ---- UDP receive capture (for synchronous request/reply) ---- */
static volatile int g_udp_got;
static uint16_t     g_udp_port;
static uint8_t      g_udp_data[1024];
static int          g_udp_len;
static void udp_input(const uint8_t *u, int len) {
    if (len < 8) return;
    if (get16(u + 2) != g_udp_port) return;
    int dlen = (int)get16(u + 4) - 8;
    if (dlen < 0) return;
    if (dlen > len - 8) dlen = len - 8;
    if (dlen > (int)sizeof g_udp_data) dlen = sizeof g_udp_data;
    memcpy(g_udp_data, u + 8, dlen);
    g_udp_len = dlen; g_udp_got = 1;
}

/* ---- IPv4 ---- */
static void ip_input(const uint8_t *ip, int len) {
    if (len < 20 || (ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0xF) * 4;
    if (ihl < 20 || len < ihl) return;
    uint16_t tot = get16(ip + 2);
    if (tot > len) tot = (uint16_t)len;
    int pll = tot - ihl;
    if (pll < 0) return;
    const uint8_t *pl = ip + ihl;
    if (ip[9] == 1)  icmp_input(get32(ip + 12), pl, pll);
    else if (ip[9] == 17) udp_input(pl, pll);
    else if (ip[9] == 6)  tcp_input(get32(ip + 12), pl, pll);
}
int net_ip_send(uint32_t dst, uint8_t proto, const void *payload, int plen) {
    static uint16_t ipid = 1;
    uint8_t *p = g_tx + 14;
    p[0] = 0x45; p[1] = 0; put16(p + 2, (uint16_t)(20 + plen));
    put16(p + 4, ipid++); put16(p + 6, 0);
    p[8] = 64; p[9] = proto; put16(p + 10, 0);
    put32(p + 12, g_ip); put32(p + 16, dst);
    put16(p + 10, fold(sum16(p, 20, 0)));
    memcpy(p + 20, payload, plen);
    if (dst == g_ip || (dst & 0xFF000000u) == 0x7F000000u) { lo_enqueue(p, 20 + plen); return 0; }  /* loopback: our IP or 127/8 */
    const uint8_t *mac;
    if (dst == 0xFFFFFFFFu) mac = BCAST;
    else {
        uint32_t nh = ((dst & g_mask) == (g_ip & g_mask)) ? dst : g_gw;
        mac = arp_lookup(nh);
        if (!mac) return -1;
    }
    for (int i = 0; i < 6; i++) { g_tx[i] = mac[i]; g_tx[6 + i] = g_mac[i]; }
    put16(g_tx + 12, 0x0800);
    return nic_tx(g_tx, 14 + 20 + plen);
}

/* ---- UDP ---- */
static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, int dlen) {
    static uint8_t u[1200];
    if (dlen > (int)sizeof u - 8) return -1;
    put16(u + 0, sport); put16(u + 2, dport); put16(u + 4, (uint16_t)(8 + dlen)); put16(u + 6, 0);
    memcpy(u + 8, data, dlen);                       /* checksum 0 = not computed (legal for IPv4) */
    return net_ip_send(dst, 17, u, 8 + dlen);
}

/* ---- receive dispatch + poll ---- */
static void net_rx_process(const uint8_t *f, int len) {
    if (len < 14) return;
    uint16_t et = get16(f + 12);
    if (et == 0x0806) arp_input(f + 14, len - 14);
    else if (et == 0x0800) ip_input(f + 14, len - 14);
}
int net_poll(void) {
    if (g_lohead != g_lotail) {                       /* deliver a loopback packet first */
        int i = g_lohead; g_lohead = (g_lohead + 1) % LO_SLOTS;
        ip_input(g_lo[i], g_lolen[i]);
        return 1;
    }
    int n = nic_rx(g_rx, sizeof g_rx);
    if (n <= 0) return 0;
    net_rx_process(g_rx, n);
    return 1;
}

/* Clear the RX backlog so a full recv buffer can't drop the reply we are about to await. */
void net_drain(void) { int d = 0; while (d++ < 8192 && net_poll()) ; }

static const uint8_t *arp_resolve(uint32_t ip) {
    const uint8_t *m = arp_lookup(ip);
    if (m) return m;
    for (int t = 0; t < 4; t++) {
        net_drain();                                 /* free recv-buffer room for the reply */
        arp_send(1, ip, 0);
        for (uint32_t g = 0; g < 20000000u; g++) {
            if (net_poll() && (m = arp_lookup(ip))) return m;
            __asm__ volatile("pause");
        }
    }
    return arp_lookup(ip);
}

int net_ping(uint32_t dst, uint32_t *rtt_us) {
    if (dst != g_ip && (dst & 0xFF000000u) != 0x7F000000u) {   /* loopback needs no ARP */
        uint32_t nh = ((dst & g_mask) == (g_ip & g_mask)) ? dst : g_gw;
        if (!arp_resolve(nh)) return -1;
    }
    static uint16_t seq = 0;
    uint8_t ic[40];
    ic[0] = 8; ic[1] = 0; put16(ic + 2, 0);
    g_ping_id = 0x1234; g_ping_seq = ++seq;
    put16(ic + 4, g_ping_id); put16(ic + 6, g_ping_seq);
    for (int i = 0; i < 32; i++) ic[8 + i] = (uint8_t)('a' + (i & 15));
    put16(ic + 2, fold(sum16(ic, 40, 0)));
    net_drain();                                     /* free recv-buffer room for the echo reply */
    g_ping_got = 0;
    uint64_t t0 = micros();
    if (net_ip_send(dst, 1, ic, 40) != 0) return -1;
    for (uint32_t g = 0; g < 80000000u; g++) {
        if (net_poll() && g_ping_got) { if (rtt_us) *rtt_us = (uint32_t)(micros() - t0); return 0; }
        __asm__ volatile("pause");
    }
    return -1;
}

/* ---- DNS (A record) ---- */
static int dns_skip_name(const uint8_t *r, int rl, int p) {
    while (p < rl) {
        uint8_t b = r[p];
        if (b == 0) return p + 1;
        if ((b & 0xC0) == 0xC0) return p + 2;        /* compression pointer ends the name */
        p += b + 1;
    }
    return p;
}
static uint32_t dns_resolve(const char *name) {
    uint8_t q[300]; int o = 12;
    uint16_t txid = 0xABCD;
    put16(q + 0, txid); put16(q + 2, 0x0100);        /* RD = 1 */
    put16(q + 4, 1); put16(q + 6, 0); put16(q + 8, 0); put16(q + 10, 0);
    const char *s = name;
    while (*s) {
        int l = 0; while (s[l] && s[l] != '.') l++;
        if (l > 63 || o + l + 1 > (int)sizeof q - 6) return 0;
        q[o++] = (uint8_t)l;
        for (int i = 0; i < l; i++) q[o++] = (uint8_t)s[i];
        s += l; if (*s == '.') s++;
    }
    q[o++] = 0; put16(q + o, 1); o += 2; put16(q + o, 1); o += 2;   /* QTYPE A, QCLASS IN */

    g_udp_port = 0xC000;
    if (!arp_resolve(((g_dns & g_mask) == (g_ip & g_mask)) ? g_dns : g_gw)) return 0;
    net_drain();                                     /* free recv-buffer room for the DNS reply */
    g_udp_got = 0;
    if (udp_send(g_dns, 0xC000, 53, q, o) != 0) return 0;
    for (uint32_t g = 0; g < 120000000u; g++) {
        if (net_poll() && g_udp_got) break;
        __asm__ volatile("pause");
    }
    if (!g_udp_got) return 0;

    const uint8_t *r = g_udp_data; int rl = g_udp_len;
    if (rl < 12 || get16(r) != txid) return 0;
    int an = get16(r + 6);
    if (an < 1) return 0;
    int p = dns_skip_name(r, rl, 12) + 4;            /* skip the question */
    for (int i = 0; i < an && p + 10 <= rl; i++) {
        p = dns_skip_name(r, rl, p);
        if (p + 10 > rl) break;
        uint16_t type = get16(r + p), rdlen = get16(r + p + 8);
        p += 10;
        if (type == 1 && rdlen == 4 && p + 4 <= rl) return get32(r + p);
        p += rdlen;
    }
    return 0;
}

static void put_ip(uint32_t ip) {
    kput_u32dec(ip >> 24); kputc('.'); kput_u32dec((ip >> 16) & 0xFF);
    kputc('.'); kput_u32dec((ip >> 8) & 0xFF); kputc('.'); kput_u32dec(ip & 0xFF);
}

/* ---- DHCP client: learn ip/mask/gw/dns from the network (needed off SLIRP) ---- */
#define DHCP_XID 0x3903F326u
static uint8_t g_dhcp_rx[600];
static int dhcp_opt(const uint8_t *o, int olen, uint8_t want, uint8_t *out, int outmax) {
    int i = 0;
    while (i < olen) {
        uint8_t code = o[i++];
        if (code == 0) continue;                     /* pad */
        if (code == 255 || i >= olen) break;         /* end */
        int l = o[i++];
        if (i + l > olen) break;
        if (code == want) { int n = l < outmax ? l : outmax; for (int j = 0; j < n; j++) out[j] = o[i + j]; return n; }
        i += l;
    }
    return -1;
}
static int dhcp_txn(int type, uint32_t reqip, uint32_t server, int *rlen) {
    static uint8_t p[300];
    memset(p, 0, sizeof p);
    p[0] = 1; p[1] = 1; p[2] = 6;                    /* op=BOOTREQUEST, htype=ETH, hlen=6 */
    put32(p + 4, DHCP_XID);
    put16(p + 10, 0x8000);                           /* ask the server to broadcast the reply */
    for (int i = 0; i < 6; i++) p[28 + i] = g_mac[i];/* chaddr */
    put32(p + 236, 0x63825363);                      /* magic cookie */
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = (uint8_t)type;
    if (type == 3) {                                 /* REQUEST echoes the offered IP + server id */
        p[o++] = 50; p[o++] = 4; put32(p + o, reqip); o += 4;
        p[o++] = 54; p[o++] = 4; put32(p + o, server); o += 4;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;   /* request mask, router, dns */
    p[o++] = 255;

    g_udp_port = 68;
    net_drain();                                        /* free recv-buffer room for the reply */
    g_udp_got = 0;
    if (udp_send(0xFFFFFFFFu, 68, 67, p, o) != 0) return -1;
    for (uint32_t g = 0; g < 200000000u; g++) {
        if (net_poll() && g_udp_got) {
            if (g_udp_len >= 240 && get32(g_udp_data + 4) == DHCP_XID) break;   /* our transaction */
            g_udp_got = 0;                            /* stray DHCP traffic, keep waiting */
        }
        __asm__ volatile("pause");
    }
    if (!g_udp_got) return -1;
    int n = g_udp_len; if (n > (int)sizeof g_dhcp_rx) n = sizeof g_dhcp_rx;
    for (int i = 0; i < n; i++) g_dhcp_rx[i] = g_udp_data[i];
    *rlen = n;
    return 0;
}
static int dhcp_once(void) {
    uint32_t saved = g_ip;
    g_ip = 0;                                         /* no address during discovery */
    int rlen;
    if (dhcp_txn(1, 0, 0, &rlen) != 0) { g_ip = saved; return -1; }         /* DISCOVER -> OFFER */
    uint32_t yi = get32(g_dhcp_rx + 16);
    uint8_t sid[4]; uint32_t server = (dhcp_opt(g_dhcp_rx + 240, rlen - 240, 54, sid, 4) == 4) ? get32(sid) : 0;
    if (dhcp_txn(3, yi, server, &rlen) != 0) { g_ip = saved; return -1; }   /* REQUEST -> ACK */
    uint8_t mt[1];
    if (dhcp_opt(g_dhcp_rx + 240, rlen - 240, 53, mt, 1) != 1 || mt[0] != 5) { g_ip = saved; return -1; }
    uint32_t yiaddr = get32(g_dhcp_rx + 16);
    if (!yiaddr) { g_ip = saved; return -1; }
    uint8_t v[4];
    g_ip   = yiaddr;
    g_mask = (dhcp_opt(g_dhcp_rx + 240, rlen - 240, 1, v, 4) == 4) ? get32(v) : IP4(255,255,255,0);
    g_gw   = (dhcp_opt(g_dhcp_rx + 240, rlen - 240, 3, v, 4) == 4) ? get32(v) : 0;
    g_dns  = (dhcp_opt(g_dhcp_rx + 240, rlen - 240, 6, v, 4) == 4) ? get32(v) : g_gw;
    return 0;
}
static int dhcp_configure(void) {                    /* retry: the OFFER/ACK is often lost on a busy switch */
    for (int attempt = 0; attempt < 6; attempt++)
        if (dhcp_once() == 0) return 0;
    return -1;
}

int net_init(void) { nic_mac(g_mac); g_up = 1; return 1; }

/* Snapshot the current interface config for userspace ifconfig. */
void net_ifconfig(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns, uint8_t *mac, int *up) {
    if (ip) *ip = g_ip;
    if (mask) *mask = g_mask;
    if (gw) *gw = g_gw;
    if (dns) *dns = g_dns;
    if (mac) for (int i = 0; i < 6; i++) mac[i] = g_mac[i];
    if (up) *up = g_up;
}

/* Apply a static IPv4 configuration (host order); drops stale ARP since the gateway may change. */
void net_setcfg(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns) {
    g_ip = ip; g_mask = mask; g_gw = gw; g_dns = dns;
    for (int i = 0; i < 8; i++) g_arp[i].valid = 0;
}

uint32_t net_resolve(const char *name) { return dns_resolve(name); }   /* userspace DNS */

/* N2 gate: ping the gateway (ARP + IP + ICMP) and resolve a DNS name (UDP). */
int net_selftest(void) {
    if (!g_up) return -1;

    if (dhcp_configure() == 0) {
        kputs("[net] dhcp: ip "); put_ip(g_ip); kputs(" gw "); put_ip(g_gw);
        kputs(" dns "); put_ip(g_dns); kputc('\n');
    } else {
        kputs("[net] dhcp: no lease (using "); put_ip(g_ip); kputs(")\n");
    }
    { int nt = netvsc_diag(); if (nt > 0) { kputs("[net] netvsc frames sent "); kput_u32dec((uint32_t)nt); kputc('\n'); } }

    { uint32_t us = 0;                               /* loopback ICMP: exercises the echo responder */
      if (net_ping(0x7F000001u, &us) == 0) kputs("[net] ping 127.0.0.1 -> reply OK\n");
      else kputs("[net] ping 127.0.0.1 -> FAILED\n"); }

    if (arp_resolve(g_gw)) {                          /* ARP reply is unicast: proves directed RX */
        kputs("[net] gateway "); put_ip(g_gw); kputs(" ARP OK\n");
        if (net_ping(g_gw, 0) == 0) kputs("[net] ping -> reply OK\n");
        else kputs("[net] ping -> no ICMP reply (often blocked)\n");
    } else {
        kputs("[net] gateway "); put_ip(g_gw); kputs(" ARP FAILED (directed RX?)\n");
    }

    uint32_t a = dns_resolve("example.com");         /* depends on the host resolver via SLIRP */
    if (a) {
        kputs("[net] dns example.com -> "); put_ip(a); kputc('\n');
        uint32_t us = 0;                             /* external ICMP: works only if the path allows it */
        if (net_ping(a, &us) == 0) kputs("[net] ping example.com -> reply OK\n");
        else kputs("[net] ping example.com -> no reply (ICMP blocked or SLIRP/unicast RX)\n");
    } else kputs("[net] dns example.com -> no response (host resolver?)\n");

    kputs(tcp_selftest() == 0 ? "[net] tcp loopback connect+data+close OK\n"
                              : "[net] tcp loopback self-test FAILED\n");

    if (a) {                                         /* real-NIC TCP: GET a URL via SLIRP (best-effort) */
        int c = tcp_connect(a, 80);
        if (c >= 0) {
            static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
            tcp_send(c, req, sizeof req - 1);
            char resp[80]; int n = tcp_recv(c, resp, sizeof resp - 1);
            if (n > 0) {
                kputs("[net] http GET example.com -> ");
                for (int i = 0; i < n && resp[i] != '\r' && resp[i] != '\n'; i++) kputc(resp[i]);
                kputc('\n');
            } else kputs("[net] http GET example.com -> no data\n");
            tcp_close(c);
        } else kputs("[net] http GET example.com -> connect failed (egress?)\n");
    }

    {   /* N4: serve one HTTP response over loopback (proves the server path) */
        int srv = tcp_listen(8080);
        int cli = (srv >= 0) ? tcp_connect(net_local_ip(), 8080) : -1;
        int conn = (cli >= 0) ? tcp_accept(srv) : -1;
        int okh = 0;
        if (conn >= 0) {
            static const char get[] = "GET / HTTP/1.0\r\n\r\n";
            static const char rsp[] = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nhi";
            char t[64];
            tcp_send(cli, get, sizeof get - 1);
            tcp_recv(conn, t, sizeof t);                 /* server reads the request  */
            tcp_send(conn, rsp, sizeof rsp - 1);         /* server serves the response */
            int rn = tcp_recv(cli, t, sizeof t - 1);     /* client reads it            */
            if (rn >= 12) okh = (t[9] == '2' && t[10] == '0' && t[11] == '0');
        }
        tcp_close(cli); tcp_close(conn); tcp_close(srv);
        kputs(okh ? "[net] httpd serve loopback -> client got 200 OK\n"
                  : "[net] httpd serve loopback FAILED\n");
    }
    return 0;
}
