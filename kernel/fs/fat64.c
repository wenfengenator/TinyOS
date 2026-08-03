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

/* Read-ahead cache for the device-backed disks (virtio/StorVSC): one device
 * transfer fills an 8-sector (4 KiB) window, so small sequential reads — TCC
 * scanning a 2.5 MB libc.a in ~1 KB chunks — don't each pay a synchronous
 * round trip.  Any write invalidates the covering line to stay coherent. */
#define RC_W     8
#define RC_LINES 16
static uint8_t  g_rc[RC_LINES][RC_W * BLK];
static uint32_t g_rc_base[RC_LINES];
static int      g_rc_valid[RC_LINES];

static int  blk_read(uint32_t lba, void *buf);
static int  blk_write(uint32_t lba, const void *buf);
static void blk_seed_if_blank(void);

static void rc_reset(void) { for (int i = 0; i < RC_LINES; i++) g_rc_valid[i] = 0; }
static void rc_invalidate(uint32_t lba) {
    uint32_t base = (lba / RC_W) * RC_W;
    int line = (int)((lba / RC_W) % RC_LINES);
    if (g_rc_valid[line] && g_rc_base[line] == base) g_rc_valid[line] = 0;
}
/* fill one window straight from the device (never via the cache — no recursion) */
static int rc_fill(uint32_t base, uint8_t *dst) {
    if (g_stor) return storvsc_read_many(base, dst, RC_W);
    for (uint32_t i = 0; i < RC_W; i++)
        if (vblk_read(base + i, dst + (uint64_t)i * BLK) < 0) return -1;
    return 0;
}

