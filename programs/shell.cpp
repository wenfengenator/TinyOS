// tinyos shell (C++), a ramdisk program run in its own isolated address space.
// Reads commands from the keyboard; navigates the unified VFS (ram fs + /mnt
// FAT16 disk) with a current working directory.
#include "ulib.h"

static bool streq(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}

static char g_cwd[128] = "/";

// resolve arg (absolute or relative to cwd) into a normalized absolute path
static void resolve(const char *arg, char *out) {
    char comps[24][24]; int nc = 0;
    const char *srcs[2]; int ns = 0;
    if (arg[0] != '/') srcs[ns++] = g_cwd;
    srcs[ns++] = arg;
    for (int s = 0; s < ns; s++) {
        const char *p = srcs[s];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            char c[24]; int k = 0; while (*p && *p != '/' && k < 23) c[k++] = *p++; c[k] = 0;
            if (streq(c, ".")) continue;
            if (streq(c, "..")) { if (nc > 0) nc--; continue; }
            if (nc < 24) { int j = 0; while (c[j]) { comps[nc][j] = c[j]; j++; } comps[nc][j] = 0; nc++; }
        }
    }
    if (nc == 0) { out[0] = '/'; out[1] = 0; return; }
    int t = 0;
    for (int i = 0; i < nc; i++) { out[t++] = '/'; int j = 0; while (comps[i][j]) out[t++] = comps[i][j++]; }
    out[t] = 0;
}

static void split(const char *line, char *cmd, char *arg) {
    int i = 0, j = 0;
    while (line[i] == ' ') i++;
    while (line[i] && line[i] != ' ') { cmd[j++] = line[i++]; } cmd[j] = 0;
    while (line[i] == ' ') i++;
    j = 0;
    while (line[i] && line[i] != ' ') { arg[j++] = line[i++]; } arg[j] = 0;
}

struct taskinfo_t { unsigned pid, state; char name[16]; };
static const char *STATES[] = { "unused", "ready", "run", "block", "exit" };

extern "C" int umain() {
    sys::write("[shell] tinyos shell. type 'help'.\n");
    char line[128], cmd[64], arg[80], path[128], buf[512], nb[12];

    for (;;) {
        sys::write(g_cwd); sys::write(" $ ");
        int n = sys::read(sys::STDIN, line, 127); if (n < 0) n = 0; line[n] = 0;
        split(line, cmd, arg);
        if (!cmd[0]) continue;

        if (streq(cmd, "help")) {
            sys::write("  help  ls  cd <d>  pwd  cat <f>  run <p>  vi <f>  ps  fork  race  exit\n");
        } else if (streq(cmd, "pwd")) {
            sys::write(g_cwd); sys::write("\n");
        } else if (streq(cmd, "cd")) {
            if (!arg[0]) { g_cwd[0] = '/'; g_cwd[1] = 0; continue; }
            resolve(arg, path);
            sys::stat_t st;
            if (sys::stat(path, &st) < 0 || !st.isdir) { sys::write("  no such directory\n"); continue; }
            int i = 0; while (path[i]) { g_cwd[i] = path[i]; i++; } g_cwd[i] = 0;
        } else if (streq(cmd, "ls")) {
            if (arg[0]) resolve(arg, path); else { int i = 0; while (g_cwd[i]) { path[i] = g_cwd[i]; i++; } path[i] = 0; }
            char nm[24];
            for (int i = 0; sys::readdir(path, i, nm) == 0; i++) { sys::write("  "); sys::write(nm); sys::write("\n"); }
        } else if (streq(cmd, "cat")) {
            if (!arg[0]) { sys::write("  usage: cat <file>\n"); continue; }
            resolve(arg, path);
            int m = sys::readfile(path, buf, 511);
            if (m < 0) { sys::write("  no such file\n"); continue; }
            buf[m] = 0; sys::write(buf);
        } else if (streq(cmd, "run")) {
            if (!arg[0]) { sys::write("  usage: run <program>\n"); continue; }
            int pid = sys::exec(arg);
            if (pid < 0) { sys::write("  no such program\n"); continue; }
            sys::wait(pid);
        } else if (streq(cmd, "vi")) {
            if (arg[0]) resolve(arg, path); else path[0] = 0;
            int p = sys::exec("vi", path);
            if (p < 0) { sys::write("  vi not found\n"); continue; }
            sys::wait(p);
        } else if (streq(cmd, "ps")) {
            taskinfo_t ti;
            for (int i = 0; sys::taskinfo(i, &ti) == 0; i++) {
                int l = sys::strcat_(buf, 0, "  pid "); sys::utoa(ti.pid, nb); l = sys::strcat_(buf, l, nb);
                l = sys::strcat_(buf, l, "  "); l = sys::strcat_(buf, l, ti.state < 5 ? STATES[ti.state] : "?");
                l = sys::strcat_(buf, l, "  "); l = sys::strcat_(buf, l, ti.name); l = sys::strcat_(buf, l, "\n");
                sys::write(buf);
            }
        } else if (streq(cmd, "fork")) {
            int pid = sys::fork();
            if (pid == 0) { sys::write("  [child]  hello from the forked child!\n"); sys::exit(0); }
            int l = sys::strcat_(buf, 0, "  [parent] forked child pid ");
            sys::utoa(pid, nb); l = sys::strcat_(buf, l, nb); sys::strcat_(buf, l, "\n");
            sys::write(buf); sys::wait(pid);
        } else if (streq(cmd, "race")) {
            sys::write("  two isolated 'count' tasks (watch them interleave)\n");
            int a = sys::exec("count"), b = sys::exec("count");
            sys::wait(a); sys::wait(b);
        } else if (streq(cmd, "exit")) {
            sys::write("[shell] bye\n"); return 0;
        } else {
            sys::write("  unknown command (try 'help')\n");
        }
    }
}
