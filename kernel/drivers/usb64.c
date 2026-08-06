/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 — USB host (xHCI) + Mass Storage (Bulk-Only Transport) + SCSI.
 *
 * The user wants to copy files to/from a USB stick.  Modern machines (and the
 * T440s) expose USB through an xHCI controller, so this brings one up over PCI,
 * enumerates the first attached device, and — if it is a USB Mass Storage class
 * device (bulk-only, SCSI transparent) — exposes it as a 512-byte block device
 * (usb_read/usb_write/usb_sectors) just like the AHCI SATA driver.  The FAT32
 * data-volume logic (kernel/fs/datavol64.c generalised over a block backend) can
 * then mount a partition on the stick.
 *
 * Scope is deliberately minimal but real: one controller, one device, one LUN,
 * bulk IN/OUT.  Every wait is bounded (millis() deadline) so a wedged controller
 * or unplugged device can never hang the machine.  All DMA structures live in the
 * identity-mapped heap arena (VA==PA); we require 64-bit addressing (AC64). */
#include "boot64.h"

extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

/* ---- port I/O for PCI config space (0xCF8/0xCFC) ---- */
static inline void   outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p)); return r; }
static uint32_t pci_rd(int b,int d,int f,int o){ outl(0xCF8,0x80000000u|(b<<16)|(d<<11)|(f<<8)|(o&0xFC)); return inl(0xCFC); }
static void     pci_wr(int b,int d,int f,int o,uint32_t v){ outl(0xCF8,0x80000000u|(b<<16)|(d<<11)|(f<<8)|(o&0xFC)); outl(0xCFC,v); }

/* ---- xHCI MMIO register regions ---- */
static volatile uint8_t *g_cap;     /* capability registers (MMIO base)  */
static volatile uint8_t *g_op;      /* operational registers             */
static volatile uint8_t *g_rt;      /* runtime registers                 */
static volatile uint8_t *g_db;      /* doorbell array                    */

static inline uint32_t rd32(volatile uint8_t *b, uint32_t o){ return *(volatile uint32_t *)(b + o); }
static inline void     wr32(volatile uint8_t *b, uint32_t o, uint32_t v){ *(volatile uint32_t *)(b + o) = v; }
static inline void     wr64(volatile uint8_t *b, uint32_t o, uint64_t v){
    *(volatile uint32_t *)(b + o) = (uint32_t)v;
    *(volatile uint32_t *)(b + o + 4) = (uint32_t)(v >> 32);
}

/* operational register offsets */
#define O_USBCMD  0x00
#define O_USBSTS  0x04
#define O_PAGESZ  0x08
#define O_CRCR    0x18
#define O_DCBAAP  0x30
#define O_CONFIG  0x38
#define O_PORTSC(p) (0x400 + ((p)-1)*0x10)
/* interrupter-0 runtime register offsets (from g_rt) */
#define R_IMAN    0x20
#define R_IMOD    0x24
#define R_ERSTSZ  0x28
#define R_ERSTBA  0x30
#define R_ERDP    0x38

/* ---- TRB rings ---- */
#define RING_TRBS 64
struct ring { volatile uint32_t *trb; uint32_t enq, cycle; uint64_t pa; };
struct evt  { uint32_t cc, xfer, slot, epid; uint64_t trb; };

static void ring_init(struct ring *r, volatile uint32_t *mem) {
    r->trb = mem; r->enq = 0; r->cycle = 1; r->pa = (uint64_t)(uintptr_t)mem;
    for (uint32_t i = 0; i < RING_TRBS * 4; i++) mem[i] = 0;
    volatile uint32_t *L = mem + (RING_TRBS - 1) * 4;             /* Link TRB back to start */
    L[0] = (uint32_t)r->pa; L[1] = (uint32_t)(r->pa >> 32); L[2] = 0;
    L[3] = (6u << 10) | (1u << 1);                               /* type=Link, Toggle Cycle */
}
/* Write a TRB; return its physical address (to match its completion event). */
static uint64_t ring_push(struct ring *r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile uint32_t *t = r->trb + r->enq * 4;
    uint64_t pa = r->pa + (uint64_t)r->enq * 16;
    t[0] = d0; t[1] = d1; t[2] = d2;
    __asm__ volatile("" ::: "memory");
    t[3] = d3 | r->cycle;                                        /* cycle bit last */
    __asm__ volatile("" ::: "memory");
    r->enq++;
    if (r->enq == RING_TRBS - 1) {                              /* reached the Link TRB */
        volatile uint32_t *L = r->trb + (RING_TRBS - 1) * 4;
        L[3] = (L[3] & ~1u) | r->cycle;
        __asm__ volatile("" ::: "memory");
        r->cycle ^= 1;
        r->enq = 0;
    }
    return pa;
}

