/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos FAT16 reader/writer — mounts a filesystem image from the second disk
 * (via kernel/ata.c).  Reads directories and files created with tools/mkfatfs.py
 * or mkfs.vfat -F16, and writes whole files back (fat_write) so edits persist to
 * the mounted image. */
#include "kernel.h"

static int      f_mounted = 0;
static uint8_t  f_spc, f_nfats;
static uint16_t f_reserved, f_fatsz, f_root_entries, f_total;
static uint32_t f_fat_start, f_root_start, f_root_sectors, f_data_start;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

int fat_mounted(void) { return f_mounted; }

int fat_mount(void) {
    static uint8_t bs[512];
    if (ata_read(0, 1, bs) < 0) return -1;
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
    if (ata_read(f_fat_start + off / 512, 1, sb) < 0) return 0xFFFF;
    return sb[off % 512] | (sb[off % 512 + 1] << 8);
}

/* fetch the raw 32-byte directory slot #i of a directory (root if cluster==0) */
static int dir_raw(uint32_t dir_cluster, uint32_t i, uint8_t *e) {
    static uint8_t sb[512];
    const uint32_t eps = 512 / 32;                          /* 16 entries/sector */
    if (dir_cluster == 0) {
        if (i >= f_root_entries) return -1;
        if (ata_read(f_root_start + i / eps, 1, sb) < 0) return -1;
    } else {
        uint32_t epc = (uint32_t)f_spc * eps;
        uint16_t c = dir_cluster;
        for (uint32_t k = 0; k < i / epc; k++) { c = fat_next(c); if (c < 2 || c >= 0xFFF8) return -1; }
        uint32_t within = i % epc;
        if (ata_read(f_data_start + (uint32_t)(c - 2) * f_spc + within / eps, 1, sb) < 0) return -1;
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

/* list the which-th usable entry of a directory. attr/cluster/size are out. */
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

/* find an entry by (case-insensitive) name in a directory */
int fat_lookup(uint32_t dir_cluster, const char *want, uint8_t *attr, uint16_t *cluster, uint32_t *size) {
    char nm[16];
    for (int i = 0; fat_list(dir_cluster, i, nm, attr, cluster, size) == 0; i++) {
        int j = 0; while (nm[j] && lc(want[j]) == nm[j]) j++;
        if (!nm[j] && !want[j]) return 0;
    }
    return -1;
}

/* read a file (given its first cluster + size) into buf, up to maxlen bytes */
int fat_read(uint16_t cluster, uint32_t size, void *buf, uint32_t maxlen) {
    uint8_t *out = buf; uint32_t got = 0;
    static uint8_t sb[512];
    while (cluster >= 2 && cluster < 0xFFF8 && got < size && got < maxlen) {
        uint32_t sec = f_data_start + (uint32_t)(cluster - 2) * f_spc;
        for (uint32_t s = 0; s < f_spc && got < size && got < maxlen; s++) {
            if (ata_read(sec + s, 1, sb) < 0) return (int)got;
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

/* highest cluster number + 1 (i.e. one past the last usable data cluster) */
static uint32_t cluster_count(void) {
    if (f_total < f_data_start) return 2;
    return (f_total - f_data_start) / f_spc + 2;
}

/* write one FAT entry into every FAT copy on disk */
static void fat_set(uint16_t c, uint16_t val) {
    uint32_t off = (uint32_t)c * 2;
    static uint8_t sb[512];
    for (uint8_t fi = 0; fi < f_nfats; fi++) {
        uint32_t sec = f_fat_start + (uint32_t)fi * f_fatsz + off / 512;
        if (ata_read(sec, 1, sb) < 0) return;
        sb[off % 512]     = (uint8_t)(val & 0xFF);
        sb[off % 512 + 1] = (uint8_t)((val >> 8) & 0xFF);
        ata_write(sec, 1, sb);
    }
}

/* find a free cluster, mark it end-of-chain, return its number (0 = none) */
static uint16_t fat_alloc(void) {
    uint32_t n = cluster_count();
    for (uint16_t c = 2; c < n; c++)
        if (fat_next(c) == 0x0000) { fat_set(c, 0xFFFF); return c; }
    return 0;
}

static void fat_free_chain(uint16_t c) {
    while (c >= 2 && c < 0xFFF8) { uint16_t nx = fat_next(c); fat_set(c, 0); c = nx; }
}

/* build an 8.3 raw name (11 bytes, space padded, upper-cased) from a component */
static void to83(const char *name, uint8_t raw[11]) {
    for (int i = 0; i < 11; i++) raw[i] = ' ';
    int i = 0, k = 0;
    while (name[i] && name[i] != '.' && k < 8) raw[k++] = (uint8_t)up(name[i++]);
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; int e = 8; while (name[i] && e < 11) raw[e++] = (uint8_t)up(name[i++]); }
}

/* physical (LBA, byte offset) of directory slot #i (dir_cluster 0 = root dir) */
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
        if (!*t) { *pdir = cur; kstrcpy(name, comp); return 0; }
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_lookup(cur, comp, &at, &cl, &sz) < 0) return -1;   /* missing dir */
        if (!(at & 0x10)) return -1;                               /* not a dir   */
        cur = cl; sub = t;
    }
}

/* create/overwrite file `sub` (relative to the mount) with `len` bytes.
 * Returns bytes written, or -1.  Allocates a fresh cluster chain, updates every
 * FAT copy and the directory entry, so the change persists to the disk image. */
int fat_write(const char *sub, const void *buf, uint32_t len) {
    if (!f_mounted) return -1;
    uint16_t dir; char fname[16];
    if (fat_parent(sub, &dir, fname) < 0) return -1;
    uint8_t raw[11]; to83(fname, raw);

    /* find an existing entry, remembering the first reusable free slot */
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

    /* allocate a fresh chain and write the file data cluster by cluster */
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
            ata_write(f_data_start + (uint32_t)(c - 2) * f_spc + s, 1, sb);
        }
        prev = c;
    }
    if (prev) fat_set(prev, 0xFFFF);                               /* terminate chain */

    /* write the directory entry back to disk */
    memset(e, 0, 32);
    for (int j = 0; j < 11; j++) e[j] = raw[j];
    e[11] = 0x20;                                                  /* archive attr */
    e[26] = (uint8_t)(first & 0xFF); e[27] = (uint8_t)((first >> 8) & 0xFF);
    e[28] = (uint8_t)(len & 0xFF);        e[29] = (uint8_t)((len >> 8) & 0xFF);
    e[30] = (uint8_t)((len >> 16) & 0xFF); e[31] = (uint8_t)((len >> 24) & 0xFF);
    uint32_t lba, off;
    if (dirent_loc(dir, slot, &lba, &off) < 0) return -1;
    if (ata_read(lba, 1, sb) < 0) return -1;
    for (int j = 0; j < 32; j++) sb[off + j] = e[j];
    if (ata_write(lba, 1, sb) < 0) return -1;
    return (int)len;
}
