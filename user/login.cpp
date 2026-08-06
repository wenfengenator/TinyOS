/* SPDX-License-Identifier: GPL-2.0-only */

// tinyos login program (C++), ring 3.
// Prompts for credentials, reads them from stdin (int 0x80 SYS_READ), then
// authenticates against /etc/passwd via the file syscalls.  On success it
// spawns the worker tasks and exits.
#include "ulib.h"

// compare "name" against a "user:pass\n"-style /etc/passwd buffer
static bool authenticate(const char *pw, int pwlen, const char *user, const char *pass) {
    int i = 0;
    while (i < pwlen) {
        // parse one line:  U ':' P '\n'
        char u[32], p[32];
        int ui = 0, pi = 0;
        while (i < pwlen && pw[i] != ':' && pw[i] != '\n' && ui < 31) u[ui++] = pw[i++];
        u[ui] = 0;
        if (i < pwlen && pw[i] == ':') i++;
        while (i < pwlen && pw[i] != '\n' && pi < 31) p[pi++] = pw[i++];
        p[pi] = 0;
        if (i < pwlen && pw[i] == '\n') i++;

        // strcmp
        bool um = true; for (int k = 0; user[k] || u[k]; k++) if (user[k] != u[k]) { um = false; break; }
        bool pm = true; for (int k = 0; pass[k] || p[k]; k++) if (pass[k] != p[k]) { pm = false; break; }
        if (um && pm) return true;
    }
    return false;
}

extern "C" void login_main() {
    char user[32], pass[32], pwbuf[128];

    sys::write("[login] tinyos secure login\n");

    sys::write("[login] username: ");
    sys::echo(0);                                   // show typed characters
    int un = sys::read(sys::STDIN, user, 31); if (un < 0) un = 0; user[un] = 0;

    sys::write("[login] password: ");
    sys::echo(1);                                   // mask with '*'
    int pn = sys::read(sys::STDIN, pass, 31); if (pn < 0) pn = 0; pass[pn] = 0;
    sys::echo(0);

    int fd = sys::open("/etc/passwd");
    int n  = (fd >= 0) ? sys::read(fd, pwbuf, 127) : 0; if (n < 0) n = 0; pwbuf[n] = 0;
    if (fd >= 0) sys::close(fd);

    if (authenticate(pwbuf, n, user, pass)) {
        sys::write("[login] authenticated. welcome, ");
        sys::write(user); sys::write("!\n");
        sys::write("[login] starting shell...\n");
        int pid = sys::exec("shell");
        if (pid < 0) sys::write("[login] exec failed\n");
        sys::exit(0);
    } else {
        sys::write("[login] authentication FAILED.\n");
        sys::exit(1);
    }
    for (;;) {}
}
