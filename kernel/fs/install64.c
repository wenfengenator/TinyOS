/* tinyos x64 — clean-install disk imager.
 *
 * Writes a fresh MBR + FAT32 EFI System Partition (type 0xEF) containing TinyOS's
 * own BOOTX64.EFI to the internal SATA disk via AHCI, so the machine boots TinyOS
 * from the SSD through the UEFI removable-media path \EFI\BOOT\BOOTX64.EFI.  The
 * T440s already UEFI-boots an MBR disk (Ubuntu), so MBR+ESP is known to work on
 * this firmware and needs no GPT CRC / NVRAM boot entry.
 *
 * This is a CLEAN install: it erases the partition table.  Invoked only from the
 * `install yes` console command after an explicit warning.  The whole ESP is
 * built in an identity-mapped heap buffer (DMA-safe) and written, then the MBR
 * last so a half-written disk is never left "bootable". */
#include "boot64.h"

#define SEC 512

extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* One FAT 8.3 directory entry; `name` is exactly 11 bytes (space-padded). */
static void dirent(uint8_t *e, const char *name, uint8_t attr, uint32_t first, uint32_t size) {
    for (int i = 0; i < 11; i++) e[i] = (uint8_t)name[i];
    e[11] = attr;
    w16(e + 20, (uint16_t)(first >> 16));
    w16(e + 26, (uint16_t)(first & 0xFFFF));
    w32(e + 28, size);
}

/* Build a FAT32 volume (esp_sectors sectors) into buf: /EFI/BOOT/BOOTX64.EFI. */
static int build_esp(uint8_t *buf, uint32_t esp_sectors, uint32_t part_lba,
                     const uint8_t *efi, uint32_t efi_len) {
    const uint32_t reserved = 32, nfats = 2, spc = 1;
    uint32_t tmp2  = (256 * spc + nfats) / 2;                      /* MS FAT32 sizing */
    uint32_t fatsz = (esp_sectors - reserved + tmp2 - 1) / tmp2;
    uint32_t data_start = reserved + nfats * fatsz;                /* sector of cluster 2 */
    uint32_t nclusters  = (esp_sectors - data_start) / spc;
    uint32_t efi_clusters = (efi_len + SEC - 1) / SEC;
    if (3 + efi_clusters + 2 > nclusters) return -1;               /* won't fit */

    uint8_t *bs = buf;                                            /* boot sector / BPB */
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "MSWIN4.1", 8);
    w16(bs + 11, 512); bs[13] = (uint8_t)spc; w16(bs + 14, (uint16_t)reserved); bs[16] = (uint8_t)nfats;
    bs[21] = 0xF8;
    w16(bs + 24, 63); w16(bs + 26, 255); w32(bs + 28, part_lba);
    w32(bs + 32, esp_sectors); w32(bs + 36, fatsz);
    w32(bs + 44, 2); w16(bs + 48, 1); w16(bs + 50, 6);
    bs[64] = 0x80; bs[66] = 0x29; w32(bs + 67, 0x54494E59);
    memcpy(bs + 71, "TINYOS ESP ", 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;

    uint8_t *fsi = buf + 1 * SEC;                                 /* FSInfo + backups */
    w32(fsi + 0, 0x41615252); w32(fsi + 484, 0x61417272);
    w32(fsi + 488, 0xFFFFFFFF); w32(fsi + 492, 0xFFFFFFFF);
    fsi[510] = 0x55; fsi[511] = 0xAA;
    memcpy(buf + 6 * SEC, bs, SEC);
    memcpy(buf + 7 * SEC, fsi, SEC);

    uint8_t *fat = buf + (uint64_t)reserved * SEC;                /* FAT #0 */
    w32(fat + 0 * 4, 0x0FFFFFF8); w32(fat + 1 * 4, 0x0FFFFFFF);
    w32(fat + 2 * 4, 0x0FFFFFFF); w32(fat + 3 * 4, 0x0FFFFFFF); w32(fat + 4 * 4, 0x0FFFFFFF);
    for (uint32_t i = 0; i < efi_clusters; i++) {
        uint32_t c = 5 + i;
        w32(fat + (uint64_t)c * 4, (i == efi_clusters - 1) ? 0x0FFFFFFF : (c + 1));
    }
    memcpy(buf + (uint64_t)(reserved + fatsz) * SEC, fat, (uint64_t)fatsz * SEC);   /* mirror FAT #1 */

    uint8_t *root    = buf + (uint64_t)data_start * SEC;          /* cluster 2 */
    uint8_t *efidir  = root + (uint64_t)spc * SEC;               /* cluster 3 */
    uint8_t *bootdir = efidir + (uint64_t)spc * SEC;            /* cluster 4 */
    uint8_t *filed   = bootdir + (uint64_t)spc * SEC;          /* cluster 5 */
    dirent(root + 0,    "EFI        ", 0x10, 3, 0);
    dirent(efidir + 0,  ".          ", 0x10, 3, 0);
    dirent(efidir + 32, "..         ", 0x10, 0, 0);
    dirent(efidir + 64, "BOOT       ", 0x10, 4, 0);
    dirent(bootdir + 0, ".          ", 0x10, 4, 0);
    dirent(bootdir + 32,"..         ", 0x10, 3, 0);
    dirent(bootdir + 64,"BOOTX64 EFI", 0x20, 5, efi_len);
    memcpy(filed, efi, efi_len);
    return 0;
}

