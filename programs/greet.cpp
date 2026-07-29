// tinyos interactive ring-3 demo: prompts, reads a line you type in the terminal
// (via SYS_READ), then greets you.  Proves keyboard input reaches user mode.
#include "ulib.h"

extern "C" int umain() {
    char name[64];
    sys::write("what's your name? ");
    int n = sys::read(sys::STDIN, name, 63);
    if (n < 0) n = 0;
    name[n] = 0;
    sys::write("hello, ");
    sys::write(name[0] ? name : "stranger");
    sys::write("! you are running in ring 3.\n");
    return 0;
}
