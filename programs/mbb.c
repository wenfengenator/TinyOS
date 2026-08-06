/* SPDX-License-Identifier: GPL-2.0-only */

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
#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "tinygl.h"        /* TinyGL client library (the UMD under test) */
#include "tnet.h"          /* kernel DNS resolver + ICMP echo helpers */

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
/* pager prompt: one raw key from /dev/tty (space=page, Enter=line, q=quit) */
static int more_key(int tty) {
    struct termios old, raw;
    tcgetattr(1, &old); raw = old; raw.c_lflag &= ~(ICANON | ECHO); tcsetattr(1, TCSANOW, &raw);
    out1("\x1b[7m--More--\x1b[0m");
    char c = ' '; if (read(tty, &c, 1) <= 0) c = 'q';
    tcsetattr(1, TCSANOW, &old);
    out1("\r\x1b[K");                              /* erase the prompt */
    return (unsigned char)c;
}
/* more [file...] — page long output a screenful at a time.  Keys come from
 * /dev/tty so pagination works even when the content is piped (`ls | more`). */
static int ap_more(int argc, char **argv) {
    struct winsize ws; int rows = 24;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 1) rows = ws.ws_row;
    int tty = open("/dev/tty", O_RDONLY);
    int nf = 0; for (int i = 1; i < argc; i++) if (argv[i][0] != '-') nf++;
    int quit = 0, rc = 0, shown = 0;
    for (int idx = 0; !quit; idx++) {
        int fd;
        if (nf) {
            int fi = -1, seen = 0;
            for (int i = 1; i < argc; i++) if (argv[i][0] != '-' && seen++ == idx) { fi = i; break; }
            if (fi < 0) break;
            fd = open(argv[fi], O_RDONLY);
            if (fd < 0) { outs(2, "more: "); outs(2, argv[fi]); outs(2, ": not found\n"); rc = 1; continue; }
        } else { if (idx > 0) break; fd = 0; }
        char b[512]; int n;
        while (!quit && (n = read(fd, b, sizeof b)) > 0) {
            for (int j = 0; j < n; j++) {
                write(1, &b[j], 1);
                if (b[j] != '\n') continue;
                if (++shown < rows - 1) continue;
                if (tty < 0) { shown = 0; continue; }         /* no key source: keep scrolling */
                int k = more_key(tty);
                if (k == 'q' || k == 'Q') { quit = 1; break; }
                shown = (k == '\n' || k == '\r') ? rows - 2 : 0;   /* Enter=one line, else a page */
            }
        }
        if (fd != 0) close(fd);
    }
    if (tty >= 0) close(tty);
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
/* join dir + '/' + name -> out (bounded) */
static void cp_join(char *out, int cap, const char *dir, const char *name) {
    int n = 0;
    for (const char *p = dir; *p && n < cap - 2; p++) out[n++] = *p;
    if (n && out[n - 1] != '/') out[n++] = '/';
    for (const char *p = name; *p && n < cap - 1; p++) out[n++] = *p;
    out[n] = 0;
}
static const char *cp_base(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
    return b;
}
static int cp_is_dir(const char *p) {
    struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
/* copy one regular file src -> dst (full paths); routes across /, /mnt, /usb */
static int cp_file(const char *src, const char *dst) {
    int s = open(src, O_RDONLY);
    if (s < 0) { outs(2, "cp: cannot open "); outs(2, src); outs(2, "\n"); return 1; }
    int d = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (d < 0) { close(s); outs(2, "cp: cannot create "); outs(2, dst); outs(2, "\n"); return 1; }
    char b[512]; int n, rc = 0;
    while ((n = read(s, b, sizeof b)) > 0) if (write(d, b, n) != n) { rc = 1; break; }
    if (n < 0) rc = 1;
    close(s); close(d);
    return rc;
}
/* copy src -> dst; recurse into directories when rflag is set */
static int cp_tree(const char *src, const char *dst, int rflag) {
    struct stat st;
    if (stat(src, &st) != 0) { outs(2, "cp: "); outs(2, src); outs(2, ": not found\n"); return 1; }
    if (!S_ISDIR(st.st_mode)) return cp_file(src, dst);
    if (!rflag) { outs(2, "cp: omitting directory "); outs(2, src); outs(2, " (use -r)\n"); return 1; }
    mkdir(dst, 0755);                                  /* ok if it already exists */
    DIR *dir = opendir(src);
    if (!dir) { outs(2, "cp: cannot read "); outs(2, src); outs(2, "\n"); return 1; }
    struct dirent *e; int rc = 0;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        char sp[288], dp[288];
        cp_join(sp, sizeof sp, src, e->d_name);
        cp_join(dp, sizeof dp, dst, e->d_name);
        if (cp_tree(sp, dp, rflag)) rc = 1;
    }
    closedir(dir);
    return rc;
}
/* cp [-r] SRC... DST  — like coreutils cp: recursive, multiple sources into a
 * directory, and works across the /, /mnt and /usb filesystems. */
