/* tinyos x64 FAT16 filesystem — a port of kernel/fat.c backed by a RAM-resident
 * disk image embedded in the kernel (build/fatfs.h, built from disk/ by
 * tools/mkfatfs.py).  The image is copied into writable pages at boot so whole-
 * file writes (fat_write) persist for the session.  Ring-3 programs reach it via
 * the readdir/readfile/writefile syscalls (see kernel/arch64.c). */
#include "boot64.h"
#include "fat64.h"
#include "fatfs.h"                 /* static const unsigned char fatfs_img[] */

/* ------------------------------ block device ----------------------------
 * A persistent virtio-blk disk when present (QEMU dev), else a RAM copy of the
 * embedded image (Hyper-V etc. — ephemeral until the VMBus StorVSC driver). */
#define BLK 512
static uint8_t  *g_disk;
static uint32_t  g_disk_sectors;
static int       g_vblk;
static int       g_stor;

static int  blk_read(uint32_t lba, void *buf);
static int  blk_write(uint32_t lba, const void *buf);
static void blk_seed_if_blank(void);

void blk_init(void) {
    if (vblk_init())    { g_vblk = 1; return; }         /* virtio-blk (QEMU dev)     */
    if (storvsc_init()) { g_stor = 1; blk_seed_if_blank(); return; }  /* Hyper-V SCSI */
    uint32_t len   = (uint32_t)sizeof(fatfs_img);       /* RAM fallback              */
    uint32_t pages = (len + 4095) / 4096;
    g_disk = (uint8_t *)page_alloc(pages);
    if (!g_disk) { g_disk_sectors = 0; return; }
    memcpy(g_disk, fatfs_img, len);
    g_disk_sectors = len / BLK;
}
static int blk_read(uint32_t lba, void *buf) {
    if (g_vblk) return vblk_read(lba, buf);
    if (g_stor) return storvsc_read(lba, buf);
    if (!g_disk || lba >= g_disk_sectors) return -1;
    memcpy(buf, g_disk + (uint64_t)lba * BLK, BLK);
    return 0;
}
static int blk_write(uint32_t lba, const void *buf) {
    if (g_vblk) return vblk_write(lba, buf);
    if (g_stor) return storvsc_write(lba, buf);
    if (!g_disk || lba >= g_disk_sectors) return -1;
    memcpy(g_disk + (uint64_t)lba * BLK, buf, BLK);
    return 0;
}

/* Lay the embedded image onto a persistent disk that has no FAT16 boot sector
 * yet (a fresh Hyper-V VHDX), so there is a filesystem to mount.  Writing only
 * when blank keeps previously persisted data intact across reboots. */
static void blk_seed_if_blank(void) {
    uint8_t bs[BLK];
    if (blk_read(0, bs) == 0 &&
        bs[510] == 0x55 && bs[511] == 0xAA && bs[11] == 0x00 && bs[12] == 0x02)
        return;                                             /* already formatted */
    uint32_t len  = (uint32_t)sizeof(fatfs_img);
    uint32_t secs = (len + BLK - 1) / BLK;
    for (uint32_t s = 0; s < secs; s++) {
        uint8_t tmp[BLK]; memset(tmp, 0, BLK);
        uint32_t n = len - s * BLK; if (n > BLK) n = BLK;
        memcpy(tmp, fatfs_img + (uint64_t)s * BLK, n);
        if (blk_write(s, tmp) < 0) return;
    }
}

/* --------------------------------- FAT16 --------------------------------- */
static int      f_mounted = 0;
static uint8_t  f_spc, f_nfats;
static uint16_t f_reserved, f_fatsz, f_root_entries, f_total;
static uint32_t f_fat_start, f_root_start, f_root_sectors, f_data_start;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int fat_mounted(void) { return f_mounted; }

