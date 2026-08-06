/* SPDX-License-Identifier: GPL-2.0-only */

// tinyos worker app (C++), ring 3.
// Tasks A and B run this same code concurrently; the scheduler preempts them
// so their output interleaves.  Each also exercises the file syscalls by
// appending a line to /tmp/log.
#include "ulib.h"

static void spin(volatile int n) { while (n-- > 0) { } }

// build "[worker <pid>] <tail>" into buf, return length
static int msg(char *buf, int pid, const char *tail) {
    char nb[12];
    int l = sys::strcat_(buf, 0, "[worker ");
    sys::utoa((unsigned)pid, nb);
    l = sys::strcat_(buf, l, nb);
    l = sys::strcat_(buf, l, "] ");
    l = sys::strcat_(buf, l, tail);
    return l;
}

extern "C" void worker_main() {
    int  pid = sys::getpid();
    char buf[80];

    msg(buf, pid, "starting\n");                 sys::write(buf);

    for (int k = 0; k < 3; k++) {
        char nb[12]; sys::utoa((unsigned)k, nb);
        char tail[24]; int t = sys::strcat_(tail, 0, "tick ");
        t = sys::strcat_(tail, t, nb); sys::strcat_(tail, t, "\n");
        msg(buf, pid, tail);                     sys::write(buf);
        spin(120000);   // burn CPU so the preemptive scheduler switches us out
    }

    // exercise the filesystem: append a record to /tmp/log
    int fd = sys::open("/tmp/log", sys::O_APPEND);
    if (fd >= 0) {
        int r = msg(buf, pid, "done\n");
        sys::write_n(fd, buf, r);
        sys::close(fd);
    }

    msg(buf, pid, "exiting\n");                   sys::write(buf);
    sys::exit(0);
    for (;;) {}
}