/* ---- event ring ---- */
static volatile uint32_t *g_ering;
static uint32_t g_edeq, g_ecycle;

static void erdp_write(void) {
    uint64_t dp = (uint64_t)(uintptr_t)(g_ering + g_edeq * 4) | (1u << 3); /* EHB=1 to clear */
    wr64(g_rt, R_ERDP, dp);
}
/* Wait (bounded) for a command/transfer completion event; if match!=0 only that
 * TRB's event is accepted (others are consumed).  0 = Success/ShortPacket. */
static int wait_event(uint64_t match, struct evt *out) {
    uint64_t deadline = millis() + 3000;
    for (;;) {
        if (millis() > deadline) return -100;               /* bound EVERY iteration: a real
                                                             * xHCI can flood the event ring
                                                             * with unrelated events that never
                                                             * match, which would otherwise spin
                                                             * here forever and hang the system */
        volatile uint32_t *e = g_ering + g_edeq * 4;
        if ((e[3] & 1u) == g_ecycle) {
            uint32_t d0 = e[0], d1 = e[1], d2 = e[2], d3 = e[3];
            g_edeq++;
            if (g_edeq == RING_TRBS) { g_edeq = 0; g_ecycle ^= 1; }
            erdp_write();
            uint32_t type = (d3 >> 10) & 0x3F;
            uint64_t etrb = (uint64_t)d0 | ((uint64_t)d1 << 32);
            if (type == 33 || type == 32) {                     /* Command / Transfer event */
                if (match == 0 || etrb == match) {
                    if (out) { out->cc = (d2 >> 24) & 0xFF; out->xfer = d2 & 0x1FFFFF;
                               out->slot = (d3 >> 24) & 0xFF; out->epid = (d3 >> 16) & 0x1F; out->trb = etrb; }
                    uint32_t cc = (d2 >> 24) & 0xFF;
                    return (cc == 1 || cc == 13) ? 0 : -(int)cc; /* 1=Success 13=ShortPacket */
                }
            }
            /* else: port status change / unrelated -> keep draining */
        } else {
            __asm__ volatile("pause");
        }
    }
}

static inline void ring_db(uint32_t slot, uint32_t target) { wr32(g_db, slot * 4, target); }

/* ---- device state (single device / single LUN) ---- */
static struct ring g_cmd, g_ep0, g_in, g_out;
static uint32_t  g_ctxsz;                    /* 32 or 64 byte contexts     */
static uint32_t  g_slot;                     /* assigned slot id           */
static uint32_t  g_maxslots, g_maxports;
static volatile uint32_t *g_dcbaa;
static volatile uint32_t *g_inctx, *g_devctx;
static int       g_ready;                    /* MSC device usable          */
static int       g_ctrl_up;                  /* xHCI controller brought up */
static uint32_t  g_portnum;                  /* root port of the MSC device */
static int       g_gen;                      /* bumps on connect/disconnect (mount invalidation) */
static uint64_t  g_nsect;                    /* capacity in 512B sectors   */
static uint32_t  g_blksz = 512;
static int       g_dci_in, g_dci_out;        /* bulk endpoint DCIs         */
static uint8_t  *g_small;                    /* CBW/CSW/descriptor DMA buf */
static uint8_t  *g_bounce;                   /* 64KB-aligned data DMA buf  */
static uint32_t  g_tag = 1;

/* context field helpers (byte offsets into a context array) */
static volatile uint32_t *cx(volatile uint32_t *base, int index) {
    return base + (index * g_ctxsz) / 4;
}

/* ---- command ring: issue one TRB, ring DB0, wait ---- */
static int cmd_run(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, struct evt *ev) {
    uint64_t trb = ring_push(&g_cmd, d0, d1, d2, d3);
    ring_db(0, 0);
    return wait_event(trb, ev);
}

