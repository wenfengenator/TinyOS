// tinyos "vi" — a minimal modal full-screen editor (ramdisk program, ring 3).
// Normal mode: h j k l move, i insert, a append, o open line, x delete char,
//              : command (:w write, :q quit, :wq).  Insert mode: type, Enter,
//              Backspace, ESC to return to normal.  Saves via writefile, so files
//              under /mnt persist to the mounted FAT16 disk image.
#include "ulib.h"

#define MAXL 128
#define MAXC 78

static char buf[MAXL][MAXC + 2];
static int  len[MAXL];
static int  nlines = 1;
static int  cx = 0, cy = 0, top = 0;
static char fname[128];
static bool dirty = false;
static int  VH = 24;                   // visible text rows (terminal height - 1 status)

static void load(const char *path) {
    static char d[8192];
    int n = path[0] ? sys::readfile(path, d, 8191) : -1;
    if (n < 0) n = 0;
    nlines = 0; int c = 0;
    for (int i = 0; i < n && nlines < MAXL; i++) {
        if (d[i] == '\n') { len[nlines] = c; buf[nlines][c] = 0; nlines++; c = 0; }
        else if (c < MAXC) buf[nlines][c++] = d[i];
    }
    if (nlines < MAXL && (c > 0 || nlines == 0)) { len[nlines] = c; buf[nlines][c] = 0; nlines++; }
    if (nlines == 0) { nlines = 1; len[0] = 0; buf[0][0] = 0; }
}

static void save() {
    static char out[8192]; int o = 0;
    for (int i = 0; i < nlines; i++) {
        for (int j = 0; j < len[i] && o < 8191; j++) out[o++] = buf[i][j];
        if (o < 8191) out[o++] = '\n';
    }
    sys::writefile(fname, out, o);
    dirty = false;
}

static void redraw(const char *status) {
    sys::clear();
    for (int r = 0; r < VH && top + r < nlines; r++) {
        sys::gotoxy(r, 0);
        sys::write_n(sys::STDOUT, buf[top + r], len[top + r]);
    }
    sys::gotoxy(VH, 0); sys::write(status);
    sys::gotoxy(cy - top, cx);
}

static void ins_char(char c) {
    if (len[cy] >= MAXC) return;
    for (int i = len[cy]; i > cx; i--) buf[cy][i] = buf[cy][i - 1];
    buf[cy][cx] = c; len[cy]++; buf[cy][len[cy]] = 0; cx++; dirty = true;
}
static void del_char() {
    if (cx >= len[cy]) return;
    for (int i = cx; i < len[cy] - 1; i++) buf[cy][i] = buf[cy][i + 1];
    len[cy]--; buf[cy][len[cy]] = 0; dirty = true;
}
static void shift_down(int from) {
    for (int i = nlines; i > from; i--) { for (int j = 0; j <= len[i - 1]; j++) buf[i][j] = buf[i - 1][j]; len[i] = len[i - 1]; }
    nlines++;
}
static void split_line() {
    if (nlines >= MAXL) return;
    shift_down(cy + 1);
    int m = 0; for (int j = cx; j < len[cy]; j++) buf[cy + 1][m++] = buf[cy][j];
    len[cy + 1] = m; buf[cy + 1][m] = 0;
    len[cy] = cx; buf[cy][cx] = 0;
    cy++; cx = 0; dirty = true;
}
static void open_line() {
    if (nlines >= MAXL) return;
    shift_down(cy + 1); cy++; len[cy] = 0; buf[cy][0] = 0; cx = 0; dirty = true;
}
static void backspace() {
    if (cx > 0) { cx--; del_char(); }
    else if (cy > 0) {
        int pl = len[cy - 1];
        if (pl + len[cy] < MAXC) {
            for (int i = 0; i < len[cy]; i++) buf[cy - 1][pl + i] = buf[cy][i];
            len[cy - 1] = pl + len[cy]; buf[cy - 1][len[cy - 1]] = 0;
            for (int i = cy; i < nlines - 1; i++) { for (int j = 0; j <= len[i + 1]; j++) buf[i][j] = buf[i + 1][j]; len[i] = len[i + 1]; }
            nlines--; cy--; cx = pl; dirty = true;
        }
    }
}

