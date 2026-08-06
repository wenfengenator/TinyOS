/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 Hyper-V synthetic NIC (netvsc + RNDIS) — Cycle 8 N-hyperv.
 *
 * Hyper-V Gen 2 VMs have no virtio; their NIC is a synthetic VMBus device driven
 * by the NVSP (netvsc) transport with RNDIS on top.  This brings the same
 * Ethernet frame API as kernel/drivers/vnet64.c (init/mac/tx/rx) to the daily
 * Hyper-V machine, so the whole IP/TCP/socket stack runs unchanged.
 *
 * It reuses the VMBus connection + channel/ring/GPADL machinery that vmbus64.c
 * and storvsc64.c established (find offer -> open a channel with its own ring
 * GPADL -> hand over a receive buffer GPADL).  The protocol:
 *   NVSP: INIT (negotiate version) -> SEND_NDIS_VERSION -> SEND_RECV_BUF.
 *   RNDIS (carried in NVSP SEND_RNDIS_PKT): INITIALIZE -> SET packet filter ->
 *   QUERY OID_802_3_PERMANENT_ADDRESS (the MAC).  RNDIS replies + received
 *   frames arrive as VMBus transfer-page packets referencing the receive buffer.
 *
 * VMBus can only be validated on real Hyper-V (never QEMU), so this is a STAGED,
 * heavily-logged bring-up like StorVSC: netvsc_stage() (1..9) shows how far it
 * got (surfaced as "NET STG" on the system monitor).  netvsc_init() returns 0 on
 * any failure (and on QEMU, where vmbus_ready() is false), so a partial driver
 * never breaks anything — networking simply stays off. */
#include "boot64.h"

extern void *memset(void *, int, unsigned long);
extern void *memcpy(void *, const void *, unsigned long);

/* synthetic network interface GUID f8615163-df3e-46c5-913f-f2d2f965ed0e */
static const uint8_t NET_GUID[16] = {
    0x63,0x51,0x61,0xf8, 0x3e,0xdf, 0xc5,0x46, 0x91,0x3f, 0xf2,0xd2,0xf9,0x65,0xed,0x0e
};

/* VMBus channel messages (match vmbus64.c / storvsc64.c) */
#define MSG_GPADL_HEADER   8
#define MSG_GPADL_CREATED  10
#define MSG_OPENCHANNEL    5
#define MSG_OPEN_RESULT    6

/* VMBus packet types */
#define PKT_INBAND      6      /* NVSP control (no data buffer)          */
#define PKT_XFER_PAGES  7      /* host -> guest: data in the recv buffer  */
#define PKT_GPA_DIRECT  9      /* guest -> host: data via a GPA range     */
#define PKT_COMPLETION  11     /* completion (releases an xfer-page pkt)  */

/* NVSP message types */
#define NVSP_INIT                1
#define NVSP_INIT_COMPLETE       2
#define NVSP_SEND_NDIS_VER       100
#define NVSP_SEND_RECV_BUF       101
#define NVSP_SEND_RECV_BUF_CMPLT 102
#define NVSP_SEND_SEND_BUF       104
#define NVSP_SEND_SEND_BUF_CMPLT 105
#define NVSP_SEND_RNDIS_PKT      107
#define NVSP_SEND_RNDIS_PKT_CMPLT 108
#define NVSP_STAT_SUCCESS        1
#define NVSP_VERSION_1           2      /* NVSP_PROTOCOL_VERSION_1 */

/* RNDIS message types */
#define RNDIS_PACKET     0x00000001
#define RNDIS_INIT       0x00000002
#define RNDIS_INIT_CMPLT 0x80000002
#define RNDIS_QUERY      0x00000004
#define RNDIS_QUERY_CMPLT 0x80000004
#define RNDIS_SET        0x00000005
#define RNDIS_SET_CMPLT  0x80000005
#define OID_GEN_CURRENT_PACKET_FILTER 0x0001010E
#define OID_802_3_PERMANENT_ADDRESS   0x01010101
#define OID_802_3_CURRENT_ADDRESS     0x01010102
/* NDIS packet-filter bits (OID_GEN_CURRENT_PACKET_FILTER) */
#define NDIS_PF_DIRECTED   0x01
#define NDIS_PF_ALLMULTI   0x04
#define NDIS_PF_BROADCAST  0x08
#define NDIS_PF_PROMISC    0x20
#define RNDIS_FILTER_BASIC (NDIS_PF_DIRECTED | NDIS_PF_BROADCAST)                              /* 0x09 */
#define RNDIS_FILTER_WIDE  (NDIS_PF_DIRECTED | NDIS_PF_ALLMULTI | NDIS_PF_BROADCAST | NDIS_PF_PROMISC) /* 0x2D */

