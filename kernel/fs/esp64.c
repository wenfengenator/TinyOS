/* tinyos x64 — FAT32 reader for the internal-disk EFI System Partition, over AHCI.
 *
 * Toward installing TinyOS to the internal disk: locate the ESP in the GPT, parse
 * its FAT32 BPB, and read directories/files.  Independent of the FAT16 RAM-disk
 * driver (kernel/fs/fat64.c) — different filesystem, different backend (ahci64).
 *
 * Stage 2 is read-only: enumerate the ESP so we can verify we read the real disk
 * without touching it.  Writes (installing BOOTX64.EFI) come next and are gated
 * behind this working.  All chain walks are bounded so a corrupt FAT can't hang. */
#include "boot64.h"

#define SEC 512
static int      g_mounted;
static uint64_t g_part_lba;          /* ESP first LBA on the disk        */
static uint64_t g_fat_lba;           /* first FAT sector                 */
static uint64_t g_data_lba;          /* first data sector (cluster 2)    */
static uint32_t g_spc;               /* sectors per cluster              */
static uint32_t g_root_clus;         /* root directory cluster           */
static uint32_t g_fatsz;             /* sectors per FAT                  */

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* EFI System Partition type GUID, little-endian on-disk byte order. */
static const uint8_t ESP_GUID[16] = {
    0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};

/* Validate a FAT32 volume at partition LBA `esp`; on success set the globals. */
static int try_fat32(uint64_t esp, uint8_t *sec) {
    if (!esp || ahci_read(esp, 1, sec) != 0) return 0;
    uint32_t bps   = rd16(sec + 11);
    uint32_t spc   = sec[13];
    uint32_t rsvd  = rd16(sec + 14);
    uint32_t nfats = sec[16];
    uint32_t fatsz = rd32(sec + 36);                               /* FAT32 32-bit FAT size */
    uint32_t root  = rd32(sec + 44);
    if (bps != SEC || !spc || (spc & (spc - 1)) || !fatsz || !nfats || root < 2) return 0;
    g_part_lba  = esp;
    g_fat_lba   = esp + rsvd;
    g_data_lba  = esp + rsvd + (uint64_t)nfats * fatsz;
    g_spc       = spc;
    g_root_clus = root;
    g_fatsz     = fatsz;
    g_mounted   = 1;
    return 1;
}

int esp_mount(void) {
    if (g_mounted) return 1;
    if (!ahci_present() && !ahci_init()) return 0;
    static uint8_t sec[SEC] __attribute__((aligned(16)));
    static uint8_t mbr[SEC] __attribute__((aligned(16)));
    if (ahci_read(0, 1, mbr) != 0) return 0;

    /* GPT: protective MBR + "EFI PART" at LBA1, scan for the ESP type GUID. */
    if (ahci_read(1, 1, sec) == 0 && sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' '&&
        sec[4]=='P'&&sec[5]=='A'&&sec[6]=='R'&&sec[7]=='T') {
        uint64_t parr = rd64(sec + 72);
        uint32_t nparts = rd32(sec + 80), psize = rd32(sec + 84);
        if (!psize) psize = 128;
        uint32_t per = SEC / psize; if (!per) per = 1;
        static uint8_t pb[SEC] __attribute__((aligned(16)));
        for (uint32_t i = 0; i < nparts && i < 512; i++) {
            if (i % per == 0 && ahci_read(parr + i / per, 1, pb) != 0) break;
            const uint8_t *e = pb + (i % per) * psize;
            int match = 1;
            for (int k = 0; k < 16; k++) if (e[k] != ESP_GUID[k]) { match = 0; break; }
            if (match && try_fat32(rd64(e + 32), sec)) return 1;
        }
    }

    /* MBR: the T440s uses an MBR disk with a type-0xEF EFI System Partition
     * (Ubuntu's /boot/efi).  Prefer 0xEF, then any FAT partition type. */
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        static const uint8_t pref[] = { 0xEF, 0x0C, 0x0B, 0x06, 0x0E, 0x01, 0x04 };
        for (unsigned pass = 0; pass < sizeof(pref); pass++)
            for (int i = 0; i < 4; i++) {
                const uint8_t *e = mbr + 0x1BE + i * 16;
                if (e[4] == pref[pass] && try_fat32(rd32(e + 8), sec)) return 1;
            }
    }
    return 0;
}