int fat_mount(void) {
    static uint8_t bs[512];
    if (blk_read(0, bs) < 0) return -1;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return -1;
    if (!(bs[11] == 0x00 && bs[12] == 0x02)) return -1;      /* 512 bytes/sector */
    f_spc          = bs[13];
    f_reserved     = bs[14] | (bs[15] << 8);
    f_nfats        = bs[16];
    f_root_entries = bs[17] | (bs[18] << 8);
    f_total        = bs[19] | (bs[20] << 8);
    f_fatsz        = bs[22] | (bs[23] << 8);
    f_fat_start    = f_reserved;
    f_root_start   = f_reserved + (uint32_t)f_nfats * f_fatsz;
    f_root_sectors = ((uint32_t)f_root_entries * 32 + 511) / 512;
    f_data_start   = f_root_start + f_root_sectors;
    f_mounted = 1;
    return 0;
}

static uint16_t fat_next(uint16_t c) {
    uint32_t off = (uint32_t)c * 2;
    static uint8_t sb[512];
    if (blk_read(f_fat_start + off / 512, sb) < 0) return 0xFFFF;
    return sb[off % 512] | (sb[off % 512 + 1] << 8);
}

/* fetch the raw 32-byte directory slot #i of a directory (root if cluster==0) */
static int dir_raw(uint32_t dir_cluster, uint32_t i, uint8_t *e) {
    static uint8_t sb[512];
    const uint32_t eps = 512 / 32;                          /* 16 entries/sector */
    if (dir_cluster == 0) {
        if (i >= f_root_entries) return -1;
        if (blk_read(f_root_start + i / eps, sb) < 0) return -1;
    } else {
        uint32_t epc = (uint32_t)f_spc * eps;
        uint16_t c = (uint16_t)dir_cluster;
        for (uint32_t k = 0; k < i / epc; k++) { c = fat_next(c); if (c < 2 || c >= 0xFFF8) return -1; }
        uint32_t within = i % epc;
        if (blk_read(f_data_start + (uint32_t)(c - 2) * f_spc + within / eps, sb) < 0) return -1;
        i = within;
    }
    memcpy(e, sb + (i % eps) * 32, 32);
    return 0;
}

static void name83(const uint8_t *e, char *out) {
    int k = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[k++] = lc((char)e[i]);
    if (e[8] != ' ') { out[k++] = '.'; for (int i = 8; i < 11 && e[i] != ' '; i++) out[k++] = lc((char)e[i]); }
    out[k] = 0;
}

int fat_list(uint32_t dir_cluster, int which, char *name, uint8_t *attr, uint16_t *cluster, uint32_t *size) {
    uint8_t e[32]; int count = 0;
    for (uint32_t i = 0; dir_raw(dir_cluster, i, e) == 0; i++) {
        if (e[0] == 0x00) break;                            /* end of directory */
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;  /* deleted/LFN/volume */
        if (count++ != which) continue;
        if (name) name83(e, name);
        if (attr) *attr = e[11];
        if (cluster) *cluster = e[26] | (e[27] << 8);
        if (size) *size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
        return 0;
    }
    return -1;
}

int fat_lookup(uint32_t dir_cluster, const char *want, uint8_t *attr, uint16_t *cluster, uint32_t *size) {
    char nm[16];
    for (int i = 0; fat_list(dir_cluster, i, nm, attr, cluster, size) == 0; i++) {
        int j = 0; while (nm[j] && lc(want[j]) == nm[j]) j++;
        if (!nm[j] && !want[j]) return 0;
    }
    return -1;
}

/* resolve a '/'-separated path from the root; "" or "/" => the root directory */
int fat_resolve(const char *path, uint8_t *attr, uint16_t *cluster, uint32_t *size) {
    uint16_t cur = 0;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) { if (attr) *attr = 0x10; if (cluster) *cluster = 0; if (size) *size = 0; return 0; }
    for (;;) {
        char comp[16]; int k = 0;
        while (*p && *p != '/' && k < 15) comp[k++] = *p++;
        comp[k] = 0;
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_lookup(cur, comp, &at, &cl, &sz) < 0) return -1;
        while (*p == '/') p++;
        if (!*p) { if (attr) *attr = at; if (cluster) *cluster = cl; if (size) *size = sz; return 0; }
        if (!(at & 0x10)) return -1;                        /* intermediate not a dir */
        cur = cl;
    }
}