static int ap_cp(int argc, char **argv) {
    int rflag = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r' || *f == 'R' || *f == 'a') rflag = 1;
            else { outs(2, "cp: unknown option -"); write(2, f, 1); outs(2, "\n"); return 1; }
        }
    if (argc - i < 2) { outs(2, "usage: cp [-r] SRC... DST\n"); return 1; }
    const char *dst = argv[argc - 1];
    int into_dir = cp_is_dir(dst);
    if (argc - i > 2 && !into_dir) { outs(2, "cp: target '"); outs(2, dst); outs(2, "' is not a directory\n"); return 1; }
    int rc = 0;
    for (int k = i; k < argc - 1; k++) {
        if (into_dir) {
            char dp[288]; cp_join(dp, sizeof dp, dst, cp_base(argv[k]));
            if (cp_tree(argv[k], dp, rflag)) rc = 1;
        } else if (cp_tree(argv[k], dst, rflag)) rc = 1;
    }
    return rc;
}
/* mv [-r] SRC... DST  — copy then remove the sources (files only). */
static int ap_mv(int argc, char **argv) {
    if (ap_cp(argc, argv)) return 1;
    int i = 1; for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {}
    for (int k = i; k < argc - 1; k++) unlink(argv[k]);
    return 0;
}
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
#define VI_MAXL (512 * 3)
#define VI_MAXC (200 * 2)
#define VI_GUT  5
#define VK_UP 0x80
#define VK_DOWN 0x81
#define VK_LEFT 0x82
#define VK_RIGHT 0x83
static char vbuf[VI_MAXL][VI_MAXC + 2];
static int  vlen[VI_MAXL], vnl, vcx, vcy, vtop, vVH = 23, vVW = 75, vdirty;
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
                num[0] = (ln >= 1000) ? (char)('0' + (ln / 1000) % 10) : ' ';
                num[1] = (ln >= 100)  ? (char)('0' + (ln / 100) % 10)  : ' ';
                num[2] = (ln >= 10)   ? (char)('0' + (ln / 10) % 10)   : ' ';
                num[3] = (char)('0' + ln % 10); num[4] = ' ';
            } else { num[0] = num[1] = num[2] = num[3] = num[4] = ' '; }
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

    int ins = 0, run = 1, count = 0;           /* count = pending numeric prefix, e.g. 10G */
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
            if ((c >= '1' && c <= '9') || (c == '0' && count > 0)) { count = count * 10 + (c - '0'); continue; }
            int cnt = count; count = 0;        /* consume any pending [count] for this command */
            if (c == 'h' || c == VK_LEFT) { if (vcx > 0) vcx--; }
            else if (c == 'l' || c == VK_RIGHT) { if (vcx < vlen[vcy]) vcx++; }
            else if (c == 'j' || c == VK_DOWN) { if (vcy < vnl - 1) { vcy++; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == 'k' || c == VK_UP) { if (vcy > 0) { vcy--; if (vcx > vlen[vcy]) vcx = vlen[vcy]; } }
            else if (c == '0') vcx = 0;
            else if (c == '$') vcx = vlen[vcy] ? vlen[vcy] - 1 : 0;
            else if (c == 'G') { vcy = cnt > 0 ? cnt - 1 : vnl - 1; if (vcy >= vnl) vcy = vnl - 1; if (vcy < 0) vcy = 0; vcx = 0; }
            else if (c == 'g') { char g2; if (read(0, &g2, 1) > 0 && g2 == 'g') { vcy = cnt > 0 ? cnt - 1 : 0; if (vcy >= vnl) vcy = vnl - 1; if (vcy < 0) vcy = 0; vcx = 0; } }
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

/* ---- net: ping + a tiny text-mode web browser ---------------------------- */
/* case-insensitive prefix compare: 0 when a[0..n) equals b[0..n) */
static int ncase(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]); if (ca != cb) return 1; if (!ca) return 0; }
    return 0;
}
static int starts(const char *s, const char *pfx) { return ncase(s, pfx, (int)strlen(pfx)) == 0; }

