#include "ulib.h"

extern "C" int umain() {
    int result = sys::exec("./musl.bin", "");
    if (result < 0) {
        sys::write("[elf-fs] nested pathname exec failed\n");
        return 1;
    }
    sys::write("[elf-fs] nested pathname exec returned cleanly\n");
    return result;
}