/* ring: 1 control + 2 data pages per direction (same shape as storvsc) */
#define RDP 2
#define SP  (1 + RDP)
#define RP  (1 + RDP)
#define RING_PAGES (SP + RP)
#define RING_SZ    (RDP * 4096u)
#define NET_RING_GPADL 0x3001
#define NET_RECV_GPADL 0x3002
#define NET_OPENID     3
#define RECVBUF_PAGES  16              /* 64 KiB recv buffer; PFN list fits one GPADL msg */
#define RECVBUF_ID     0xCAFE
#define NET_SEND_GPADL 0x3003
#define SENDBUF_PAGES  16
#define SENDBUF_ID     0xCAFF

static uint32_t g_relid, g_connid, g_stage, g_diag, g_txn;
static uint8_t *g_send, *g_recv, *g_recvbuf, *g_sendbuf, *g_txdma;
static uint8_t  g_mac[6];
static int      g_ready, g_have_mac;
static uint64_t g_txid = 1;
static uint32_t g_rndis_req = 1;

/* ---- ring send (this channel's own ring) ---- */
static void ring_out(uint8_t *data, uint32_t *w, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) { data[*w] = s[i]; *w = (*w + 1) % RING_SZ; }
}
/* Wait (bounded) for the host to drain enough send-ring space; avoids overwriting
 * unread completions when a busy network floods us with RX to acknowledge. */
static void ring_wait_space(uint32_t need) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    for (uint32_t t = 0; t < 2000000; t++) {
        uint32_t freeb = (ctl[1] - ctl[0] - 1 + RING_SZ) % RING_SZ;
        if (freeb >= need) return;
        __asm__ volatile("pause");
    }
}
static void pkt_send(uint16_t type, const void *payload, uint32_t plen, int want_comp, uint64_t txid) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    uint8_t *data = g_send + 4096;
    uint32_t pad = (uint32_t)((-(int)plen) & 7);
    ring_wait_space(16 + plen + pad + 8);
    uint32_t w = ctl[0], start = w;
    uint8_t desc[16]; memset(desc, 0, 16);
    *(uint16_t *)(desc + 0) = type;
    *(uint16_t *)(desc + 2) = 2;                                  /* data offset (16/8) */
    *(uint16_t *)(desc + 4) = (uint16_t)((16 + plen + pad) / 8);  /* total len8         */
    *(uint16_t *)(desc + 6) = want_comp ? 1 : 0;
    *(uint64_t *)(desc + 8) = txid;
    ring_out(data, &w, desc, 16);
    ring_out(data, &w, payload, plen);
    uint8_t z[8] = {0}; ring_out(data, &w, z, pad);
    uint64_t prev = start; ring_out(data, &w, &prev, 8);
    __asm__ volatile("mfence" ::: "memory");
    ctl[0] = w;
    __asm__ volatile("mfence" ::: "memory");
    vmbus_signal_event(g_connid);
}

/* Send an RNDIS message to the host: NVSP SEND_RNDIS_PKT carrying the message in
 * a GPA-direct range (g_txdma).  chan_type 1 = control, 0 = data. */