int fat_read(uint16_t cluster, uint32_t size, void *buf, uint32_t maxlen) {
    uint8_t *out = buf; uint32_t got = 0;
    static uint8_t sb[512];
    while (cluster >= 2 && cluster < 0xFFF8 && got < size && got < maxlen) {
        uint32_t sec = f_data_start + (uint32_t)(cluster - 2) * f_spc;
        for (uint32_t s = 0; s < f_spc && got < size && got < maxlen; s++) {
            if (blk_read(sec + s, sb) < 0) return (int)got;
            uint32_t n = 512;
            if (got + n > size) n = size - got;
            if (got + n > maxlen) n = maxlen - got;
            memcpy(out + got, sb, n); got += n;
        }
        cluster = fat_next(cluster);
    }
    return (int)got;
}

/* ============================ write-back support ========================== */

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static uint32_t cluster_count(void) {
    if (f_total < f_data_start) return 2;
    return (f_total - f_data_start) / f_spc + 2;
}

static void fat_set(uint16_t c, uint16_t val) {
    uint32_t off = (uint32_t)c * 2;
    static uint8_t sb[512];
    for (uint8_t fi = 0; fi < f_nfats; fi++) {
        uint32_t sec = f_fat_start + (uint32_t)fi * f_fatsz + off / 512;
        if (blk_read(sec, sb) < 0) return;
        sb[off % 512]     = (uint8_t)(val & 0xFF);
        sb[off % 512 + 1] = (uint8_t)((val >> 8) & 0xFF);
        blk_write(sec, sb);
    }
}

static uint16_t fat_alloc(void) {
    uint32_t n = cluster_count();
    for (uint16_t c = 2; c < n; c++)
        if (fat_next(c) == 0x0000) { fat_set(c, 0xFFFF); return c; }
    return 0;
}

static void fat_free_chain(uint16_t c) {
    while (c >= 2 && c < 0xFFF8) { uint16_t nx = fat_next(c); fat_set(c, 0); c = nx; }
}

static void to83(const char *name, uint8_t raw[11]) {
    for (int i = 0; i < 11; i++) raw[i] = ' ';
    int i = 0, k = 0;
    while (name[i] && name[i] != '.' && k < 8) raw[k++] = (uint8_t)up(name[i++]);
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; int e = 8; while (name[i] && e < 11) raw[e++] = (uint8_t)up(name[i++]); }
}

static int dirent_loc(uint32_t dir_cluster, uint32_t i, uint32_t *lba, uint32_t *off) {
    const uint32_t eps = 512 / 32;
    if (dir_cluster == 0) {
        if (i >= f_root_entries) return -1;
        *lba = f_root_start + i / eps;
    } else {
        uint32_t epc = (uint32_t)f_spc * eps;
        uint16_t c = (uint16_t)dir_cluster;
        for (uint32_t k = 0; k < i / epc; k++) { c = fat_next(c); if (c < 2 || c >= 0xFFF8) return -1; }
        uint32_t within = i % epc;
        *lba = f_data_start + (uint32_t)(c - 2) * f_spc + within / eps;
        i = within;
    }
    *off = (i % eps) * 32;
    return 0;
}

/* walk to the parent directory of the final path component of `sub` */
static int fat_parent(const char *sub, uint16_t *pdir, char *name) {
    uint16_t cur = 0;
    for (;;) {
        while (*sub == '/') sub++;
        if (!*sub) return -1;
        char comp[16]; int k = 0;
        while (*sub && *sub != '/' && k < 15) comp[k++] = *sub++;
        comp[k] = 0;
        const char *t = sub; while (*t == '/') t++;
        if (!*t) { *pdir = cur; int j = 0; while (comp[j]) { name[j] = comp[j]; j++; } name[j] = 0; return 0; }
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_lookup(cur, comp, &at, &cl, &sz) < 0) return -1;   /* missing dir */
        if (!(at & 0x10)) return -1;                               /* not a dir   */
        cur = cl; sub = t;
    }
}

