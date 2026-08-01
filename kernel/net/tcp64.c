/* tinyos x64 minimal TCP — Cycle 8 Phase N3.
 *
 * A compact, correct-enough TCP for a teaching OS: the connection state machine
 * (active + passive open, orderly close), sequence/acknowledgement handling, a
 * per-connection receive buffer, and stop-and-wait sending with retransmit of
 * the single outstanding segment.  It runs over the polled IP layer in
 * kernel/net/net64.c (net_ip_send / net_poll) and is exercised end to end by a
 * loopback self-test (a local client and server exchange bytes and close).
 *
 * Not implemented (not needed for the gate): sliding windows / pipelining,
 * congestion control, out-of-order reassembly, urgent data, options (MSS/SACK). */
#include "boot64.h"

/* ---- serialization + checksum (local copies; net64.c has its own) ---- */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static uint16_t cksum(const void *data, int len, uint32_t sum) {
    const uint8_t *p = data;
    while (len > 1) { sum += get16(p); p += 2; len -= 2; }
    if (len) sum += (uint16_t)(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- flags + states ---- */
#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
enum { CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTAB, FIN_WAIT1, FIN_WAIT2, CLOSE_WAIT, LAST_ACK, TIME_WAIT, CLOSING };

#define NTCB   8
#define RXCAP  8192
#define MSS    1200
#define GUARD  120000000u
#define IO_TIMEOUT_MS  20000u    /* wall-clock idle timeout for blocking send/recv (slow servers) */
#define SYN_TIMEOUT_MS 10000u    /* connect() SYN-ACK wait */
#define RETX_MS        200u      /* retransmit interval for the single outstanding segment */

struct tcb {
    int      used, state, listener, parent;
    uint32_t laddr, raddr;
    uint16_t lport, rport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    uint8_t  rx[RXCAP]; int rxlen;
    int      finrecv;
    uint8_t  ur[MSS]; int urlen; uint32_t urseq; uint8_t urflags; int urpending;   /* retransmit slot */
    int      backlog[NTCB], nback;
};
static struct tcb T[NTCB];
static uint32_t g_iss = 0x4B10000;
static uint16_t g_ephem = 40000;

static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }
static int seq_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

static struct tcb *tcb_alloc(void) {
    for (int i = 0; i < NTCB; i++) if (!T[i].used) { memset(&T[i], 0, sizeof T[i]); T[i].used = 1; T[i].parent = -1; return &T[i]; }
    return 0;
}
static int tcb_idx(struct tcb *t) { return (int)(t - T); }

/* Build + send one segment.  For SYN/data/FIN the caller advances snd_nxt. */
static void tcp_out(struct tcb *t, uint8_t flags, const uint8_t *data, int dlen, uint32_t seq) {
    uint8_t s[20 + MSS];
    put16(s + 0, t->lport); put16(s + 2, t->rport);
    put32(s + 4, seq); put32(s + 8, t->rcv_nxt);
    s[12] = 0x50; s[13] = flags;
    int wnd = RXCAP - t->rxlen; if (wnd < 0) wnd = 0;
    put16(s + 14, (uint16_t)wnd); put16(s + 16, 0); put16(s + 18, 0);
    if (dlen > 0) memcpy(s + 20, data, dlen);
    uint32_t ph = (t->laddr >> 16) + (t->laddr & 0xFFFF) + (t->raddr >> 16) + (t->raddr & 0xFFFF)
                + 6u + (uint32_t)(20 + dlen);
    put16(s + 16, cksum(s, 20 + dlen, ph));
    net_ip_send(t->raddr, 6, s, 20 + dlen);
}
/* Send and remember the segment so a lost SYN/data/FIN can be retransmitted. */
static void tcp_arm(struct tcb *t, uint8_t flags, const uint8_t *data, int dlen, uint32_t seq) {
    t->urflags = flags; t->urlen = dlen; t->urseq = seq;
    if (dlen > 0) memcpy(t->ur, data, dlen);
    t->urpending = 1;
    tcp_out(t, flags, data, dlen, seq);
}
static void tcp_retx(struct tcb *t) { if (t->urpending) tcp_out(t, t->urflags, t->ur, t->urlen, t->urseq); }

void tcp_input(uint32_t src, const uint8_t *seg, int len) {
    if (len < 20) return;
    uint16_t sport = get16(seg + 0), dport = get16(seg + 2);
    uint32_t seq = get32(seg + 4), ack = get32(seg + 8);
    uint8_t flags = seg[13];
    int doff = (seg[12] >> 4) * 4; if (doff < 20) doff = 20;
    const uint8_t *data = seg + doff;
    int dlen = len - doff; if (dlen < 0) dlen = 0;
    uint32_t dst = net_local_ip();

    struct tcb *t = 0, *lst = 0;
    for (int i = 0; i < NTCB; i++) {
        struct tcb *c = &T[i];
        if (!c->used) continue;
        if (c->state != LISTEN && c->lport == dport && c->rport == sport && c->raddr == src && c->laddr == dst) { t = c; break; }
        if (c->state == LISTEN && c->lport == dport) lst = c;
    }

    if (!t) {                                          /* new connection to a listener */
        if (lst && (flags & F_SYN)) {
            struct tcb *c = tcb_alloc();
            if (!c) return;
            c->laddr = dst; c->lport = dport; c->raddr = src; c->rport = sport;
            c->rcv_nxt = seq + 1;
            c->snd_una = c->snd_nxt = g_iss; g_iss += 0x10000;
            c->state = SYN_RCVD; c->parent = tcb_idx(lst);
            tcp_arm(c, F_SYN | F_ACK, 0, 0, c->snd_nxt); c->snd_nxt++;
        }
        return;
    }

    if (flags & F_RST) { t->state = CLOSED; t->used = 0; return; }

    if (flags & F_ACK) {                               /* advance send window + clear retransmit */
        if (seq_gt(ack, t->snd_una)) t->snd_una = ack;
        if (t->urpending) {
            uint32_t span = (uint32_t)t->urlen + ((t->urflags & F_SYN) ? 1 : 0) + ((t->urflags & F_FIN) ? 1 : 0);
            if (seq_ge(ack, t->urseq + span)) t->urpending = 0;
        }
    }

    if (t->state == SYN_SENT) {
        if ((flags & F_SYN) && (flags & F_ACK)) {
            t->rcv_nxt = seq + 1; t->state = ESTAB;
            tcp_out(t, F_ACK, 0, 0, t->snd_nxt);
        } else if (flags & F_SYN) {
            t->rcv_nxt = seq + 1; t->state = SYN_RCVD;
            tcp_arm(t, F_SYN | F_ACK, 0, 0, t->snd_una);
        }
        return;
    }
    if (t->state == SYN_RCVD) {
        if (flags & F_ACK) {
            t->state = ESTAB;
            if (t->parent >= 0) { struct tcb *l = &T[t->parent];
                if (l->used && l->state == LISTEN && l->nback < NTCB) l->backlog[l->nback++] = tcb_idx(t); }
        }
        if (t->state != ESTAB) return;
    }

    int acked = 0;
    if (dlen > 0 && seq == t->rcv_nxt &&
        (t->state == ESTAB || t->state == FIN_WAIT1 || t->state == FIN_WAIT2)) {
        int room = RXCAP - t->rxlen, n = dlen; if (n > room) n = room;
        memcpy(t->rx + t->rxlen, data, n); t->rxlen += n; t->rcv_nxt += n; acked = 1;
    } else if (dlen > 0) {
        acked = 1;                                     /* dup/out-of-order -> re-ACK */
    }
    if ((flags & F_FIN) && seq + dlen == t->rcv_nxt) { /* FIN sits just after this segment's data */
        t->rcv_nxt++; t->finrecv = 1; acked = 1;
        if (t->state == ESTAB) t->state = CLOSE_WAIT;
        else if (t->state == FIN_WAIT1) t->state = t->urpending ? CLOSING : TIME_WAIT;
        else if (t->state == FIN_WAIT2) t->state = TIME_WAIT;
    }
    if (acked) tcp_out(t, F_ACK, 0, 0, t->snd_nxt);

    if (t->state == FIN_WAIT1 && !t->urpending) t->state = FIN_WAIT2;
    if (t->state == CLOSING   && !t->urpending) t->state = TIME_WAIT;
    if (t->state == LAST_ACK  && !t->urpending) { t->state = CLOSED; t->used = 0; }
}

/* ---- blocking client/server API (used by the socket layer + self-test) ---- */
int tcp_connect(uint32_t dst, uint16_t dport) {
    struct tcb *t = tcb_alloc();
    if (!t) return -1;
    t->laddr = net_local_ip(); t->raddr = dst; t->rport = dport; t->lport = g_ephem++;
    t->snd_una = t->snd_nxt = g_iss; g_iss += 0x10000;
    t->state = SYN_SENT;
    net_drain();                                    /* clear RX backlog so the SYN-ACK isn't dropped */
    tcp_arm(t, F_SYN, 0, 0, t->snd_nxt); t->snd_nxt++;
    uint64_t deadline = millis() + SYN_TIMEOUT_MS, next_retx = millis() + RETX_MS;
    while (millis() < deadline) {
        net_poll();
        if (t->state == ESTAB) return tcb_idx(t);
        if (!t->used || t->state == CLOSED) return -1;
        if (millis() >= next_retx) { tcp_retx(t); next_retx = millis() + RETX_MS; }
        __asm__ volatile("pause");
    }
    return -1;
}
int tcp_listen(uint16_t port) {
    struct tcb *t = tcb_alloc();
    if (!t) return -1;
    t->laddr = net_local_ip(); t->lport = port; t->state = LISTEN; t->listener = 1;
    return tcb_idx(t);
}
int tcp_accept(int lidx) {
    if (lidx < 0 || lidx >= NTCB) return -1;
    struct tcb *l = &T[lidx];
    for (uint32_t g = 0; g < GUARD; g++) {
        if (l->nback > 0) {
            int c = l->backlog[0];
            for (int i = 1; i < l->nback; i++) l->backlog[i - 1] = l->backlog[i];
            l->nback--;
            return c;
        }
        net_poll();
        __asm__ volatile("pause");
    }
    return -1;
}
int tcp_send(int idx, const void *data, int len) {
    if (idx < 0 || idx >= NTCB) return -1;
    struct tcb *t = &T[idx];
    net_drain();                                    /* clear backlog so ACKs aren't dropped */
    const uint8_t *p = data; int off = 0;
    while (off < len) {
        uint64_t deadline = millis() + IO_TIMEOUT_MS, next_retx = millis() + RETX_MS;
        while (t->urpending && millis() < deadline) { net_poll(); if (millis() >= next_retx) { tcp_retx(t); next_retx = millis() + RETX_MS; } __asm__ volatile("pause"); }
        if (t->urpending) return off ? off : -1;
        if (t->state != ESTAB && t->state != CLOSE_WAIT) return off ? off : -1;
        int chunk = len - off; if (chunk > MSS) chunk = MSS;
        tcp_arm(t, F_ACK | F_PSH, p + off, chunk, t->snd_nxt);
        t->snd_nxt += chunk; off += chunk;
    }
    uint64_t deadline = millis() + IO_TIMEOUT_MS, next_retx = millis() + RETX_MS;
    while (t->urpending && millis() < deadline) { net_poll(); if (millis() >= next_retx) { tcp_retx(t); next_retx = millis() + RETX_MS; } __asm__ volatile("pause"); }
    return off;
}
int tcp_recv(int idx, void *buf, int max) {
    if (idx < 0 || idx >= NTCB) return -1;
    struct tcb *t = &T[idx];
    net_drain();                                    /* process pending segments + clear backlog */
    uint64_t deadline = millis() + IO_TIMEOUT_MS;    /* wall-clock idle wait (slow servers != EOF) */
    for (;;) {
        if (t->rxlen > 0) {
            int n = t->rxlen; if (n > max) n = max;
            memcpy(buf, t->rx, n);
            memcpy(t->rx, t->rx + n, t->rxlen - n);    /* shift left (dst < src: safe) */
            t->rxlen -= n;
            return n;
        }
        if (t->finrecv) return 0;                      /* EOF */
        if (!t->used || t->state == CLOSED) return -1;
        if (millis() >= deadline) return 0;            /* no data for the whole window: give up */
        net_poll();
        __asm__ volatile("pause");
    }
}
int tcp_close(int idx) {
    if (idx < 0 || idx >= NTCB) return -1;
    struct tcb *t = &T[idx];
    if (t->listener) { t->used = 0; return 0; }
    if (t->state == ESTAB)          { tcp_arm(t, F_FIN | F_ACK, 0, 0, t->snd_nxt); t->snd_nxt++; t->state = FIN_WAIT1; }
    else if (t->state == CLOSE_WAIT){ tcp_arm(t, F_FIN | F_ACK, 0, 0, t->snd_nxt); t->snd_nxt++; t->state = LAST_ACK; }
    else { t->used = 0; return 0; }
    for (uint32_t g = 0; g < 40000000u && t->used &&
         t->state != CLOSED && t->state != TIME_WAIT; g++) {
        net_poll(); if ((g & 0x3FFFFF) == 0x3FFFFF) tcp_retx(t); __asm__ volatile("pause");
    }
    t->used = 0;
    return 0;
}
void tcp_peer(int idx, uint32_t *ip, uint16_t *port) {
    if (idx < 0 || idx >= NTCB) { if (ip) *ip = 0; if (port) *port = 0; return; }
    if (ip) *ip = T[idx].raddr;
    if (port) *port = T[idx].rport;
}

/* N3 gate: a local client and server complete a connection over loopback,
 * exchange bytes both ways, and close. */
int tcp_selftest(void) {
    for (int i = 0; i < NTCB; i++) T[i].used = 0;
    int srv = tcp_listen(9000);
    if (srv < 0) return -1;
    int cli = tcp_connect(net_local_ip(), 9000);
    if (cli < 0) return -2;
    int s = tcp_accept(srv);
    if (s < 0) return -3;

    if (tcp_send(cli, "ping", 4) != 4) return -4;
    char b[16];
    int n = tcp_recv(s, b, sizeof b);
    if (n != 4 || b[0] != 'p' || b[1] != 'i' || b[2] != 'n' || b[3] != 'g') return -5;
    if (tcp_send(s, "pong!", 5) != 5) return -6;
    int n2 = tcp_recv(cli, b, sizeof b);
    if (n2 != 5 || b[0] != 'p' || b[4] != '!') return -7;

    tcp_close(cli); tcp_close(s); tcp_close(srv);
    return 0;
}