/* ---- control transfer on EP0 ---- */
static int ctrl_xfer(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint16_t len, void *buf, int in) {
    uint32_t s0 = (uint32_t)rt | ((uint32_t)req << 8) | ((uint32_t)val << 16);
    uint32_t s1 = (uint32_t)idx | ((uint32_t)len << 16);
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&g_ep0, s0, s1, 8, (2u << 10) | (1u << 6) | (trt << 16));       /* Setup (IDT) */
    if (len) {
        uint64_t pa = (uint64_t)(uintptr_t)buf;
        ring_push(&g_ep0, (uint32_t)pa, (uint32_t)(pa >> 32), len,
                  (3u << 10) | (in ? (1u << 16) : 0));                        /* Data */
    }
    int sdir = len ? (in ? 0 : 1) : 1;                                        /* status dir */
    uint64_t st = ring_push(&g_ep0, 0, 0, 0, (4u << 10) | (sdir ? (1u << 16) : 0) | (1u << 5)); /* Status IOC */
    ring_db(g_slot, 1);
    return wait_event(st, 0);
}

/* ---- bulk transfer ---- */
static int bulk_xfer(int dci, void *buf, uint32_t len, int in, struct evt *ev) {
    struct ring *r = in ? &g_in : &g_out;
    uint64_t pa = (uint64_t)(uintptr_t)buf;
    uint64_t trb = ring_push(r, (uint32_t)pa, (uint32_t)(pa >> 32), len,
                             (1u << 10) | (1u << 5) | (in ? (1u << 2) : 0));  /* Normal, IOC, ISP */
    ring_db(g_slot, (uint32_t)dci);
    return wait_event(trb, ev);
}

/* ---- SCSI over Bulk-Only Transport ---- */
static int scsi(const uint8_t *cdb, int cdblen, void *data, uint32_t dlen, int in) {
    static int g_utrace;                            /* cap trace spam */
    uint8_t *cbw = g_small;                         /* 31-byte Command Block Wrapper */
    memset(cbw, 0, 31);
    uint32_t tag = g_tag++;
    cbw[0]=0x55; cbw[1]=0x53; cbw[2]=0x42; cbw[3]=0x43;             /* 'USBC' */
    cbw[4]=(uint8_t)tag; cbw[5]=(uint8_t)(tag>>8); cbw[6]=(uint8_t)(tag>>16); cbw[7]=(uint8_t)(tag>>24);
    cbw[8]=(uint8_t)dlen; cbw[9]=(uint8_t)(dlen>>8); cbw[10]=(uint8_t)(dlen>>16); cbw[11]=(uint8_t)(dlen>>24);
    cbw[12]= in ? 0x80 : 0x00;                                     /* direction */
    cbw[13]=0;                                                     /* LUN 0 */
    cbw[14]=(uint8_t)cdblen;
    for (int i = 0; i < cdblen && i < 16; i++) cbw[15+i] = cdb[i];
    int r;
    if ((r = bulk_xfer(g_dci_out, cbw, 31, 0, 0)) != 0) {          /* CBW */
        if (g_utrace++ < 24) { kputs("[usb] CBW fail op=0x"); kput_u64hex(cdb[0]); kputs(" rc="); kput_u32dec((uint32_t)-r); kputc('\n'); }
        return -1;
    }
    if (dlen) {                                                    /* data phase (bounce) */
        if (!in) memcpy(g_bounce, data, dlen);
        if ((r = bulk_xfer(in ? g_dci_in : g_dci_out, g_bounce, dlen, in, 0)) != 0) {
            if (g_utrace++ < 24) { kputs("[usb] DATA fail op=0x"); kput_u64hex(cdb[0]); kputs(in?" IN":" OUT"); kputs(" rc="); kput_u32dec((uint32_t)-r); kputc('\n'); }
            return -2;
        }
        if (in) memcpy(data, g_bounce, dlen);
    }
    uint8_t *csw = g_small;                                        /* 13-byte Command Status Wrapper */
    memset(csw, 0, 13);
    if ((r = bulk_xfer(g_dci_in, csw, 13, 1, 0)) != 0) {
        if (g_utrace++ < 24) { kputs("[usb] CSW fail op=0x"); kput_u64hex(cdb[0]); kputs(" rc="); kput_u32dec((uint32_t)-r); kputc('\n'); }
        return -3;
    }
    if (csw[0]!=0x55 || csw[1]!=0x53 || csw[2]!=0x42 || csw[3]!=0x53) {  /* 'USBS' */
        if (g_utrace++ < 24) { kputs("[usb] CSW badsig op=0x"); kput_u64hex(cdb[0]); kputc('\n'); }
        return -4;
    }
    if (csw[12] && g_utrace++ < 24) { kputs("[usb] CSW status="); kput_u32dec(csw[12]); kputs(" op=0x"); kput_u64hex(cdb[0]); kputc('\n'); }
    return csw[12];                                                /* CSW status: 0 = good */
}