static void rndis_out(const uint8_t *msg, uint32_t mlen, uint32_t chan_type) {
    memcpy(g_txdma, msg, mlen);
    uint64_t pa = (uint64_t)(uintptr_t)g_txdma;                  /* VA == PA */
    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    uint8_t *data = g_send + 4096;
    ring_wait_space(16 + 24 + 32 + 8);
    uint32_t w = ctl[0], start = w;

    uint32_t nvsp[8]; memset(nvsp, 0, sizeof nvsp);
    nvsp[0] = NVSP_SEND_RNDIS_PKT; nvsp[1] = chan_type; nvsp[2] = 0xFFFFFFFFu; nvsp[3] = 0;
    uint16_t off8 = (16 + 24) / 8;                               /* desc + gpa header */
    uint16_t len8 = (uint16_t)(off8 + sizeof(nvsp) / 8);

    uint8_t desc[16]; memset(desc, 0, 16);
    *(uint16_t *)(desc + 0) = PKT_GPA_DIRECT;
    *(uint16_t *)(desc + 2) = off8;
    *(uint16_t *)(desc + 4) = len8;
    *(uint16_t *)(desc + 6) = 1;                                 /* completion requested */
    *(uint64_t *)(desc + 8) = g_txid++;
    ring_out(data, &w, desc, 16);
    uint32_t ghdr[2] = { 0, 1 };                                 /* reserved, rangecount */
    ring_out(data, &w, ghdr, 8);
    uint32_t rng[2] = { mlen, (uint32_t)(pa & 0xFFF) };          /* byte_count, byte_offset */
    ring_out(data, &w, rng, 8);
    uint64_t pfn = pa >> 12; ring_out(data, &w, &pfn, 8);
    ring_out(data, &w, nvsp, sizeof(nvsp));
    uint64_t prev = start; ring_out(data, &w, &prev, 8);
    __asm__ volatile("mfence" ::: "memory");
    ctl[0] = w;
    __asm__ volatile("mfence" ::: "memory");
    vmbus_signal_event(g_connid);
}

/* ---- ring receive ---- */
struct rxpkt {
    uint16_t type;
    uint64_t txid;
    uint8_t  nvsp[64]; uint32_t nvsp_len;    /* the packet's own payload (NVSP header) */
    uint8_t *data;     uint32_t data_len;    /* for xfer-pages: region in the recv buffer */
};
static int recv_packet(struct rxpkt *o) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_recv;
    const uint8_t *ring = g_recv + 4096;
    uint32_t r = ctl[1], wr = ctl[0];
    if (r == wr) return 0;
    __asm__ volatile("mfence" ::: "memory");
    uint8_t hdr[16];
    for (int i = 0; i < 16; i++) hdr[i] = ring[(r + i) % RING_SZ];
    o->type = *(uint16_t *)(hdr + 0);
    uint16_t off8 = *(uint16_t *)(hdr + 2), len8 = *(uint16_t *)(hdr + 4);
    o->txid = *(uint64_t *)(hdr + 8);
    o->data = 0; o->data_len = 0;
    if (o->type == PKT_XFER_PAGES) {                             /* data sits in the recv buffer */
        uint8_t xh[8]; for (int i = 0; i < 8; i++) xh[i] = ring[(r + 16 + i) % RING_SZ];
        uint8_t rg[8]; for (int i = 0; i < 8; i++) rg[i] = ring[(r + 24 + i) % RING_SZ];
        uint32_t bc = *(uint32_t *)(rg + 0), bo = *(uint32_t *)(rg + 4);
        if ((uint64_t)bo + bc <= RECVBUF_PAGES * 4096u) { o->data = g_recvbuf + bo; o->data_len = bc; }
        (void)xh;
    }
    uint32_t ppos = (r + off8 * 8u) % RING_SZ;
    uint32_t plen = (uint32_t)(len8 - off8) * 8u;
    if (plen > sizeof o->nvsp) plen = sizeof o->nvsp;
    for (uint32_t i = 0; i < plen; i++) o->nvsp[i] = ring[(ppos + i) % RING_SZ];
    o->nvsp_len = plen;
    ctl[1] = (r + len8 * 8u + 8u) % RING_SZ;
    __asm__ volatile("mfence" ::: "memory");
    return 1;
}
static void pause_a_bit(void) { for (volatile int d = 0; d < 300000; d++) __asm__ volatile("pause"); }

/* Release an xfer-page packet back to the host (NVSP SEND_RNDIS_PKT_COMPLETE). */
static void send_rx_completion(uint64_t txid) {
    uint32_t body[2] = { NVSP_SEND_RNDIS_PKT_CMPLT, NVSP_STAT_SUCCESS };
    pkt_send(PKT_COMPLETION, body, sizeof body, 0, txid);
}

