/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos VFS: unifies the in-RAM filesystem (fs.c) and the FAT16 mount (fat.c)
 * under one path namespace so the shell can cd / ls / cat across both.
 *   /mnt, /mnt/...  -> FAT16 disk (read/write; edits persist to the image)
 *   everything else -> in-RAM filesystem (flat names used as a hierarchy) */
#include "kernel.h"

/* pointer just past "/mnt" if path is under the mount, else 0 */
static const char *mnt_sub(const char *path) {
    if (kstrncmp(path, "/mnt", 4) != 0) return 0;
    if (path[4] == 0 || path[4] == '/') return path + 4;
    return 0;
}

/* walk a FAT path (relative to the mount root) to an entry */
static int fat_walk(const char *sub, uint16_t *cluster, uint32_t *size, int *isdir) {
    uint16_t cur = 0; *cluster = 0; *size = 0; *isdir = 1;
    for (;;) {
        while (*sub == '/') sub++;
        if (!*sub) { return 0; }                    /* directory (root or here) */
        char comp[16]; int k = 0;
        while (*sub && *sub != '/' && k < 15) comp[k++] = *sub++;
        comp[k] = 0;
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_lookup(cur, comp, &at, &cl, &sz) < 0) return -1;
        const char *t = sub; while (*t == '/') t++;
        if (*t) { if (!(at & 0x10)) return -1; cur = cl; sub = t; }
        else { *cluster = cl; *size = sz; *isdir = (at & 0x10) ? 1 : 0; return 0; }
    }
}

int vfs_stat(const char *path, uint32_t *size, int *isdir) {
    const char *sub = mnt_sub(path);
    if (sub) {
        if (!fat_mounted()) return -1;
        uint16_t c; uint32_t s; int d;
        if (fat_walk(sub, &c, &s, &d) < 0) return -1;
        *size = s; *isdir = d; return 0;
    }
    if (kstrcmp(path, "/") == 0) { *size = 0; *isdir = 1; return 0; }
    for (int i = 0; i < NFILE; i++)
        if (g_files[i].used && kstrcmp(g_files[i].name, path) == 0) { *size = g_files[i].size; *isdir = 0; return 0; }
    unsigned pl = kstrlen(path);                     /* directory prefix? */
    for (int i = 0; i < NFILE; i++)
        if (g_files[i].used && kstrncmp(g_files[i].name, path, pl) == 0 && g_files[i].name[pl] == '/') { *size = 0; *isdir = 1; return 0; }
    return -1;
}

/* list entry #index of directory `path`; name gets a trailing '/' for dirs */
int vfs_readdir(const char *path, int index, char *name) {
    const char *sub = mnt_sub(path);
    if (sub) {
        if (!fat_mounted()) return -1;
        uint16_t dir; uint32_t sz; int d;
        if (fat_walk(sub, &dir, &sz, &d) < 0 || !d) return -1;
        char nm[16]; uint8_t at;
        if (fat_list(dir, index, nm, &at, 0, 0) < 0) return -1;
        int k = 0; while (nm[k]) { name[k] = nm[k]; k++; }
        if (at & 0x10) name[k++] = '/';
        name[k] = 0;
        return 0;
    }
    /* ram fs: enumerate unique children of `path` */
    char seen[16][24]; int types[16], nseen = 0;
    int root = (kstrcmp(path, "/") == 0);
    unsigned pl = kstrlen(path);
    for (int i = 0; i < NFILE; i++) {
        if (!g_files[i].used) continue;
        const char *nm = g_files[i].name, *rest;
        if (root) rest = nm + 1;
        else { if (kstrncmp(nm, path, pl) != 0 || nm[pl] != '/') continue; rest = nm + pl + 1; }
        char comp[24]; int k = 0; while (rest[k] && rest[k] != '/' && k < 23) { comp[k] = rest[k]; k++; }
        comp[k] = 0; int isd = (rest[k] == '/');
        int f = 0; for (int j = 0; j < nseen; j++) if (kstrcmp(seen[j], comp) == 0) { f = 1; break; }
        if (!f && nseen < 16) { kstrcpy(seen[nseen], comp); types[nseen] = isd; nseen++; }
    }
    if (root && fat_mounted()) {
        int f = 0; for (int j = 0; j < nseen; j++) if (kstrcmp(seen[j], "mnt") == 0) f = 1;
        if (!f && nseen < 16) { kstrcpy(seen[nseen], "mnt"); types[nseen] = 1; nseen++; }
    }
    if (index >= nseen) return -1;
    kstrcpy(name, seen[index]);
    int k = kstrlen(name); if (types[index]) { name[k++] = '/'; name[k] = 0; }
    return 0;
}

/* read a whole file by path (ram or FAT) into buf; returns bytes read */int vfs_readfile(const char *path, void *buf, unsigned maxlen) {
    const char *sub = mnt_sub(path);
    if (sub) {
        if (!fat_mounted()) return -1;
        uint16_t c; uint32_t s; int d;
        if (fat_walk(sub, &c, &s, &d) < 0 || d) return -1;
        return fat_read(c, s, buf, maxlen);
    }
    for (int i = 0; i < NFILE; i++)
        if (g_files[i].used && kstrcmp(g_files[i].name, path) == 0) {
            unsigned n = g_files[i].size; if (n > maxlen) n = maxlen;
            memcpy(buf, g_files[i].data, n); return (int)n;
        }
    return -1;
}

/* write a whole file by path: /mnt -> FAT disk (persists), else -> RAM fs */
int vfs_writefile(const char *path, const void *buf, unsigned len) {
    const char *sub = mnt_sub(path);
    if (sub) {
        if (!fat_mounted()) return -1;
        return fat_write(sub, buf, len);
    }
    return fs_writefile(path, buf, len);
}
