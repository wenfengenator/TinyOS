/* mbb — a musl-static multi-call "mini-busybox" for TinyOS.
 *
 * Proves the Tier-1 Unix substrate: a shell that runs pipelines (fork + pipe +
 * dup2 + wait4) plus a broad set of coreutils-style applets.  Invoked multi-call
 * ("mbb APPLET args" / "/MBB.BIN sh"), by applet name (argv[0], symlink-style),
 * or with no args (a built-in `echo hello | grep hello` self-test).
 *
 * Only applets the current kernel syscalls support are included; things needing
 * networking, real permissions, symlinks or rename are intentionally left out. */
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>

extern char **environ;

static void outs(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out1(const char *s) { outs(1, s); }
static void outnum(int fd, long v) {
    char t[24]; int i = 0, neg = v < 0; unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
    if (!u) t[i++] = '0'; else while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    char b[26]; int n = 0; if (neg) b[n++] = '-'; while (i) b[n++] = t[--i];
    write(fd, b, n);
}
static int contains(const char *h, const char *n) {
    if (!*n) return 1;
    for (; *h; h++) { const char *a = h, *b = n; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
    return 0;
}
static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) if ((*a | 32) != (*b | 32)) return 0;
    return *a == *b;
}

/* ---- filters ------------------------------------------------------------- */
static int ap_echo(int argc, char **argv) {
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = 0; i++; }
    for (; i < argc; i++) { outs(1, argv[i]); if (i + 1 < argc) write(1, " ", 1); }
    if (nl) write(1, "\n", 1);
    return 0;
}
static int catfd(int fd) { char b[512]; int n; while ((n = read(fd, b, sizeof b)) > 0) if (write(1, b, n) != n) return 1; return 0; }
/* input fd for a filter: open the file arg at argv[i], else stdin (0); -1 on error */
static int in_fd(int argc, char **argv, int i, const char *name) {
    if (i >= argc) return 0;
    int fd = open(argv[i], O_RDONLY);
    if (fd < 0) { outs(2, name); outs(2, ": "); outs(2, argv[i]); outs(2, ": not found\n"); }
    return fd;
}
static int ap_cat(int argc, char **argv) {
    if (argc < 2) return catfd(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) { outs(2, "cat: "); outs(2, argv[i]); outs(2, ": not found\n"); rc = 1; continue; }
        if (catfd(fd)) rc = 1;
        close(fd);
    }
    return rc;
}
static int ap_grep(int argc, char **argv) {
    int inv = 0, ic = 0, num = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'v') inv = 1; else if (*f == 'i') ic = 1; else if (*f == 'n') num = 1; }
    if (i >= argc) { outs(2, "usage: grep [-vin] PATTERN [FILE]\n"); return 2; }
    const char *pat = argv[i]; i++;
    int fd = in_fd(argc, argv, i, "grep"); if (fd < 0) return 2;
    char line[1024], low[1024]; int li = 0, found = 0; long ln = 0; char c;
    for (;;) {
        int r = read(fd, &c, 1);
        if (r <= 0 && li == 0) break;
        if (c == '\n' || r <= 0) {
            line[li] = 0; ln++;
            int m; if (ic) { for (int k = 0; k <= li; k++) low[k] = (char)(line[k] | 32); m = contains(low, pat); } else m = contains(line, pat);
            if (m != inv) { if (num) { outnum(1, ln); write(1, ":", 1); } write(1, line, li); write(1, "\n", 1); found = 1; }
            li = 0; if (r <= 0) break;
        } else if (li < 1023) line[li++] = c;
    }
    if (fd) close(fd);
    return found ? 0 : 1;
}
static int ap_wc(int argc, char **argv) {
    int wl = 0, ww = 0, wc = 0, any = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'l') wl = 1; else if (*f == 'w') ww = 1; else if (*f == 'c') wc = 1; any = 1; }
    if (!any) wl = ww = wc = 1;
    int fd = in_fd(argc, argv, i, "wc"); if (fd < 0) return 1;
    long lines = 0, words = 0, chars = 0; int inw = 0, r; char b[512];
    while ((r = read(fd, b, sizeof b)) > 0) for (int k = 0; k < r; k++) {
        char c = b[k]; chars++; if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t') inw = 0; else if (!inw) { inw = 1; words++; }
    }
    if (fd) close(fd);
    if (wl) { outnum(1, lines); write(1, " ", 1); }
    if (ww) { outnum(1, words); write(1, " ", 1); }
    if (wc) { outnum(1, chars); write(1, " ", 1); }
    write(1, "\n", 1);
    return 0;
}
static int ap_head(int argc, char **argv) {
    int n = 10, i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) { n = atoi(argv[i + 1]); i += 2; }
    int fd = in_fd(argc, argv, i, "head"); if (fd < 0) return 1;
    long out = 0; char c;
    while (out < n && read(fd, &c, 1) > 0) { write(1, &c, 1); if (c == '\n') out++; }
    if (fd) close(fd);
    return 0;
}
static int ap_tail(int argc, char **argv) {
    int n = 10, i = 1;
    if (i < argc && !strcmp(argv[i], "-n") && i + 1 < argc) { n = atoi(argv[i + 1]); i += 2; }
    int fd = in_fd(argc, argv, i, "tail"); if (fd < 0) return 1;
    static char buf[65536]; int len = 0, r; char b[512];
    while ((r = read(fd, b, sizeof b)) > 0) for (int k = 0; k < r && len < (int)sizeof buf; k++) buf[len++] = b[k];
    if (fd) close(fd);
    int start = 0, nl = 0;
    for (int k = len - 1; k >= 0; k--) if (buf[k] == '\n' && k != len - 1) { if (++nl >= n) { start = k + 1; break; } }
    write(1, buf + start, len - start);
    return 0;
}
static int ap_rev(int argc, char **argv) {
    int fd = in_fd(argc, argv, 1, "rev"); if (fd < 0) return 1;
    char line[1024]; int li = 0; char c;
    for (;;) { int r = read(fd, &c, 1); if (r <= 0 && li == 0) break;
        if (c == '\n' || r <= 0) { for (int k = li - 1; k >= 0; k--) write(1, &line[k], 1); write(1, "\n", 1); li = 0; if (r <= 0) break; }
        else if (li < 1023) line[li++] = c; }
    if (fd) close(fd);
    return 0;
}
static int ap_tee(int argc, char **argv) {
    int fds[8], nf = 0;
    for (int i = 1; i < argc && nf < 8; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT | O_TRUNC); if (fd >= 0) fds[nf++] = fd; }
    char b[512]; int n;
    while ((n = read(0, b, sizeof b)) > 0) { write(1, b, n); for (int i = 0; i < nf; i++) write(fds[i], b, n); }
    for (int i = 0; i < nf; i++) close(fds[i]);
    return 0;
}
static int ap_nl(int argc, char **argv) {
    int fd = in_fd(argc, argv, 1, "nl"); if (fd < 0) return 1;
    long ln = 0; char c; int start = 1;
    for (;;) { int r = read(fd, &c, 1); if (r <= 0) break; if (start) { ln++; outnum(1, ln); write(1, "\t", 1); start = 0; } write(1, &c, 1); if (c == '\n') start = 1; }
    if (fd) close(fd);
    return 0;
}
static int ap_yes(int argc, char **argv) {
    const char *s = argc > 1 ? argv[1] : "y";
    for (;;) { if (write(1, s, strlen(s)) <= 0) return 0; if (write(1, "\n", 1) <= 0) return 0; }
}
static int ap_seq(int argc, char **argv) {
    long a = 1, b;
    if (argc == 2) b = atol(argv[1]); else if (argc >= 3) { a = atol(argv[1]); b = atol(argv[2]); } else return 1;
    for (long v = a; v <= b; v++) { outnum(1, v); write(1, "\n", 1); }
    return 0;
}
static int ap_sort(int argc, char **argv) {
    int rev = argc > 1 && !strcmp(argv[1], "-r");
    static char buf[65536]; int len = 0; char c;
    while (read(0, &c, 1) > 0) if (len < (int)sizeof buf - 1) buf[len++] = c;
    buf[len] = 0;
    char *lines[2048]; int nl = 0; char *p = buf;
    while (*p && nl < 2048) { lines[nl++] = p; char *q = strchr(p, '\n'); if (!q) break; *q = 0; p = q + 1; }
    for (int i = 0; i < nl; i++) for (int j = i + 1; j < nl; j++) { int cmp = strcmp(lines[i], lines[j]); if ((rev && cmp < 0) || (!rev && cmp > 0)) { char *t = lines[i]; lines[i] = lines[j]; lines[j] = t; } }
    for (int i = 0; i < nl; i++) { out1(lines[i]); write(1, "\n", 1); }
    return 0;
}
static int ap_uniq(int argc, char **argv) {
    (void)argc; (void)argv;
    char cur[1024], prev[1024]; int ci = 0, first = 1; prev[0] = 0; char c;
    for (;;) { int r = read(0, &c, 1); if (c == '\n' || r <= 0) { cur[ci] = 0; if (first || strcmp(cur, prev)) { out1(cur); write(1, "\n", 1); strcpy(prev, cur); first = 0; } ci = 0; if (r <= 0) break; } else if (ci < 1023) cur[ci++] = c; }
    return 0;
}
static int ap_tr(int argc, char **argv) {
    if (argc < 3) { outs(2, "usage: tr SET1 SET2\n"); return 1; }
    const char *s1 = argv[1], *s2 = argv[2]; int l2 = strlen(s2); char c;
    while (read(0, &c, 1) > 0) { const char *m = strchr(s1, c); if (m && *m) { int idx = (int)(m - s1); char o = l2 ? s2[idx < l2 ? idx : l2 - 1] : c; if (l2) write(1, &o, 1); } else write(1, &c, 1); }
    return 0;
}