int fat_write(const char *sub, const void *buf, uint32_t len) {
    if (!f_mounted) return -1;
    uint16_t dir; char fname[16];
    if (fat_parent(sub, &dir, fname) < 0) return -1;
    uint8_t raw[11]; to83(fname, raw);

    uint32_t slot = 0xFFFFFFFF, freeslot = 0xFFFFFFFF;
    uint16_t oldcluster = 0;
    uint8_t e[32];
    for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++) {
        if (e[0] == 0x00) { if (freeslot == 0xFFFFFFFF) freeslot = i; break; }
        if (e[0] == 0xE5) { if (freeslot == 0xFFFFFFFF) freeslot = i; continue; }
        if (e[11] == 0x0F || (e[11] & 0x08)) continue;              /* LFN/volume */
        int same = 1; for (int j = 0; j < 11; j++) if (e[j] != raw[j]) { same = 0; break; }
        if (same) { slot = i; oldcluster = e[26] | (e[27] << 8); break; }
    }
    if (slot == 0xFFFFFFFF) slot = freeslot;
    if (slot == 0xFFFFFFFF) return -1;                             /* directory full */
    if (oldcluster >= 2) fat_free_chain(oldcluster);               /* recycle old data */

    uint32_t bpc = (uint32_t)f_spc * 512;
    uint32_t nclus = (len + bpc - 1) / bpc;
    uint16_t first = 0, prev = 0;
    static uint8_t sb[512];
    for (uint32_t ci = 0; ci < nclus; ci++) {
        uint16_t c = fat_alloc();
        if (!c) { if (first) fat_free_chain(first); return -1; }   /* out of space */
        if (prev) fat_set(prev, c); else first = c;
        for (uint32_t s = 0; s < f_spc; s++) {
            memset(sb, 0, 512);
            uint32_t base = ci * bpc + s * 512;
            if (base < len) { uint32_t n = len - base; if (n > 512) n = 512; memcpy(sb, (const uint8_t *)buf + base, n); }
            blk_write(f_data_start + (uint32_t)(c - 2) * f_spc + s, sb);
        }
        prev = c;
    }
    if (prev) fat_set(prev, 0xFFFF);                               /* terminate chain */

    memset(e, 0, 32);
    for (int j = 0; j < 11; j++) e[j] = raw[j];
    e[11] = 0x20;                                                  /* archive attr */
    e[26] = (uint8_t)(first & 0xFF); e[27] = (uint8_t)((first >> 8) & 0xFF);
    e[28] = (uint8_t)(len & 0xFF);        e[29] = (uint8_t)((len >> 8) & 0xFF);
    e[30] = (uint8_t)((len >> 16) & 0xFF); e[31] = (uint8_t)((len >> 24) & 0xFF);
    uint32_t lba, off;
    if (dirent_loc(dir, slot, &lba, &off) < 0) return -1;
    if (blk_read(lba, sb) < 0) return -1;
    for (int j = 0; j < 32; j++) sb[off + j] = e[j];
    if (blk_write(lba, sb) < 0) return -1;
    return (int)len;
}

/* ============================ directory ops ============================== */