/* ---- public block API ---- */
int usb_present(void) {
    if (g_ready && g_ctrl_up && g_portnum && !(rd32(g_op, O_PORTSC(g_portnum)) & 1)) {
        g_ready = 0; g_gen++;                                   /* device was unplugged */
    }
    return g_ready;
}
int usb_gen(void) { return g_gen; }
uint64_t usb_sectors(void) { return g_nsect; }

int usb_read(uint64_t lba, uint32_t count, void *buf) {
    if (!g_ready) return -1;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count > 64 ? 64 : count;                     /* <=32KB per BOT xfer */
        uint8_t cdb[10] = { 0x28, 0, (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8),
                            (uint8_t)lba, 0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        if (scsi(cdb, 10, out, n * g_blksz, 1) != 0) return -1;
        lba += n; count -= n; out += n * g_blksz;
    }
    return 0;
}
int usb_write(uint64_t lba, uint32_t count, const void *buf) {
    if (!g_ready) return -1;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count > 64 ? 64 : count;
        uint8_t cdb[10] = { 0x2A, 0, (uint8_t)(lba>>24), (uint8_t)(lba>>16), (uint8_t)(lba>>8),
                            (uint8_t)lba, 0, (uint8_t)(n>>8), (uint8_t)n, 0 };
        if (scsi(cdb, 10, (void *)in, n * g_blksz, 0) != 0) return -1;
        lba += n; count -= n; in += n * g_blksz;
    }
    return 0;
}

/* ---- enumeration ---- */
static uint32_t ep0_mps(uint32_t speed) {
    switch (speed) { case 2: return 8; case 4: return 512; default: return 64; }  /* LS / SS / FS,HS */
}

/* Build EP0 into the input context; program A0|A1; return input-ctx PA. */
static uint64_t setup_input_ctx(uint32_t port, uint32_t speed, uint32_t mps0) {
    for (uint32_t i = 0; i < 1024; i++) g_inctx[i] = 0;
    volatile uint32_t *ictl = cx(g_inctx, 0);
    ictl[1] = 0x3;                                               /* Add A0 (slot) + A1 (EP0) */
    volatile uint32_t *slot = cx(g_inctx, 1);
    slot[0] = (1u << 27) | ((speed & 0xF) << 20);               /* Context Entries=1, Speed */
    slot[1] = (port & 0xFF) << 16;                              /* Root Hub Port Number */
    volatile uint32_t *ep0 = cx(g_inctx, 2);
    ep0[1] = (4u << 3) | (3u << 1) | (mps0 << 16);             /* EP Type=Control, CErr=3, MPS */
    ep0[2] = (uint32_t)g_ep0.pa | 1u;                          /* TR Dequeue | DCS */
    ep0[3] = (uint32_t)(g_ep0.pa >> 32);
    ep0[4] = 8;                                                 /* Average TRB Length */
    return (uint64_t)(uintptr_t)g_inctx;
}