/* dotted-quad literal or DNS name -> IPv4 in host order (0 on failure) */
static uint32_t resolve_host(const char *h) {
    uint32_t lit = ntohl(inet_addr(h));
    if (lit != 0xFFFFFFFFu && lit != 0) return lit;
    return net_resolve(h);
}
static void put_ip(int fd, uint32_t ip) {
    outnum(fd, (ip >> 24) & 255); write(fd, ".", 1); outnum(fd, (ip >> 16) & 255); write(fd, ".", 1);
    outnum(fd, (ip >> 8) & 255);  write(fd, ".", 1); outnum(fd, ip & 255);
}
static void put_hex2(int fd, unsigned v) {
    static const char h[] = "0123456789abcdef";
    char b[2] = { h[(v >> 4) & 15], h[v & 15] }; write(fd, b, 2);
}

/* ping [-c count] host  — ICMP echo via the kernel (NET_SYS_PING). */
static int ap_ping(int argc, char **argv) {
    int count = 4, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++)
        if (!strcmp(argv[i], "-c") && i + 1 < argc) { count = atoi(argv[++i]); if (count <= 0) count = 4; }
    if (i >= argc) { outs(2, "usage: ping [-c count] host\n"); return 2; }
    const char *host = argv[i];
    uint32_t ip = resolve_host(host);
    if (!ip) { outs(2, "ping: cannot resolve "); outs(2, host); outs(2, "\n"); return 1; }
    out1("PING "); out1(host); out1(" ("); put_ip(1, ip); out1(")\n");
    int rx = 0;
    for (int s = 1; s <= count; s++) {
        long us = net_ping(ip);
        if (us >= 0) {
            rx++;
            out1("reply from "); put_ip(1, ip); out1(": seq="); outnum(1, s);
            out1(" time="); outnum(1, us / 1000); write(1, ".", 1); outnum(1, (us % 1000) / 100); out1(" ms\n");
        } else { out1("request timeout: seq="); outnum(1, s); write(1, "\n", 1); }
        if (s < count) { struct timespec ts = { 1, 0 }; nanosleep(&ts, 0); }
    }
    out1("--- "); out1(host); out1(" ping statistics ---\n");
    outnum(1, count); out1(" transmitted, "); outnum(1, rx); out1(" received\n");
    return rx > 0 ? 0 : 1;
}

