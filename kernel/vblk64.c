/* tinyos x64 virtio-blk (legacy PCI) block driver.
 *
 * Persistent storage for the QEMU dev environment: 512-byte sector read/write to
 * a virtio-blk disk, so filesystem changes survive reboots.  Uses the legacy
 * (virtio 0.9.5) port-I/O interface — the simplest to drive without interrupts:
 * find the PCI device, set up one split virtqueue, and submit each request
 * synchronously (header + data + status descriptors) then poll the used ring.
 *
 * If no virtio-blk device is present (e.g. Hyper-V, which uses VMBus StorVSC),
 * vblk_init() returns 0 and kernel/fat64.c falls back to the RAM-disk image. */
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
#define VIO_QADDR    0x08          /* queue physical page number */
#define VIO_QSIZE    0x0C
#define VIO_QSEL     0x0E
#define VIO_QNOTIFY  0x10
#define VIO_STATUS   0x12
#define VIO_CONFIG   0x14          /* device-specific config (no MSI-X) */
#define ST_ACK 1
#define ST_DRV 2
#define ST_OK  4

/* ---- split virtqueue ---- */
#define VRING_ALIGN 4096
#define VD_NEXT  1
#define VD_WRITE 2                 /* buffer is written by the device */
struct vdesc { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; } __attribute__((packed));
struct vavail { uint16_t flags, idx; uint16_t ring[]; };
struct vused_elem { uint32_t id, len; };
struct vused { uint16_t flags, idx; struct vused_elem ring[]; };

static uint16_t       g_io;
static uint16_t       g_qsz;
static struct vdesc  *g_desc;
static struct vavail *g_avail;
static struct vused  *g_used;
static uint16_t       g_last_used;
static int            g_ready;

/* virtio-blk request header + status byte (identity-mapped => VA == PA) */
struct vblk_req { uint32_t type, reserved; uint64_t sector; } __attribute__((packed));
static struct vblk_req  g_req;
static volatile uint8_t g_status;

static int find_dev(int *bo, int *do_, int *fo) {
    for (int bus = 0; bus < 16; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                uint32_t id = pci_rd(bus, dev, fn, 0x00);
                uint16_t vendor = (uint16_t)id, device = (uint16_t)(id >> 16);
                if (vendor == 0xFFFF) { if (fn == 0) break; else continue; }
                if (vendor == 0x1AF4 && (device == 0x1001 || device == 0x1042)) {
                    *bo = bus; *do_ = dev; *fo = fn; return 1;
                }
            }
    return 0;
}

int vblk_init(void) {
    int bus, dev, fn;
    if (!find_dev(&bus, &dev, &fn)) { kputs("[vblk] no virtio-blk device (RAM disk)\n"); return 0; }

    pci_wr(bus, dev, fn, 0x04, pci_rd(bus, dev, fn, 0x04) | 0x5);   /* I/O + bus master */
    uint32_t bar0 = pci_rd(bus, dev, fn, 0x10);
    if (!(bar0 & 1)) { kputs("[vblk] BAR0 is not I/O (modern-only?)\n"); return 0; }
    g_io = (uint16_t)(bar0 & ~0x3u);

    outb(g_io + VIO_STATUS, 0);                                    /* reset */
    outb(g_io + VIO_STATUS, ST_ACK);
    outb(g_io + VIO_STATUS, ST_ACK | ST_DRV);
    (void)inl(g_io + VIO_DEV_FEAT);
    outl(g_io + VIO_DRV_FEAT, 0);                                  /* accept no features */

    outw(g_io + VIO_QSEL, 0);
    g_qsz = inw(g_io + VIO_QSIZE);
    if (g_qsz == 0 || g_qsz > 1024) { kputs("[vblk] bad queue size\n"); return 0; }

    uint32_t desc_b  = 16u * g_qsz;
    uint32_t avail_b = 6u + 2u * g_qsz;
    uint32_t used_off = (desc_b + avail_b + (VRING_ALIGN - 1)) & ~(VRING_ALIGN - 1);
    uint32_t used_b  = 6u + 8u * g_qsz;
    uint32_t total   = used_off + used_b;
    uint8_t *region = (uint8_t *)page_alloc((total + 4095) / 4096);
    if (!region) { kputs("[vblk] queue alloc failed\n"); return 0; }

    g_desc  = (struct vdesc  *)region;
    g_avail = (struct vavail *)(region + desc_b);
    g_used  = (struct vused  *)(region + used_off);
    g_last_used = 0;

    outl(g_io + VIO_QADDR, (uint32_t)((uint64_t)(uintptr_t)region >> 12));   /* queue PFN */
    outb(g_io + VIO_STATUS, ST_ACK | ST_DRV | ST_OK);

    uint64_t cap = (uint64_t)inl(g_io + VIO_CONFIG) | ((uint64_t)inl(g_io + VIO_CONFIG + 4) << 32);
    kputs("[vblk] virtio-blk ready; "); kput_u32dec((uint32_t)(cap / 2048));
    kputs(" MB persistent disk\n");
    g_ready = 1;
    return 1;
}

static int vblk_rw(int write, uint32_t lba, void *buf) {
    if (!g_ready) return -1;
    g_req.type = write ? 1u : 0u;            /* 0 = read (IN), 1 = write (OUT) */
    g_req.reserved = 0;
    g_req.sector = lba;
    g_status = 0xFF;

    g_desc[0].addr = (uint64_t)(uintptr_t)&g_req;
    g_desc[0].len = 16; g_desc[0].flags = VD_NEXT;                       g_desc[0].next = 1;
    g_desc[1].addr = (uint64_t)(uintptr_t)buf;
    g_desc[1].len = 512; g_desc[1].flags = VD_NEXT | (write ? 0 : VD_WRITE); g_desc[1].next = 2;
    g_desc[2].addr = (uint64_t)(uintptr_t)&g_status;
    g_desc[2].len = 1; g_desc[2].flags = VD_WRITE;                       g_desc[2].next = 0;

    uint16_t idx = g_avail->idx;
    g_avail->ring[idx % g_qsz] = 0;          /* head of the chain = descriptor 0 */
    barrier();
    g_avail->idx = (uint16_t)(idx + 1);
    barrier();
    outw(g_io + VIO_QNOTIFY, 0);

    uint32_t guard = 0;
    while (g_used->idx == g_last_used) {
        if (++guard > 200000000u) { kputs("[vblk] I/O timeout\n"); return -1; }
        __asm__ volatile("pause");
    }
    g_last_used = g_used->idx;
    barrier();
    return (g_status == 0) ? 0 : -1;
}

int vblk_read(uint32_t lba, void *buf)        { return vblk_rw(0, lba, buf); }
int vblk_write(uint32_t lba, const void *buf) { return vblk_rw(1, lba, (void *)buf); }