static int enumerate(uint32_t port) {
    uint32_t portsc = rd32(g_op, O_PORTSC(port));
    if (!(portsc & (1u << 9))) {                                /* ensure Port Power is on */
        wr32(g_op, O_PORTSC(port), (portsc & ~((1u<<1) | 0x00FE0000u)) | (1u<<9));
        sleep_ms(20);
        portsc = rd32(g_op, O_PORTSC(port));
    }
    if (!(portsc & 1)) return 0;                                /* CCS: no device connected */
    sleep_ms(100);                                              /* connect debounce (hotplug) */
    portsc = rd32(g_op, O_PORTSC(port));
    if (!(portsc & 1)) return 0;

    if (!(portsc & (1u << 1))) {                                /* not enabled -> reset (USB2) */
        uint32_t v = portsc & ~((1u<<1) | 0x00FE0000u);         /* keep PED=0, don't clear RW1C */
        wr32(g_op, O_PORTSC(port), v | (1u << 4));              /* PR = port reset */
        uint64_t dl = millis() + 500;
        while (!(rd32(g_op, O_PORTSC(port)) & (1u << 21))) {    /* PRC: reset complete */
            if (millis() > dl) break;
            __asm__ volatile("pause");
        }
        wr32(g_op, O_PORTSC(port), (rd32(g_op,O_PORTSC(port)) & ~((1u<<1)|0x00FE0000u)) | (1u<<21)); /* ack PRC */
    }
    portsc = rd32(g_op, O_PORTSC(port));
    if (!(portsc & (1u << 1))) return 0;                        /* still not enabled */
    uint32_t speed = (portsc >> 10) & 0xF;
    uint32_t mps0 = ep0_mps(speed);

    struct evt ev;
    if (cmd_run(0, 0, 0, (9u << 10), &ev) != 0) return 0;       /* Enable Slot */
    g_slot = ev.slot;
    if (!g_slot || g_slot > g_maxslots) return 0;

    ring_init(&g_ep0, g_ep0.trb);                               /* fresh EP0 transfer ring */
    for (uint32_t i = 0; i < 1024; i++) g_devctx[i] = 0;
    uint64_t dcpa = (uint64_t)(uintptr_t)g_devctx;
    wr64((volatile uint8_t *)g_dcbaa, g_slot * 8, dcpa);        /* DCBAA[slot] = device ctx */

    uint64_t ic = setup_input_ctx(port, speed, mps0);
    if (cmd_run((uint32_t)ic, (uint32_t)(ic >> 32), 0, (11u << 10) | (g_slot << 24), &ev) != 0)
        return 0;                                               /* Address Device */

    /* device descriptor: first 8 bytes to learn the real bMaxPacketSize0 */
    uint8_t *db = g_small;
    if (ctrl_xfer(0x80, 6, 0x0100, 0, 8, db, 1) != 0) return 0;
    uint32_t real_mps = (speed == 4) ? (1u << db[7]) : db[7];
    if (real_mps && real_mps != mps0) {                         /* Evaluate Context: fix EP0 MPS */
        for (uint32_t i = 0; i < 1024; i++) g_inctx[i] = 0;
        cx(g_inctx, 0)[1] = 0x2;                                /* A1 only */
        volatile uint32_t *ep0 = cx(g_inctx, 2);
        ep0[1] = (4u << 3) | (3u << 1) | (real_mps << 16);
        ep0[2] = (uint32_t)g_ep0.pa | 1u; ep0[3] = (uint32_t)(g_ep0.pa >> 32); ep0[4] = 8;
        cmd_run((uint32_t)ic, (uint32_t)(ic >> 32), 0, (13u << 10) | (g_slot << 24), &ev);
        mps0 = real_mps;
    }

    /* configuration descriptor: header first (for wTotalLength), then the full set */
    if (ctrl_xfer(0x80, 6, 0x0200, 0, 9, db, 1) != 0) return 0;
    uint32_t total = db[2] | (db[3] << 8);
    if (total > 512) total = 512;
    if (ctrl_xfer(0x80, 6, 0x0200, 0, (uint16_t)total, db, 1) != 0) return 0;
    uint8_t cfgval = db[5];

    int msc = 0, in_ep = 0, out_ep = 0; uint32_t in_mps = 512, out_mps = 512;
    for (uint32_t o = 0; o + 2 <= total; ) {
        uint8_t blen = db[o], btype = db[o + 1];
        if (blen < 2) break;
        if (btype == 4) {                                       /* interface */
            msc = (db[o+5] == 0x08 && db[o+7] == 0x50);         /* class MSC, protocol BOT */
        } else if (btype == 5 && msc) {                         /* endpoint */
            uint8_t addr = db[o+2], attr = db[o+3];
            uint32_t mps = db[o+4] | (db[o+5] << 8);
            if ((attr & 3) == 2) {                              /* bulk */
                if (addr & 0x80) { in_ep = addr & 0x0F; in_mps = mps; }
                else             { out_ep = addr & 0x0F; out_mps = mps; }
            }
        }
        o += blen;
    }
    if (!msc || !in_ep || !out_ep) { kputs("[usb] device is not USB mass storage\n"); return 0; }

    if (ctrl_xfer(0x00, 9, cfgval, 0, 0, 0, 0) != 0) return 0;  /* SET_CONFIGURATION */

    /* Configure Endpoint: add the two bulk endpoints */
    g_dci_in  = in_ep * 2 + 1;
    g_dci_out = out_ep * 2;
    int maxdci = g_dci_in > g_dci_out ? g_dci_in : g_dci_out;
    ring_init(&g_in, g_in.trb);
    ring_init(&g_out, g_out.trb);
    for (uint32_t i = 0; i < 1024; i++) g_inctx[i] = 0;
    cx(g_inctx, 0)[1] = 1u | (1u << g_dci_in) | (1u << g_dci_out);   /* A0 + both EP flags */
    volatile uint32_t *slotc = cx(g_inctx, 1);
    slotc[0] = ((uint32_t)maxdci << 27) | ((speed & 0xF) << 20);
    slotc[1] = (port & 0xFF) << 16;
    volatile uint32_t *epo = cx(g_inctx, g_dci_out + 1);
    epo[1] = (2u << 3) | (3u << 1) | (out_mps << 16);          /* Bulk OUT */
    epo[2] = (uint32_t)g_out.pa | 1u; epo[3] = (uint32_t)(g_out.pa >> 32); epo[4] = out_mps;
    volatile uint32_t *epi = cx(g_inctx, g_dci_in + 1);
    epi[1] = (6u << 3) | (3u << 1) | (in_mps << 16);           /* Bulk IN */
    epi[2] = (uint32_t)g_in.pa | 1u; epi[3] = (uint32_t)(g_in.pa >> 32); epi[4] = in_mps;
    if (cmd_run((uint32_t)ic, (uint32_t)(ic >> 32), 0, (12u << 10) | (g_slot << 24), &ev) != 0)
        return 0;                                               /* Configure Endpoint */

    /* SCSI bring-up: TEST UNIT READY (retry), READ CAPACITY(10) */
    uint8_t tur[6] = {0,0,0,0,0,0};
    for (int i = 0; i < 5; i++) { if (scsi(tur, 6, 0, 0, 0) == 0) break; sleep_ms(100); }
    uint8_t rc10[10] = {0x25,0,0,0,0,0,0,0,0,0};
    uint8_t cap[8];
    if (scsi(rc10, 10, cap, 8, 1) != 0) { kputs("[usb] READ CAPACITY failed\n"); return 0; }
    uint32_t last = ((uint32_t)cap[0]<<24)|((uint32_t)cap[1]<<16)|((uint32_t)cap[2]<<8)|cap[3];
    uint32_t bs   = ((uint32_t)cap[4]<<24)|((uint32_t)cap[5]<<16)|((uint32_t)cap[6]<<8)|cap[7];
    if (!bs) bs = 512;
    g_blksz = bs;
    g_nsect = (uint64_t)(last + 1) * (bs / 512 ? bs / 512 : 1);
    kputs("[usb] MSC disk slot="); kput_u32dec(g_slot);
    kputs(" bs="); kput_u32dec(bs);
    kputs(" sectors="); kput_u64hex(g_nsect);
    kputs(" ("); kput_u32dec((uint32_t)(g_nsect / 2048)); kputs(" MB)\n");
    g_ready = 1; g_portnum = port; g_gen++;
    return 1;
}