/* create the subdirectory `sub` (absolute path); 0 on success, -1 on failure */
int fat_mkdir(const char *sub) {
    if (!f_mounted) return -1;
    uint16_t dir; char name[16];
    if (fat_parent(sub, &dir, name) < 0) return -1;
    uint8_t at; uint16_t cl; uint32_t sz;
    if (fat_lookup(dir, name, &at, &cl, &sz) == 0) return -1;      /* already exists */
    uint8_t raw[11]; to83(name, raw);

    uint32_t slot = 0xFFFFFFFF;                                    /* free slot in parent */
    uint8_t e[32];
    for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++)
        if (e[0] == 0x00 || e[0] == 0xE5) { slot = i; break; }
    if (slot == 0xFFFFFFFF) return -1;                             /* directory full */

    uint16_t nc = fat_alloc();                                    /* cluster for the new dir */
    if (!nc) return -1;

    /* initialise the cluster with "." and ".." then zeros */
    static uint8_t sb[512];
    memset(sb, 0, 512);
    sb[0] = '.'; for (int j = 1; j < 11; j++) sb[j] = ' ';
    sb[11] = 0x10; sb[26] = (uint8_t)(nc & 0xFF); sb[27] = (uint8_t)((nc >> 8) & 0xFF);
    sb[32] = '.'; sb[33] = '.'; for (int j = 2; j < 11; j++) sb[32 + j] = ' ';
    sb[43] = 0x10; sb[58] = (uint8_t)(dir & 0xFF); sb[59] = (uint8_t)((dir >> 8) & 0xFF);
    blk_write(f_data_start + (uint32_t)(nc - 2) * f_spc, sb);
    memset(sb, 0, 512);
    for (uint32_t s = 1; s < f_spc; s++) blk_write(f_data_start + (uint32_t)(nc - 2) * f_spc + s, sb);

    memset(e, 0, 32);                                             /* dir entry in parent */
    for (int j = 0; j < 11; j++) e[j] = raw[j];
    e[11] = 0x10;
    e[26] = (uint8_t)(nc & 0xFF); e[27] = (uint8_t)((nc >> 8) & 0xFF);
    uint32_t lba, off;
    if (dirent_loc(dir, slot, &lba, &off) < 0) return -1;
    if (blk_read(lba, sb) < 0) return -1;
    for (int j = 0; j < 32; j++) sb[off + j] = e[j];
    if (blk_write(lba, sb) < 0) return -1;
    return 0;
}

/* delete the file `sub` (absolute path).  0 ok, -1 not found, -2 is a directory */
int fat_unlink(const char *sub) {
    if (!f_mounted) return -1;
    uint16_t dir; char name[16];
    if (fat_parent(sub, &dir, name) < 0) return -1;
    uint8_t raw[11]; to83(name, raw);
    uint8_t e[32];
    for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++) {
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
        int same = 1; for (int j = 0; j < 11; j++) if (e[j] != raw[j]) { same = 0; break; }
        if (!same) continue;
        if (e[11] & 0x10) return -2;                              /* directory: refuse */
        uint16_t cl = e[26] | (e[27] << 8);
        if (cl >= 2) fat_free_chain(cl);
        uint32_t lba, off;
        if (dirent_loc(dir, i, &lba, &off) < 0) return -1;
        static uint8_t sb[512];
        if (blk_read(lba, sb) < 0) return -1;
        sb[off] = 0xE5;                                           /* mark the entry deleted */
        if (blk_write(lba, sb) < 0) return -1;
        return 0;
    }
    return -1;                                                    /* not found */
}

/* ============================ current directory ========================= */

static char g_cwd[80] = "/";

static void s_copy(char *d, const char *s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0; }

/* resolve a user path against the cwd into `out` (absolute).  "" or "." => cwd. */
void fs_resolve(const char *path, char *out) {
    if (!path || !path[0] || (path[0] == '.' && path[1] == 0)) { s_copy(out, g_cwd); return; }
    if (path[0] == '/') { s_copy(out, path); return; }
    int p = 0;
    for (int i = 0; g_cwd[i]; i++) out[p++] = g_cwd[i];
    if (!(g_cwd[0] == '/' && g_cwd[1] == 0)) out[p++] = '/';      /* no double slash at root */
    for (int i = 0; path[i]; i++) out[p++] = path[i];
    out[p] = 0;
}

/* change the current directory; 0 on success, -1 if the target is not a directory */
int fs_chdir(const char *path) {
    if (path && path[0] == '.' && path[1] == '.' && path[2] == 0) { s_copy(g_cwd, "/"); return 0; }
    char full[80]; fs_resolve(path, full);
    if (full[0] == '/' && full[1] == 0) { s_copy(g_cwd, "/"); return 0; }
    uint8_t at; uint16_t cl; uint32_t sz;
    if (fat_resolve(full, &at, &cl, &sz) < 0 || !(at & 0x10)) return -1;
    s_copy(g_cwd, full);
    return 0;
}

void fs_getcwd(char *out) { s_copy(out, g_cwd); }