/* ---- files --------------------------------------------------------------- */
static int ap_ls(int argc, char **argv) {
    int all = 0, lng = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        for (const char *f = argv[i] + 1; *f; f++) { if (*f == 'a') all = 1; else if (*f == 'l') lng = 1; }
    const char *dir = i < argc ? argv[i] : ".";
    DIR *d = opendir(dir);
    if (!d) { outs(2, "ls: "); outs(2, dir); outs(2, ": not a directory\n"); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!all && e->d_name[0] == '.') continue;
        if (lng) {
            char full[288]; int n = 0;
            for (const char *p = dir; *p && n < 200; p++) full[n++] = *p;
            if (n && full[n - 1] != '/') full[n++] = '/';
            for (const char *p = e->d_name; *p && n < 280; p++) full[n++] = *p;
            full[n] = 0;
            struct stat st; int isdir = 0; long sz = 0;
            if (stat(full, &st) == 0) { isdir = S_ISDIR(st.st_mode); sz = (long)st.st_size; }
            out1(isdir ? "drwxr-xr-x 1 root root " : "-rw-r--r-- 1 root root ");
            outnum(1, sz); write(1, "\t", 1);
        }
        out1(e->d_name);
        if (lng) write(1, "\n", 1); else write(1, "  ", 2);
    }
    if (!lng) write(1, "\n", 1);
    closedir(d);
    return 0;
}
static int ap_cp(int argc, char **argv) {
    if (argc < 3) { outs(2, "usage: cp SRC DST\n"); return 1; }
    int s = open(argv[1], O_RDONLY); if (s < 0) { outs(2, "cp: cannot open source\n"); return 1; }
    int d = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC); if (d < 0) { close(s); outs(2, "cp: cannot open dest\n"); return 1; }
    char b[512]; int n; while ((n = read(s, b, sizeof b)) > 0) write(d, b, n);
    close(s); close(d); return 0;
}
static int ap_mv(int argc, char **argv) { if (ap_cp(argc, argv)) return 1; unlink(argv[1]); return 0; }
static int ap_rm(int argc, char **argv) {
    int rc = 0, i = 1; while (i < argc && argv[i][0] == '-') i++;
    for (; i < argc; i++) if (unlink(argv[i]) < 0) { outs(2, "rm: "); outs(2, argv[i]); outs(2, ": failed\n"); rc = 1; }
    return rc;
}
static int ap_mkdir(int argc, char **argv) {
    int rc = 0, i = 1; while (i < argc && argv[i][0] == '-') i++;
    for (; i < argc; i++) if (mkdir(argv[i], 0755) < 0) { outs(2, "mkdir: "); outs(2, argv[i]); outs(2, ": failed\n"); rc = 1; }
    return rc;
}
static int ap_rmdir(int argc, char **argv) { int rc = 0; for (int i = 1; i < argc; i++) if (rmdir(argv[i]) < 0) rc = 1; return rc; }
static int ap_touch(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT); if (fd >= 0) close(fd); }
    return 0;
}
static int ap_cmp(int argc, char **argv) {
    if (argc < 3) return 2;
    int a = open(argv[1], O_RDONLY), b = open(argv[2], O_RDONLY);
    if (a < 0 || b < 0) { outs(2, "cmp: open failed\n"); return 2; }
    char ca, cb; long pos = 0; int diff = 0;
    for (;;) { int ra = read(a, &ca, 1), rb = read(b, &cb, 1); if (ra <= 0 && rb <= 0) break; pos++; if (ra != rb || ca != cb) { diff = 1; break; } }
    close(a); close(b);
    if (diff) { out1("differ at byte "); outnum(1, pos); write(1, "\n", 1); return 1; }
    return 0;
}
static int ap_stat(int argc, char **argv) {
    if (argc < 2) return 1;
    struct stat st; if (stat(argv[1], &st) < 0) { outs(2, "stat: not found\n"); return 1; }
    out1("  File: "); out1(argv[1]);
    out1(S_ISDIR(st.st_mode) ? "  (directory)\n  Size: " : "  (regular file)\n  Size: ");
    outnum(1, (long)st.st_size); write(1, "\n", 1);
    return 0;
}
static int ap_basename(int argc, char **argv) {
    if (argc < 2) return 1; const char *b = argv[1]; for (const char *p = argv[1]; *p; p++) if (*p == '/') b = p + 1;
    out1(b); write(1, "\n", 1); return 0;
}
static int ap_dirname(int argc, char **argv) {
    if (argc < 2) return 1; char d[288]; int last = -1, n = 0;
    for (const char *p = argv[1]; *p && n < 280; p++) { d[n] = *p; if (*p == '/') last = n; n++; }
    if (last < 0) out1("."); else if (last == 0) out1("/"); else { d[last] = 0; out1(d); }
    write(1, "\n", 1); return 0;
}