void blk_init(void) {
    rc_reset();
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
    if (g_stor || g_vblk) {
        uint32_t base = (lba / RC_W) * RC_W;
        int line = (int)((lba / RC_W) % RC_LINES);
        if (!(g_rc_valid[line] && g_rc_base[line] == base)) {
            if (rc_fill(base, g_rc[line]) < 0) {        /* e.g. window past disk end */
                if (g_stor) return storvsc_read(lba, buf);
                return vblk_read(lba, buf);
            }
            g_rc_base[line] = base; g_rc_valid[line] = 1;
        }
        memcpy(buf, g_rc[line] + (uint64_t)(lba - base) * BLK, BLK);
        return 0;
    }
    if (!g_disk || lba >= g_disk_sectors) return -1;
    memcpy(buf, g_disk + (uint64_t)lba * BLK, BLK);
    return 0;
}
static int blk_write(uint32_t lba, const void *buf) {
    if (g_stor || g_vblk) rc_invalidate(lba);
    if (g_vblk) return vblk_write(lba, buf);
    if (g_stor) return storvsc_write(lba, buf);
    if (!g_disk || lba >= g_disk_sectors) return -1;
    memcpy(g_disk + (uint64_t)lba * BLK, buf, BLK);
    return 0;
}
static int blk_write_many(uint32_t lba, const void *buf, uint32_t sectors) {
    if (g_stor) {
        for (uint32_t i = 0; i < sectors; i++) rc_invalidate(lba + i);
        return storvsc_write_many(lba, buf, sectors);
    }
    static uint8_t io[BLK];
    for (uint32_t i = 0; i < sectors; i++) {
        memcpy(io, (const uint8_t *)buf + (uint64_t)i * BLK, BLK);
        if (blk_write(lba + i, io) < 0) return -1;
    }
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
static uint8_t  f_fat_cache[512];
static uint32_t f_fat_cache_lba;
static int      f_fat_cache_valid;
static int      f_fat_cache_dirty;
static uint16_t f_alloc_hint;

/* forward-resume memo for fat_cluster_at: avoids re-walking a chain from its
 * first cluster on every read of a large file (linking libc.a was O(n^2)). */
static uint16_t f_walk_first;   /* memoized file's first cluster (0 = invalid) */
static uint32_t f_walk_index;   /* cluster index last resolved */
static uint16_t f_walk_cl;      /* cluster number at f_walk_index */

/* read-side index->cluster cache: one full chain walk, then O(1) random seeks
 * (TCC seeks around a 2.5MB libc.a). Invalidated on any chain mutation. */
#define FAT_RCHAIN_MAX 65536
static uint16_t *f_rchain;
static uint16_t  f_rchain_first;   /* 0 = invalid */
static uint32_t  f_rchain_len;

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

int fat_mounted(void) { return f_mounted; }

int fat_mount(void) {
    static uint8_t bs[512];
    f_fat_cache_valid = 0;
    f_fat_cache_dirty = 0;
    f_alloc_hint = 2;
    f_walk_first = 0;
    f_rchain_first = 0;
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

static int fat_cache_flush(void) {
    if (!f_fat_cache_valid || !f_fat_cache_dirty) return 0;
    uint32_t index = f_fat_cache_lba - f_fat_start;
    for (uint8_t fi = 0; fi < f_nfats; fi++)
        if (blk_write(f_fat_start + (uint32_t)fi * f_fatsz + index, f_fat_cache) < 0)
            return -1;
    f_fat_cache_dirty = 0;
    return 0;
}

static int fat_cache_load(uint32_t lba) {
    if (f_fat_cache_valid && f_fat_cache_lba == lba) return 0;
    if (fat_cache_flush() < 0 || blk_read(lba, f_fat_cache) < 0) return -1;
    f_fat_cache_lba = lba;
    f_fat_cache_valid = 1;
    return 0;
}

static uint16_t fat_next(uint16_t c) {
    uint32_t off = (uint32_t)c * 2;
    uint32_t lba = f_fat_start + off / 512;
    if (fat_cache_load(lba) < 0) return 0xFFFF;
    return f_fat_cache[off % 512] | (f_fat_cache[off % 512 + 1] << 8);
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
    char lfn[256]; int have_lfn = 0; uint8_t lfn_sum = 0;                /* reconstructed VFAT long name */
    for (uint32_t i = 0; dir_raw(dir_cluster, i, e) == 0; i++) {
        if (e[0] == 0x00) break;                            /* end of directory */
        if (e[0] == 0xE5) { have_lfn = 0; continue; }        /* deleted */
        if (e[11] == 0x0F) {                                 /* long-filename component */
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) { have_lfn = 1; lfn_sum = e[13]; memset(lfn, 0, sizeof lfn); }
            if (!have_lfn || e[13] != lfn_sum || ord < 1 || ord > 19) { have_lfn = 0; continue; }
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            int base = (ord - 1) * 13;
            for (int k = 0; k < 13 && base + k < (int)sizeof lfn - 1; k++) {
                uint16_t ch = (uint16_t)(e[pos[k]] | (e[pos[k] + 1] << 8));
                lfn[base + k] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 0x80 ? (char)ch : '_');
            }
            continue;
        }
        if (e[11] & 0x08) { have_lfn = 0; continue; }        /* volume label */
        if (count++ == which) {                              /* the visible short entry */
            int use_lfn = 0;
            if (have_lfn && lfn[0]) {
                uint8_t sum = 0;                             /* short-name checksum ties LFN to this entry */
                for (int k = 0; k < 11; k++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + e[k]);
                if (sum == lfn_sum) use_lfn = 1;
            }
            if (name) { if (use_lfn) { int j = 0; while (lfn[j] && j < 255) { name[j] = lfn[j]; j++; } name[j] = 0; } else name83(e, name); }
            if (attr) *attr = e[11];
            if (cluster) *cluster = e[26] | (e[27] << 8);
            if (size) *size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
            return 0;
        }
        have_lfn = 0;
    }
    return -1;
}

int fat_lookup(uint32_t dir_cluster, const char *want, uint8_t *attr, uint16_t *cluster, uint32_t *size) {
    char nm[256];
    for (int i = 0; fat_list(dir_cluster, i, nm, attr, cluster, size) == 0; i++) {
        int j = 0; while (nm[j] && want[j] && lc(nm[j]) == lc(want[j])) j++;
        if (!nm[j] && !want[j]) return 0;                    /* case-insensitive full match */
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
        char comp[256]; int k = 0;
        while (*p && *p != '/' && k < 255) comp[k++] = *p++;
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
    if (fat_cache_load(f_fat_start + off / 512) < 0) return;
    f_fat_cache[off % 512]     = (uint8_t)(val & 0xFF);
    f_fat_cache[off % 512 + 1] = (uint8_t)(val >> 8);
    f_fat_cache_dirty = 1;
    f_walk_first = 0;                        /* chain changed: drop the walk memo */
    f_rchain_first = 0;                      /* and the read chain cache */
}

static uint16_t fat_alloc(void) {
    uint32_t n = cluster_count();
    for (uint32_t i = 0; i < n - 2; i++) {
        uint16_t c = (uint16_t)(2 + (f_alloc_hint - 2 + i) % (n - 2));
        if (fat_next(c) != 0x0000) continue;
        fat_set(c, 0xFFFF);
        f_alloc_hint = (uint16_t)(c + 1);
        if (f_alloc_hint >= n) f_alloc_hint = 2;
        return c;
    }
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
        char comp[256]; int k = 0;
        while (*sub && *sub != '/' && k < 255) comp[k++] = *sub++;
        comp[k] = 0;
        const char *t = sub; while (*t == '/') t++;
        if (!*t) { *pdir = cur; int j = 0; while (comp[j]) { name[j] = comp[j]; j++; } name[j] = 0; return 0; }
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_lookup(cur, comp, &at, &cl, &sz) < 0) return -1;   /* missing dir */
        if (!(at & 0x10)) return -1;                               /* not a dir   */
        cur = cl; sub = t;
    }
}