/* Wait for an inband/completion NVSP reply of a given message type. */
static int wait_nvsp(uint32_t want, struct rxpkt *o) {
    int got = 0; uint16_t ftype = 0; uint32_t fword = 0;
    for (int t = 0; t < 2000; t++) {
        if (recv_packet(o)) {
            if (!got) { ftype = o->type; fword = o->nvsp_len >= 4 ? *(uint32_t *)o->nvsp : 0; }
            got++;
            if (o->type == PKT_XFER_PAGES) { send_rx_completion(o->txid); continue; }
            if (o->nvsp_len >= 4 && *(uint32_t *)o->nvsp == want) return 0;
            continue;
        }
        pause_a_bit();
    }
    g_diag = ((uint32_t)ftype << 24) | ((uint32_t)(got & 0xFF) << 8) | (fword & 0xFF);
    kputs("[netvsc] wait timeout: got "); kput_u32dec((uint32_t)got);
    kputs(" pkts, first type "); kput_u32dec(ftype);
    kputs(" word "); kput_u64hex(fword); kputc('\n');
    return -1;
}

/* Wait for an RNDIS control reply (arrives as an xfer-page packet) matching an
 * expected message type + request id.  Copies the RNDIS message into out. */
static int wait_rndis(uint32_t want, uint32_t req, uint8_t *out, uint32_t max) {
    int got = 0; uint16_t ftype = 0;
    for (int t = 0; t < 2000; t++) {
        struct rxpkt o;
        if (recv_packet(&o)) {
            if (!got) ftype = o.type;
            got++;
            if (o.type == PKT_XFER_PAGES && o.data && o.data_len >= 12) {
                uint32_t mt = *(uint32_t *)(o.data + 0), rid = *(uint32_t *)(o.data + 8);
                if (mt == want && rid == req) {
                    uint32_t n = o.data_len; if (n > max) n = max;
                    memcpy(out, o.data, n);
                    send_rx_completion(o.txid);
                    return (int)n;
                }
                send_rx_completion(o.txid);
            } else if (o.type == PKT_XFER_PAGES) {
                send_rx_completion(o.txid);
            }
            continue;
        }
        pause_a_bit();
    }
    kputs("[netvsc] rndis wait timeout: got "); kput_u32dec((uint32_t)got);
    kputs(" pkts, first type "); kput_u32dec(ftype); kputc('\n');
    return -1;
}

/* ---- GPADL: hand a page range to the host, wait for GPADL_CREATED ---- */
static int create_gpadl(uint32_t handle, void *buf, int npages) {
    uint8_t msg[256]; memset(msg, 0, sizeof msg);
    uint32_t *m = (uint32_t *)msg;
    m[0] = MSG_GPADL_HEADER; m[2] = g_relid; m[3] = handle;
    uint16_t *m16 = (uint16_t *)(msg + 16);
    m16[0] = (uint16_t)(8 + 8 * npages); m16[1] = 1;             /* range buflen, range count */
    uint32_t *rng = (uint32_t *)(msg + 20);
    rng[0] = (uint32_t)npages * 4096; rng[1] = 0;
    uint64_t base = (uint64_t)(uintptr_t)buf, *pfn = (uint64_t *)(msg + 28);
    for (int i = 0; i < npages; i++) pfn[i] = (base >> 12) + i;
    if (!vmbus_msg_post(msg, 28 + 8 * npages)) return -1;
    volatile uint8_t *p = vmbus_msg_poll(20000000);
    if (!p || *(volatile uint32_t *)p != MSG_GPADL_CREATED) { if (p) vmbus_msg_done(); return -1; }
    uint32_t gs = *(volatile uint32_t *)(p + 16); vmbus_msg_done();
    return gs ? -1 : 0;
}

