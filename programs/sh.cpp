// tinyos ring-3 shell: reads commands from the terminal (SYS_READ) and runs them.
// Built-ins, plus launching the other embedded programs via sys::exec (SYS_EXEC).
#include "ulib.h"

static bool eq(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return false;
    return a[i] == b[i];
}
static void split(const char *l, char *cmd, char *arg) {
    int i = 0, j = 0;
    while (l[i] == ' ') i++;
    while (l[i] && l[i] != ' ') cmd[j++] = l[i++];
    cmd[j] = 0;
    while (l[i] == ' ') i++;
    j = 0; while (l[i]) arg[j++] = l[i++];
    arg[j] = 0;
}

extern "C" int umain() {
    sys::write("tinyos shell (ring 3).  type 'help'; 'exit' to leave.\n");
    char line[128], cmd[64], arg[80], cwd[80];
    for (;;) {
        sys::getcwd(cwd);
        sys::write(cwd); sys::write(" $ ");
        int n = sys::read(sys::STDIN, line, 127);
        if (n < 0) n = 0;
        line[n] = 0;
        split(line, cmd, arg);
        if (!cmd[0]) continue;

        if (eq(cmd, "help")) {
            sys::write("  fs: pwd  cd <d>  ls  cat <f>  wr <f> <t>  mkdir <d>  rm|del <f>\n");
            sys::write("  sys: ps  echo <t>  ver  exit    programs: hello  count  greet  vi <f>\n");
        } else if (eq(cmd, "echo")) {
            sys::write(arg); sys::write("\n");
        } else if (eq(cmd, "ver") || eq(cmd, "about")) {
            sys::write("tinyos x64 shell, running in ring 3.\n");
        } else if (eq(cmd, "pwd")) {
            sys::write(cwd); sys::write("\n");
        } else if (eq(cmd, "cd")) {
            const char *tgt = arg[0] ? arg : "/";
            if (sys::chdir(tgt) < 0) { sys::write("cd: no such directory: "); sys::write(tgt); sys::write("\n"); }
        } else if (eq(cmd, "ls")) {
            char nm[16];
            for (int i = 0; ; i++) {
                int t = sys::readdir(arg, i, nm);       // arg empty -> current dir
                if (t < 0) break;
                if (eq(nm, ".") || eq(nm, "..")) continue;
                sys::write(nm);
                if (t == 1) sys::write("/");
                sys::write("\n");
            }
        } else if (eq(cmd, "cat")) {
            if (!arg[0]) { sys::write("usage: cat <file>\n"); }
            else {
                static char fbuf[4096];
                int fn = sys::readfile(arg, fbuf, sizeof(fbuf) - 1);
                if (fn < 0) { sys::write(arg); sys::write(": no such file\n"); }
                else {
                    fbuf[fn] = 0; sys::write_n(sys::STDOUT, fbuf, fn);
                    if (fn == 0 || fbuf[fn - 1] != '\n') sys::write("\n");
                }
            }
        } else if (eq(cmd, "wr")) {                 // wr <file> <text> -> writefile
            char fname[64], text[80]; split(arg, fname, text);
            if (!fname[0]) { sys::write("usage: wr <file> <text>\n"); }
            else {
                int w = sys::writefile(fname, text, (int)sys::strlen(text));
                if (w < 0) sys::write("wr: write failed\n");
                else { char nb[12]; sys::utoa((unsigned)w, nb); sys::write("wrote "); sys::write(nb); sys::write(" bytes\n"); }
            }
        } else if (eq(cmd, "mkdir")) {
            if (!arg[0]) sys::write("usage: mkdir <name>\n");
            else if (sys::mkdir(arg) < 0) { sys::write("mkdir: cannot create "); sys::write(arg); sys::write("\n"); }
        } else if (eq(cmd, "rm") || eq(cmd, "del")) {
            if (!arg[0]) sys::write("usage: rm <file>\n");
            else {
                int r = sys::unlink(arg);
                if (r == -2) { sys::write("rm: "); sys::write(arg); sys::write(": is a directory\n"); }
                else if (r < 0) { sys::write("rm: "); sys::write(arg); sys::write(": no such file\n"); }
            }
        } else if (eq(cmd, "ps")) {
            sys::write("  PID  RING   STATE  NAME\n");
            sys::taskinfo_t ti;
            for (int i = 0; sys::taskinfo(i, &ti) == 0; i++) {
                char nb[12]; sys::utoa((unsigned)ti.pid, nb);
                char ln[48]; int p = 0;
                ln[p++] = ' '; ln[p++] = ' ';
                for (int j = 0; nb[j]; j++) ln[p++] = nb[j];
                while (p < 7)  ln[p++] = ' ';
                const char *rg = ti.ring == 0 ? "ring0" : "ring3";
                for (int j = 0; rg[j]; j++) ln[p++] = rg[j];
                while (p < 14) ln[p++] = ' ';
                const char *st = ti.state == 0 ? "run" : "wait";
                for (int j = 0; st[j]; j++) ln[p++] = st[j];
                while (p < 21) ln[p++] = ' ';
                for (int j = 0; ti.name[j]; j++) ln[p++] = ti.name[j];
                ln[p++] = '\n'; ln[p] = 0;
                sys::write(ln);
            }
        } else if (eq(cmd, "exit") || eq(cmd, "quit")) {
            sys::write("bye\n");
            return 0;
        } else {
            int rc = sys::exec(cmd, arg);         // run an embedded program (passes arg)
            if (rc < 0) { sys::write(cmd); sys::write(": command not found\n"); }
        }
    }
}