/* ===================== VFAT long-name write support ===================== */
static int valid83(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '~' || c == '!' || c == '#' || c == '$' || c == '%' ||
           c == '&' || c == '(' || c == ')' || c == '@' || c == '^' || c == '{' || c == '}';
}
static int is_canon83(const char *name) {          /* representable as an uppercase 8.3? */
    int base = 0, ext = 0, dot = 0;
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') { if (dot) return 0; dot = 1; continue; }
        if (c >= 'a' && c <= 'z') return 0;         /* lowercase -> need LFN to keep case */
        if (!valid83(c)) return 0;
        if (dot) { if (++ext > 3) return 0; } else { if (++base > 8) return 0; }
    }
    return base > 0;
}
static int s_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static uint8_t lfn_cksum(const uint8_t raw[11]) {
    uint8_t s = 0; for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + raw[i]); return s;
}
/* build an 8.3 alias "BASE~N.EXT" (upper-cased, invalid chars -> '_') */
static void alias83(const char *name, int tilde, uint8_t raw[11]) {
    for (int i = 0; i < 11; i++) raw[i] = ' ';
    const char *dot = 0; for (const char *p = name; *p; p++) if (*p == '.') dot = p;
    int nd = 0, t = tilde ? tilde : 1; for (int u = t; u; u /= 10) nd++;
    int blim = 8 - 1 - nd; if (blim < 1) blim = 1;
    int k = 0;
    for (const char *p = name; *p && (!dot || p < dot) && k < blim; p++) {
        if (*p == ' ' || *p == '.') continue;
        raw[k++] = (uint8_t)up(valid83(*p) ? *p : '_');
    }
    raw[k++] = '~';
    char num[8]; int nn = 0; for (int u = t; u; u /= 10) num[nn++] = (char)('0' + u % 10);
    while (nn && k < 8) raw[k++] = (uint8_t)num[--nn];
    if (dot) { int e = 8; for (const char *p = dot + 1; *p && e < 11; p++) raw[e++] = (uint8_t)up(valid83(*p) ? *p : '_'); }
}
static void gen_alias(uint32_t dir, const char *name, uint8_t raw[11]) {
    for (int t = 1; t < 100000; t++) {
        alias83(name, t, raw);
        uint8_t e[32]; int taken = 0;
        for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++) {
            if (e[0] == 0x00) break;
            if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
            int same = 1; for (int j = 0; j < 11; j++) if (e[j] != raw[j]) { same = 0; break; }
            if (same) { taken = 1; break; }
        }
        if (!taken) return;
    }
}
/* find an entry by long-or-short name; returns its short-entry slot in *short_i,
 * the first LFN slot (== short_i if none) in *lfn_i, and the 32-byte entry in oe. */