extern "C" int umain(const char *arg) {
    int i = 0; if (arg) while (arg[i] && i < 127) { fname[i] = arg[i]; i++; } fname[i] = 0;
    if (!fname[0]) { sys::write("vi: usage: vi <file>\n"); return 1; }
    load(fname);
    { int c = 0, r = 0; sys::winsize(&c, &r); if (r > 2) VH = r - 1; }
    bool insert = false;

    for (;;) {
        char st[128]; int l;
        if (insert) l = sys::strcat_(st, 0, "-- INSERT --  (ESC=normal)");
        else { l = sys::strcat_(st, 0, "\""); l = sys::strcat_(st, l, fname);
               l = sys::strcat_(st, l, dirty ? "\" [modified]  " : "\"  ");
               l = sys::strcat_(st, l, "i insert  x del  o open  :w save  :q quit"); }
        redraw(st);

        int k = sys::getchar();
        if (insert) {
            if (k == 27) insert = false;
            else if (k == '\r' || k == '\n') split_line();
            else if (k == 8 || k == 127) backspace();
            else if (k == sys::KEY_DEL) del_char();
            else if (k == sys::KEY_LEFT)  { if (cx > 0) cx--; }
            else if (k == sys::KEY_RIGHT) { if (cx < len[cy]) cx++; }
            else if (k == sys::KEY_UP)    { if (cy > 0) { cy--; if (cx > len[cy]) cx = len[cy]; } }
            else if (k == sys::KEY_DOWN)  { if (cy < nlines - 1) { cy++; if (cx > len[cy]) cx = len[cy]; } }
            else if (k == sys::KEY_HOME)  cx = 0;
            else if (k == sys::KEY_END)   cx = len[cy];
            else if (k >= 32 && k < 127) ins_char((char)k);
        } else {
            if (k == 'h' || k == sys::KEY_LEFT) { if (cx > 0) cx--; }
            else if (k == 'l' || k == sys::KEY_RIGHT) { if (cx < len[cy]) cx++; }
            else if (k == 'j' || k == sys::KEY_DOWN) { if (cy < nlines - 1) { cy++; if (cx > len[cy]) cx = len[cy]; } }
            else if (k == 'k' || k == sys::KEY_UP) { if (cy > 0) { cy--; if (cx > len[cy]) cx = len[cy]; } }
            else if (k == sys::KEY_HOME) cx = 0;
            else if (k == sys::KEY_END)  cx = len[cy];
            else if (k == 'i') insert = true;
            else if (k == 'a') { if (cx < len[cy]) cx++; insert = true; }
            else if (k == 'x' || k == sys::KEY_DEL) del_char();
            else if (k == 'o') { open_line(); insert = true; }
            else if (k == ':') {
                char cmd[16]; int ci = 0;
                for (;;) {
                    char line[82]; int p = 0; line[p++] = ':';
                    for (int z = 0; z < ci; z++) line[p++] = cmd[z];
                    while (p < 80) line[p++] = ' ';
                    line[p] = 0;
                    sys::gotoxy(24, 0); sys::write(line);
                    sys::gotoxy(24, 1 + ci);
                    int c = sys::getchar();
                    if (c == '\r' || c == '\n') break;
                    if (c == 27) { ci = 0; break; }
                    if (c == 8 || c == 127) { if (ci > 0) ci--; continue; }
                    if (c >= 32 && c < 127 && ci < 15) cmd[ci++] = (char)c;
                }
                cmd[ci] = 0;
                bool w = false, q = false;
                for (int z = 0; cmd[z]; z++) { if (cmd[z] == 'w') w = true; if (cmd[z] == 'q') q = true; }
                if (w) save();
                if (q) { sys::clear(); sys::gotoxy(0, 0); return 0; }
            }
        }
        if (cy < top) top = cy;
        if (cy >= top + VH) top = cy - (VH - 1);
        if (cx > len[cy]) cx = len[cy];
    }
}