/* ---- controller bring-up ---- */
static int scan_ports(void);

static void legacy_handoff(uint32_t hcc1) {
    uint32_t ecp = (hcc1 >> 16) & 0xFFFF;
    if (!ecp) return;
    uint32_t off = ecp * 4, guard = 0;
    while (off && guard++ < 64) {
        uint32_t c = rd32(g_cap, off);
        uint8_t id = c & 0xFF, nxt = (c >> 8) & 0xFF;
        if (id == 1) {                                          /* USB Legacy Support */
            wr32(g_cap, off, c | (1u << 24));                   /* HC OS Owned */
            uint64_t dl = millis() + 1000;
            while ((rd32(g_cap, off) & (1u << 16)) && millis() < dl) __asm__ volatile("pause");
            wr32(g_cap, off + 4, 0);                            /* disable all SMIs */
            return;
        }
        if (!nxt) break;
        off += nxt * 4;
    }
}
/* Intel xHCI: the USB2 ports default to the companion EHCI controller, so a USB2
 * stick is invisible on xHCI until we switch the ports over (this is why the
 * T440s showed "no device" while QEMU worked).  Mirror Linux's quirk: enable
 * SuperSpeed on all SS-capable ports, then route all USB2 ports to xHCI. */
static void intel_port_switchover(int bus, int dev, int fn) {
    if ((pci_rd(bus, dev, fn, 0) & 0xFFFF) != 0x8086) return;      /* Intel only */
    uint32_t ss = pci_rd(bus, dev, fn, 0xDC);                      /* USB3PRM  */
    pci_wr(bus, dev, fn, 0xD8, ss);                               /* -> USB3_PSSEN */
    uint32_t u2 = pci_rd(bus, dev, fn, 0xD4);                      /* XUSB2PRM */
    pci_wr(bus, dev, fn, 0xD0, u2);                               /* -> XUSB2PR    */
    kputs("[usb] intel port switchover: usb3ss=0x"); kput_u64hex(ss);
    kputs(" usb2xhci=0x"); kput_u64hex(u2); kputc('\n');
}