static int dir_find(uint32_t dir, const char *want, uint32_t *short_i, uint32_t *lfn_i, uint8_t oe[32]) {
    uint8_t e[32]; char lfn[256]; int have = 0; uint8_t sum = 0; uint32_t lstart = 0;
    for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++) {
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5) { have = 0; continue; }
        if (e[11] == 0x0F) {
            int ord = e[0] & 0x1F;
            if (e[0] & 0x40) { have = 1; sum = e[13]; for (int z = 0; z < 256; z++) lfn[z] = 0; lstart = i; }
            if (!have || e[13] != sum || ord < 1 || ord > 19) { have = 0; continue; }
            static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
            int base = (ord - 1) * 13;
            for (int k = 0; k < 13 && base + k < 255; k++) {
                uint16_t ch = (uint16_t)(e[pos[k]] | (e[pos[k] + 1] << 8));
                lfn[base + k] = (ch == 0 || ch == 0xFFFF) ? 0 : (ch < 0x80 ? (char)ch : '_');
            }
            continue;
        }
        if (e[11] & 0x08) { have = 0; continue; }
        char nm[256]; int use = 0;
        if (have && lfn[0]) { uint8_t s = 0; for (int k = 0; k < 11; k++) s = (uint8_t)(((s & 1) << 7) + (s >> 1) + e[k]); if (s == sum) use = 1; }
        if (use) { int j = 0; while (lfn[j] && j < 255) { nm[j] = lfn[j]; j++; } nm[j] = 0; } else name83(e, nm);
        int j = 0; while (nm[j] && want[j] && lc(nm[j]) == lc(want[j])) j++;
        if (!nm[j] && !want[j]) {
            if (short_i) *short_i = i;
            if (lfn_i) *lfn_i = use ? lstart : i;
            if (oe) for (int z = 0; z < 32; z++) oe[z] = e[z];
            return 0;
        }
        have = 0;
    }
    return -1;
}
/* find `need` contiguous free directory slots; 0 + *start on success. */
static int dir_free_run(uint32_t dir, int need, uint32_t *start) {
    uint8_t e[32]; int run = 0; uint32_t rs = 0;
    for (uint32_t i = 0; dir_raw(dir, i, e) == 0; i++) {
        if (e[0] == 0x00 || e[0] == 0xE5) { if (!run) rs = i; run++; } else run = 0;
        if (run >= need) { *start = rs; return 0; }
        if (e[0] == 0x00) {                             /* remaining slots are all free */
            uint8_t t[32];
            if (dir_raw(dir, rs + (uint32_t)need - 1, t) == 0) { *start = rs; return 0; }
            return -1;
        }
    }
    return -1;
}
/* write `nlfn` LFN entries + the short entry for `name` at slot run `start`. */
static int dir_write_run(uint32_t dir, uint32_t start, const char *name, const uint8_t raw[11],
                         int nlfn, uint8_t attr, uint16_t cluster, uint32_t size) {
    static uint8_t sb[512];
    uint8_t ck = lfn_cksum(raw); int nl = s_len(name);
    static const uint8_t pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int ord = nlfn; ord >= 1; ord--) {
        uint8_t e[32]; for (int z = 0; z < 32; z++) e[z] = 0;
        e[0] = (uint8_t)(ord | (ord == nlfn ? 0x40 : 0)); e[11] = 0x0F; e[13] = ck;
        int base = (ord - 1) * 13;
        for (int k = 0; k < 13; k++) {
            uint16_t ch = (base + k < nl) ? (uint16_t)(uint8_t)name[base + k] : (base + k == nl ? 0 : 0xFFFF);
            e[pos[k]] = (uint8_t)(ch & 0xFF); e[pos[k] + 1] = (uint8_t)(ch >> 8);
        }
        uint32_t lba, off; if (dirent_loc(dir, start + (uint32_t)(nlfn - ord), &lba, &off) < 0) return -1;
        if (blk_read(lba, sb) < 0) return -1;
        for (int z = 0; z < 32; z++) sb[off + z] = e[z];
        if (blk_write(lba, sb) < 0) return -1;
    }
    uint8_t e[32]; for (int z = 0; z < 32; z++) e[z] = 0;
    for (int j = 0; j < 11; j++) e[j] = raw[j];
    e[11] = attr;
    e[26] = (uint8_t)(cluster & 0xFF); e[27] = (uint8_t)(cluster >> 8);
    e[28] = (uint8_t)(size & 0xFF); e[29] = (uint8_t)(size >> 8); e[30] = (uint8_t)(size >> 16); e[31] = (uint8_t)(size >> 24);
    uint32_t lba, off; if (dirent_loc(dir, start + (uint32_t)nlfn, &lba, &off) < 0) return -1;
    if (blk_read(lba, sb) < 0) return -1;
    for (int z = 0; z < 32; z++) sb[off + z] = e[z];
    return blk_write(lba, sb) < 0 ? -1 : 0;
}
/* append a zeroed cluster to a subdirectory so it can hold more entries; the
 * FAT16 root directory is a fixed region and cannot grow (returns -1 for it).
 * Long names cost 4+ slots each (git object dirs fill a one-cluster dir after
 * only three 38-char entries), so directories must extend on demand. */