/* ifconfig [ip mask gw [dns]] — show, or set a static interface config. */
static int ap_ifconfig(int argc, char **argv) {
    if (argc >= 4) {                                  /* static config from dotted-quad args */
        uint32_t ip = ntohl(inet_addr(argv[1])), mask = ntohl(inet_addr(argv[2]));
        uint32_t gw = ntohl(inet_addr(argv[3])), dns = argc > 4 ? ntohl(inet_addr(argv[4])) : gw;
        if (net_setcfg(ip, mask, gw, dns) != 0) { outs(2, "ifconfig: set failed\n"); return 1; }
    }
    struct tnet_ifcfg c;
    if (net_ifconfig(&c) != 0) { outs(2, "ifconfig: network unavailable\n"); return 1; }
    out1("eth0      Link encap:Ethernet  HWaddr ");
    for (int i = 0; i < 6; i++) { if (i) write(1, ":", 1); put_hex2(1, c.mac[i]); }
    write(1, "\n", 1);
    out1("          inet "); put_ip(1, c.ip); out1("  netmask "); put_ip(1, c.mask);
    out1("  gateway "); put_ip(1, c.gw); write(1, "\n", 1);
    out1("          dns "); put_ip(1, c.dns); out1("  MTU 1500  status ");
    out1(c.up ? "UP\n" : "DOWN\n");
    out1("lo        Link encap:Local Loopback\n");
    out1("          inet 127.0.0.1  netmask 255.0.0.0  status UP\n");
    return 0;
}

/* --- text web browser: fetch HTTP and render HTML as flowing text --- */
#define WEB_CAP  (128 * 1024)
#define WEB_MAXL 128
#define WEB_URLM 512
static char web_buf[WEB_CAP];
static char web_host[256];
static int  web_port;
static char web_path[WEB_URLM];
static char web_links[WEB_MAXL][WEB_URLM];
static int  web_nlink;
static int  web_pnl, web_psp;          /* pending newlines / pending space (whitespace collapse) */

static void web_emit(char c) {
    if (c == '\n') { if (web_pnl < 2) web_pnl++; web_psp = 0; return; }
    if (c == ' ' || c == '\t' || c == '\r') { web_psp = 1; return; }
    while (web_pnl > 0) { write(1, "\n", 1); web_pnl--; }
    if (web_psp) { write(1, " ", 1); web_psp = 0; }
    write(1, &c, 1);
}
static void web_emits(const char *s) { while (*s) web_emit(*s++); }