static int open_channel(void) {
    uint8_t *ring = (uint8_t *)vmbus_palloc(RING_PAGES);
    if (!ring) { kputs("[netvsc] ring alloc failed\n"); return 0; }
    g_send = ring; g_recv = ring + SP * 4096;
    if (create_gpadl(NET_RING_GPADL, ring, RING_PAGES) != 0) { kputs("[netvsc] ring GPADL failed\n"); return 0; }
    g_stage = 3;

    uint8_t msg[256]; memset(msg, 0, sizeof msg);
    uint32_t *m = (uint32_t *)msg;
    m[0] = MSG_OPENCHANNEL; m[2] = g_relid; m[3] = NET_OPENID;
    m[4] = NET_RING_GPADL; m[5] = 0; m[6] = SP;                  /* gpadl, target vp, recv ring offset */
    if (!vmbus_msg_post(msg, 28 + 120)) { kputs("[netvsc] OPENCHANNEL post failed\n"); return 0; }
    volatile uint8_t *p = vmbus_msg_poll(20000000);
    if (!p || *(volatile uint32_t *)p != MSG_OPEN_RESULT) { kputs("[netvsc] no OPEN_RESULT\n"); if (p) vmbus_msg_done(); return 0; }
    uint32_t os = *(volatile uint32_t *)(p + 16); vmbus_msg_done();
    if (os) { kputs("[netvsc] channel open status "); kput_u32dec(os); kputc('\n'); return 0; }
    g_stage = 4;
    return 1;
}

/* ---- NVSP handshake: version, NDIS version, receive buffer ---- */
static int nvsp_handshake(void) {
    struct rxpkt o;
    uint8_t init[32]; memset(init, 0, sizeof init);              /* NVS messages are fixed 32 bytes */
    *(uint32_t *)(init + 0) = NVSP_INIT;
    *(uint32_t *)(init + 4) = NVSP_VERSION_1;
    *(uint32_t *)(init + 8) = NVSP_VERSION_1;
    pkt_send(PKT_INBAND, init, sizeof init, 1, g_txid++);
    if (wait_nvsp(NVSP_INIT_COMPLETE, &o)) { kputs("[netvsc] NVSP INIT no reply\n"); return 0; }
    if (o.nvsp_len >= 16 && *(uint32_t *)(o.nvsp + 12) != NVSP_STAT_SUCCESS) {
        kputs("[netvsc] NVSP INIT status "); kput_u32dec(*(uint32_t *)(o.nvsp + 12)); kputc('\n'); return 0;
    }
    g_stage = 5;

    uint8_t ndis[32]; memset(ndis, 0, sizeof ndis);
    *(uint32_t *)(ndis + 0) = NVSP_SEND_NDIS_VER;
    *(uint32_t *)(ndis + 4) = 6; *(uint32_t *)(ndis + 8) = 1;    /* NDIS 6.1, fire-and-forget */
    pkt_send(PKT_INBAND, ndis, sizeof ndis, 0, g_txid++);

    if (create_gpadl(NET_RECV_GPADL, g_recvbuf, RECVBUF_PAGES) != 0) { kputs("[netvsc] recv GPADL failed\n"); return 0; }
    uint8_t srb[32]; memset(srb, 0, sizeof srb);
    *(uint32_t *)(srb + 0) = NVSP_SEND_RECV_BUF;
    *(uint32_t *)(srb + 4) = NET_RECV_GPADL;
    *(uint16_t *)(srb + 8) = RECVBUF_ID;
    pkt_send(PKT_INBAND, srb, sizeof srb, 1, g_txid++);
    if (wait_nvsp(NVSP_SEND_RECV_BUF_CMPLT, &o)) { kputs("[netvsc] SEND_RECV_BUF no reply\n"); return 0; }
    if (o.nvsp_len >= 8 && *(uint32_t *)(o.nvsp + 4) != NVSP_STAT_SUCCESS) {
        kputs("[netvsc] SEND_RECV_BUF status "); kput_u32dec(*(uint32_t *)(o.nvsp + 4)); kputc('\n'); return 0;
    }
    g_stage = 6;

    /* Establish a send buffer too — some hosts expect it before accepting TX.
     * Best-effort: TX still uses GPA-direct, so a missing reply is non-fatal. */
    if (create_gpadl(NET_SEND_GPADL, g_sendbuf, SENDBUF_PAGES) == 0) {
        uint8_t ssb[32]; memset(ssb, 0, sizeof ssb);
        *(uint32_t *)(ssb + 0) = NVSP_SEND_SEND_BUF;
        *(uint32_t *)(ssb + 4) = NET_SEND_GPADL;
        *(uint16_t *)(ssb + 8) = SENDBUF_ID;
        pkt_send(PKT_INBAND, ssb, sizeof ssb, 1, g_txid++);
        wait_nvsp(NVSP_SEND_SEND_BUF_CMPLT, &o);
    }
    return 1;
}