static int dir_grow(uint32_t dir) {
    if (dir == 0) return -1;                                  /* fixed FAT16 root */
    uint16_t c = (uint16_t)dir, last = (uint16_t)dir;
    while (c >= 2 && c < 0xFFF8) { last = c; c = fat_next(c); }
    uint16_t nc = fat_alloc();
    if (!nc) return -1;                                       /* disk full */
    fat_set(last, nc); fat_set(nc, 0xFFFF);
    static uint8_t zb[512];
    for (int z = 0; z < 512; z++) zb[z] = 0;
    for (uint32_t s = 0; s < f_spc; s++)
        if (blk_write(f_data_start + (uint32_t)(nc - 2) * f_spc + s, zb) < 0) return -1;
    return fat_cache_flush();                                 /* persist the new link */
}
/* create a directory entry for `name` (LFN if it isn't a plain uppercase 8.3). */
static int dir_create(uint32_t dir, const char *name, uint8_t attr, uint16_t cluster, uint32_t size) {
    uint8_t raw[11]; int nlfn = 0;
    if (is_canon83(name)) to83(name, raw);
    else { gen_alias(dir, name, raw); nlfn = (s_len(name) + 12) / 13; }
    uint32_t start; int tries = 0;
    while (dir_free_run(dir, nlfn + 1, &start) < 0) {         /* grow a full subdir */
        if (tries++ >= 8 || dir_grow(dir) < 0) return -1;
    }
    return dir_write_run(dir, start, name, raw, nlfn, attr, cluster, size);
}

int fat_write(const char *sub, const void *buf, uint32_t len) {
    if (!f_mounted) return -1;
    uint16_t dir; char fname[256];
    if (fat_parent(sub, &dir, fname) < 0) return -1;

    uint32_t si, li; uint8_t oe[32];
    int exists = (dir_find(dir, fname, &si, &li, oe) == 0);
    if (exists && (oe[11] & 0x10)) return -1;                     /* refuse to clobber a directory */
    uint16_t oldcluster = exists ? (uint16_t)(oe[26] | (oe[27] << 8)) : 0;
    if (oldcluster >= 2) fat_free_chain(oldcluster);              /* recycle old data */

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
    if (fat_cache_flush() < 0) return -1;
    if (exists) {                                                 /* update the short entry in place */
        uint32_t lba, off;
        if (dirent_loc(dir, si, &lba, &off) < 0) return -1;
        if (blk_read(lba, sb) < 0) return -1;
        sb[off + 11] = 0x20;
        sb[off + 26] = (uint8_t)(first & 0xFF); sb[off + 27] = (uint8_t)(first >> 8);
        sb[off + 28] = (uint8_t)(len & 0xFF); sb[off + 29] = (uint8_t)(len >> 8);
        sb[off + 30] = (uint8_t)(len >> 16); sb[off + 31] = (uint8_t)(len >> 24);
        if (blk_write(lba, sb) < 0) return -1;
    } else if (dir_create(dir, fname, 0x20, first, len) < 0) {
        if (first) fat_free_chain(first);
        return -1;
    }
    return (int)len;
}

static int fat_file_entry(const char *path, uint16_t *first, uint32_t *size,
                          uint32_t *entry_lba, uint32_t *entry_off) {
    uint16_t dir; char name[256];
    if (fat_parent(path, &dir, name) < 0) return -1;
    uint32_t si, li; uint8_t e[32];
    if (dir_find(dir, name, &si, &li, e) < 0 || (e[11] & 0x10)) return -1;
    *first = e[26] | (e[27] << 8);
    *size = e[28] | (e[29] << 8) | (e[30] << 16) | (e[31] << 24);
    return dirent_loc(dir, si, entry_lba, entry_off);
}

static uint16_t fat_cluster_at(uint16_t first, uint32_t index) {
    uint16_t cluster; uint32_t i;
    if (first == f_walk_first && f_walk_cl >= 2 && index >= f_walk_index) {
        cluster = f_walk_cl; i = index - f_walk_index;   /* resume forward */
    } else {
        cluster = first; i = index;
    }
    while (i-- && cluster >= 2 && cluster < 0xFFF8) cluster = fat_next(cluster);
    if (cluster >= 2 && cluster < 0xFFF8) {
        f_walk_first = first; f_walk_index = index; f_walk_cl = cluster;
    }
    return cluster;
}

/* O(1) cluster-at-index for reads: caches the whole chain of one file. */
static uint16_t fat_locate(uint16_t first, uint32_t index) {
    if (first < 2) return 0xFFFF;
    if (first != f_rchain_first) {
        if (!f_rchain) {
            f_rchain = (uint16_t *)page_alloc((FAT_RCHAIN_MAX * 2 + 4095) / 4096);
            if (!f_rchain) return fat_cluster_at(first, index);   /* no heap: fall back */
        }
        uint32_t n = 0; uint16_t c = first;
        while (c >= 2 && c < 0xFFF8 && n < FAT_RCHAIN_MAX) { f_rchain[n++] = c; c = fat_next(c); }
        f_rchain_first = first; f_rchain_len = n;
    }
    return index < f_rchain_len ? f_rchain[index] : 0xFFFF;
}