uint32_t esp_root_cluster(void) { return g_root_clus; }

/* Next cluster in a FAT32 chain (0 on end-of-chain / error). */
static uint32_t fat_next(uint32_t clus) {
    static uint8_t fs[SEC] __attribute__((aligned(16)));
    uint64_t off = (uint64_t)clus * 4;
    if (ahci_read(g_fat_lba + off / SEC, 1, fs) != 0) return 0;
    uint32_t v = rd32(fs + (off % SEC)) & 0x0FFFFFFFu;
    return (v >= 0x0FFFFFF8u || v < 2) ? 0 : v;
}

/* 8.3 short entry -> "NAME.EXT"; returns 0 if it's a volume label / LFN / empty. */
static int shortname(const uint8_t *e, char *out) {
    if (e[0] == 0x00 || e[0] == 0xE5) return 0;
    if ((e[11] & 0x0F) == 0x0F) return 0;                          /* LFN slot  */
    if (e[11] & 0x08) return 0;                                    /* volume id */
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = e[i];
    if (e[8] != ' ') { out[n++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) out[n++] = e[i]; }
    out[n] = 0;
    return n > 0;
}

/* Walk a directory (by first cluster) and hand each entry to cb.  Bounded. */
void esp_list(uint32_t dir_cluster, esp_dirent_cb cb, void *ctx) {
    if (!g_mounted || dir_cluster < 2) return;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t clus = dir_cluster, guard = 0;
    while (clus >= 2 && guard++ < 100000) {
        uint64_t base = g_data_lba + (uint64_t)(clus - 2) * g_spc;
        for (uint32_t s = 0; s < g_spc; s++) {
            if (ahci_read(base + s, 1, sb) != 0) return;
            for (int i = 0; i < SEC; i += 32) {
                const uint8_t *e = sb + i;
                if (e[0] == 0x00) return;                          /* end of directory */
                char name[16];
                if (!shortname(e, name)) continue;
                int isdir = (e[11] & 0x10) ? 1 : 0;
                uint32_t size = rd32(e + 28);
                uint32_t first = (rd16(e + 20) << 16) | rd16(e + 26);   /* hi<<16 | lo */
                cb(name, size, isdir, first, ctx);
            }
        }
        clus = fat_next(clus);
    }
}

/* Resolve a backslash/forward-slash path (e.g. "/EFI/BOOT") to a directory or
 * file cluster.  Returns 1 on success. */
struct find_ctx { const char *want; uint32_t clus, size; int isdir, hit; };
static void find_cb(const char *name, uint32_t size, int isdir, uint32_t first, void *ctx) {
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (f->hit) return;
    const char *a = name, *b = f->want; int i = 0;
    for (;; i++) {                                                 /* case-insensitive compare */
        char ca = a[i], cb2 = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb2 >= 'a' && cb2 <= 'z') cb2 -= 32;
        if (ca != cb2) return;
        if (!ca) break;
    }
    f->clus = first; f->size = size; f->isdir = isdir; f->hit = 1;
}

int esp_find(const char *path, uint32_t *clus, uint32_t *size, int *isdir) {
    if (!g_mounted) return 0;
    uint32_t cur = g_root_clus, sz = 0; int dir = 1;
    const char *p = path;
    while (*p == '/' || *p == '\\') p++;
    while (*p) {
        char comp[16]; int n = 0;
        while (*p && *p != '/' && *p != '\\' && n < 15) comp[n++] = *p++;
        comp[n] = 0;
        while (*p == '/' || *p == '\\') p++;
        if (!n) break;
        struct find_ctx f = { comp, 0, 0, 0, 0 };
        esp_list(cur, find_cb, &f);
        if (!f.hit) return 0;
        cur = f.clus; sz = f.size; dir = f.isdir;
    }
    if (clus) *clus = cur;
    if (size) *size = sz;
    if (isdir) *isdir = dir;
    return 1;
}
