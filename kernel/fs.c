/* tinyos in-RAM filesystem: a tiny fixed table of named files. */
#include "kernel.h"

file_t g_files[NFILE];
fd_t   g_fds[NFD];

static int find_file(const char *name) {
    for (int i = 0; i < NFILE; i++)
        if (g_files[i].used && kstrcmp(g_files[i].name, name) == 0) return i;
    return -1;
}
static int alloc_file(const char *name) {
    for (int i = 0; i < NFILE; i++)
        if (!g_files[i].used) {
            memset(&g_files[i], 0, sizeof(file_t));
            unsigned n = kstrlen(name); if (n >= FNAME) n = FNAME - 1;
            memcpy(g_files[i].name, name, n); g_files[i].name[n] = 0;
            g_files[i].used = 1;
            return i;
        }
    return -1;
}
static void seed(const char *name, const char *content) {
    int i = alloc_file(name); if (i < 0) return;
    unsigned n = kstrlen(content); if (n > FSIZE) n = FSIZE;
    memcpy(g_files[i].data, content, n); g_files[i].size = n;
}

void fs_init(void) {
    memset(g_files, 0, sizeof(g_files));
    memset(g_fds, 0, sizeof(g_fds));
    for (int i = 0; i < NFD; i++) g_fds[i].file = -1;
    seed("/etc/passwd", "root:toor\nguest:guest\n");
    seed("/README",     "tinyos: a tiny teaching OS. Kernel in C, apps in C++.\n");
    alloc_file("/tmp/log");          /* empty, writable */
}

int fs_open(const char *name, int flags) {
    int fi = find_file(name); if (fi < 0) return -1;
    for (int fd = 3; fd < NFD; fd++)
        if (!g_fds[fd].used) {
            g_fds[fd].used = 1; g_fds[fd].file = fi;
            g_fds[fd].off = (flags & O_APPEND) ? g_files[fi].size : 0;
            return fd;
        }
    return -1;
}
int fs_read(int fd, void *buf, unsigned len) {
    if (fd < 3 || fd >= NFD || !g_fds[fd].used) return -1;
    file_t *f = &g_files[g_fds[fd].file];
    unsigned off = g_fds[fd].off;
    if (off >= f->size) return 0;
    unsigned n = f->size - off; if (n > len) n = len;
    memcpy(buf, f->data + off, n); g_fds[fd].off += n;
    return (int)n;
}
int fs_write(int fd, const void *buf, unsigned len) {
    if (fd < 3 || fd >= NFD || !g_fds[fd].used) return -1;
    file_t *f = &g_files[g_fds[fd].file];
    unsigned off = g_fds[fd].off;
    if (off + len > FSIZE) len = (off < FSIZE) ? (FSIZE - off) : 0;
    memcpy(f->data + off, buf, len); g_fds[fd].off += len;
    if (g_fds[fd].off > f->size) f->size = g_fds[fd].off;
    return (int)len;
}
int fs_close(int fd) {
    if (fd < 3 || fd >= NFD || !g_fds[fd].used) return -1;
    g_fds[fd].used = 0; g_fds[fd].file = -1;
    return 0;
}
int fs_writefile(const char *path, const void *buf, unsigned len) {
    int fi = find_file(path);
    if (fi < 0) { fi = alloc_file(path); if (fi < 0) return -1; }
    if (len > FSIZE) len = FSIZE;
    memcpy(g_files[fi].data, buf, len); g_files[fi].size = len;
    return (int)len;
}
int fs_stat(const char *name, uint32_t *size) {
    int fi = find_file(name); if (fi < 0) return -1;
    if (size) *size = g_files[fi].size;
    return 0;
}
int fs_lseek(int fd, int off, int whence) {
    if (fd < 3 || fd >= NFD || !g_fds[fd].used) return -1;
    uint32_t base = (whence == 1) ? g_fds[fd].off
                  : (whence == 2) ? g_files[g_fds[fd].file].size : 0;
    g_fds[fd].off = base + (uint32_t)off;
    return (int)g_fds[fd].off;
}