int fat_pread(const char *path, void *buf, uint32_t len, uint32_t offset) {
    if (!f_mounted || !len) return 0;
    uint16_t first; uint32_t size, entry_lba, entry_off;
    if (fat_file_entry(path, &first, &size, &entry_lba, &entry_off) < 0) return -1;
    (void)entry_lba; (void)entry_off;
    if (offset >= size) return 0;
    if (len > size - offset) len = size - offset;

    uint32_t bpc = (uint32_t)f_spc * 512;
    uint16_t cluster = fat_locate(first, offset / bpc);
    uint32_t within = offset % bpc, done = 0;
    static uint8_t sb[512];
    while (cluster >= 2 && cluster < 0xFFF8 && done < len) {
        uint32_t sector = within / 512;
        uint32_t byte = within % 512;
        for (; sector < f_spc && done < len; sector++, byte = 0) {
            if (blk_read(f_data_start + (uint32_t)(cluster - 2) * f_spc + sector, sb) < 0)
                return done ? (int)done : -1;
            uint32_t n = 512 - byte;
            if (n > len - done) n = len - done;
            memcpy((uint8_t *)buf + done, sb + byte, n);
            done += n;
        }
        within = 0;
        cluster = fat_next(cluster);
    }
    return (int)done;
}

static int fat_zero_cluster(uint16_t cluster) {
    static uint8_t zero[512];
    memset(zero, 0, sizeof(zero));
    for (uint32_t sector = 0; sector < f_spc; sector++)
        if (blk_write(f_data_start + (uint32_t)(cluster - 2) * f_spc + sector, zero) < 0) return -1;
    return 0;
}

static int fat_zero_span(uint16_t first, uint32_t offset, uint32_t len) {
    if (!len) return 0;
    uint32_t bpc = (uint32_t)f_spc * 512;
    uint16_t cluster = fat_cluster_at(first, offset / bpc);
    uint32_t within = offset % bpc, done = 0;
    static uint8_t sb[512];
    while (cluster >= 2 && cluster < 0xFFF8 && done < len) {
        uint32_t sector = within / 512;
        uint32_t byte = within % 512;
        for (; sector < f_spc && done < len; sector++, byte = 0) {
            uint32_t lba = f_data_start + (uint32_t)(cluster - 2) * f_spc + sector;
            uint32_t n = 512 - byte; if (n > len - done) n = len - done;
            if (blk_read(lba, sb) < 0) return -1;
            memset(sb + byte, 0, n);
            if (blk_write(lba, sb) < 0) return -1;
            done += n;
        }
        within = 0; cluster = fat_next(cluster);
    }
    return done == len ? 0 : -1;
}

static int fat_update_file_entry(uint32_t lba, uint32_t off, uint16_t first, uint32_t size) {
    static uint8_t sb[512];
    if (fat_cache_flush() < 0) return -1;
    if (blk_read(lba, sb) < 0) return -1;
    sb[off + 26] = (uint8_t)first;
    sb[off + 27] = (uint8_t)(first >> 8);
    sb[off + 28] = (uint8_t)size;
    sb[off + 29] = (uint8_t)(size >> 8);
    sb[off + 30] = (uint8_t)(size >> 16);
    sb[off + 31] = (uint8_t)(size >> 24);
    return blk_write(lba, sb);
}

int fat_truncate(const char *path, uint32_t new_size) {
    uint16_t first; uint32_t old_size, entry_lba, entry_off;
    if (!f_mounted || fat_file_entry(path, &first, &old_size, &entry_lba, &entry_off) < 0) return -1;
    if (new_size == old_size) return 0;
    uint32_t bpc = (uint32_t)f_spc * 512;
    uint32_t old_count = (old_size + bpc - 1) / bpc;
    uint32_t new_count = (new_size + bpc - 1) / bpc;
    uint16_t last = first;
    if (old_count) for (uint32_t i = 1; i < old_count; i++) last = fat_next(last);
    while (old_count < new_count) {
        uint16_t cluster = fat_alloc();
        if (!cluster || fat_zero_cluster(cluster) < 0) return -1;
        if (old_count) fat_set(last, cluster); else first = cluster;
        last = cluster; old_count++;
    }
    if (new_count < old_count) {
        if (!new_count) { fat_free_chain(first); first = 0; }
        else {
            last = fat_cluster_at(first, new_count - 1);
            uint16_t tail = fat_next(last);
            fat_set(last, 0xFFFF);
            if (tail >= 2 && tail < 0xFFF8) fat_free_chain(tail);
        }
    }
    if (new_size > old_size && fat_zero_span(first, old_size, new_size - old_size) < 0) return -1;
    if (new_size < old_size && new_size &&
        fat_zero_span(first, new_size, new_count * bpc - new_size) < 0) return -1;
    return fat_update_file_entry(entry_lba, entry_off, first, new_size);
}

