/* tinyos ramdisk: a block-backed image embedded in the kernel (see the
 * generated build/ramdisk.s / .incbin).  It is a tiny read-only archive of
 * program images used by exec().  On real hardware the same bytes could come
 * from a disk via block_read(); here they are RAM-resident. */
#include "kernel.h"

extern uint8_t __ramdisk_start[];

struct rd_ent { char name[16]; uint32_t off, size; } __attribute__((packed));

#define BLOCK_SIZE 512

static int rd_ok(void) {
    return __ramdisk_start[0] == 'T' && __ramdisk_start[1] == 'R' && __ramdisk_start[2] == 'D';
}
uint32_t rd_count(void) { return rd_ok() ? *(uint32_t *)(__ramdisk_start + 4) : 0; }
static struct rd_ent *rd_ents(void) { return (struct rd_ent *)(__ramdisk_start + 8); }

const char *rd_name(uint32_t i) { return i < rd_count() ? rd_ents()[i].name : 0; }
uint32_t    rd_isize(uint32_t i) { return i < rd_count() ? rd_ents()[i].size : 0; }

/* look a program up by name -> pointer into the ramdisk + size */
int rd_get(const char *name, uint8_t **ptr, uint32_t *size) {
    for (uint32_t i = 0; i < rd_count(); i++)
        if (kstrcmp(rd_ents()[i].name, name) == 0) {
            *ptr  = __ramdisk_start + rd_ents()[i].off;
            *size = rd_ents()[i].size;
            return 0;
        }
    return -1;
}

/* block-device view of the ramdisk (documents the "block-backed" nature) */
int block_read(uint32_t block, void *buf, uint32_t nblocks) {
    memcpy(buf, __ramdisk_start + block * BLOCK_SIZE, nblocks * BLOCK_SIZE);
    return 0;
}
