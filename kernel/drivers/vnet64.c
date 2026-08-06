/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 virtio-net (legacy PCI) network driver — Cycle 8 Phase N1.
 *
 * Moves Ethernet frames in and out over the QEMU paravirtual NIC using the same
 * legacy (virtio 0.9.5) port-I/O split-virtqueue approach as vblk64.c: find the
 * PCI device, set up the RX (queue 0) and TX (queue 1) rings, read the MAC from
 * device config, pre-fill RX buffers, and poll the used rings (no interrupts).
 *
 * This is the frame transport the TCP/IP stack (N2) will sit on.  The built-in
 * self-test proves the path end to end: it broadcasts an ARP request for the
 * SLIRP gateway (10.0.2.2) and waits for the ARP reply.  If no virtio-net device
 * is present, vnet_init() returns 0 and the system runs without networking. */
#include "boot64.h"

/* ---- port I/O ---- */
static inline void     outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void     outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void     outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t  inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
#define barrier() __asm__ volatile("mfence" ::: "memory")

/* ---- legacy PCI config space (0xCF8/0xCFC) ---- */
static uint32_t pci_rd(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_wr(int bus, int dev, int fn, int off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

/* ---- virtio legacy I/O registers (offset from the BAR0 I/O base) ---- */
#define VIO_DEV_FEAT 0x00
#define VIO_DRV_FEAT 0x04
#define VIO_QADDR    0x08
#define VIO_QSIZE    0x0C
#define VIO_QSEL     0x0E
#define VIO_QNOTIFY  0x10
#define VIO_STATUS   0x12
#define VIO_CONFIG   0x14          /* device-specific config (no MSI-X) */
#define ST_ACK 1
#define ST_DRV 2
#define ST_OK  4
#define VIRTIO_NET_F_MAC (1u << 5)

/* ---- split virtqueue ---- */
#define VRING_ALIGN 4096
#define VD_NEXT  1
#define VD_WRITE 2
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vavail { uint16_t flags, idx; uint16_t ring[]; };
struct vused_elem { uint32_t id, len; };
struct vused { uint16_t flags, idx; struct vused_elem ring[]; };

struct vring {
    uint16_t       qsz;
    struct vdesc  *desc;
    struct vavail *avail;
    struct vused  *used;
    uint16_t       last_used;
};

/* legacy virtio-net header prepended to every frame (no mergeable rx buffers) */
#define VNET_HDR_LEN 10
#define RX_BUFS      16
#define BUF_SZ       2048

static uint16_t     g_io;
static struct vring g_rx, g_tx;
static uint8_t      g_mac[6];
static int          g_ready;

/* DMA buffers live in the identity-mapped kernel image, so VA == PA. */
static uint8_t g_rxbuf[RX_BUFS][BUF_SZ];
static uint8_t g_txbuf[BUF_SZ];

static void puthex2(uint8_t b) {
    const char *h = "0123456789abcdef";
    kputc(h[b >> 4]); kputc(h[b & 0xF]);
}

static int find_dev(int *bo, int *do_, int *fo) {
    for (int bus = 0; bus < 16; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)id, device = (uint16_t)(id >> 16);
                if (vendor == 0xFFFF) { if (fn == 0) break; else continue; }
                if (vendor == 0x1AF4 && (device == 0x1000 || device == 0x1041)) {
                    *bo = bus; *do_ = dev; *fo = fn; return 1;
                }
            }
    return 0;
}

static int setup_queue(int qidx, struct vring *vr) {
    outw(g_io + VIO_QSEL, (uint16_t)qidx);
    uint16_t qsz = inw(g_io + VIO_QSIZE);
    if (qsz == 0 || qsz > 1024) return -1;
    vr->qsz = qsz;
    uint32_t desc_b   = 16u * qsz;
    uint32_t avail_b  = 6u + 2u * qsz;
    uint32_t used_off = (desc_b + avail_b + (VRING_ALIGN - 1)) & ~(VRING_ALIGN - 1);
    uint32_t used_b   = 6u + 8u * qsz;
    uint32_t total    = used_off + used_b;
    uint8_t *region = (uint8_t *)page_alloc((total + 4095) / 4096);   /* zeroed */
    if (!region) return -1;
    vr->desc  = (struct vdesc  *)region;
    vr->avail = (struct vavail *)(region + desc_b);
    vr->used  = (struct vused  *)(region + used_off);
    vr->last_used = 0;
    outw(g_io + VIO_QSEL, (uint16_t)qidx);
    outl(g_io + VIO_QADDR, (uint32_t)((uint64_t)(uintptr_t)region >> 12));
    return 0;
}

/* Hand RX descriptor `i` (its buffer) back to the device to be filled. */
static void rx_post(int i) {
    g_rx.desc[i].addr  = (uint64_t)(uintptr_t)g_rxbuf[i];
    g_rx.desc[i].len   = BUF_SZ;
    g_rx.desc[i].flags = VD_WRITE;
    g_rx.desc[i].next  = 0;
    uint16_t idx = g_rx.avail->idx;
    g_rx.avail->ring[idx % g_rx.qsz] = (uint16_t)i;
    barrier();
    g_rx.avail->idx = (uint16_t)(idx + 1);
}

int vnet_init(void) {
    int bus, dev, fn;
    if (!find_dev(&bus, &dev, &fn)) { kputs("[net] no virtio-net device\n"); return 0; }

    pci_wr(bus, dev, fn, 0x04, pci_rd(bus, dev, fn, 0x04) | 0x5);   /* I/O + bus master */
    uint32_t bar0 = pci_rd(bus, dev, fn, 0x10);
    if (!(bar0 & 1)) { kputs("[net] BAR0 not I/O (modern-only NIC)\n"); return 0; }
    g_io = (uint16_t)(bar0 & ~0x3u);

    outb(g_io + VIO_STATUS, 0);
    outb(g_io + VIO_STATUS, ST_ACK);
    outb(g_io + VIO_STATUS, ST_ACK | ST_DRV);
    uint32_t feat = inl(g_io + VIO_DEV_FEAT);
    outl(g_io + VIO_DRV_FEAT, feat & VIRTIO_NET_F_MAC);            /* MAC only, plain rx buffers */

    if (setup_queue(0, &g_rx) != 0 || setup_queue(1, &g_tx) != 0) {
        kputs("[net] queue setup failed\n"); return 0;
    }
    for (int i = 0; i < 6; i++) g_mac[i] = inb(g_io + VIO_CONFIG + i);

    outb(g_io + VIO_STATUS, ST_ACK | ST_DRV | ST_OK);

    for (int i = 0; i < RX_BUFS; i++) rx_post(i);                  /* offer RX buffers */
    barrier();
    outw(g_io + VIO_QNOTIFY, 0);

    kputs("[net] virtio-net ready; MAC ");
    for (int i = 0; i < 6; i++) { puthex2(g_mac[i]); if (i < 5) kputc(':'); }
    kputc('\n');
    g_ready = 1;
    return 1;
}

void vnet_mac(uint8_t out[6]) { for (int i = 0; i < 6; i++) out[i] = g_mac[i]; }

int vnet_tx(const void *frame, uint32_t len) {
    if (!g_ready || !len || len > BUF_SZ - VNET_HDR_LEN) return -1;
    memset(g_txbuf, 0, VNET_HDR_LEN);                             /* zero virtio-net header */
    memcpy(g_txbuf + VNET_HDR_LEN, frame, len);

    g_tx.desc[0].addr  = (uint64_t)(uintptr_t)g_txbuf;
    g_tx.desc[0].len   = VNET_HDR_LEN + len;
    g_tx.desc[0].flags = 0;                                       /* device reads */
    g_tx.desc[0].next  = 0;
    uint16_t idx = g_tx.avail->idx;
    g_tx.avail->ring[idx % g_tx.qsz] = 0;
    barrier();
    g_tx.avail->idx = (uint16_t)(idx + 1);
    barrier();
    outw(g_io + VIO_QNOTIFY, 1);

    uint32_t guard = 0;
    while (g_tx.used->idx == g_tx.last_used) {
        if (++guard > 200000000u) return -1;
        __asm__ volatile("pause");
    }
    g_tx.last_used = g_tx.used->idx;
    return 0;
}

/* Return one received frame (Ethernet payload, header stripped) into buf, or 0
 * if the RX ring is empty.  The consumed buffer is recycled to the device. */
int vnet_rx(void *buf, uint32_t max) {
    if (!g_ready) return -1;
    if (g_rx.used->idx == g_rx.last_used) return 0;
    barrier();
    struct vused_elem *e = &g_rx.used->ring[g_rx.last_used % g_rx.qsz];
    int      i    = (int)e->id;                                   /* head == buffer index */
    uint32_t rlen = e->len;
    uint32_t plen = rlen > VNET_HDR_LEN ? rlen - VNET_HDR_LEN : 0;
    if (i < 0 || i >= RX_BUFS) { g_rx.last_used++; return 0; }
    if (plen > max) plen = max;
    memcpy(buf, g_rxbuf[i] + VNET_HDR_LEN, plen);
    g_rx.last_used++;
    rx_post(i);
    barrier();
    outw(g_io + VIO_QNOTIFY, 0);
    return (int)plen;
}

/* N1 gate: broadcast an ARP request for the SLIRP gateway and wait for the
 * reply — proves TX, RX, and the device round-trip all work.  0 on success. */
int vnet_selftest(void) {
    if (!g_ready) return -1;
    uint8_t f[42];
    memset(f, 0xFF, 6);                                           /* dst: broadcast */
    memcpy(f + 6, g_mac, 6);                                      /* src: our MAC    */
    f[12] = 0x08; f[13] = 0x06;                                   /* ethertype ARP   */
    f[14] = 0x00; f[15] = 0x01;                                   /* htype Ethernet  */
    f[16] = 0x08; f[17] = 0x00;                                   /* ptype IPv4      */
    f[18] = 6;    f[19] = 4;                                      /* hlen, plen      */
    f[20] = 0x00; f[21] = 0x01;                                   /* op: request     */
    memcpy(f + 22, g_mac, 6);                                     /* sender MAC      */
    f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15;                 /* sender IP .15   */
    memset(f + 32, 0, 6);                                         /* target MAC      */
    f[38] = 10; f[39] = 0; f[40] = 2; f[41] = 2;                  /* target IP .2 (gw) */

    if (vnet_tx(f, sizeof f) != 0) { kputs("[net] ARP TX failed\n"); return -2; }

    uint8_t rx[BUF_SZ];
    uint32_t guard = 0;
    for (;;) {
        int n = vnet_rx(rx, sizeof rx);
        if (n >= 42 && rx[12] == 0x08 && rx[13] == 0x06 &&        /* ARP */
            rx[20] == 0x00 && rx[21] == 0x02 &&                   /* reply */
            rx[28] == 10 && rx[29] == 0 && rx[30] == 2 && rx[31] == 2) {
            kputs("[net] ARP reply from 10.0.2.2 is ");
            for (int i = 0; i < 6; i++) { puthex2(rx[22 + i]); if (i < 5) kputc(':'); }
            kputc('\n');
            return 0;
        }
        if (++guard > 400000000u) return -3;
        __asm__ volatile("pause");
    }
}