/* ---- RNDIS: initialize, set the packet filter, query the MAC ---- */
static int rndis_query_mac(uint32_t oid, uint8_t *rep, uint32_t repmax) {
    uint32_t rid = g_rndis_req++;
    uint8_t qm[28]; memset(qm, 0, sizeof qm);
    *(uint32_t *)(qm + 0) = RNDIS_QUERY; *(uint32_t *)(qm + 4) = 28; *(uint32_t *)(qm + 8) = rid;
    *(uint32_t *)(qm + 12) = oid;
    *(uint32_t *)(qm + 20) = 20;                                        /* info offset past the request struct */
    rndis_out(qm, 28, 1);
    int n = wait_rndis(RNDIS_QUERY_CMPLT, rid, rep, repmax);
    if (n < 0) { kputs("[netvsc] QUERY oid "); kput_u64hex(oid); kputs(" no reply\n"); return -1; }
    uint32_t qst = n >= 16 ? *(uint32_t *)(rep + 12) : 0xFFFFFFFFu;
    uint32_t ilen = n >= 20 ? *(uint32_t *)(rep + 16) : 0;
    uint32_t ioff = n >= 24 ? *(uint32_t *)(rep + 20) : 0;
    kputs("[netvsc] QUERY oid "); kput_u64hex(oid); kputs(" n "); kput_u32dec((uint32_t)n);
    kputs(" st "); kput_u64hex(qst); kputs(" ilen "); kput_u32dec(ilen); kputc('\n');
    if (qst != 0 || ilen < 6) return -1;
    uint32_t base = 8 + ioff;
    if (base + 6 > (uint32_t)n) return -1;
    for (int i = 0; i < 6; i++) g_mac[i] = rep[base + i];
    return 0;
}
/* Set OID_GEN_CURRENT_PACKET_FILTER to `val`; 0 on success (SET_CMPLT status 0). */
static int rndis_set_filter(uint32_t val) {
    uint32_t rid = g_rndis_req++;
    uint8_t sm[32]; memset(sm, 0, sizeof sm);
    *(uint32_t *)(sm + 0) = RNDIS_SET; *(uint32_t *)(sm + 4) = 32; *(uint32_t *)(sm + 8) = rid;
    *(uint32_t *)(sm + 12) = OID_GEN_CURRENT_PACKET_FILTER;
    *(uint32_t *)(sm + 16) = 4;                                         /* info buffer length */
    *(uint32_t *)(sm + 20) = 20;                                        /* info offset (from req id) */
    *(uint32_t *)(sm + 28) = val;
    uint8_t rep[64];
    rndis_out(sm, 32, 1);
    return (wait_rndis(RNDIS_SET_CMPLT, rid, rep, sizeof rep) >= 16 && *(uint32_t *)(rep + 12) == 0) ? 0 : -1;
}
static int rndis_control(void) {
    uint8_t rep[256];

    uint32_t rid = g_rndis_req++;
    uint32_t im[6] = { RNDIS_INIT, 24, rid, 1, 0, 0x4000 };             /* type,len,req,major,minor,maxxfer */
    rndis_out((uint8_t *)im, 24, 1);
    if (wait_rndis(RNDIS_INIT_CMPLT, rid, rep, sizeof rep) < 16 ||
        *(uint32_t *)(rep + 12) != 0) { kputs("[netvsc] RNDIS INIT failed\n"); return 0; }
    g_stage = 7;

    /* Query the MAC while the packet filter is still off, so no inbound traffic
     * competes with the reply.  Use CURRENT first: that is the address the vSwitch
     * matches directed frames against (permanent may differ from a configured MAC). */
    if (rndis_query_mac(OID_802_3_CURRENT_ADDRESS,   rep, sizeof rep) != 0 &&
        rndis_query_mac(OID_802_3_PERMANENT_ADDRESS, rep, sizeof rep) != 0) {
        kputs("[netvsc] RNDIS QUERY MAC failed\n"); return 0;
    }
    g_have_mac = 1;
    g_stage = 8;

    /* DIRECTED|BROADCAST only: delivers our unicast + broadcast.  PROMISCUOUS (0x2D)
     * floods the recv buffer on a busy switch and drops the DHCP reply, so avoid it. */
    if (rndis_set_filter(RNDIS_FILTER_BASIC) != 0) { kputs("[netvsc] RNDIS SET filter failed\n"); return 0; }
    kputs("[netvsc] filter directed+bcast 0x09\n");
    g_stage = 9;
    return 1;
}

