/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 AHCI (SATA) driver — real-hardware disk access.
 *
 * The T440s (and any modern UEFI PC) exposes its SATA SSD through an AHCI HBA,
 * not virtio/StorVSC, so this is what lets TinyOS read and write the internal
 * disk (toward booting from it instead of USB).  Minimal but real: discover the
 * HBA over PCI, take the first SATA port with a device, rebase its command list
 * / FIS receive area into identity-mapped DMA memory, and issue single 48-bit
 * READ/WRITE DMA EXT commands with a one-entry PRDT.
 *
 * Every wait is bounded (millis timeout / capped spins) so a wedged controller
 * can never hang the machine.  Writes are exposed but callers gate them behind
 * read-only verification first.  DMA structures live in the heap arena, which is
 * identity-mapped (VA == PA) and may sit above 4 GiB, so all HBA pointers set
 * their upper 32 bits (64-bit addressing; we require CAP.S64A). */
#include "boot64.h"

static inline void   outl(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint32_t inl(uint16_t p) { uint32_t r; __asm__ volatile("inl %1,%0" : "=a"(r) : "Nd"(p)); return r; }

/* ---- PCI config (0xCF8/0xCFC) ---- */
static uint32_t pci_rd(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
static void pci_wr(int bus, int dev, int fn, int off, uint32_t v) {
    outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) | (fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}

/* ---- HBA memory registers (ABAR + offset) ---- */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_PI    0x0C
#define HBA_CAP2  0x24
#define GHC_AE    (1u << 31)
#define GHC_HR    (1u << 0)
#define CAP_S64A  (1u << 31)

/* ---- per-port registers (ABAR + 0x100 + port*0x80 + off) ---- */
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_IE    0x14
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SCTL  0x2C
#define P_SERR  0x30
#define P_CI    0x38
#define CMD_ST  (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR  (1u << 14)
#define CMD_CR  (1u << 15)
#define TFD_BSY (1u << 7)
#define TFD_DRQ (1u << 3)
#define TFD_ERR (1u << 0)
#define IS_TFES (1u << 30)
#define SIG_SATA 0x00000101u

/* ---- command header / table / FIS ---- */
struct cmd_hdr {
    uint8_t  cfl_flags;          /* [0:4]=FIS len (dwords), [6]=W */
    uint8_t  pmp;
    uint16_t prdtl;              /* PRDT entry count */
    volatile uint32_t prdbc;     /* bytes transferred */
    uint32_t ctba, ctbau;        /* command-table base */
    uint32_t rsv[4];
} __attribute__((packed));

struct prdt_ent { uint32_t dba, dbau, rsv; uint32_t dbc; } __attribute__((packed));
struct cmd_tbl  { uint8_t cfis[64]; uint8_t acmd[16]; uint8_t rsv[48]; struct prdt_ent prdt[1]; } __attribute__((packed));

struct fis_h2d {
    uint8_t fis_type, pmport_c, command, featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth, icc, control;
    uint8_t rsv[4];
} __attribute__((packed));

static volatile uint32_t *g_abar;
static int      g_port = -1;
static uint64_t g_nsect;
static struct cmd_hdr *g_clb;         /* command list (32 headers)     */
static struct cmd_tbl *g_ctbl;        /* one command table (slot 0)    */
static uint8_t *g_fis;
static uint16_t *g_ident;
static int g_bus = -1, g_dev, g_fn;

static inline uint32_t hr(uint32_t o)            { return g_abar[o >> 2]; }
static inline void     hw(uint32_t o, uint32_t v){ g_abar[o >> 2] = v; }
static inline uint32_t pr(uint32_t o)            { return g_abar[(0x100 + g_port * 0x80 + o) >> 2]; }
static inline void     pw(uint32_t o, uint32_t v){ g_abar[(0x100 + g_port * 0x80 + o) >> 2] = v; }

static int is_ahci(int bus, int dev, int fn) {
    uint32_t id = pci_rd(bus, dev, fn, 0x00);
    if ((uint16_t)id == 0xFFFF) return 0;
    uint32_t cls = pci_rd(bus, dev, fn, 0x08);               /* rev|progif|subcls|class */
    return (cls >> 24) == 0x01 && ((cls >> 16) & 0xFF) == 0x06 && ((cls >> 8) & 0xFF) == 0x01;
}

static void port_stop(void) {
    pw(P_CMD, pr(P_CMD) & ~CMD_ST);
    for (int i = 0; i < 500000 && (pr(P_CMD) & CMD_CR); i++) __asm__ volatile("pause");
    pw(P_CMD, pr(P_CMD) & ~CMD_FRE);
    for (int i = 0; i < 500000 && (pr(P_CMD) & CMD_FR); i++) __asm__ volatile("pause");
}
static void port_start(void) {
    for (int i = 0; i < 500000 && (pr(P_CMD) & CMD_CR); i++) __asm__ volatile("pause");
    pw(P_CMD, pr(P_CMD) | CMD_FRE);
    pw(P_CMD, pr(P_CMD) | CMD_ST);
}

/* Issue one already-built command in slot 0 and wait (bounded) for completion. */
static int port_run(void) {
    for (int i = 0; i < 1000000 && (pr(P_TFD) & (TFD_BSY | TFD_DRQ)); i++) __asm__ volatile("pause");
    if (pr(P_TFD) & (TFD_BSY | TFD_DRQ)) return -1;                 /* port stuck busy */
    pw(P_IS, ~0u);                                                   /* clear port interrupts */
    pw(P_CI, 1u);                                                    /* issue slot 0 */
    uint64_t deadline = millis() + 4000;
    while (pr(P_CI) & 1u) {
        if (pr(P_IS) & IS_TFES) return -2;                          /* task-file error */
        if (millis() > deadline) return -3;                         /* timeout */
        __asm__ volatile("pause");
    }
    if (pr(P_TFD) & TFD_ERR) return -4;
    return 0;
}

/* Build a slot-0 command for `cmd` at `lba`/`count`, DMA into/out of `buf`. */
static int ahci_cmd(uint8_t cmd, uint64_t lba, uint16_t count, void *buf, int write) {
    if (g_port < 0) return -1;
    uint64_t bpa = (uint64_t)(uintptr_t)buf;
    uint32_t bytes = (uint32_t)count * 512u;

    struct cmd_hdr *h = &g_clb[0];
    h->cfl_flags = (uint8_t)((sizeof(struct fis_h2d) / 4) | (write ? 0x40 : 0));
    h->pmp = 0;
    h->prdtl = count ? 1 : 0;
    h->prdbc = 0;
    uint64_t tpa = (uint64_t)(uintptr_t)g_ctbl;
    h->ctba = (uint32_t)tpa; h->ctbau = (uint32_t)(tpa >> 32);

    for (unsigned i = 0; i < sizeof(*g_ctbl); i++) ((uint8_t *)g_ctbl)[i] = 0;
    if (count) {
        g_ctbl->prdt[0].dba  = (uint32_t)bpa;
        g_ctbl->prdt[0].dbau = (uint32_t)(bpa >> 32);
        g_ctbl->prdt[0].dbc  = bytes - 1;                           /* byte count - 1 */
    }

    struct fis_h2d *f = (struct fis_h2d *)g_ctbl->cfis;
    f->fis_type = 0x27;                                             /* H2D register FIS */
    f->pmport_c = 0x80;                                            /* C = command */
    f->command  = cmd;
    f->device   = (cmd == 0xEC) ? 0 : 0x40;                        /* LBA mode for R/W */
    f->lba0 = (uint8_t)lba;        f->lba1 = (uint8_t)(lba >> 8);  f->lba2 = (uint8_t)(lba >> 16);
    f->lba3 = (uint8_t)(lba >> 24); f->lba4 = (uint8_t)(lba >> 32); f->lba5 = (uint8_t)(lba >> 40);
    f->countl = (uint8_t)count;    f->counth = (uint8_t)(count >> 8);
    return port_run();
}

int ahci_read(uint64_t lba, uint32_t count, void *buf) {
    while (count) {
        uint32_t n = count > 2048 ? 2048 : count;                 /* 1 MiB / one PRDT entry */
        if (ahci_cmd(0x25, lba, (uint16_t)n, buf, 0) != 0) return -1;   /* READ DMA EXT */
        lba += n; count -= n; buf = (uint8_t *)buf + n * 512u;
    }
    return 0;
}
int ahci_write(uint64_t lba, uint32_t count, const void *buf) {
    while (count) {
        uint32_t n = count > 2048 ? 2048 : count;                 /* 1 MiB / one PRDT entry */
        if (ahci_cmd(0x35, lba, (uint16_t)n, (void *)buf, 1) != 0) return -1;  /* WRITE DMA EXT */
        lba += n; count -= n; buf = (const uint8_t *)buf + n * 512u;
    }
    return 0;
}
uint64_t ahci_sectors(void) { return g_nsect; }
int      ahci_present(void) { return g_port >= 0; }

/* Bring up one AHCI controller; returns 1 if a SATA disk was found + rebased. */
static int bringup(int bus, int dev, int fn) {
    pci_wr(bus, dev, fn, 0x04, pci_rd(bus, dev, fn, 0x04) | 0x6);   /* mem space + bus master */
    uint32_t bar5 = pci_rd(bus, dev, fn, 0x24);
    if (bar5 & 1) return 0;                                         /* ABAR must be memory */
    g_abar = (volatile uint32_t *)(uintptr_t)(bar5 & ~0xFu);
    if (!g_abar) return 0;
    hw(HBA_GHC, hr(HBA_GHC) | GHC_AE);                             /* enable AHCI mode */
    if (!(hr(HBA_CAP) & CAP_S64A)) { kputs("[ahci] no 64-bit DMA; skip\n"); return 0; }

    uint32_t pi = hr(HBA_PI);
    g_port = -1;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        g_port = p;
        if ((pr(P_SSTS) & 0x0F) != 3) { g_port = -1; continue; }   /* no device / no PHY  */
        if (pr(P_SIG) != SIG_SATA)    { g_port = -1; continue; }   /* not a plain SATA disk */
        break;
    }
    if (g_port < 0) return 0;

    kputs("[ahci] HBA @ "); kput_u64hex((uint64_t)(uintptr_t)g_abar);
    kputs(" port "); kput_u32dec((uint32_t)g_port); kputc('\n');

    port_stop();
    uint8_t *dma = (uint8_t *)page_alloc(1);                        /* cmd list + FIS + table */
    if (!dma) { g_port = -1; return 0; }
    for (int i = 0; i < 4096; i++) dma[i] = 0;
    g_clb  = (struct cmd_hdr *)(dma + 0);                           /* 1 KiB, 1 KiB-aligned */
    g_fis = dma + 1024;                                            /* 256 B, 256-aligned    */
    g_ctbl = (struct cmd_tbl *)(dma + 1280);                       /* 128-aligned           */
    uint64_t clbpa = (uint64_t)(uintptr_t)g_clb, fispa = (uint64_t)(uintptr_t)g_fis;
    pw(P_CLB, (uint32_t)clbpa); pw(P_CLBU, (uint32_t)(clbpa >> 32));
    pw(P_FB,  (uint32_t)fispa); pw(P_FBU,  (uint32_t)(fispa >> 32));
    pw(P_SERR, ~0u); pw(P_IS, ~0u);
    port_start();

    g_ident = (uint16_t *)page_alloc(1);                            /* IDENTIFY -> sector count */
    if (g_ident && ahci_cmd(0xEC, 0, 1, g_ident, 0) == 0) {
        g_nsect = (uint64_t)g_ident[100] | ((uint64_t)g_ident[101] << 16) |
                  ((uint64_t)g_ident[102] << 32) | ((uint64_t)g_ident[103] << 48);
        if (!g_nsect) g_nsect = (uint32_t)g_ident[60] | ((uint32_t)g_ident[61] << 16);
    }
    kputs("[ahci] SATA disk sectors="); kput_u64hex(g_nsect);
    kputs(" ("); kput_u32dec((uint32_t)(g_nsect / 2048)); kputs(" MB)\n");
    g_bus = bus; g_dev = dev; g_fn = fn;
    return 1;
}

int ahci_suspend(void) {
    if (g_port < 0) return 0;
    if (ahci_cmd(0xEA, 0, 0, 0, 0) != 0) return -1;                /* FLUSH CACHE EXT */
    uint64_t deadline = millis() + 1000;
    while (pr(P_CI) && millis() < deadline) __asm__ volatile("pause");
    if (pr(P_CI)) return -2;
    pw(P_IE, 0);
    port_stop();
    return (pr(P_CMD) & (CMD_CR | CMD_FR)) ? -3 : 0;
}

int ahci_resume(void) {
    if (g_port < 0) return 0;
    if (g_bus < 0 || !g_clb || !g_fis || !g_ctbl) return -1;
    pci_wr(g_bus, g_dev, g_fn, 0x04, pci_rd(g_bus, g_dev, g_fn, 0x04) | 0x6);
    hw(HBA_GHC, hr(HBA_GHC) | GHC_AE);
    if ((pr(P_SSTS) & 0x0F) != 3) {
        pw(P_SCTL, (pr(P_SCTL) & ~0x0Fu) | 1u); sleep_ms(2);
        pw(P_SCTL, pr(P_SCTL) & ~0x0Fu); sleep_ms(20);              /* COMRESET */
    }
    uint64_t clbpa = (uint64_t)(uintptr_t)g_clb, fispa = (uint64_t)(uintptr_t)g_fis;
    pw(P_CLB, (uint32_t)clbpa); pw(P_CLBU, (uint32_t)(clbpa >> 32));
    pw(P_FB, (uint32_t)fispa); pw(P_FBU, (uint32_t)(fispa >> 32));
    pw(P_SERR, ~0u); pw(P_IS, ~0u); pw(P_IE, 0);
    port_start();
    return g_ident && ahci_cmd(0xEC, 0, 1, g_ident, 0) == 0 ? 0 : -2;
}

int ahci_init(void) {
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                if ((uint16_t)pci_rd(bus, dev, fn, 0x00) == 0xFFFF) { if (fn == 0) break; else continue; }
                if (is_ahci(bus, dev, fn) && bringup(bus, dev, fn)) return 1;
            }
    kputs("[ahci] no SATA disk found\n");
    return 0;
}
