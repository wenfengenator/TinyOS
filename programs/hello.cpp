/* SPDX-License-Identifier: GPL-2.0-only */

// tinyos ramdisk program: a standalone image exec'd into its own isolated,
// paged address space at USER_BASE (0x40000000).  Ring 3, syscalls via int 0x80.
#include "ulib.h"

extern "C" int umain() {
    int pid = sys::getpid();
    char buf[96], nb[12];
    int l = sys::strcat_(buf, 0, "[hello] I am a separate program from the ramdisk, pid=");
    sys::utoa((unsigned)pid, nb); l = sys::strcat_(buf, l, nb);
    l = sys::strcat_(buf, l, ", isolated @0x40000000\n");
    sys::write(buf);
    return 0;
}