static int bringup(int bus, int dev, int fn) {
    pci_wr(bus, dev, fn, 0x04, pci_rd(bus, dev, fn, 0x04) | 0x6);   /* mem space + bus master */
    intel_port_switchover(bus, dev, fn);
    uint32_t bar0 = pci_rd(bus, dev, fn, 0x10);
    uint64_t mmio = bar0 & ~0xFu;
    if ((bar0 & 0x6) == 0x4) mmio |= (uint64_t)pci_rd(bus, dev, fn, 0x14) << 32;  /* 64-bit BAR */
    if (!mmio) return 0;
    g_cap = (volatile uint8_t *)(uintptr_t)mmio;

    uint32_t caplen = rd32(g_cap, 0) & 0xFF;
    uint32_t hcs1 = rd32(g_cap, 0x04);
    uint32_t hcs2 = rd32(g_cap, 0x08);
    uint32_t hcc1 = rd32(g_cap, 0x10);
    uint32_t dboff = rd32(g_cap, 0x14) & ~0x3u;
    uint32_t rtsoff = rd32(g_cap, 0x18) & ~0x1Fu;
    g_op = g_cap + caplen;
    g_rt = g_cap + rtsoff;
    g_db = g_cap + dboff;
    g_maxslots = hcs1 & 0xFF;
    g_maxports = (hcs1 >> 24) & 0xFF;
    g_ctxsz = (hcc1 & (1u << 2)) ? 64 : 32;
    if (!(hcc1 & 1u)) { kputs("[usb] no 64-bit addressing; skip\n"); return 0; }
    uint32_t scratch = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);

    legacy_handoff(hcc1);

    /* reset */
    uint64_t dl = millis() + 1000;
    while ((rd32(g_op, O_USBSTS) & (1u << 11)) && millis() < dl) __asm__ volatile("pause"); /* CNR */
    if (rd32(g_op, O_USBCMD) & 1u) {
        wr32(g_op, O_USBCMD, rd32(g_op, O_USBCMD) & ~1u);
        dl = millis() + 1000;
        while (!(rd32(g_op, O_USBSTS) & 1u) && millis() < dl) __asm__ volatile("pause");    /* HCH */
    }
    wr32(g_op, O_USBCMD, rd32(g_op, O_USBCMD) | (1u << 1));                                 /* HCRST */
    dl = millis() + 1000;
    while ((rd32(g_op, O_USBCMD) & (1u << 1)) && millis() < dl) __asm__ volatile("pause");
    dl = millis() + 1000;
    while ((rd32(g_op, O_USBSTS) & (1u << 11)) && millis() < dl) __asm__ volatile("pause"); /* CNR */

    if (g_maxslots > 32) g_maxslots = 32;
    wr32(g_op, O_CONFIG, g_maxslots);

    /* DCBAA (+ scratchpad) */
    g_dcbaa = (volatile uint32_t *)page_alloc(1);
    if (scratch) {
        volatile uint64_t *sp = (volatile uint64_t *)page_alloc(1);
        for (uint32_t i = 0; i < scratch; i++) sp[i] = (uint64_t)(uintptr_t)page_alloc(1);
        ((volatile uint64_t *)g_dcbaa)[0] = (uint64_t)(uintptr_t)sp;
    }
    wr64(g_op, O_DCBAAP, (uint64_t)(uintptr_t)g_dcbaa);

    /* command ring */
    ring_init(&g_cmd, (volatile uint32_t *)page_alloc(1));
    wr64(g_op, O_CRCR, g_cmd.pa | 1u);                          /* RCS = 1 */

    /* event ring (single segment) + ERST */
    g_ering = (volatile uint32_t *)page_alloc(1);
    g_edeq = 0; g_ecycle = 1;
    volatile uint32_t *erst = (volatile uint32_t *)page_alloc(1);
    uint64_t epa = (uint64_t)(uintptr_t)g_ering;
    erst[0] = (uint32_t)epa; erst[1] = (uint32_t)(epa >> 32); erst[2] = RING_TRBS; erst[3] = 0;
    wr32(g_rt, R_ERSTSZ, 1);
    wr64(g_rt, R_ERDP, epa);
    wr64(g_rt, R_ERSTBA, (uint64_t)(uintptr_t)erst);

    /* per-device structures */
    g_inctx  = (volatile uint32_t *)page_alloc(1);
    g_devctx = (volatile uint32_t *)page_alloc(1);
    ring_init(&g_ep0, (volatile uint32_t *)page_alloc(1));
    ring_init(&g_in,  (volatile uint32_t *)page_alloc(1));
    ring_init(&g_out, (volatile uint32_t *)page_alloc(1));
    g_small  = (uint8_t *)page_alloc(1);
    uint8_t *braw = (uint8_t *)page_alloc(32);                  /* 128KB -> 64KB-aligned 64KB */
    g_bounce = (uint8_t *)(((uintptr_t)braw + 0xFFFF) & ~(uintptr_t)0xFFFF);

    /* run */
    wr32(g_op, O_USBCMD, rd32(g_op, O_USBCMD) | 1u);           /* R/S = 1 */
    dl = millis() + 1000;
    while ((rd32(g_op, O_USBSTS) & 1u) && millis() < dl) __asm__ volatile("pause"); /* HCH clear */
    sleep_ms(100);                                             /* let ports settle after power-on */

    kputs("[usb] xHCI @ "); kput_u64hex(mmio);
    kputs(" slots="); kput_u32dec(g_maxslots);
    kputs(" ports="); kput_u32dec(g_maxports); kputc('\n');
    g_ctrl_up = 1;
    return scan_ports();
}

/* Scan all root ports for a connected device and try to enumerate it as MSC.
 * Reuses the already-allocated controller + per-device structures, so it is safe
 * to retry (e.g. the user plugged the stick in after boot). */
static int scan_ports(void) {
    if (!g_ctrl_up) return 0;
    for (uint32_t p = 1; p <= g_maxports; p++) {
        if (rd32(g_op, O_PORTSC(p)) & 1u) {                    /* CCS: device connected */
            if (enumerate(p)) return 1;
            g_ready = 0;                                       /* try the next port */
        }
    }
    return 0;
}

int usb_init(void) {
    if (usb_present()) return 1;
    if (g_ctrl_up) return scan_ports();                        /* controller up: just re-enumerate */
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                if ((uint16_t)id == 0xFFFF) { if (fn == 0) break; else continue; }
                uint32_t cls = pci_rd(bus, dev, fn, 0x08);     /* class/subclass/progif */
                if ((cls >> 8) == 0x0C0330) {                  /* xHCI */
                    if (bringup(bus, dev, fn)) return 1;
                }
            }
    kputs("[usb] no USB mass-storage device found\n");
    return 0;
}
