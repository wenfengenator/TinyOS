// tinyos user-space runtime (C++), ring 3.
// The only channel to the kernel is `int 0x80` (see syscall() below).
#ifndef TINYOS_ULIB_H
#define TINYOS_ULIB_H

namespace sys {

enum {                          // must match kernel/kernel.h
    EXIT = 1, READ = 3, WRITE = 4, OPEN = 5, CLOSE = 6,
    YIELD = 7, GETPID = 8, SPAWN = 10,
    EXEC = 9, STAT = 11, LSEEK = 12, FORK = 13, WAIT = 14, READDIR = 15, TASKINFO = 16, ECHO = 17, READFILE = 18,
    GETCHAR = 19, CLEAR = 20, GOTOXY = 21, WRITEFILE = 22,
};
enum { STDIN = 0, STDOUT = 1 };
enum {                          // extended key codes from the PS/2 driver (match kernel.h)
    KEY_UP = 0x80, KEY_DOWN = 0x81, KEY_LEFT = 0x82, KEY_RIGHT = 0x83,
    KEY_DEL = 0x84, KEY_HOME = 0x85, KEY_END = 0x86, KEY_PGUP = 0x87, KEY_PGDN = 0x88,
};

static inline int syscall(int n, int a = 0, int b = 0, int c = 0) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}

static inline unsigned strlen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

// stdout / console
static inline int  write(const char *s)               { return syscall(WRITE, STDOUT, (int)s, (int)strlen(s)); }
static inline int  write_n(int fd, const char *s, int n){ return syscall(WRITE, fd, (int)s, n); }
// stdin / files
static inline int  read(int fd, char *b, int n)       { return syscall(READ, fd, (int)b, n); }
static inline int  open(const char *path, int flags = 0) { return syscall(OPEN, (int)path, flags); }
    enum { O_APPEND = 1 };
static inline int  close(int fd)                      { return syscall(CLOSE, fd); }
// process
static inline void exit(int code)                     { syscall(EXIT, code); }
static inline int  getpid()                           { return syscall(GETPID); }
static inline void yield()                            { syscall(YIELD); }
static inline void spawn(int task_index)              { syscall(SPAWN, task_index); }
static inline void echo(int mode)                     { syscall(ECHO, mode); }

// tiny unsigned-to-decimal; writes into buf, returns length
static inline int utoa(unsigned v, char *buf) {
    char tmp[12]; int i = 0;
    do { tmp[i++] = char('0' + v % 10); v /= 10; } while (v);
    for (int j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = 0; return i;
}

// append src to dst (which must be big enough); returns new length
static inline int strcat_(char *dst, int len, const char *src) {
    int i = 0; while (src[i]) { dst[len + i] = src[i]; i++; } dst[len + i] = 0; return len + i;
}


// ---- process / fs extras ----
struct stat_t { unsigned size; unsigned isdir; };
static inline int  exec(const char *path, const char *arg = 0) { return syscall(EXEC, (int)path, (int)arg); }
static inline int  fork()                            { return syscall(FORK); }
static inline int  wait(int pid)                     { return syscall(WAIT, pid); }
static inline int  stat(const char *path, stat_t *st){ return syscall(STAT, (int)path, (int)st); }
static inline int  lseek(int fd, int off, int wh)    { return syscall(LSEEK, fd, off, wh); }
static inline int  readdir(const char *dir, int i, char *name) { return syscall(READDIR, (int)dir, i, (int)name); }
static inline int  readfile(const char *path, char *buf, int max){ return syscall(READFILE, (int)path, (int)buf, max); }
static inline int  writefile(const char *path, const char *buf, int n){ return syscall(WRITEFILE, (int)path, (int)buf, n); }
static inline int  getchar()                         { return syscall(GETCHAR); }
static inline void clear()                           { syscall(CLEAR); }
static inline void gotoxy(int row, int col)          { syscall(GOTOXY, row, col); }
static inline int  taskinfo(int i, void *buf)        { return syscall(TASKINFO, i, (int)buf); }

} // namespace sys
#endif
