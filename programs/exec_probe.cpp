/* SPDX-License-Identifier: GPL-2.0-only */

#include "ulib.h"

extern "C" int umain() {
    int result = sys::exec("./musl.bin", "");
    if (result != 0) {
        sys::write("[elf-fs] nested pathname exec failed\n");
        return 1;
    }
    sys::write("[elf-fs] nested pathname exec returned cleanly\n");

    result = sys::exec("./tccrun.bin", "");
    if (result != 0) {
        sys::write("[tcc] on-device compilation failed\n");
        return 1;
    }
    sys::write("[tcc] compiler returned cleanly\n");

    result = sys::exec("./made.bin", "");
    if (result != 0) {
        sys::write("[tcc] generated ELF launch failed\n");
        return 1;
    }
    sys::write("[tcc] compile + execute cycle complete\n");
    return 0;
}