int netvsc_init(void) {
    if (!vmbus_ready()) return 0;                                       /* not Hyper-V */
    if (!vmbus_find_offer(NET_GUID, &g_relid, &g_connid)) { kputs("[netvsc] no synthetic NIC offered\n"); return 0; }
    kputs("[netvsc] NIC channel relid "); kput_u32dec(g_relid);
    kputs(" connid "); kput_u32dec(g_connid); kputc('\n');
    g_stage = 2;

    g_recvbuf = (uint8_t *)vmbus_palloc(RECVBUF_PAGES);
    g_sendbuf = (uint8_t *)vmbus_palloc(SENDBUF_PAGES);
    g_txdma   = (uint8_t *)vmbus_palloc(1);
    if (!g_recvbuf || !g_sendbuf || !g_txdma) { kputs("[netvsc] buffer alloc failed\n"); return 0; }

    if (!open_channel())   return 0;
    if (!nvsp_handshake()) return 0;
    if (!rndis_control())  return 0;
    if (!g_have_mac)       return 0;

    kputs("[netvsc] ready; MAC ");
    for (int i = 0; i < 6; i++) { const char *h = "0123456789abcdef";
        kputc(h[g_mac[i] >> 4]); kputc(h[g_mac[i] & 0xF]); if (i < 5) kputc(':'); }
    kputc('\n');
    g_ready = 1;

    { uint8_t tmp[1600]; int frames = 0;                               /* passive RX probe: is any traffic arriving? */
      for (uint32_t g = 0; g < 40000000u && frames < 40; g++) {
          if (netvsc_rx(tmp, sizeof tmp) > 0) frames++;
          __asm__ volatile("pause");
      }
      kputs("[netvsc] rx probe: "); kput_u32dec((uint32_t)frames); kputs(" frames received\n"); }
    return 1;
}

void netvsc_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = g_mac[i]; }
int  netvsc_stage(void) { return (int)g_stage; }
int  netvsc_diag(void)  { return (int)g_txn; }

/* Send one Ethernet frame: wrap it in an RNDIS_PACKET_MSG (data channel). */
int netvsc_tx(const void *frame, uint32_t len) {
    if (!g_ready || !len || len > 1514) return -1;
    uint8_t pm[44 + 1514];
    memset(pm, 0, 44);
    *(uint32_t *)(pm + 0) = RNDIS_PACKET;
    *(uint32_t *)(pm + 4) = 44 + len;                                  /* msg length */
    *(uint32_t *)(pm + 8) = 36;                                        /* data offset (from field @8) */
    *(uint32_t *)(pm + 12) = len;                                      /* data length */
    memcpy(pm + 44, frame, len);
    rndis_out(pm, 44 + len, 0);
    g_txn++;
    return 0;
}

/* Poll for one received Ethernet frame; returns its length, 0 if none. */
int netvsc_rx(void *buf, uint32_t max) {
    if (!g_ready) return -1;
    struct rxpkt o;
    if (!recv_packet(&o)) return 0;
    if (o.type != PKT_XFER_PAGES || !o.data || o.data_len < 44) {
        if (o.type == PKT_XFER_PAGES) send_rx_completion(o.txid);
        return 0;
    }
    int r = 0;
    if (*(uint32_t *)(o.data + 0) == RNDIS_PACKET) {                   /* an Ethernet frame */
        uint32_t doff = *(uint32_t *)(o.data + 8), dlen = *(uint32_t *)(o.data + 12);
        uint32_t start = 8 + doff;
        if ((uint64_t)start + dlen <= o.data_len) {
            if (dlen > max) dlen = max;
            memcpy(buf, o.data + start, dlen);
            r = (int)dlen;
        }
    }
    send_rx_completion(o.txid);
    return r;
}