static int web_is_block(const char *n) {
    static const char *b[] = { "p", "br", "div", "li", "tr", "ul", "ol", "hr", "h1", "h2", "h3", "h4",
                               "h5", "h6", "table", "thead", "tbody", "section", "article", "header",
                               "footer", "nav", "blockquote", "pre", "form", "dl", "dt", "dd", 0 };
    for (int i = 0; b[i]; i++) if (!strcmp(n, b[i])) return 1;
    return 0;
}
static void web_entity(const char *e) {
    static const struct { const char *k, *v; } t[] = {
        { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" }, { "apos", "'" },
        { "nbsp", " " }, { "copy", "(c)" }, { "reg", "(r)" }, { "trade", "(tm)" }, { "mdash", "--" },
        { "ndash", "-" }, { "hellip", "..." }, { "raquo", ">>" }, { "laquo", "<<" }, { "middot", "*" }, { 0, 0 } };
    if (e[0] == '#') {                                     /* numeric entity */
        long cp = (e[1] == 'x' || e[1] == 'X') ? strtol(e + 2, 0, 16) : atol(e + 1);
        if (cp >= 32 && cp < 127) web_emit((char)cp);
        else if (cp == 160) web_emit(' ');
        else web_emit('?');
        return;
    }
    for (int i = 0; t[i].k; i++) if (!strcmp(e, t[i].k)) { web_emits(t[i].v); return; }
    web_emit('&'); web_emits(e); web_emit(';');           /* unknown: leave literal */
}
/* extract attribute `name`'s value from the tag body [s,end); 1 if found */
static int web_attr(const char *s, const char *end, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    for (const char *p = s; p + nl < end; p++) {
        if ((p == s || !isalnum((unsigned char)p[-1])) && ncase(p, name, nl) == 0) {
            const char *q = p + nl; while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q < end && *q == '=') {
                q++; while (q < end && (*q == ' ' || *q == '\t')) q++;
                char quote = 0; if (q < end && (*q == '"' || *q == '\'')) quote = *q++;
                int o = 0;
                while (q < end && o < cap - 1) {
                    if (quote) { if (*q == quote) break; } else if (*q == ' ' || *q == '\t' || *q == '>') break;
                    out[o++] = *q++;
                }
                out[o] = 0; return 1;
            }
        }
    }
    return 0;
}
static void web_render(const char *body) {
    web_pnl = web_psp = 0; web_nlink = 0;
    int in_script = 0, in_style = 0, cur = -1;
    for (const char *p = body; *p; ) {
        if (*p == '<') {
            const char *t = p + 1; int close = (*t == '/'); if (close) t++;
            char name[16]; int ni = 0;
            while (*t && isalnum((unsigned char)*t) && ni < 15) name[ni++] = (char)tolower((unsigned char)*t++);
            name[ni] = 0;
            const char *e = p + 1; while (*e && *e != '>') e++;
            if (!strcmp(name, "script")) in_script = !close;
            else if (!strcmp(name, "style")) in_style = !close;
            else if (!in_script && !in_style && ni > 0) {
                if (!strcmp(name, "a")) {
                    if (!close) {
                        char href[WEB_URLM];
                        if (web_attr(p + 1, e, "href", href, sizeof href) && href[0] && href[0] != '#' && web_nlink < WEB_MAXL) {
                            strncpy(web_links[web_nlink], href, WEB_URLM - 1); web_links[web_nlink][WEB_URLM - 1] = 0;
                            cur = web_nlink++;
                        }
                    } else if (cur >= 0) { char m[16]; snprintf(m, sizeof m, "[%d]", cur + 1); web_emits(m); cur = -1; }
                } else if (web_is_block(name)) {
                    web_emit('\n');
                    if (!strcmp(name, "li") && !close) web_emits("- ");
                }
            }
            p = (*e == '>') ? e + 1 : e;
            continue;
        }
        if (in_script || in_style) { p++; continue; }
        if (*p == '&') {
            char ent[12]; int k = 0; const char *q = p + 1;
            while (*q && *q != ';' && *q != '<' && *q != ' ' && *q != '&' && k < 10) ent[k++] = *q++;
            ent[k] = 0;
            if (*q == ';' && k > 0) { web_entity(ent); p = q + 1; continue; }
            web_emit('&'); p++; continue;
        }
        web_emit(*p); p++;
    }
    write(1, "\n", 1);
}
/* split a URL into web_host/web_port/web_path; -1 for https or a malformed URL */
static int web_parse(const char *url) {
    const char *p = url;
    if (starts(p, "http://")) p += 7;
    else if (starts(p, "https://")) return -1;
    int hi = 0; web_port = 80;
    while (*p && *p != '/' && *p != ':' && hi < 255) web_host[hi++] = *p++;
    web_host[hi] = 0;
    if (!web_host[0]) return -1;
    if (*p == ':') { p++; web_port = atoi(p); while (*p && *p != '/') p++; }
    if (*p == '/') { int pi = 0; while (*p && pi < WEB_URLM - 1) web_path[pi++] = *p++; web_path[pi] = 0; }
    else strcpy(web_path, "/");
    return 0;
}
static int web_fetch(void) {                              /* GET web_path into web_buf; body length or -1 */
    uint32_t ip = resolve_host(web_host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return -1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)web_port); a.sin_addr.s_addr = htonl(ip);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { close(s); return -1; }
    char req[2048];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: tinyos-web/1.0\r\nConnection: close\r\n\r\n",
                      web_path, web_host);
    if (rl < 0) { close(s); return -1; }
    if (rl >= (int)sizeof req) rl = (int)sizeof req - 1;
    if (write(s, req, rl) != rl) { close(s); return -1; }
    int len = 0, n;
    while (len < WEB_CAP - 1 && (n = read(s, web_buf + len, WEB_CAP - 1 - len)) > 0) len += n;
    web_buf[len] = 0; close(s);
    return len;
}
/* case-insensitive header lookup within [hdr,end); 1 if found */
static int web_header(const char *hdr, const char *end, const char *name, char *out, int cap) {
    int nl = (int)strlen(name);
    for (const char *p = hdr; p < end; ) {
        const char *le = p; while (le < end && *le != '\n') le++;
        if (le - p > nl && ncase(p, name, nl) == 0 && p[nl] == ':') {
            const char *v = p + nl + 1; while (v < le && (*v == ' ' || *v == '\t')) v++;
            const char *ve = le; while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ')) ve--;
            int o = 0; for (const char *q = v; q < ve && o < cap - 1; q++) out[o++] = *q; out[o] = 0; return 1;
        }
        p = (le < end) ? le + 1 : end;
    }
    return 0;
}
/* resolve `link` (absolute, root-relative or relative) against the current page -> absolute http URL */
static void web_abs(char *out, int cap, const char *link) {
    if (starts(link, "http://") || starts(link, "https://")) { strncpy(out, link, cap - 1); out[cap - 1] = 0; return; }
    int o = 0;
    for (const char *p = "http://"; *p && o < cap - 1; p++) out[o++] = *p;
    for (const char *p = web_host; *p && o < cap - 1; p++) out[o++] = *p;
    if (web_port != 80) { char pb[8]; snprintf(pb, sizeof pb, ":%d", web_port); for (char *q = pb; *q && o < cap - 1; q++) out[o++] = *q; }
    if (link[0] == '/') { for (const char *p = link; *p && o < cap - 1; p++) out[o++] = *p; }
    else {
        int cut = 0; for (int i = 0; web_path[i]; i++) if (web_path[i] == '/') cut = i + 1;
        for (int i = 0; i < cut && o < cap - 1; i++) out[o++] = web_path[i];
        for (const char *p = link; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}
/* load a URL: parse, fetch, follow http redirects, print status + rendered text */
static int web_load(const char *url) {
    char cur[WEB_URLM]; strncpy(cur, url, sizeof cur - 1); cur[sizeof cur - 1] = 0;
    for (int hop = 0; hop < 5; hop++) {
        if (web_parse(cur) != 0) { outs(2, "web: only http:// URLs are supported\n"); return -1; }
        out1("[fetching "); out1(web_host); out1(web_path); out1("]\n");
        if (web_fetch() < 0) { outs(2, "web: cannot fetch "); outs(2, web_host); outs(2, "\n"); return -1; }
        char *hdr_end = strstr(web_buf, "\r\n\r\n");
        int code = 0;
        if (starts(web_buf, "http/")) { const char *sp = web_buf; while (*sp && *sp != ' ') sp++; code = atoi(sp); }
        if (hdr_end) {
            const char *nl = strchr(web_buf, '\n');       /* echo the status line */
            if (nl) { int sl = (int)(nl - web_buf); if (sl > 0 && web_buf[sl - 1] == '\r') sl--; write(1, web_buf, sl); write(1, "\n", 1); }
            if (code >= 300 && code < 400) {
                char loc[WEB_URLM];
                if (web_header(web_buf, hdr_end, "location", loc, sizeof loc) && loc[0]) {
                    if (starts(loc, "https://")) { outs(2, "web: redirect to https (TLS unsupported): "); outs(2, loc); outs(2, "\n"); return -1; }
                    char nx[WEB_URLM]; web_abs(nx, sizeof nx, loc);
                    strncpy(cur, nx, sizeof cur - 1); cur[sizeof cur - 1] = 0;
                    continue;
                }
            }
            web_render(hdr_end + 4);
        } else web_render(web_buf);
        return 0;
    }
    outs(2, "web: too many redirects\n");
    return -1;
}
static int ap_web(int argc, char **argv) {
    if (argc < 2) { outs(2, "usage: web <url>   (http only, e.g. web example.com  or  web http://10.0.2.2/)\n"); return 1; }
    char url[WEB_URLM]; strncpy(url, argv[1], sizeof url - 1); url[sizeof url - 1] = 0;
    for (;;) {
        out1("\n");
        if (web_load(url) == 0 && web_nlink > 0) {
            out1("\n---- links ----\n");
            for (int i = 0; i < web_nlink; i++) { char n[16]; snprintf(n, sizeof n, "[%d] ", i + 1); out1(n); out1(web_links[i]); write(1, "\n", 1); }
        }
        out1("\n[web] number=open link, u <url>=go, r=reload, q=quit > ");
        char line[WEB_URLM]; int li = 0;
        for (;;) { char ch; if (read(0, &ch, 1) <= 0) { write(1, "\n", 1); return 0; } if (ch == '\n') break; if (li < (int)sizeof line - 1) line[li++] = ch; }
        line[li] = 0;
        char *s = line; while (*s == ' ') s++;
        char *e = s + strlen(s); while (e > s && (e[-1] == ' ' || e[-1] == '\r')) *--e = 0;
        if (!*s) continue;
        if (!strcmp(s, "q")) break;
        if (!strcmp(s, "r")) continue;
        if ((s[0] == 'u' || s[0] == 'g') && s[1] == ' ') { char *u = s + 2; while (*u == ' ') u++; strncpy(url, u, sizeof url - 1); url[sizeof url - 1] = 0; continue; }
        int isnum = 1; for (char *t = s; *t; t++) if (*t < '0' || *t > '9') { isnum = 0; break; }
        if (isnum) {
            int idx = atoi(s);
            if (idx >= 1 && idx <= web_nlink) { char nx[WEB_URLM]; web_abs(nx, sizeof nx, web_links[idx - 1]); strncpy(url, nx, sizeof url - 1); url[sizeof url - 1] = 0; }
            else out1("no such link\n");
            continue;
        }
        strncpy(url, s, sizeof url - 1); url[sizeof url - 1] = 0;
    }
    return 0;
}

/* ---- dispatch ------------------------------------------------------------ */
static const struct { const char *name; int (*fn)(int, char **); } g_applets[] = {
    { "sh", run_shell }, { "echo", ap_echo }, { "cat", ap_cat }, { "more", ap_more }, { "grep", ap_grep },
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
    { "expr", ap_expr }, { "vi", ap_vi }, { "ping", ap_ping }, { "web", ap_web },
    { "ifconfig", ap_ifconfig }, { "help", ap_help },
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

/* Locate and exec a command.  A name containing '/' is taken as a literal path
 * (/abs/foo or ./rel/foo).  A bare name is resolved as a "command on the path"
 * WITHOUT needing ./ or a cwd match: TinyOS ships programs as uppercase 8.3
 * files at the root (/GIT.BIN, /WGET.BIN, ...) and FAT lookup is case-
 * insensitive, so `git` -> /git.bin -> /GIT.BIN works from any directory.
 * Returns only on failure (control returns iff no exec succeeded). */
static void exec_command(char **argv) {
    const char *cmd = argv[0];
    if (strchr(cmd, '/')) { execv(cmd, argv); return; }         /* explicit path */
    char cand[128];
    snprintf(cand, sizeof cand, "/%s", cmd);         execv(cand, argv);  /* /git       */
    snprintf(cand, sizeof cand, "/%s.bin", cmd);     execv(cand, argv);  /* /GIT.BIN   */
    snprintf(cand, sizeof cand, "/bin/%s", cmd);     execv(cand, argv);  /* /bin/git   */
    snprintf(cand, sizeof cand, "/bin/%s.bin", cmd); execv(cand, argv);  /* /bin/GIT.BIN */
    execvp(cmd, argv);                                          /* PATH fallback */
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
            exec_command(argv);                 /* /abs, ./rel, or a bare name -> /NAME.BIN on the path */
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

/* Headless TinyGL check: open a zero-copy buffer object, draw into the mapped
 * pixels, and read them back through the mapping (proving the app writes land in
 * the kernel BO with no copy).  Does not present, so it never disturbs the boot
 * display.  0 = ok. */
static int gfx_selftest(void) {
    tgl_t g;
    if (tgl_open(&g, 64, 48) != 0) return -1;
    tgl_clear(&g, 0x123456);
    tgl_pixel(&g, 10, 10, 0xABCDEF);
    uint32_t want_bg = tgl_rgb(&g, 0x123456), want_px = tgl_rgb(&g, 0xABCDEF);
    int ok = g.px && g.px[0] == want_bg && g.px[10 * g.stride + 10] == want_px;
    tgl_finish(&g);
    return ok ? 0 : -2;
}

/* Headless 3D check: rasterize into a mapped BO offscreen and confirm the depth
 * test and back-face cull select the right fragments (the G4 gate, user-side). */
static int gfx3d_selftest(void) {
    tgl_t g;
    if (tgl_open(&g, 64, 64) != 0) return -1;
    tgl3d_t r;
    if (tgl3d_begin(&r, &g) != 0) { tgl_finish(&g); return -1; }
    uint32_t rp = tgl_rgb(&g, 0xFF0000), gp = tgl_rgb(&g, 0x00FF00);
    tgl_tex rtex = { &rp, 1, 1 }, gtex = { &gp, 1, 1 };
    tgl_vert na = {2,2,TGL_FXONE,0,0}, nb = {61,2,TGL_FXONE,0,0}, nc = {30,61,TGL_FXONE,0,0};
    tgl_vert fa = {2,2,TGL_FXONE/2,0,0}, fb2 = {61,2,TGL_FXONE/2,0,0}, fc = {30,61,TGL_FXONE/2,0,0};
    int ci = 20 * g.stride + 30;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r); tgl3d_cull(&r, 0);
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* near red */
    int ok = g.px[ci] == rp;
    tgl3d_tri(&r, &fa, &fb2, &fc, &gtex, 256);               /* far green loses depth */
    ok = ok && g.px[ci] == rp;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r);
    tgl3d_tri(&r, &fa, &fb2, &fc, &gtex, 256);               /* far first */
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* near wins regardless of order */
    ok = ok && g.px[ci] == rp;

    tgl_clear(&g, 0x000000); tgl3d_clear_depth(&r); tgl3d_cull(&r, 1);
    tgl3d_tri(&r, &nc, &nb, &na, &rtex, 256);                /* reversed winding -> culled */
    ok = ok && g.px[ci] == 0u;
    tgl3d_tri(&r, &na, &nb, &nc, &rtex, 256);                /* front-facing -> drawn */
    ok = ok && g.px[ci] == rp;

    tgl3d_end(&r); tgl_finish(&g);
    return ok ? 0 : -2;
}

/* Headless socket check: a local server and client over the kernel TCP loopback
 * exchange bytes both ways using the BSD socket syscalls (the N3 gate). */
static int net_sock_selftest(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return -1;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(9100);
    a.sin_addr.s_addr = htonl(0x0A00020Fu);          /* 10.0.2.15 -> kernel loopback */
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0) return -2;
    if (listen(ls, 1) != 0) return -3;
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    if (cs < 0) return -4;
    if (connect(cs, (struct sockaddr *)&a, sizeof a) != 0) return -5;
    int as = accept(ls, 0, 0);
    if (as < 0) return -6;
    if (write(cs, "ping", 4) != 4) return -7;
    char b[16];
    int n = read(as, b, sizeof b);
    if (n != 4 || memcmp(b, "ping", 4)) return -8;
    if (write(as, "pong!", 5) != 5) return -9;
    n = read(cs, b, sizeof b);
    if (n != 5 || memcmp(b, "pong!", 5)) return -10;
    close(cs); close(as); close(ls);
    return 0;
}

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
        outs(1, gfx_selftest() == 0 ? "[tinygl-umd] BO map + draw + zero-copy readback ok\n"
                                    : "[tinygl-umd] BO self-test FAILED\n");
        outs(1, gfx3d_selftest() == 0 ? "[tinygl-umd] 3D raster + depth + cull ok\n"
                                      : "[tinygl-umd] 3D self-test FAILED\n");
        outs(1, net_sock_selftest() == 0 ? "[net-umd] socket loopback connect+data+close ok\n"
                                         : "[net-umd] socket self-test FAILED\n");
        return ok ? 0 : 1;
    }
    (void)ci_eq;
    return run_applet(argv[1], argc - 1, argv + 1);             /* multi-call: mbb APPLET ... */
}
