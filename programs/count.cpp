/* SPDX-License-Identifier: GPL-2.0-only */

// tinyos CPU-bound program: prints a few ticks with busy-work between them, so
// two concurrent instances visibly interleave under the preemptive scheduler.
#include "ulib.h"

static void spin(volatile int n) { while (n-- > 0) {} }

extern "C" int umain() {
    int pid = sys::getpid();
    char buf[48], nb[12];
    for (int k = 0; k < 3; k++) {
        int l = sys::strcat_(buf, 0, "  [count pid ");
        sys::utoa((unsigned)pid, nb); l = sys::strcat_(buf, l, nb);
        l = sys::strcat_(buf, l, "] "); sys::utoa((unsigned)k, nb); l = sys::strcat_(buf, l, nb);
        sys::strcat_(buf, l, "\n");
        sys::write(buf);
        spin(4000000);
    }
    return 0;
}