int fat_pwrite(const char *path, const void *buf, uint32_t len, uint32_t offset) {
    if (!f_mounted || !len || offset + len < offset) return len ? -1 : 0;
    uint16_t first; uint32_t size, entry_lba, entry_off;
    if (fat_file_entry(path, &first, &size, &entry_lba, &entry_off) < 0) return -1;
    uint32_t bpc = (uint32_t)f_spc * 512;
    uint32_t need = (offset + len + bpc - 1) / bpc;
    uint16_t last = first; uint32_t have = 0;
    if (last >= 2) {
        have = 1;
        for (;;) {
            uint16_t next = fat_next(last);
            if (next < 2 || next >= 0xFFF8) break;
            last = next; have++;
        }
    }
    while (have < need) {
        uint16_t cluster = fat_alloc();
        if (!cluster) return -1;
        if (last >= 2) fat_set(last, cluster); else first = cluster;
        last = cluster; have++;
    }
    if (offset > size && fat_zero_span(first, size, offset - size) < 0) return -1;

    uint16_t cluster = fat_cluster_at(first, offset / bpc);
    uint32_t within = offset % bpc, done = 0;
    static uint8_t sb[512];
    while (cluster >= 2 && cluster < 0xFFF8 && done < len) {
        if (!within && f_spc == 1 && len - done >= 512) {
            uint32_t run = 1, max = (len - done) / 512;
            if (max > 8) max = 8;
            uint16_t tail = cluster;
            while (run < max) {
                uint16_t next = fat_next(tail);
                if (next != (uint16_t)(tail + 1)) break;
                tail = next; run++;
            }
            uint32_t lba = f_data_start + (uint32_t)(cluster - 2);
            if (blk_write_many(lba, (const uint8_t *)buf + done, run) < 0)
                return done ? (int)done : -1;
            done += run * 512;
            cluster = fat_next(tail);
            continue;
        }
        uint32_t sector = within / 512;
        uint32_t byte = within % 512;
        for (; sector < f_spc && done < len; sector++, byte = 0) {
            uint32_t lba = f_data_start + (uint32_t)(cluster - 2) * f_spc + sector;
            uint32_t n = 512 - byte;
            if (n > len - done) n = len - done;
            if ((byte || n < 512) && blk_read(lba, sb) < 0) return done ? (int)done : -1;
            if (!byte && n == 512) memcpy(sb, (const uint8_t *)buf + done, 512);
            else memcpy(sb + byte, (const uint8_t *)buf + done, n);
            if (blk_write(lba, sb) < 0) return done ? (int)done : -1;
            done += n;
        }
        within = 0;
        cluster = fat_next(cluster);
    }

    uint32_t new_size = offset + done;
    if (new_size < size) new_size = size;
    if (fat_update_file_entry(entry_lba, entry_off, first, new_size) < 0) return -1;
    return (int)done;
}

/* ============================ directory ops ============================== */

/* create the subdirectory `sub` (absolute path); 0 on success, -1 on failure */
int fat_mkdir(const char *sub) {
    if (!f_mounted) return -1;
    uint16_t dir; char name[256];
    if (fat_parent(sub, &dir, name) < 0) return -1;
    uint8_t at; uint16_t cl; uint32_t sz;
    if (fat_lookup(dir, name, &at, &cl, &sz) == 0) return -1;      /* already exists */

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

    if (fat_cache_flush() < 0) return -1;                         /* parent entry (LFN if long) */
    if (dir_create(dir, name, 0x10, nc, 0) < 0) { fat_free_chain(nc); return -1; }
    return 0;
}

/* delete the file `sub` (absolute path).  0 ok, -1 not found, -2 is a directory */
int fat_unlink(const char *sub) {
    if (!f_mounted) return -1;
    uint16_t dir; char name[256];
    if (fat_parent(sub, &dir, name) < 0) return -1;
    uint32_t si, li; uint8_t e[32];
    if (dir_find(dir, name, &si, &li, e) < 0) return -1;          /* not found */
    if (e[11] & 0x10) return -2;                                  /* directory: refuse */
    uint16_t cl = e[26] | (e[27] << 8);
    if (cl >= 2) fat_free_chain(cl);
    if (fat_cache_flush() < 0) return -1;
    static uint8_t sb[512];
    for (uint32_t i = li; i <= si; i++) {                         /* clear LFN entries + short */
        uint32_t lba, off;
        if (dirent_loc(dir, i, &lba, &off) < 0) return -1;
        if (blk_read(lba, sb) < 0) return -1;
        sb[off] = 0xE5;
        if (blk_write(lba, sb) < 0) return -1;
    }
    return 0;
}

