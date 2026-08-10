/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 — generic FAT32 read/write engine over a pluggable block backend.
 *
 * Originally the read/write driver for the internal-disk data partition (created
 * by the installer, type 0x0C, mounted at /mnt).  Now parameterised over a 512B
 * block backend (fatvol_t.rd / .wr) so the same code mounts either the internal
 * SATA data partition (AHCI backend) or a USB stick (xHCI/MSC backend, see the
 * `usb` console command).  The thin dv_* wrappers keep the original internal-disk
 * API (used by term64.c and the /mnt routing in kernel/arch64.c) unchanged.
 *
 * Both partitioned (MBR type 0x0B/0x0C) and unpartitioned "superfloppy" FAT32
 * layouts are handled.  All FAT/dir walks are bounded so a corrupt filesystem
 * cannot hang the machine. */
#include "boot64.h"

#define SEC 512
extern void *memcpy(void *, const void *, unsigned long);
extern void *memset(void *, int, unsigned long);

static uint32_t r16(const uint8_t *p){ return p[0]|(p[1]<<8); }
static uint32_t r32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static void     w16(uint8_t *p,uint16_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void     w32(uint8_t *p,uint32_t v){ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }

/* Block backend dispatch by tag (direct calls — runtime function pointers are
 * miscompiled in this -fPIC PE/EFI build; see fatvol_t in boot64.h). */
static uint64_t g_op_dl;                          /* per-op watchdog deadline (0 = disabled) */
static fatvol_t *g_fc_v; static uint64_t g_fc_lba;    /* one-sector FAT cache (see below) */
static uint8_t g_fc[SEC] __attribute__((aligned(16)));
static int g_fc_valid, g_fc_dirty;
static void fat_flush(void);                       /* defined with the FAT cache below */
static void fv_arm(void) { g_op_dl = millis() + 8000; fat_flush(); }  /* bound each FS op + persist
                                                           * the previous op's cached FAT sector */
static int fv_rd(fatvol_t *v, uint64_t lba, uint32_t cnt, void *buf) {
    if (g_op_dl && millis() > g_op_dl) return -1;         /* watchdog: abort a wedged operation */
    return v->backend == FV_USB ? usb_read(lba, cnt, buf) : ahci_read(lba, cnt, buf);
}
static int fv_wr(fatvol_t *v, uint64_t lba, uint32_t cnt, const void *buf) {
    if (g_op_dl && millis() > g_op_dl) return -1;
    return v->backend == FV_USB ? usb_write(lba, cnt, buf) : ahci_write(lba, cnt, buf);
}

/* Validate + record geometry of a FAT32 BPB located at absolute LBA `part`. */
static int try_parse(fatvol_t *v, uint64_t part) {
    static uint8_t bpb[SEC] __attribute__((aligned(16)));
    if (fv_rd(v, part, 1, bpb) != 0) return 0;
    if (bpb[510] != 0x55 || bpb[511] != 0xAA) return 0;
    uint32_t bps = r16(bpb+11), spc = bpb[13], rsvd = r16(bpb+14), nfats = bpb[16];
    uint32_t fatsz = r32(bpb+36), root = r32(bpb+44), total = r32(bpb+32);
    if (bps != SEC || !spc || (spc&(spc-1)) || !fatsz || !nfats || root < 2) return 0;
    if (!total) return 0;
    v->part = part; v->fat = part + rsvd; v->data = part + rsvd + (uint64_t)nfats*fatsz;
    v->spc = spc; v->root = root; v->fatsz = fatsz;
    v->maxclus = (total - (rsvd + nfats*fatsz)) / spc + 1;
    v->alloc_hint = 2;
    return 1;
}

int fv_mount(fatvol_t *v, int backend) {
    g_fc_valid = 0; g_fc_dirty = 0;                /* drop any cached FAT sector (no flush to a
                                                    * possibly-different/remounted device) */
    g_op_dl = millis() + 8000;
    if (v->mnt) return 1;
    v->backend = backend;
    static uint8_t mbr[SEC] __attribute__((aligned(16)));
    if (fv_rd(v, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {                                /* MBR-partitioned FAT32? */
        const uint8_t *e = mbr + 0x1BE + i * 16;
        uint8_t type = e[4]; uint32_t start = r32(e + 8);
        if (start && (type == 0x0C || type == 0x0B || type == 0x06 || type == 0x0E) && try_parse(v, start)) {
            v->mnt = 1; return 1;
        }
    }
    if (try_parse(v, 0)) { v->mnt = 1; return 1; }               /* unpartitioned superfloppy */
    return 0;
}

/* ---- FAT entry get/set (writes both FAT copies) ---- */
/* ---- one-sector FAT cache ------------------------------------------------
 * alloc_clus scans thousands of clusters that share the same 512-byte FAT
 * sector; without caching, each check was a fresh ~20 ms USB read, so scanning
 * for a free cluster blew past the op watchdog and made every USB write fail.
 * Cache the current FAT sector; flush the dirty copy (both FAT copies) when a
 * different sector is loaded and at each op boundary (fv_arm). */
static void fat_flush(void) {
    if (g_fc_valid && g_fc_dirty && g_fc_v) {
        fv_wr(g_fc_v, g_fc_lba, 1, g_fc);
        fv_wr(g_fc_v, g_fc_lba + g_fc_v->fatsz, 1, g_fc);       /* mirror FAT #1 */
    }
    g_fc_dirty = 0;
}
static uint8_t *fat_sector(fatvol_t *v, uint64_t lba) {
    if (g_fc_valid && g_fc_v == v && g_fc_lba == lba) return g_fc;
    fat_flush();
    g_fc_v = v; g_fc_lba = lba; g_fc_valid = 0;
    if (fv_rd(v, lba, 1, g_fc) != 0) return 0;
    g_fc_valid = 1;
    return g_fc;
}
static uint32_t fat_get(fatvol_t *v, uint32_t c) {
    uint64_t off = (uint64_t)c * 4;
    uint8_t *fs = fat_sector(v, v->fat + off/SEC);
    if (!fs) return 0x0FFFFFFF;
    return r32(fs + off%SEC) & 0x0FFFFFFF;
}
static int fat_set(fatvol_t *v, uint32_t c, uint32_t val) {
    uint64_t off = (uint64_t)c * 4;
    uint8_t *fs = fat_sector(v, v->fat + off/SEC);
    if (!fs) return -1;
    w32(fs + off%SEC, (r32(fs+off%SEC) & 0xF0000000) | (val & 0x0FFFFFFF));
    g_fc_dirty = 1;
    return 0;
}
static uint32_t clus_next(fatvol_t *v, uint32_t c) { uint32_t x = fat_get(v, c); return (x >= 0x0FFFFFF8 || x < 2) ? 0 : x; }
static uint32_t alloc_clus(fatvol_t *v) {
    for (uint32_t n = 0; n < v->maxclus; n++) {
        uint32_t c = 2 + (v->alloc_hint - 2 + n) % (v->maxclus - 2);
        if (fat_get(v, c) == 0) { fat_set(v, c, 0x0FFFFFFF); v->alloc_hint = c + 1; return c; }
    }
    return 0;
}
static uint64_t clus_lba(fatvol_t *v, uint32_t c) { return v->data + (uint64_t)(c - 2) * v->spc; }

/* ---- 8.3 names ---- */
static int name_to_83(const char *name, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    while (name[i] && name[i] != '.' && o < 8) { char c = name[i++]; if (c>='a'&&c<='z') c-=32; out[o++]=c; }
    while (name[i] && name[i] != '.') i++;
    if (name[i] == '.') { i++; int e = 8; while (name[i] && e < 11) { char c=name[i++]; if(c>='a'&&c<='z')c-=32; out[e++]=c; } }
    return 1;
}
static void name_from_83(const uint8_t *e, char *out) {
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) out[n++] = e[i];
    if (e[8] != ' ') { out[n++]='.'; for (int i=8;i<11&&e[i]!=' ';i++) out[n++]=e[i]; }
    out[n] = 0;
}

/* ---- VFAT long filename (LFN) helpers ---- */
static uint32_t kstrlen(const char *s){ uint32_t n=0; while(s[n]) n++; return n; }
static char lc(char c){ return (c>='A'&&c<='Z') ? (char)(c+32) : c; }
static int ci_eq(const char *a, const char *b){ int i=0; for(;a[i]&&b[i];i++) if(lc(a[i])!=lc(b[i])) return 0; return a[i]==b[i]; }
static int str_eq(const char *a, const char *b){ int i=0; for(;a[i]&&b[i];i++) if(a[i]!=b[i]) return 0; return a[i]==b[i]; }
static uint8_t lfn_csum(const uint8_t *n83){ uint8_t s=0; for(int i=0;i<11;i++) s=(uint8_t)(((s&1)<<7)+(s>>1)+n83[i]); return s; }
/* true if `name` does not round-trip through a plain 8.3 entry (needs an LFN). */
static int needs_lfn(const char *name){
    char n83[11]; name_to_83(name,n83);
    char r[13]; name_from_83((const uint8_t*)n83,r);
    return !str_eq(r, name);
}
/* byte offsets of the 13 UCS-2 chars inside a 32-byte LFN entry */
static const int LFN_OFF[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};

/* Scan a directory chain, reconstructing VFAT long names.  cb is called once per
 * real 8.3 entry with its reconstructed long name (or 0 if none) and the byte
 * position of both the 8.3 entry and the start of its LFN group. */
struct dscan { const char *want; int found; uint32_t clus, size; int isdir; uint64_t slot, slot_start; };
static void dir_scan(fatvol_t *v, uint32_t dir_clus,
                     void (*cb)(const uint8_t *, const char *, uint64_t, uint64_t, void *), void *ctx) {
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t clus = dir_clus ? dir_clus : v->root, guard = 0;
    char lname[264]; int have = 0; uint8_t csum = 0; uint64_t grp = 0;
    while (clus >= 2 && guard++ < 100000) {
        uint64_t base = clus_lba(v, clus);
        for (uint32_t s = 0; s < v->spc; s++) {
            if (fv_rd(v, base + s, 1, sb) != 0) return;
            for (int i = 0; i < SEC; i += 32) {
                const uint8_t *e = sb + i; uint64_t pos = ((base+s)<<9)|i;
                if (e[0] == 0x00) return;                            /* end of directory */
                if (e[0] == 0xE5) { have = 0; continue; }            /* deleted */
                if ((e[11] & 0x0F) == 0x0F) {                        /* LFN fragment */
                    int seq = e[0] & 0x1F;
                    if (e[0] & 0x40) { have = 1; csum = e[13]; for (int k=0;k<264;k++) lname[k]=0; grp = pos; }
                    if (have && seq >= 1 && seq <= 20) {
                        int b = (seq-1)*13;
                        for (int k=0;k<13;k++){ uint16_t ch = e[LFN_OFF[k]] | (e[LFN_OFF[k]+1]<<8);
                            char c = (ch==0||ch==0xFFFF)?0:(ch<128?(char)ch:'_');
                            if (b+k < 263) lname[b+k]=c; }
                    }
                    continue;
                }
                if (e[11] & 0x08) { have = 0; continue; }            /* volume label */
                const char *nm = (have && lname[0] && lfn_csum(e)==csum) ? lname : 0;
                cb(e, nm, pos, nm ? grp : pos, ctx);
                have = 0;
            }
        }
        clus = clus_next(v, clus);
    }
}
static void find_cb(const uint8_t *e, const char *lname, uint64_t pos, uint64_t start, void *ctx) {
    struct dscan *d = ctx;
    if (d->found) return;
    char r[13]; name_from_83(e, r);
    if (!ci_eq(d->want, lname ? lname : r)) return;
    d->found = 1; d->clus = (r16(e+20)<<16)|r16(e+26); d->size = r32(e+28);
    d->isdir = (e[11]&0x10)?1:0; d->slot = pos; d->slot_start = start;
}
static int dir_find(fatvol_t *v, uint32_t dir_clus, const char *want, struct dscan *out) {
    struct dscan d; memset(&d,0,sizeof d); d.want = want;
    dir_scan(v, dir_clus, find_cb, &d);
    *out = d; return d.found;
}

/* Resolve "/a/b/c" (already stripped of the /mnt prefix) to cluster/size/isdir. */
int fv_resolve(fatvol_t *v, const char *path, uint32_t *clus, uint32_t *size, int *isdir) {
    fv_arm();
    if (!v->mnt) return 0;
    uint32_t cur = v->root, sz = 0; int dir = 1;
    const char *p = path; while (*p=='/'||*p=='\\') p++;
    while (*p) {
        char comp[256]; int n = 0;
        while (*p && *p!='/' && *p!='\\' && n<255) comp[n++]=*p++;
        comp[n]=0; while (*p=='/'||*p=='\\') p++;
        if (!n) break;
        struct dscan d; if (!dir_find(v, cur, comp, &d)) return 0;
        cur = d.clus ? d.clus : v->root; sz = d.size; dir = d.isdir;
    }
    if (clus) *clus = cur;
    if (size) *size = sz;
    if (isdir) *isdir = dir;
    return 1;
}

/* Read a file's data (up to max bytes). */
int fv_read(fatvol_t *v, uint32_t first, uint32_t size, void *buf, uint32_t max) {
    fv_arm();
    if (!v->mnt) return -1;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t want = size < max ? size : max, done = 0, clus = first, guard = 0;
    uint8_t *out = buf;
    while (clus >= 2 && done < want && guard++ < 100000000u) {
        uint64_t base = clus_lba(v, clus);
        for (uint32_t s = 0; s < v->spc && done < want; s++) {
            if (fv_rd(v, base + s, 1, sb) != 0) return (int)done;
            uint32_t n = want - done; if (n > SEC) n = SEC;
            memcpy(out + done, sb, n); done += n;
        }
        clus = clus_next(v, clus);
    }
    return (int)done;
}

/* readdir by index: fill name + metadata of the idx-th real entry. */
struct rd { int want, seen; char *name; int got; uint8_t attr; uint32_t clus, size; };
static void rd_cb(const uint8_t *e, const char *lname, uint64_t pos, uint64_t start, void *ctx) {
    (void)pos; (void)start; struct rd *r = ctx;
    if (r->got) return;
    if (r->seen++ == r->want) {
        if (lname) { int k=0; while (lname[k] && k<255) { r->name[k]=lname[k]; k++; } r->name[k]=0; }
        else name_from_83(e, r->name);
        r->attr = e[11]; r->clus = (r16(e+20)<<16)|r16(e+26); r->size = r32(e+28);
        r->got = 1;
    }
}
int fv_readdir_m(fatvol_t *v, uint32_t dir_clus, int idx, char *name,
                 uint8_t *attr, uint32_t *clus, uint32_t *size) {
    fv_arm();
    if (!v->mnt) return -1;
    struct rd r; memset(&r, 0, sizeof r); r.want = idx; r.name = name;
    dir_scan(v, dir_clus, rd_cb, &r);
    if (!r.got) return -1;
    if (attr) *attr = r.attr;
    if (clus) *clus = r.clus;
    if (size) *size = r.size;
    return 0;
}
int fv_readdir(fatvol_t *v, uint32_t dir_clus, int idx, char *name) {
    return fv_readdir_m(v, dir_clus, idx, name, 0, 0, 0);
}

/* Free a cluster chain. */
static void free_chain(fatvol_t *v, uint32_t c) {
    uint32_t guard = 0;
    while (c >= 2 && guard++ < 100000000u) { uint32_t nx = clus_next(v, c); fat_set(v, c, 0); c = nx; }
}

/* Low-level: write a single 8.3 directory entry (create a fresh slot or reuse a
 * free one); optionally return its byte position in *out_slot. */
static int dir_put_83(fatvol_t *v, uint32_t dir_clus, const char n83[11], uint8_t attr,
                      uint32_t first, uint32_t size, uint64_t *out_slot) {
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t clus = dir_clus ? dir_clus : v->root, guard = 0, last = clus;
    while (clus >= 2 && guard++ < 100000) {
        uint64_t base = clus_lba(v, clus); last = clus;
        for (uint32_t s = 0; s < v->spc; s++) {
            if (fv_rd(v, base + s, 1, sb) != 0) return -1;
            for (int i = 0; i < SEC; i += 32) {
                uint8_t *e = sb + i;
                if (e[0]==0x00 || e[0]==0xE5) {
                    memset(e, 0, 32);
                    for (int k=0;k<11;k++) e[k]=(uint8_t)n83[k];
                    e[11]=attr; w16(e+20,(uint16_t)(first>>16)); w16(e+26,(uint16_t)(first&0xFFFF)); w32(e+28,size);
                    if (out_slot) *out_slot = ((base+s)<<9)|i;
                    return fv_wr(v, base + s, 1, sb);
                }
            }
        }
        clus = clus_next(v, clus);
    }
    uint32_t nc = alloc_clus(v); if (!nc) return -1;               /* directory full: extend */
    fat_set(v, last, nc);
    static uint8_t zb[SEC] __attribute__((aligned(16))); memset(zb,0,SEC);
    for (uint32_t s = 0; s < v->spc; s++) fv_wr(v, clus_lba(v, nc)+s, 1, zb);
    for (int k=0;k<11;k++) zb[k]=(uint8_t)n83[k];
    zb[11]=attr; w16(zb+20,(uint16_t)(first>>16)); w16(zb+26,(uint16_t)(first&0xFFFF)); w32(zb+28,size);
    if (out_slot) *out_slot = clus_lba(v, nc) << 9;
    return fv_wr(v, clus_lba(v, nc), 1, zb);
}

/* Patch one 32-byte directory entry at byte position `pos`. */
static int write_entry_at(fatvol_t *v, uint64_t pos, const uint8_t ent[32]) {
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t lba=(uint32_t)(pos>>9), off=(uint32_t)(pos&0x1FF);
    if (fv_rd(v, lba, 1, sb) != 0) return -1;
    for (int i=0;i<32;i++) sb[off+i]=ent[i];
    return fv_wr(v, lba, 1, sb);
}
/* exact-8.3 presence check (for alias uniqueness). */
struct s83 { const char *n; int found; };
static void has83_cb(const uint8_t *e, const char *lname, uint64_t pos, uint64_t start, void *ctx){
    (void)lname;(void)pos;(void)start; struct s83 *d=ctx;
    if (d->found) return;
    for (int i=0;i<11;i++) if ((char)e[i]!=d->n[i]) return;
    d->found=1;
}
static int dir_has_83(fatvol_t *v, uint32_t dir_clus, const char n83[11]){
    struct s83 d; d.n=n83; d.found=0; dir_scan(v, dir_clus, has83_cb, &d); return d.found;
}
/* Generate a unique 8.3 alias for `name` in dir_clus (mangling with ~N on clash). */
static void make_alias(fatvol_t *v, uint32_t dir_clus, const char *name, char out[11]){
    name_to_83(name, out);
    if (!dir_has_83(v, dir_clus, out)) return;
    char ext[3]; for (int i=0;i<3;i++) ext[i]=out[8+i];
    for (int n=1;n<1000000;n++){
        char cand[11]; for(int i=0;i<11;i++) cand[i]=' ';
        for(int i=0;i<3;i++) cand[8+i]=ext[i];
        char num[7]; int nl=0; int nn=n; while(nn){ num[nl++]=(char)('0'+nn%10); nn/=10; } if(!nl) num[nl++]='0';
        int basemax = 8-(1+nl); if (basemax<1) basemax=1;
        int bi=0; for(; bi<basemax && out[bi]!=' '; bi++) cand[bi]=out[bi];
        cand[bi++]='~'; for(int k=nl-1;k>=0;k--) cand[bi++]=(char)num[k];
        if (!dir_has_83(v, dir_clus, cand)){ for(int i=0;i<11;i++) out[i]=cand[i]; return; }
    }
}
/* Collect `need` consecutive free directory-entry positions, extending the dir. */
static int collect_free_slots(fatvol_t *v, uint32_t dir_clus, int need, uint64_t *slots){
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t clus = dir_clus ? dir_clus : v->root, guard=0, last=clus; int got=0, seen_end=0;
    while (clus >= 2 && guard++ < 200000) {
        uint64_t base=clus_lba(v,clus); last=clus;
        for (uint32_t s=0;s<v->spc;s++){
            if (fv_rd(v, base+s, 1, sb) != 0) return -1;
            for (int i=0;i<SEC;i+=32){
                uint8_t c0=(uint8_t)sb[i];
                if (c0==0x00) seen_end=1;
                if (seen_end || c0==0xE5){ slots[got++]=((base+s)<<9)|i; if (got==need) return 0; }
                else got=0;
            }
        }
        uint32_t nx = clus_next(v, clus);
        if (nx < 2) {                                             /* extend with a zeroed cluster */
            uint32_t nc = alloc_clus(v); if (!nc) return -1;
            static uint8_t zb[SEC] __attribute__((aligned(16))); memset(zb,0,SEC);
            for (uint32_t s=0;s<v->spc;s++) fv_wr(v, clus_lba(v,nc)+s, 1, zb);
            fat_set(v, last, nc); clus = nc; seen_end=1;
        } else clus = nx;
    }
    return -1;
}
/* Write an LFN chain + 8.3 alias for `name`; returns the 8.3 slot in *out_slot. */
static int dir_put_lfn(fatvol_t *v, uint32_t dir_clus, const char *name, uint8_t attr,
                       uint32_t first, uint32_t size, uint64_t *out_slot){
    char n83[11]; make_alias(v, dir_clus, name, n83);
    uint8_t cs = lfn_csum((const uint8_t*)n83);
    uint32_t nlen = kstrlen(name);
    int nlfn = (int)((nlen + 12) / 13); if (nlfn < 1) nlfn = 1; if (nlfn > 20) nlfn = 20;
    int need = nlfn + 1;
    uint64_t slots[24];
    if (collect_free_slots(v, dir_clus, need, slots) < 0) return -1;
    for (int k=0;k<nlfn;k++){                                     /* slots[0] = highest seq (0x40) */
        int seq = nlfn - k;
        uint8_t ent[32]; memset(ent,0,32);
        ent[0]=(uint8_t)(seq | (k==0?0x40:0)); ent[11]=0x0F; ent[13]=cs;
        int cbase=(seq-1)*13;
        for (int j=0;j<13;j++){
            int idx=cbase+j; uint16_t ch;
            if (idx < (int)nlen) ch=(uint8_t)name[idx];
            else if (idx==(int)nlen) ch=0; else ch=0xFFFF;
            ent[LFN_OFF[j]]=(uint8_t)ch; ent[LFN_OFF[j]+1]=(uint8_t)(ch>>8);
        }
        if (write_entry_at(v, slots[k], ent) != 0) return -1;
    }
    uint8_t s83[32]; memset(s83,0,32);
    for (int i=0;i<11;i++) s83[i]=(uint8_t)n83[i];
    s83[11]=attr; w16(s83+20,(uint16_t)(first>>16)); w16(s83+26,(uint16_t)(first&0xFFFF)); w32(s83+28,size);
    if (write_entry_at(v, slots[nlfn], s83) != 0) return -1;
    if (out_slot) *out_slot = slots[nlfn];
    return 0;
}
/* Create a directory entry for `name`, using an LFN chain when it does not fit a
 * plain 8.3 entry (so names round-trip exactly). */
static int dir_put_name(fatvol_t *v, uint32_t dir_clus, const char *name, uint8_t attr,
                        uint32_t first, uint32_t size, uint64_t *out_slot){
    if (needs_lfn(name)) return dir_put_lfn(v, dir_clus, name, attr, first, size, out_slot);
    char n83[11]; name_to_83(name, n83);
    return dir_put_83(v, dir_clus, n83, attr, first, size, out_slot);
}
/* Mark the 8.3 entry at target83 plus its preceding LFN group as deleted. */
static int fv_delete_entry(fatvol_t *v, uint32_t dir_clus, uint64_t target83){
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t clus = dir_clus ? dir_clus : v->root, guard=0;
    uint64_t grp[24]; int gn=0;
    while (clus >= 2 && guard++ < 100000){
        uint64_t base=clus_lba(v,clus);
        for (uint32_t s=0;s<v->spc;s++){
            if (fv_rd(v, base+s, 1, sb) != 0) return -1;
            for (int i=0;i<SEC;i+=32){
                uint64_t pos=((base+s)<<9)|i; uint8_t c0=(uint8_t)sb[i];
                if (c0==0x00) return -2;
                if (c0==0xE5){ gn=0; continue; }
                if ((sb[i+11]&0x0F)==0x0F){ if (gn<24) grp[gn++]=pos; continue; }
                if (pos==target83){
                    for (int k=0;k<gn;k++){ uint32_t l=(uint32_t)(grp[k]>>9),o=(uint32_t)(grp[k]&0x1FF);
                        if (fv_rd(v,l,1,sb)==0){ sb[o]=0xE5; fv_wr(v,l,1,sb); } }
                    uint32_t l=(uint32_t)(target83>>9),o=(uint32_t)(target83&0x1FF);
                    if (fv_rd(v,l,1,sb)==0){ sb[o]=0xE5; fv_wr(v,l,1,sb); }
                    return 0;
                }
                gn=0;
            }
        }
        clus = clus_next(v, clus);
    }
    return -2;
}

/* Split "/a/b/name" into parent dir cluster + the leaf name. */
static int split_parent(fatvol_t *v, const char *path, uint32_t *pdir, char *leaf) {
    const char *p = path, *lastslash = 0;
    while (*p) { if (*p=='/'||*p=='\\') lastslash=p; p++; }
    if (!lastslash) { *pdir = v->root; int i=0; while(path[i]&&i<255){leaf[i]=path[i];i++;} leaf[i]=0; return 1; }
    char dir[256]; int n=0; for (const char *q=path; q<lastslash && n<255; q++) dir[n++]=*q; dir[n]=0;
    int i=0; const char *l=lastslash+1; while(l[i]&&i<255){leaf[i]=l[i];i++;} leaf[i]=0;
    if (n==0) { *pdir = v->root; return 1; }
    uint32_t c; int isd; if (!fv_resolve(v, dir,&c,0,&isd) || !isd) return 0;
    *pdir = c; return 1;
}

/* Create/overwrite a file with data[len]. */
int fv_write(fatvol_t *v, const char *path, const void *data, uint32_t len) {
    fv_arm();
    if (!v->mnt) return -1;
    uint32_t pdir; char leaf[256];
    if (!split_parent(v, path, &pdir, leaf)) return -2;
    struct dscan ex; int exists = dir_find(v, pdir, leaf, &ex);
    if (exists && ex.clus) free_chain(v, ex.clus);
    /* allocate a chain for len bytes */
    uint32_t nclus = (len + v->spc*SEC - 1) / (v->spc*SEC);
    uint32_t first = 0, prev = 0;
    const uint8_t *src = data; uint32_t off = 0;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    for (uint32_t i = 0; i < nclus; i++) {
        uint32_t c = alloc_clus(v); if (!c) return -3;
        if (!first) first = c;
        if (prev) fat_set(v, prev, c);
        prev = c;
        for (uint32_t s = 0; s < v->spc; s++) {
            memset(sb, 0, SEC);
            uint32_t n = len - off; if (n > SEC) n = SEC;
            if (n) { memcpy(sb, src + off, n); off += n; }
            if (fv_wr(v, clus_lba(v, c)+s, 1, sb) != 0) return -4;
        }
    }
    if (prev) fat_set(v, prev, 0x0FFFFFFF);
    if (exists) {                                                /* rewrite the existing 8.3 slot */
        uint64_t pos = ex.slot; uint32_t lba = (uint32_t)(pos>>9), o = (uint32_t)(pos&0x1FF);
        if (fv_rd(v, lba,1,sb)!=0) return -5;
        w16(sb+o+20,(uint16_t)(first>>16)); w16(sb+o+26,(uint16_t)(first&0xFFFF)); w32(sb+o+28,len);
        return fv_wr(v, lba,1,sb);
    }
    return dir_put_name(v, pdir, leaf, 0x20, first, len, 0);
}

/* Create a directory. */
int fv_mkdir(fatvol_t *v, const char *path) {
    fv_arm();
    if (!v->mnt) return -1;
    uint32_t pdir; char leaf[256];
    if (!split_parent(v, path, &pdir, leaf)) return -2;
    struct dscan ex; if (dir_find(v, pdir, leaf, &ex)) return ex.isdir ? 0 : -17;
    uint32_t c = alloc_clus(v); if (!c) return -3;
    static uint8_t zb[SEC] __attribute__((aligned(16))); memset(zb,0,SEC);
    for (uint32_t s = 0; s < v->spc; s++) fv_wr(v, clus_lba(v, c)+s, 1, zb);
    /* . and .. entries (always plain 8.3) */
    char dot[11]; for(int i=0;i<11;i++)dot[i]=' '; dot[0]='.';
    dir_put_83(v, c, dot, 0x10, c, 0, 0);
    dot[1]='.'; dir_put_83(v, c, dot, 0x10, (pdir==v->root)?0:pdir, 0, 0);
    return dir_put_name(v, pdir, leaf, 0x10, c, 0, 0);
}

/* Delete a file (mark its dir entry + LFN group free + free its chain). */
int fv_unlink(fatvol_t *v, const char *path) {
    fv_arm();
    if (!v->mnt) return -1;
    uint32_t pdir; char leaf[256];
    if (!split_parent(v, path, &pdir, leaf)) return -2;
    struct dscan ex; if (!dir_find(v, pdir, leaf, &ex)) return -2;
    if (ex.isdir) return -21;
    if (ex.clus) free_chain(v, ex.clus);
    return fv_delete_entry(v, pdir, ex.slot);
}

/* Remove an empty directory: -2 not found, -20 not-a-dir, -39 not-empty. */
struct emptchk { int nonempty; };
static void empt_cb(const uint8_t *e, const char *lname, uint64_t pos, uint64_t start, void *ctx) {
    (void)lname;(void)pos;(void)start; struct emptchk *c = ctx;
    if (e[0]=='.') return;                                       /* skip . and .. */
    c->nonempty = 1;
}
int fv_rmdir(fatvol_t *v, const char *path) {
    fv_arm();
    if (!v->mnt) return -1;
    uint32_t pdir; char leaf[256];
    if (!split_parent(v, path, &pdir, leaf)) return -2;
    struct dscan ex; if (!dir_find(v, pdir, leaf, &ex)) return -2;
    if (!ex.isdir) return -20;
    struct emptchk chk; chk.nonempty = 0;
    dir_scan(v, ex.clus ? ex.clus : v->root, empt_cb, &chk);
    if (chk.nonempty) return -39;
    if (ex.clus) free_chain(v, ex.clus);
    return fv_delete_entry(v, pdir, ex.slot);
}


/* Resolve a file path to (first cluster, size, dir-entry slot). <0 on error. */
static int find_entry(fatvol_t *v, const char *path, uint32_t *first, uint32_t *size, uint64_t *slot) {
    uint32_t pdir; char leaf[256];
    if (!split_parent(v, path, &pdir, leaf)) return -1;
    struct dscan d; if (!dir_find(v, pdir, leaf, &d)) return -1;
    if (d.isdir) return -2;
    *first = d.clus; *size = d.size; *slot = d.slot;
    return 0;
}
/* Walk `nskip` clusters from `first` (0 => first). Returns 0 if past the chain. */
static uint32_t clus_at(fatvol_t *v, uint32_t first, uint32_t nskip) {
    uint32_t c = first;
    for (uint32_t i = 0; i < nskip && c >= 2; i++) c = clus_next(v, c);
    return c;
}
/* Zero-fill `len` bytes of an existing chain starting at byte `off`. */
static int zero_span(fatvol_t *v, uint32_t first, uint32_t off, uint32_t len) {
    if (!len) return 0;
    uint32_t bpc = v->spc * SEC;
    uint32_t c = clus_at(v, first, off / bpc), within = off % bpc, done = 0;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    while (c >= 2 && done < len) {
        for (uint32_t s = within/SEC, byte = within%SEC; s < v->spc && done < len; s++, byte = 0) {
            uint64_t lba = clus_lba(v, c) + s;
            uint32_t n = SEC - byte; if (n > len - done) n = len - done;
            if (fv_rd(v, lba, 1, sb) != 0) return -1;
            memset(sb + byte, 0, n);
            if (fv_wr(v, lba, 1, sb) != 0) return -1;
            done += n;
        }
        within = 0; c = clus_next(v, c);
    }
    return done == len ? 0 : -1;
}

/* Read up to `len` bytes at byte offset `off`; returns bytes read (0 at EOF). */
int fv_pread(fatvol_t *v, const char *path, void *buf, uint32_t len, uint32_t off) {
    fv_arm();
    if (!v->mnt || !len) return v->mnt ? 0 : -1;
    uint32_t first, size; uint64_t slot;
    if (find_entry(v, path, &first, &size, &slot) < 0) return -1;
    if (off >= size) return 0;
    if (len > size - off) len = size - off;
    uint32_t bpc = v->spc * SEC;
    uint32_t c = clus_at(v, first, off / bpc), within = off % bpc, done = 0;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    while (c >= 2 && done < len) {
        for (uint32_t s = within/SEC, byte = within%SEC; s < v->spc && done < len; s++, byte = 0) {
            if (fv_rd(v, clus_lba(v, c) + s, 1, sb) != 0) return done ? (int)done : -1;
            uint32_t n = SEC - byte; if (n > len - done) n = len - done;
            memcpy((uint8_t *)buf + done, sb + byte, n); done += n;
        }
        within = 0; c = clus_next(v, c);
    }
    return (int)done;
}

/* Write `len` bytes at byte offset `off`, extending the file as needed. */
int fv_pwrite(fatvol_t *v, const char *path, const void *buf, uint32_t len, uint32_t off) {
    fv_arm();
    if (!v->mnt || !len) return v->mnt ? 0 : -1;
    uint32_t first, size; uint64_t slot;
    if (find_entry(v, path, &first, &size, &slot) < 0) return -1;
    uint32_t bpc = v->spc * SEC;
    uint32_t need = (off + len + bpc - 1) / bpc;
    uint32_t last = first, have = 0;
    if (first >= 2) { have = 1; for (;;) { uint32_t nx = clus_next(v, last); if (nx < 2) break; last = nx; have++; } }
    while (have < need) {                                       /* extend: allocate + link only.
                                                                 * No per-sector zero-fill — the data
                                                                 * below overwrites it and real gaps
                                                                 * are handled by zero_span; zeroing
                                                                 * every cluster made USB writes crawl. */
        uint32_t c = alloc_clus(v); if (!c) return -1;
        if (last >= 2) fat_set(v, last, c); else first = c;
        last = c; have++;
    }
    if (off > size && zero_span(v, first, size, off - size) < 0) return -1;
    uint32_t c = clus_at(v, first, off / bpc), within = off % bpc, done = 0;
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    while (c >= 2 && done < len) {
        if (within == 0 && len - done >= bpc) {                 /* whole cluster in one transfer */
            if (fv_wr(v, clus_lba(v, c), v->spc, (const uint8_t *)buf + done) != 0) return done ? (int)done : -1;
            done += bpc; c = clus_next(v, c); continue;
        }
        for (uint32_t s = within/SEC, byte = within%SEC; s < v->spc && done < len; s++, byte = 0) {
            uint64_t lba = clus_lba(v, c) + s;
            uint32_t n = SEC - byte; if (n > len - done) n = len - done;
            if (n < SEC) { if (fv_rd(v, lba, 1, sb) != 0) return done ? (int)done : -1; }
            else memset(sb, 0, SEC);
            memcpy(sb + byte, (const uint8_t *)buf + done, n);
            if (fv_wr(v, lba, 1, sb) != 0) return done ? (int)done : -1;
            done += n;
        }
        within = 0; c = clus_next(v, c);
    }
    uint32_t newsize = (off + len > size) ? off + len : size;
    uint32_t lba = (uint32_t)(slot >> 9), o = (uint32_t)(slot & 0x1FF);
    if (fv_rd(v, lba, 1, sb) == 0) {
        w16(sb+o+20,(uint16_t)(first>>16)); w16(sb+o+26,(uint16_t)(first&0xFFFF)); w32(sb+o+28,newsize);
        fv_wr(v, lba, 1, sb);
    }
    return (int)done;
}

/* Truncate/extend a file to `newsize` bytes. */
int fv_truncate(fatvol_t *v, const char *path, uint32_t newsize) {
    fv_arm();
    if (!v->mnt) return -1;
    uint32_t first, size; uint64_t slot;
    if (find_entry(v, path, &first, &size, &slot) < 0) return -1;
    uint32_t bpc = v->spc * SEC;
    uint32_t oldc = (size + bpc - 1) / bpc, newc = (newsize + bpc - 1) / bpc;
    static uint8_t zb[SEC] __attribute__((aligned(16)));
    uint32_t last = first;
    if (oldc) for (uint32_t i = 1; i < oldc; i++) last = clus_next(v, last);
    while (oldc < newc) {
        uint32_t c = alloc_clus(v); if (!c) return -1;
        memset(zb, 0, SEC); for (uint32_t s = 0; s < v->spc; s++) fv_wr(v, clus_lba(v, c)+s, 1, zb);
        if (oldc) fat_set(v, last, c); else first = c;
        last = c; oldc++;
    }
    if (newc < oldc) {
        if (!newc) { free_chain(v, first); first = 0; }
        else { uint32_t l = clus_at(v, first, newc - 1); uint32_t tail = clus_next(v, l);
               fat_set(v, l, 0x0FFFFFFF); if (tail >= 2) free_chain(v, tail); }
    }
    if (newsize > size) zero_span(v, first, size, newsize - size);
    static uint8_t sb[SEC] __attribute__((aligned(16)));
    uint32_t lba = (uint32_t)(slot >> 9), o = (uint32_t)(slot & 0x1FF);
    if (fv_rd(v, lba, 1, sb) != 0) return -1;
    w16(sb+o+20,(uint16_t)(first>>16)); w16(sb+o+26,(uint16_t)(first&0xFFFF)); w32(sb+o+28,newsize);
    return fv_wr(v, lba, 1, sb);
}

/* ---- internal-disk data partition: thin wrappers over the AHCI backend ---- */
static fatvol_t g_dv;

int dv_mount(void) {
    if (g_dv.mnt) return 1;
    if (!ahci_present() && !ahci_init()) return 0;
    return fv_mount(&g_dv, FV_AHCI);
}
int dv_ready(void) { return g_dv.mnt; }
int dv_resolve(const char *path, uint32_t *clus, uint32_t *size, int *isdir) { return fv_resolve(&g_dv, path, clus, size, isdir); }
int dv_read(uint32_t first, uint32_t size, void *buf, uint32_t max) { return fv_read(&g_dv, first, size, buf, max); }
int dv_readdir(uint32_t dir_clus, int idx, char *name) { return fv_readdir(&g_dv, dir_clus, idx, name); }
int dv_write(const char *path, const void *data, uint32_t len) { return fv_write(&g_dv, path, data, len); }
void dv_sync(void) { if (g_fc_v == &g_dv) fat_flush(); }
int dv_mkdir(const char *path) { return fv_mkdir(&g_dv, path); }
int dv_unlink(const char *path) { return fv_unlink(&g_dv, path); }

/* ---- mount-point volume accessors (used by the /mnt + /usb VFS routing) ---- */
static fatvol_t g_uv;                         /* USB stick FAT32 volume */
static int      g_uv_gen = -1;                /* usb_gen() this volume was mounted at */

fatvol_t *dv_vol(void) {                       /* internal data partition, or 0 */
    if (g_dv.mnt) return &g_dv;
    return dv_mount() ? &g_dv : 0;
}
fatvol_t *uv_vol(void) {                        /* USB stick, or 0 (re-mounts on hotplug) */
    if (!usb_present() && !usb_init()) { g_uv.mnt = 0; return 0; }
    if (g_uv.mnt && g_uv_gen == usb_gen()) return &g_uv;   /* same device, still mounted */
    g_uv.mnt = 0;                                          /* first mount or device changed */
    if (!fv_mount(&g_uv, FV_USB)) return 0;
    g_uv_gen = usb_gen();
    return &g_uv;
}