/* ---- system -------------------------------------------------------------- */
static int ap_pwd(int argc, char **argv) { (void)argc; (void)argv; char b[256]; if (getcwd(b, sizeof b)) { out1(b); write(1, "\n", 1); } else out1("/\n"); return 0; }
static int ap_uname(int argc, char **argv) {
    struct utsname u; uname(&u);
    if (argc > 1 && !strcmp(argv[1], "-a")) { out1(u.sysname); write(1, " ", 1); out1(u.nodename); write(1, " ", 1); out1(u.release); write(1, " ", 1); out1(u.machine); }
    else if (argc > 1 && !strcmp(argv[1], "-r")) out1(u.release);
    else if (argc > 1 && !strcmp(argv[1], "-m")) out1(u.machine);
    else out1(u.sysname);
    write(1, "\n", 1); return 0;
}
static int ap_sleep(int argc, char **argv) { if (argc < 2) return 1; struct timespec ts = { atol(argv[1]), 0 }; nanosleep(&ts, 0); return 0; }
static int ap_date(int argc, char **argv) {
    (void)argc; (void)argv;
    time_t t = time(0);
    struct tm tm; gmtime_r(&t, &tm);
    char b[24]; int n = 0, y = tm.tm_year + 1900;
    b[n++] = '0' + (y / 1000) % 10; b[n++] = '0' + (y / 100) % 10;
    b[n++] = '0' + (y / 10) % 10;   b[n++] = '0' + y % 10;
    b[n++] = '-'; b[n++] = '0' + (tm.tm_mon + 1) / 10; b[n++] = '0' + (tm.tm_mon + 1) % 10;
    b[n++] = '-'; b[n++] = '0' + tm.tm_mday / 10; b[n++] = '0' + tm.tm_mday % 10;
    b[n++] = ' '; b[n++] = '0' + tm.tm_hour / 10; b[n++] = '0' + tm.tm_hour % 10;
    b[n++] = ':'; b[n++] = '0' + tm.tm_min / 10;  b[n++] = '0' + tm.tm_min % 10;
    b[n++] = ':'; b[n++] = '0' + tm.tm_sec / 10;  b[n++] = '0' + tm.tm_sec % 10;
    b[n++] = '\n'; write(1, b, n);
    return 0;
}
static int ap_id(int argc, char **argv) { (void)argc; (void)argv; out1("uid=0(root) gid=0(root)\n"); return 0; }
static int ap_whoami(int argc, char **argv) { (void)argc; (void)argv; out1("root\n"); return 0; }
static int ap_hostname(int argc, char **argv) { (void)argc; (void)argv; out1("tinyos\n"); return 0; }
static int ap_env(int argc, char **argv) { (void)argc; (void)argv; for (char **e = environ; e && *e; e++) { out1(*e); write(1, "\n", 1); } return 0; }
static int ap_clear(int argc, char **argv) { (void)argc; (void)argv; out1("\033[2J\033[H"); return 0; }
static int ap_free(int argc, char **argv) { (void)argc; (void)argv; int fd = open("/proc/meminfo", O_RDONLY); if (fd < 0) return 1; catfd(fd); close(fd); return 0; }
static int ap_sync(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int ap_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int ap_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }
static int ap_kill(int argc, char **argv) {
    int sig = 15, i = 1;
    if (i < argc && argv[i][0] == '-') { sig = atoi(argv[i] + 1); if (!sig) sig = 15; i++; }
    for (; i < argc; i++) kill(atoi(argv[i]), sig);
    return 0;
}
static int ap_test(int argc, char **argv) {
    int n = argc;
    if (!strcmp(argv[0], "[") && n > 1 && !strcmp(argv[n - 1], "]")) n--;
    if (n == 2) return argv[1][0] ? 0 : 1;
    if (n == 3) {
        const char *op = argv[1], *a = argv[2]; struct stat st;
        if (!strcmp(op, "-e")) return stat(a, &st) == 0 ? 0 : 1;
        if (!strcmp(op, "-f")) return (stat(a, &st) == 0 && S_ISREG(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-d")) return (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : 1;
        if (!strcmp(op, "-z")) return a[0] == 0 ? 0 : 1;
        if (!strcmp(op, "-n")) return a[0] ? 0 : 1;
        return 1;
    }
    if (n == 4) {
        const char *a = argv[1], *op = argv[2], *b = argv[3];
        if (!strcmp(op, "="))  return strcmp(a, b) == 0 ? 0 : 1;
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0 ? 0 : 1;
        long x = atol(a), y = atol(b);
        if (!strcmp(op, "-eq")) return x == y ? 0 : 1;
        if (!strcmp(op, "-ne")) return x != y ? 0 : 1;
        if (!strcmp(op, "-lt")) return x <  y ? 0 : 1;
        if (!strcmp(op, "-le")) return x <= y ? 0 : 1;
        if (!strcmp(op, "-gt")) return x >  y ? 0 : 1;
        if (!strcmp(op, "-ge")) return x >= y ? 0 : 1;
    }
    return 1;
}
static int ap_expr(int argc, char **argv) {
    if (argc != 4) return 2;
    long a = atol(argv[1]), b = atol(argv[3]); const char *op = argv[2]; long r;
    if (!strcmp(op, "+")) r = a + b; else if (!strcmp(op, "-")) r = a - b;
    else if (!strcmp(op, "*")) r = a * b; else if (!strcmp(op, "/")) r = b ? a / b : 0;
    else if (!strcmp(op, "%")) r = b ? a % b : 0; else return 2;
    outnum(1, r); write(1, "\n", 1); return r ? 0 : 1;
}

/* ---- vi: modal full-screen editor (parity with the native programs/vi.cpp) --
 * Normal: h j k l / arrows move; 0 $ line ends; i a A insert; o open; x del;
 * dd del line; : ex (:w :q :wq :x :q!).  Insert: type, Tab, Enter, Backspace,
 * arrows, ESC.  Line-number gutter + long-line wrapping like the native vi. */
#define VI_MAXL 512
#define VI_MAXC 200
#define VI_GUT  4
#define VK_UP 0x80
#define VK_DOWN 0x81
#define VK_LEFT 0x82
#define VK_RIGHT 0x83
static char vbuf[VI_MAXL][VI_MAXC + 2];
static int  vlen[VI_MAXL], vnl, vcx, vcy, vtop, vVH = 23, vVW = 76, vdirty;
static char vfname[128];

static void vout(const char *s) { int n = 0; while (s[n]) n++; if (n) write(1, s, n); }
static void vnum(int v) {
    char t[10]; int n = 0; if (!v) t[n++] = '0'; while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    char r[10]; int m = 0; while (n) r[m++] = t[--n]; write(1, r, m);
}
static void vxy(int row, int col) { vout("\x1b["); vnum(row); vout(";"); vnum(col); vout("H"); }
static int  vi_rows(int i) { int n = (vlen[i] + vVW - 1) / vVW; return n < 1 ? 1 : n; }

static void vi_load(const char *path) {
    vnl = 0;
    int fd = path[0] ? open(path, O_RDONLY) : -1;
    if (fd >= 0) {
        static char d[VI_MAXL * 40]; int n = read(fd, d, sizeof d - 1); close(fd);
        if (n < 0) n = 0;
        int c = 0;
        for (int i = 0; i < n && vnl < VI_MAXL; i++) {
            if (d[i] == '\n') { vlen[vnl] = c; vbuf[vnl][c] = 0; vnl++; c = 0; }
            else if (d[i] != '\r' && c < VI_MAXC) vbuf[vnl][c++] = d[i];
        }
        if (vnl < VI_MAXL && c > 0) { vlen[vnl] = c; vbuf[vnl][c] = 0; vnl++; }
    }
    if (vnl == 0) { vnl = 1; vlen[0] = 0; vbuf[0][0] = 0; }
}

static int vi_save(void) {
    if (!vfname[0]) return -1;
    int fd = open(vfname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    for (int i = 0; i < vnl; i++) { if (vlen[i]) write(fd, vbuf[i], vlen[i]); write(fd, "\n", 1); }
    close(fd); vdirty = 0; return 0;
}

static void vi_ins(char c) {
    if (vlen[vcy] >= VI_MAXC) return;
    for (int i = vlen[vcy]; i > vcx; i--) vbuf[vcy][i] = vbuf[vcy][i - 1];
    vbuf[vcy][vcx] = c; vlen[vcy]++; vbuf[vcy][vlen[vcy]] = 0; vcx++; vdirty = 1;
}
static void vi_del(void) {
    if (vcx >= vlen[vcy]) return;
    for (int i = vcx; i < vlen[vcy] - 1; i++) vbuf[vcy][i] = vbuf[vcy][i + 1];
    vlen[vcy]--; vbuf[vcy][vlen[vcy]] = 0; vdirty = 1;
}
static void vi_sdown(int from) { for (int i = vnl; i > from; i--) { memcpy(vbuf[i], vbuf[i - 1], VI_MAXC + 2); vlen[i] = vlen[i - 1]; } vnl++; }
static void vi_sup(int at)     { for (int i = at; i < vnl - 1; i++) { memcpy(vbuf[i], vbuf[i + 1], VI_MAXC + 2); vlen[i] = vlen[i + 1]; } vnl--; }
static void vi_split(void) {
    if (vnl >= VI_MAXL) return;
    vi_sdown(vcy + 1);
    int m = 0; for (int j = vcx; j < vlen[vcy]; j++) vbuf[vcy + 1][m++] = vbuf[vcy][j];
    vlen[vcy + 1] = m; vbuf[vcy + 1][m] = 0; vlen[vcy] = vcx; vbuf[vcy][vcx] = 0;
    vcy++; vcx = 0; vdirty = 1;
}
static void vi_openl(void) { if (vnl >= VI_MAXL) return; vi_sdown(vcy + 1); vcy++; vlen[vcy] = 0; vbuf[vcy][0] = 0; vcx = 0; vdirty = 1; }
static void vi_bs(void) {
    if (vcx > 0) { vcx--; vi_del(); }
    else if (vcy > 0 && vlen[vcy - 1] + vlen[vcy] < VI_MAXC) {
        int pl = vlen[vcy - 1], save = vcy;
        for (int i = 0; i < vlen[vcy]; i++) vbuf[vcy - 1][pl + i] = vbuf[vcy][i];
        vlen[vcy - 1] = pl + vlen[vcy]; vbuf[vcy - 1][vlen[vcy - 1]] = 0;
        vcy--; vcx = pl; vi_sup(save); vdirty = 1;
    }
}
static void vi_ddl(void) {
    if (vnl > 1) { vi_sup(vcy); if (vcy >= vnl) vcy = vnl - 1; }
    else { vlen[0] = 0; vbuf[0][0] = 0; }
    vcx = 0; vdirty = 1;
}

static void vi_redraw(const char *status) {
    vout("\x1b[2J");
    int r = 0, i = vtop;
    while (r < vVH && i < vnl) {
        int rows = vi_rows(i);
        for (int k = 0; k < rows && r < vVH; k++, r++) {
            char num[VI_GUT];
            if (k == 0) { int ln = i + 1;
                num[0] = (ln >= 100) ? (char)('0' + (ln / 100) % 10) : ' ';
                num[1] = (ln >= 10)  ? (char)('0' + (ln / 10) % 10)  : ' ';
                num[2] = (char)('0' + ln % 10); num[3] = ' ';
            } else { num[0] = num[1] = num[2] = num[3] = ' '; }
            vxy(r + 1, 1); write(1, num, VI_GUT);
            int start = k * vVW, n = vlen[i] - start; if (n > vVW) n = vVW;
            if (n > 0) write(1, vbuf[i] + start, n);
        }
        i++;
    }
    vxy(vVH + 1, 1); vout("\x1b[K"); vout(status);
    int sr = 0; for (int j = vtop; j < vcy; j++) sr += vi_rows(j); sr += vcx / vVW;
    vxy(sr + 1, VI_GUT + (vcx % vVW) + 1);
}

static int ap_vi(int argc, char **argv) {
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 2) { vVH = ws.ws_row - 1; if (ws.ws_col > VI_GUT + 4) vVW = ws.ws_col - VI_GUT; }
    vfname[0] = 0;
    if (argc > 1) { int j = 0; for (const char *p = argv[1]; *p && j < 127; p++) vfname[j++] = *p; vfname[j] = 0; }
    vi_load(vfname);
    vcx = vcy = vtop = vdirty = 0;

    struct termios oldt, raw;                  /* raw mode: keys one at a time, no echo */
    tcgetattr(0, &oldt); raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(0, TCSANOW, &raw);

    int ins = 0, run = 1;
    while (run) {
        char st[128]; int p = 0;               /* status line (native-style) */
        if (ins) { const char *m = "-- INSERT --  (ESC=normal)"; while (*m && p < 127) st[p++] = *m++; }
        else {
            st[p++] = '"'; for (const char *f = vfname[0] ? vfname : "[No Name]"; *f && p < 80; f++) st[p++] = *f;
            const char *m = vdirty ? "\" [modified]  " : "\"  "; while (*m && p < 108) st[p++] = *m++;
            m = "i insert  x del  o open  :w save  :q quit"; while (*m && p < 127) st[p++] = *m++;
        }
        st[p] = 0;
        vi_redraw(st);

        char rc; if (read(0, &rc, 1) <= 0) continue;
        int c = (unsigned char)rc;
        if (ins) {                             /* ---- INSERT ---- */
            if (c == 27) { ins = 0; if (vcx > 0) vcx--; }
            else if (c == '\n' || c == '\r') vi_split();
            else if (c == 8 || c == 127) vi_bs();
            else if (c == VK_LEFT)  { if (vcx > 0) vcx--; }
            else if (c == VK_RIGHT) { if (vcx < vlen[vcy]) vcx++; }
            else if (c == VK_UP)    { if (vcy > 0) { vcy--; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == VK_DOWN)  { if (vcy < vnl - 1) { vcy++; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == '\t') { int n = 4 - (vcx & 3); while (n-- > 0) vi_ins(' '); }
            else if (c >= 32 && c < 127) vi_ins((char)c);
        } else {                               /* ---- NORMAL ---- */
            if (c == 'h' || c == VK_LEFT) { if (vcx > 0) vcx--; }
            else if (c == 'l' || c == VK_RIGHT) { if (vcx < vlen[vcy]) vcx++; }
            else if (c == 'j' || c == VK_DOWN) { if (vcy < vnl - 1) { vcy++; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == 'k' || c == VK_UP) { if (vcy > 0) { vcy--; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == '0') vcx = 0;
            else if (c == '$') vcx = vlen[vcy] ? vlen[vcy] - 1 : 0;
            else if (c == 'i') ins = 1;
            else if (c == 'a') { if (vcx < vlen[vcy]) vcx++; ins = 1; }
            else if (c == 'A') { vcx = vlen[vcy]; ins = 1; }
            else if (c == 'x') vi_del();
            else if (c == 'o') { vi_openl(); ins = 1; }
            else if (c == 'd') { char d2; if (read(0, &d2, 1) > 0 && d2 == 'd') vi_ddl(); }
            else if (c == ':') {
                char cb[64]; int cn = 0;
                for (;;) {
                    vxy(vVH + 1, 1); vout("\x1b[K:"); if (cn > 0) write(1, cb, cn);
                    char cc; if (read(0, &cc, 1) <= 0) continue;
                    int ic = (unsigned char)cc;
                    if (ic == '\n' || ic == '\r') break;
                    if (ic == 27) { cn = -1; break; }
                    if (ic == 8 || ic == 127) { if (cn > 0) cn--; continue; }
                    if (cn < 63 && ic >= 32 && ic < 127) cb[cn++] = (char)ic;
                }
                if (cn >= 0) { cb[cn] = 0;
                    int w = 0, q = 0, bang = 0;
                    for (int k = 0; cb[k]; k++) { if (cb[k] == 'w') w = 1; else if (cb[k] == 'q') q = 1; else if (cb[k] == '!') bang = 1; else if (cb[k] == 'x') w = q = 1; }
                    if (w && vi_save() < 0) { vxy(vVH + 1, 1); vout("\x1b[K"); vout("write failed"); read(0, &rc, 1); }
                    if (q && (w || bang || !vdirty)) run = 0;
                    else if (q) { vxy(vVH + 1, 1); vout("\x1b[K"); vout("unsaved (:q! to override)"); read(0, &rc, 1); }
                }
            }
        }
        if (vcx > vlen[vcy]) vcx = vlen[vcy];
        if (vcy < vtop) vtop = vcy;            /* wrap-aware scroll to keep the cursor visible */
        for (;;) { int sr = 0; for (int j = vtop; j < vcy; j++) sr += vi_rows(j); sr += vcx / vVW; if (sr < vVH || vtop >= vcy) break; vtop++; }
    }
    tcsetattr(0, TCSANOW, &oldt);
    vout("\x1b[2J\x1b[H");
    return 0;
}

static int run_shell(int argc, char **argv);
static int ap_help(int argc, char **argv);

/* ---- dispatch ------------------------------------------------------------ */
static const struct { const char *name; int (*fn)(int, char **); } g_applets[] = {
    { "sh", run_shell }, { "echo", ap_echo }, { "cat", ap_cat }, { "grep", ap_grep },
    { "wc", ap_wc }, { "head", ap_head }, { "tail", ap_tail }, { "rev", ap_rev },
    { "tee", ap_tee }, { "nl", ap_nl }, { "yes", ap_yes }, { "seq", ap_seq },
    { "sort", ap_sort }, { "uniq", ap_uniq }, { "tr", ap_tr }, { "ls", ap_ls },
    { "cp", ap_cp }, { "mv", ap_mv }, { "rm", ap_rm }, { "mkdir", ap_mkdir },
    { "rmdir", ap_rmdir }, { "touch", ap_touch }, { "cmp", ap_cmp }, { "stat", ap_stat },
    { "basename", ap_basename }, { "dirname", ap_dirname }, { "pwd", ap_pwd },
    { "uname", ap_uname }, { "sleep", ap_sleep }, { "date", ap_date }, { "id", ap_id },
    { "whoami", ap_whoami }, { "hostname", ap_hostname }, { "env", ap_env },
    { "clear", ap_clear }, { "free", ap_free }, { "sync", ap_sync }, { "true", ap_true },
    { "false", ap_false }, { "kill", ap_kill }, { "test", ap_test }, { "[", ap_test },
    { "expr", ap_expr }, { "vi", ap_vi }, { "help", ap_help },
};
#define NAPPLET ((int)(sizeof g_applets / sizeof g_applets[0]))

static int (*applet_fn(const char *name))(int, char **) {
    for (int i = 0; i < NAPPLET; i++) if (!strcmp(g_applets[i].name, name)) return g_applets[i].fn;
    return 0;
}
static int run_applet(const char *name, int argc, char **argv) {
    int (*fn)(int, char **) = applet_fn(name);
    if (fn) return fn(argc, argv);
    outs(2, name); outs(2, ": applet not found\n");
    return 127;
}
static int ap_help(int argc, char **argv) {
    (void)argc; (void)argv;
    out1("mbb applets:");
    for (int i = 0; i < NAPPLET; i++) { write(1, (i % 10) ? " " : "\n  ", (i % 10) ? 1 : 3); out1(g_applets[i].name); }
    write(1, "\n", 1);
    return 0;
}

/* ---- pipeline + shell ---------------------------------------------------- */
static int run_pipeline(char *cmd) {
    char *stages[8]; int ns = 0;
    stages[ns++] = cmd;
    for (char *p = cmd; *p && ns < 8; p++) if (*p == '|') { *p = 0; stages[ns++] = p + 1; }

    int prev_read = -1, pids[8], np = 0;
    for (int s = 0; s < ns; s++) {
        char *argv[24]; int argc = 0;
        for (char *p = stages[s]; *p && argc < 23; ) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
        argv[argc] = 0;
        if (argc == 0) continue;

        int pfd[2] = { -1, -1 };
        if (s < ns - 1) pipe(pfd);
        int pid = fork();
        if (pid == 0) {
            signal(SIGINT, SIG_DFL);            /* foreground child dies on Ctrl-C */
            if (prev_read >= 0) { dup2(prev_read, 0); close(prev_read); }
            if (pfd[1] >= 0) { dup2(pfd[1], 1); close(pfd[1]); close(pfd[0]); }
            if (applet_fn(argv[0])) _exit(run_applet(argv[0], argc, argv));
            execv(argv[0], argv);               /* external ELF: ./foo, /abs/foo, cwd-relative foo */
            execvp(argv[0], argv);              /* PATH search for a bare name */
            outs(2, argv[0]); outs(2, ": not found\n");
            _exit(127);
        }
        pids[np++] = pid;
        if (prev_read >= 0) close(prev_read);
        if (pfd[1] >= 0) close(pfd[1]);
        prev_read = pfd[0];
    }
    if (prev_read >= 0) close(prev_read);

    int result = 0;
    for (int i = 0; i < np; i++) { int st, w = wait(&st); if (w == pids[np - 1] && WIFEXITED(st)) result = WEXITSTATUS(st); }
    return result;
}

/* Run a line: split on ';', handle shell built-ins (cd/exit), else pipeline. */
static int run_line(char *line) {
    int rc = 0;
    char *seg = line;
    for (char *p = line; ; p++) {
        if (*p == ';' || *p == 0) {
            int end = (*p == 0); *p = 0;
            const char *q = seg; while (*q == ' ') q++;
            char first[32]; int fi = 0; while (*q && *q != ' ' && fi < 31) first[fi++] = *q++;
            first[fi] = 0;
            if (!strcmp(first, "cd")) { while (*q == ' ') q++; if (chdir(*q ? q : "/") < 0) outs(2, "cd: no such directory\n"); }
            else if (!strcmp(first, "exit")) return -1;
            else if (fi) rc = run_pipeline(seg);
            if (end) break;
            seg = p + 1;
        }
    }
    return rc;
}

static int run_shell(int argc, char **argv) {
    signal(SIGINT, SIG_IGN);                    /* the shell survives Ctrl-C */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc) {
            static char buf[512]; int j = 0;
            for (const char *p = argv[i + 1]; *p && j < 511; p++) buf[j++] = *p;
            buf[j] = 0;
            int r = run_line(buf);
            return r < 0 ? 0 : r;
        }
    char line[512], cwd[256];
    for (;;) {
        if (getcwd(cwd, sizeof cwd)) out1(cwd);
        out1(" $ ");
        int li = 0; char c;
        for (;;) { int r = read(0, &c, 1); if (r <= 0) { write(1, "\n", 1); return 0; } if (c == '\n') break; if (li < 511) line[li++] = c; }
        line[li] = 0;
        if (li && run_line(line) < 0) { out1("bye\n"); return 0; }
    }
}

static const char *base(const char *p) { const char *b = p; for (const char *q = p; *q; q++) if (*q == '/') b = q + 1; return b; }

int main(int argc, char **argv) {
    const char *ap = base(argv[0]);
    if (applet_fn(ap)) return run_applet(ap, argc, argv);        /* invoked by applet name */
    if (argc < 2) {                                             /* self-test: real pipelines */
        static char a[] = "echo hello | grep hello";
        static char b[] = "seq 1 3 | wc -l | grep 3";
        int ok = (run_pipeline(a) == 0) && (run_pipeline(b) == 0);
        struct stat st;                                         /* legacy fs syscalls: mkdir/stat/rmdir */
        if (mkdir("/mbbself", 0755) != 0) ok = 0;
        else {
            struct stat rel;                                    /* relative "./" must resolve too */
            if (stat("/mbbself", &st) != 0 || !S_ISDIR(st.st_mode)) ok = 0;
            if (stat("./mbbself", &rel) != 0 || !S_ISDIR(rel.st_mode)) ok = 0;
            rmdir("/mbbself");
        }
        static char x[] = "/MBB.BIN true";                      /* shell must exec external ELFs */
        if (run_pipeline(x) != 0) ok = 0;
        if (time(0) < 1500000000L) ok = 0;                      /* wall clock seeded from UEFI GetTime */
        if (mkdir("/xt", 0755) == 0) {                          /* exec via a ".." relative path */
            if (chdir("/xt") == 0) {
                static char r[] = "../MBB.BIN true";
                if (run_pipeline(r) != 0) ok = 0;
                chdir("/");
            }
            rmdir("/xt");
        }
        return ok ? 0 : 1;
    }
    (void)ci_eq;
    return run_applet(argv[1], argc - 1, argv + 1);             /* multi-call: mbb APPLET ... */
}