/* remove the empty subdirectory `sub` (absolute path).
 * 0 ok, -1 not found, -2 not a directory, -3 not empty */
int fat_rmdir(const char *sub) {
    if (!f_mounted) return -1;
    uint16_t dir; char name[256];
    if (fat_parent(sub, &dir, name) < 0) return -1;
    uint8_t at; uint16_t cl; uint32_t sz;
    if (fat_lookup(dir, name, &at, &cl, &sz) < 0) return -1;      /* not found */
    if (!(at & 0x10)) return -2;                                  /* not a directory */
    uint8_t e[32];                                                /* must be empty */
    for (uint32_t i = 0; dir_raw(cl, i, e) == 0; i++) {
        if (e[0] == 0x00) break;
        if (e[0] == 0xE5 || e[11] == 0x0F || (e[11] & 0x08)) continue;
        char nm[16]; name83(e, nm);
        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
        return -3;                                                /* has a live entry */
    }
    if (cl >= 2) fat_free_chain(cl);
    uint32_t si, li;                                              /* clear the parent entry (+LFN) */
    if (dir_find(dir, name, &si, &li, e) < 0) return -1;
    if (fat_cache_flush() < 0) return -1;
    static uint8_t sb[512];
    for (uint32_t i = li; i <= si; i++) {
        uint32_t lba, off;
        if (dirent_loc(dir, i, &lba, &off) < 0) return -1;
        if (blk_read(lba, sb) < 0) return -1;
        sb[off] = 0xE5;
        if (blk_write(lba, sb) < 0) return -1;
    }
    return 0;
}

/* ============================ current directory ========================= */

static char g_cwd[256] = "/";

static void s_copy(char *d, const char *s) { int i = 0; while (s[i]) { d[i] = s[i]; i++; } d[i] = 0; }

/* resolve a user path against the cwd into `out` as a normalized absolute path;
 * collapses "."/".." components and duplicate slashes so "./x" and "../x" work. */
void fs_resolve(const char *path, char *out) {
    char raw[300]; int p = 0;
    if (!path || !path[0]) path = ".";
    if (path[0] != '/') {                                         /* make it absolute via cwd */
        for (int i = 0; g_cwd[i] && p < 298; i++) raw[p++] = g_cwd[i];
        if (p != 1 || raw[0] != '/') raw[p++] = '/';
    }
    for (int i = 0; path[i] && p < 298; i++) raw[p++] = path[i];
    raw[p] = 0;

    char comp[40][64]; int nc = 0;                               /* component stack (names up to 63) */
    for (int i = 0; raw[i]; ) {
        while (raw[i] == '/') i++;
        if (!raw[i]) break;
        char c[64]; int k = 0;
        while (raw[i] && raw[i] != '/' && k < 63) c[k++] = raw[i++];
        c[k] = 0;
        while (raw[i] && raw[i] != '/') i++;                      /* skip an overlong tail */
        if (c[0] == '.' && c[1] == 0) continue;                   /* "."  -> drop */
        if (c[0] == '.' && c[1] == '.' && c[2] == 0) { if (nc > 0) nc--; continue; }  /* ".." pop */
        if (nc < 40) { s_copy(comp[nc], c); nc++; }
    }
    int o = 0;                                                   /* rebuild "/a/b/c" (out >= 256B) */
    for (int ci = 0; ci < nc && o < 254; ci++) {
        out[o++] = '/';
        for (int j = 0; comp[ci][j] && o < 254; j++) out[o++] = comp[ci][j];
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
}

/* change the current directory; 0 on success, -1 if the target is not a directory */
int fs_chdir(const char *path) {
    char full[256]; fs_resolve(path, full);
    if (full[0] == '/' && full[1] == 0) { s_copy(g_cwd, "/"); return 0; }
    uint8_t at; uint16_t cl; uint32_t sz;
    if (fat_resolve(full, &at, &cl, &sz) < 0 || !(at & 0x10)) return -1;
    s_copy(g_cwd, full);
    return 0;
}

void fs_getcwd(char *out) { s_copy(out, g_cwd); }

/* set the cwd to an already-absolute, normalized path (validated by the caller);
 * used by the /mnt + /usb VFS routing in kernel/arch64.c. */
void fs_setcwd(const char *abs) { if (abs && abs[0] == '/') s_copy(g_cwd, abs); }
