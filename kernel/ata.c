/* tinyos ATA (IDE) PIO driver — reads 512-byte sectors from the second disk
 * (QEMU: -hdb file.img => primary bus, slave).  Enough to mount a filesystem
 * image.  Under the Unicorn emulator the same port I/O is emulated in
 * tools/emulate.py so `mount` works there too. */
#include "kernel.h"

#define ATA_DATA    0x1F0
#define ATA_SECCNT  0x1F2
#define ATA_LBA0    0x1F3
#define ATA_LBA1    0x1F4
#define ATA_LBA2    0x1F5
#define ATA_DRIVE   0x1F6
#define ATA_CMD     0x1F7   /* write: command */
#define ATA_STATUS  0x1F7   /* read: status   */

#define ST_BSY  0x80
#define ST_DRQ  0x08
#define ST_ERR  0x01

static inline void  outb_(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb_(uint16_t p)            { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t inw_(uint16_t p)           { uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void  outw_(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }

static int ata_poll(void) {
    for (int i = 0; i < 4; i++) inb_(ATA_STATUS);          /* 400ns delay */
    int spin = 0;
    while (inb_(ATA_STATUS) & ST_BSY) if (++spin > 10000000) return -1;
    uint8_t s = inb_(ATA_STATUS);
    if (s & ST_ERR) return -1;
    while (!(inb_(ATA_STATUS) & ST_DRQ)) if (++spin > 10000000) return -1;
    return 0;
}

/* read `count` sectors starting at LBA into buf (from primary slave, -hdb) */
int ata_read(uint32_t lba, uint32_t count, void *buf) {
    uint16_t *b = (uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++, lba++) {
        outb_(ATA_DRIVE, 0xF0 | ((lba >> 24) & 0x0F));    /* slave, LBA mode */
        outb_(ATA_SECCNT, 1);
        outb_(ATA_LBA0, lba & 0xFF);
        outb_(ATA_LBA1, (lba >> 8) & 0xFF);
        outb_(ATA_LBA2, (lba >> 16) & 0xFF);
        outb_(ATA_CMD, 0x20);                             /* READ SECTORS */
        if (ata_poll() < 0) return -1;
        for (int i = 0; i < 256; i++) *b++ = inw_(ATA_DATA);
    }
    return 0;
}

/* write `count` sectors starting at LBA from buf (primary slave, -hdb) */
int ata_write(uint32_t lba, uint32_t count, const void *buf) {
    const uint16_t *b = (const uint16_t *)buf;
    for (uint32_t s = 0; s < count; s++, lba++) {
        outb_(ATA_DRIVE, 0xF0 | ((lba >> 24) & 0x0F));
        outb_(ATA_SECCNT, 1);
        outb_(ATA_LBA0, lba & 0xFF);
        outb_(ATA_LBA1, (lba >> 8) & 0xFF);
        outb_(ATA_LBA2, (lba >> 16) & 0xFF);
        outb_(ATA_CMD, 0x30);                             /* WRITE SECTORS */
        if (ata_poll() < 0) return -1;
        for (int i = 0; i < 256; i++) outw_(ATA_DATA, *b++);
        outb_(ATA_CMD, 0xE7);                             /* CACHE FLUSH */
        for (int i = 0; i < 4; i++) inb_(ATA_STATUS);
        int spin = 0; while (inb_(ATA_STATUS) & ST_BSY) if (++spin > 10000000) break;
    }
    return 0;
}