/* Format `part_sectors` at `part_lba` as an empty FAT32 data volume (32 KiB
 * clusters to keep the FAT small).  Only metadata is written; the data region is
 * left unreferenced.  zbuf is a zeroed 1 MiB (2048-sector) scratch buffer. */
static int format_data_fat32(uint32_t part_lba, uint32_t part_sectors, uint8_t *zbuf) {
    const uint32_t reserved = 32, nfats = 2, spc = 64;
    uint32_t tmp2  = (256 * spc + nfats) / 2;
    uint32_t fatsz = (part_sectors - reserved + tmp2 - 1) / tmp2;

    uint8_t sec[SEC]; memset(sec, 0, SEC);
    sec[0]=0xEB; sec[1]=0x58; sec[2]=0x90; memcpy(sec+3,"MSWIN4.1",8);
    w16(sec+11,512); sec[13]=(uint8_t)spc; w16(sec+14,(uint16_t)reserved); sec[16]=(uint8_t)nfats;
    sec[21]=0xF8; w16(sec+24,63); w16(sec+26,255); w32(sec+28,part_lba);
    w32(sec+32,part_sectors); w32(sec+36,fatsz);
    w32(sec+44,2); w16(sec+48,1); w16(sec+50,6);
    sec[64]=0x80; sec[66]=0x29; w32(sec+67,0x54494E44);
    memcpy(sec+71,"TINYOS DATA",11); memcpy(sec+82,"FAT32   ",8);
    sec[510]=0x55; sec[511]=0xAA;
    if (ahci_write(part_lba, 1, sec) != 0 || ahci_write(part_lba + 6, 1, sec) != 0) return -1;
    uint8_t fsi[SEC]; memset(fsi, 0, SEC);
    w32(fsi,0x41615252); w32(fsi+484,0x61417272); w32(fsi+488,0xFFFFFFFF); w32(fsi+492,0xFFFFFFFF);
    fsi[510]=0x55; fsi[511]=0xAA;
    if (ahci_write(part_lba + 1, 1, fsi) != 0) return -1;

    uint32_t zsec = 2 * fatsz + spc;                              /* both FATs + root cluster */
    for (uint32_t s = 0; s < zsec; ) {
        uint32_t n = zsec - s > 2048 ? 2048 : zsec - s;
        if (ahci_write(part_lba + reserved + s, n, zbuf) != 0) return -1;
        s += n;
    }
    memset(sec, 0, SEC); w32(sec,0x0FFFFFF8); w32(sec+4,0x0FFFFFFF); w32(sec+8,0x0FFFFFFF);
    if (ahci_write(part_lba + reserved, 1, sec) != 0) return -1;
    if (ahci_write(part_lba + reserved + fatsz, 1, sec) != 0) return -1;
    return 0;
}

/* Erase the disk and install TinyOS.  Returns 0 on success, negative on error. */
int esp_install(const uint8_t *efi, uint32_t efi_len) {
    if (!ahci_present() && !ahci_init()) return -1;
    if (!efi || !efi_len) return -2;
    uint64_t disk = ahci_sectors();
    uint32_t part_lba = 2048;                                     /* 1 MiB aligned */
    uint64_t need = (uint64_t)efi_len + (16u << 20);
    uint32_t esp_sectors = (uint32_t)((((need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1))) / SEC);
    if (esp_sectors < 98304) esp_sectors = 98304;                 /* >= 48 MiB */
    if ((uint64_t)part_lba + esp_sectors > disk) return -3;

    uint64_t pages = ((uint64_t)esp_sectors * SEC + 4095) / 4096;
    uint8_t *buf = (uint8_t *)page_alloc(pages);
    if (!buf) return -4;
    memset(buf, 0, (uint64_t)esp_sectors * SEC);
    if (build_esp(buf, esp_sectors, part_lba, efi, efi_len) != 0) { page_free(buf, pages); return -5; }

    if (ahci_write(part_lba, esp_sectors, buf) != 0) { page_free(buf, pages); return -6; }
    page_free(buf, pages);

    uint8_t mbr[SEC]; memset(mbr, 0, SEC);
    uint8_t *e = mbr + 0x1BE;                                      /* partition 0: the ESP */
    e[0] = 0x80; e[1] = 0x01; e[2] = 0x01; e[3] = 0x00;
    e[4] = 0xEF; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    w32(e + 8, part_lba); w32(e + 12, esp_sectors);

    /* partition 1: a FAT32 data volume over the rest of the disk (persistent /mnt) */
    uint32_t data_lba = (part_lba + esp_sectors + 2047u) & ~2047u;
    if ((uint64_t)data_lba + 4096 < disk) {
        uint64_t avail = disk - data_lba;
        uint32_t data_sectors = avail > 0xFFFFFFF0ull ? 0xFFFFFFF0u : (uint32_t)avail;
        uint8_t *zbuf = (uint8_t *)page_alloc(256);               /* 1 MiB zeroed scratch */
        if (zbuf) {
            memset(zbuf, 0, 256u * 4096u);
            if (format_data_fat32(data_lba, data_sectors, zbuf) == 0) {
                uint8_t *e1 = mbr + 0x1BE + 16;
                e1[0] = 0x00; e1[1] = 0xFE; e1[2] = 0xFF; e1[3] = 0xFF;
                e1[4] = 0x0C; e1[5] = 0xFE; e1[6] = 0xFF; e1[7] = 0xFF;   /* FAT32 LBA */
                w32(e1 + 8, data_lba); w32(e1 + 12, data_sectors);
            }
            page_free(zbuf, 256);
        }
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return ahci_write(0, 1, mbr) ? -7 : 0;                         /* MBR last */
}
