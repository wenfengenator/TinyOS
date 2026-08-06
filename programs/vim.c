/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyOS Vim POC: standalone, TCC-compatible modal editor with C highlighting.
 * On device: ./tcc.bin /vim.c -o /vim.bin ; ./vim.bin file.c */
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <string.h>
#include <stdlib.h>

#define MAX_LINES 5000
#define MAX_COLS 1000
#define GUTTER 6
#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83

enum { NORMAL, INSERT };
enum { S_TEXT, S_COMMENT, S_STRING, S_NUMBER, S_KEYWORD, S_PREPROC };
enum { FT_TEXT, FT_C, FT_PYTHON, FT_SHELL, FT_MAKE };

static char (*lines)[MAX_COLS + 1], yank[MAX_COLS + 1];
static unsigned short lens[MAX_LINES];
static int nlines = 1, row, col, top, top_segment, left, rows = 24, cols = 80;
static int mode, dirty, running = 1, pending_d, pending_y, operator_count, count;
static int wrap_enabled, number_enabled = 1;
static int filetype, help_active, help_top;
static char filename[128], search_text[80], message[128];

static const char *help_lines[] = {
    "TinyOS Vim help", "", "MOVEMENT", "  h j k l / arrows   move cursor",
    "  [count]j/k         move several lines", "  0 / $              start / end of line",
    "  gg / G             first / last line", "  [line]gg / [line]G go to line", "",
    "INSERT", "  i / a              insert before / after cursor",
    "  I / A              insert at start / end of line", "  o / O              open line below / above",
    "  Esc                return to Normal mode", "  Tab                insert spaces", "",
    "EDIT", "  x / [count]x      delete character(s)", "  dd / [count]dd    delete line(s)",
    "  yy                 yank line", "  p                  paste yanked line below", "",
    "SEARCH", "  /text              search forward", "  n                  repeat search", "",
    "EX COMMANDS", "  :w                 write file", "  :q / :q!           quit / discard changes",
    "  :wq / :x           write and quit", "  :[line]            go to line",
    "  :set               show options", "  :set wrap/nowrap   toggle line wrapping",
    "  :set number/nonumber (nu/nonu also work)", "  :help / :h         open this help", "",
    "SYNTAX", "  Detected: C/C++, Python, shell, Makefile, plain text",
    "  Highlighting is selected from the file name.", "", "HELP CONTROLS",
    "  j/k or arrows      scroll", "  q or Esc            close help"
};
#define HELP_LINES ((int)(sizeof(help_lines) / sizeof(help_lines[0])))

static void outn(const char *s, int n) { if (n > 0) { ssize_t written = write(1, s, (size_t)n); (void)written; } }
static void outs(const char *s) { outn(s, (int)strlen(s)); }
static void number(int value) {
    char b[16]; int n = 0; if (!value) b[n++] = '0';
    while (value) { b[n++] = (char)('0' + value % 10); value /= 10; }
    while (n) outn(&b[--n], 1);
}
static void at(int r, int c) { outs("\x1b["); number(r); outs(";"); number(c); outs("H"); }
static int ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static int word_in(const char *s, int n, const char *const *words, int count) {
    for (int i = 0; i < count; i++) if ((int)strlen(words[i]) == n && !memcmp(s, words[i], (size_t)n)) return 1;
    return 0;
}
static int keyword(const char *s, int n) {
    static const char *const c_words[] = { "auto","break","case","char","const","continue","default","do","double","else","enum","extern","float","for","goto","if","inline","int","long","register","restrict","return","short","signed","sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while","bool","class","namespace","public","private","protected","template","using","NULL","true","false","nullptr" };
    static const char *const py_words[] = { "and","as","assert","async","await","break","class","continue","def","del","elif","else","except","False","finally","for","from","global","if","import","in","is","lambda","None","nonlocal","not","or","pass","raise","return","True","try","while","with","yield" };
    static const char *const sh_words[] = { "case","do","done","elif","else","esac","fi","for","function","if","in","select","then","time","until","while" };
    if (filetype == FT_C) return word_in(s, n, c_words, (int)(sizeof(c_words) / sizeof(c_words[0])));
    if (filetype == FT_PYTHON) return word_in(s, n, py_words, (int)(sizeof(py_words) / sizeof(py_words[0])));
    if (filetype == FT_SHELL) return word_in(s, n, sh_words, (int)(sizeof(sh_words) / sizeof(sh_words[0])));
    return 0;
}
static int ends_with(const char *name, const char *suffix) {
    int nl = (int)strlen(name), sl = (int)strlen(suffix);
    return nl >= sl && !memcmp(name + nl - sl, suffix, (size_t)sl);
}
static void detect_filetype(void) {
    const char *base = filename; for (const char *p = filename; *p; p++) if (*p == '/') base = p + 1;
    if (!strcmp(base, "Makefile") || !strcmp(base, "makefile") || ends_with(base, ".mk")) filetype = FT_MAKE;
    else if (ends_with(base, ".c") || ends_with(base, ".h") || ends_with(base, ".cc") || ends_with(base, ".cpp") || ends_with(base, ".cxx") || ends_with(base, ".hpp")) filetype = FT_C;
    else if (ends_with(base, ".py")) filetype = FT_PYTHON;
    else if (ends_with(base, ".sh") || ends_with(base, ".bash")) filetype = FT_SHELL;
    else filetype = FT_TEXT;
}
static const char *filetype_name(void) {
    static const char *names[] = { "text", "c", "python", "sh", "make" }; return names[filetype];
}

/* Returns whether the next line starts inside a block comment. */
static int lex_line(int r, int block, unsigned char *style) {
    int n = lens[r], i = 0, pre = 1;
    while (i < n) {
        if (filetype == FT_TEXT) { style[i++] = S_TEXT;
        } else if (block) {
            style[i] = S_COMMENT;
            if (i + 1 < n && lines[r][i] == '*' && lines[r][i + 1] == '/') { style[i + 1] = S_COMMENT; i += 2; block = 0; }
            else i++;
        } else if (filetype == FT_C && i + 1 < n && lines[r][i] == '/' && lines[r][i + 1] == '/') {
            while (i < n) style[i++] = S_COMMENT;
        } else if (filetype == FT_C && i + 1 < n && lines[r][i] == '/' && lines[r][i + 1] == '*') {
            style[i++] = S_COMMENT; style[i++] = S_COMMENT; block = 1;
        } else if ((filetype == FT_PYTHON || filetype == FT_SHELL || filetype == FT_MAKE) && lines[r][i] == '#') {
            while (i < n) style[i++] = S_COMMENT;
        } else if (lines[r][i] == '"' || lines[r][i] == '\'') {
            char quote = lines[r][i]; style[i++] = S_STRING;
            while (i < n) { style[i] = S_STRING; if (lines[r][i] == '\\' && i + 1 < n) style[++i] = S_STRING; else if (lines[r][i] == quote) { i++; break; } i++; }
        } else if (filetype == FT_C && pre && lines[r][i] == '#') {
            while (i < n) style[i++] = S_PREPROC;
        } else if ((filetype == FT_SHELL || filetype == FT_MAKE) && lines[r][i] == '$') {
            style[i++] = S_PREPROC; if (i < n && lines[r][i] == '(') { while (i < n) { style[i] = S_PREPROC; if (lines[r][i++] == ')') break; } } else while (i < n && ident(lines[r][i])) style[i++] = S_PREPROC;
        } else if (lines[r][i] >= '0' && lines[r][i] <= '9') {
            while (i < n && (ident(lines[r][i]) || lines[r][i] == '.')) style[i++] = S_NUMBER;
        } else if ((lines[r][i] >= 'a' && lines[r][i] <= 'z') || (lines[r][i] >= 'A' && lines[r][i] <= 'Z') || lines[r][i] == '_') {
            int start = i; while (i < n && ident(lines[r][i])) i++;
            int st = keyword(lines[r] + start, i - start) ? S_KEYWORD : S_TEXT;
            while (start < i) style[start++] = (unsigned char)st;
        } else { style[i] = S_TEXT; if (lines[r][i] != ' ' && lines[r][i] != '\t') pre = 0; i++; }
    }
    return block;
}
static const char *color(int style) {
    static const char *colors[] = { "\x1b[38;5;252m", "\x1b[38;5;108m", "\x1b[38;5;215m", "\x1b[38;5;209m", "\x1b[1;38;5;81m", "\x1b[38;5;176m" };
    return colors[style];
}
static int gutter_width(void) { return number_enabled ? GUTTER : 0; }
static int text_width(void) { int width = cols - gutter_width(); return width > 0 ? width : 1; }
static int line_screen_rows(int r) {
    if (!wrap_enabled) return 1;
    return lens[r] / text_width() + 1;
}
static int cursor_screen_row(void) {
    int screen = -top_segment;
    for (int r = top; r < row; r++) screen += line_screen_rows(r);
    if (wrap_enabled) screen += col / text_width();
    return screen;
}
static void advance_view_row(void) {
    if (!wrap_enabled) { top++; return; }
    top_segment++;
    if (top_segment >= line_screen_rows(top)) { top++; top_segment = 0; }
}

static void render_help(void) {
    int text_rows = rows - 2;
    outs("\x1b[?25l\x1b[H");
    for (int screen = 0; screen < text_rows; screen++) {
        int index = help_top + screen; at(screen + 1, 1); outs("\x1b[48;5;235m\x1b[K");
        if (index >= HELP_LINES) continue;
        const char *line = help_lines[index];
        if (line[0] && line[0] != ' ') outs("\x1b[1;38;5;81m"); else outs("\x1b[38;5;252m");
        int n = (int)strlen(line); if (n > cols) n = cols; outn(line, n);
    }
    at(rows - 1, 1); outs("\x1b[48;5;24m\x1b[38;5;231m VIM HELP \x1b[K");
    at(rows, 1); outs("\x1b[0m\x1b[48;5;234m\x1b[38;5;250m j/k scroll  q/Esc close\x1b[K");
}

static void render(void) {
    unsigned char styles[MAX_COLS]; int block = 0;
    for (int r = 0; r < top; r++) block = lex_line(r, block, styles);
    outs("\x1b[?25l\x1b[H");
    int text_rows = rows - 2, width = text_width(), gutter = gutter_width();
    int r = top, segment = top_segment, next_block = block;
    for (int screen = 0; screen < text_rows; screen++) {
        at(screen + 1, 1); outs("\x1b[48;5;234m\x1b[38;5;244m");
        if (number_enabled) {
            if (r < nlines && segment == 0) { char pad[5] = {' ',' ',' ',' ',' '}; int v = r + 1, p = 4; while (v && p >= 0) { pad[p--] = (char)('0' + v % 10); v /= 10; } outn(pad, 5); outs(" "); }
            else if (r < nlines) outs("      ");
            else outs("    ~ ");
        } else if (r >= nlines) outs("~");
        outs("\x1b[48;5;235m\x1b[K");
        if (r >= nlines) continue;
        next_block = lex_line(r, block, styles); int last_style = -1;
        int start = wrap_enabled ? segment * width : left;
        for (int x = start; x < lens[r] && x - start < width; x++) {
            if (styles[x] != last_style) { outs(color(styles[x])); last_style = styles[x]; }
            char c = lines[r][x] == '\t' ? ' ' : lines[r][x]; outn(&c, 1);
        }
        segment++;
        if (segment >= line_screen_rows(r)) { r++; segment = 0; block = next_block; }
    }
    at(rows - 1, 1); outs(mode == INSERT ? "\x1b[48;5;24m\x1b[38;5;231m -- INSERT -- " : "\x1b[48;5;238m\x1b[38;5;231m -- NORMAL -- ");
    outs(filename[0] ? filename : "[No Name]"); if (dirty) outs(" [+]"); outs("  ["); outs(filetype_name()); outs("]\x1b[K");
    at(rows, 1); outs("\x1b[0m\x1b[48;5;234m\x1b[38;5;250m");
    if (message[0]) outs(message); else outs("i insert  / search  dd delete  yy/p yank/paste  :help");
    outs("\x1b[K");
    int cr = cursor_screen_row() + 1;
    int cc = (wrap_enabled ? col % width : col - left) + gutter + 1;
    if (cr < 1) cr = 1;
    if (cr > text_rows) cr = text_rows;
    if (cc < gutter + 1) cc = gutter + 1;
    at(cr, cc); outs("\x1b[0m\x1b[?25h");
}

static void load_file(void) {
    int fd = filename[0] ? open(filename, O_RDONLY) : -1; nlines = 0;
    if (fd >= 0) {
        char b[4096]; int n, c = 0;
        while ((n = (int)read(fd, b, sizeof(b))) > 0 && nlines < MAX_LINES) for (int i = 0; i < n; i++) {
            if (b[i] == '\n') { lens[nlines] = (unsigned short)c; lines[nlines][c] = 0; nlines++; c = 0; }
            else if (b[i] != '\r' && c < MAX_COLS) lines[nlines][c++] = b[i];
        }
        if (c && nlines < MAX_LINES) { lens[nlines] = (unsigned short)c; lines[nlines][c] = 0; nlines++; }
        close(fd);
    }
    if (!nlines) { nlines = 1; lens[0] = 0; lines[0][0] = 0; }
}
static int save_file(void) {
    if (!filename[0]) { strcpy(message, "No file name"); return -1; }
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) { strcpy(message, "Write failed"); return -1; }
    for (int r = 0; r < nlines; r++) {
        ssize_t written;
        if (lens[r]) { written = write(fd, lines[r], lens[r]); if (written != lens[r]) { close(fd); strcpy(message, "Write failed"); return -1; } }
        written = write(fd, "\n", 1); if (written != 1) { close(fd); strcpy(message, "Write failed"); return -1; }
    }
    close(fd); dirty = 0; strcpy(message, "Written"); return 0;
}
static void insert_char(char c) {
    if (lens[row] >= MAX_COLS) return;
    memmove(lines[row] + col + 1, lines[row] + col, (size_t)(lens[row] - col + 1)); lines[row][col++] = c; lens[row]++; dirty = 1;
}
static void delete_char(void) {
    if (col >= lens[row]) return;
    memmove(lines[row] + col, lines[row] + col + 1, (size_t)(lens[row] - col)); lens[row]--; dirty = 1;
}
static void shift_down(int atrow) {
    if (nlines >= MAX_LINES) return;
    for (int r = nlines; r > atrow; r--) { memcpy(lines[r], lines[r - 1], lens[r - 1] + 1); lens[r] = lens[r - 1]; } nlines++;
}
static void split_line(void) {
    if (nlines >= MAX_LINES) return;
    shift_down(row + 1);
    int tail = lens[row] - col; memcpy(lines[row + 1], lines[row] + col, (size_t)tail); lines[row + 1][tail] = 0; lens[row + 1] = (unsigned short)tail;
    lens[row] = (unsigned short)col; lines[row][col] = 0; row++; col = 0; dirty = 1;
}
static void delete_line(void) {
    strcpy(yank, lines[row]);
    if (nlines == 1) { lens[0] = 0; lines[0][0] = 0; }
    else { for (int r = row; r < nlines - 1; r++) { memcpy(lines[r], lines[r + 1], lens[r + 1] + 1); lens[r] = lens[r + 1]; } nlines--; if (row >= nlines) row--; }
    col = 0; dirty = 1;
}
static void paste_line(void) {
    if (nlines >= MAX_LINES) return;
    shift_down(row + 1); row++; strcpy(lines[row], yank); lens[row] = (unsigned short)strlen(yank); col = 0; dirty = 1;
}
static void backspace(void) {
    if (col > 0) { col--; delete_char(); }
    else if (row > 0 && lens[row - 1] + lens[row] <= MAX_COLS) {
        int old = lens[row - 1]; memcpy(lines[row - 1] + old, lines[row], lens[row] + 1); lens[row - 1] += lens[row];
        for (int r = row; r < nlines - 1; r++) { memcpy(lines[r], lines[r + 1], lens[r + 1] + 1); lens[r] = lens[r + 1]; }
        nlines--; row--; col = old; dirty = 1;
    }
}
static void find_next(void) {
    if (!search_text[0]) return;
    for (int step = 1; step <= nlines; step++) { int r = (row + step) % nlines; char *p = strstr(lines[r], search_text); if (p) { row = r; col = (int)(p - lines[r]); strcpy(message, search_text); return; } }
    strcpy(message, "Pattern not found");
}
static int prompt(char lead, char *buf, int cap) {
    int n = 0; buf[0] = 0;
    for (;;) { at(rows, 1); outs("\x1b[0m\x1b[48;5;234m\x1b[K"); outn(&lead, 1); outn(buf, n); int c; char ch; if (read(0, &ch, 1) <= 0) continue; c = (unsigned char)ch;
        if (c == 27) return 0;
        if (c == '\n' || c == '\r') { buf[n] = 0; return 1; }
        if (c == 8 || c == 127) { if (n) n--; } else if (c >= 32 && c < 127 && n < cap - 1) buf[n++] = (char)c; buf[n] = 0;
    }
}
static void set_options(const char *args) {
    if (!*args) {
        strcpy(message, wrap_enabled ? "wrap " : "nowrap ");
        strcat(message, number_enabled ? "number" : "nonumber");
        return;
    }
    while (*args) {
        while (*args == ' ') args++;
        const char *start = args; while (*args && *args != ' ') args++;
        int n = (int)(args - start);
        if (n == 4 && !memcmp(start, "wrap", 4)) wrap_enabled = 1;
        else if (n == 6 && !memcmp(start, "nowrap", 6)) wrap_enabled = 0;
        else if ((n == 6 && !memcmp(start, "number", 6)) || (n == 2 && !memcmp(start, "nu", 2))) number_enabled = 1;
        else if ((n == 8 && !memcmp(start, "nonumber", 8)) || (n == 4 && !memcmp(start, "nonu", 4))) number_enabled = 0;
        else { strcpy(message, "Unknown option"); return; }
    }
    top_segment = 0; left = 0;
    strcpy(message, wrap_enabled ? "wrap " : "nowrap ");
    strcat(message, number_enabled ? "number" : "nonumber");
}
static void ex_command(void) {
    char cmd[80]; if (!prompt(':', cmd, sizeof(cmd))) return;
    if (!strcmp(cmd, "w")) save_file();
    else if (!strcmp(cmd, "q")) { if (dirty) strcpy(message, "No write since last change (use :q!)"); else running = 0; }
    else if (!strcmp(cmd, "q!")) running = 0;
    else if (!strcmp(cmd, "wq") || !strcmp(cmd, "x")) { if (save_file() == 0) running = 0; }
    else if (!strcmp(cmd, "set")) set_options("");
    else if (!memcmp(cmd, "set ", 4)) set_options(cmd + 4);
    else if (!strcmp(cmd, "help") || !strcmp(cmd, "h")) { help_active = 1; help_top = 0; }
    else if (cmd[0] >= '0' && cmd[0] <= '9') { int target = atoi(cmd); if (target > 0) { row = target - 1; if (row >= nlines) row = nlines - 1; col = 0; } }
    else strcpy(message, "Not an editor command");
}
static void clamp_view(void) {
    if (row < 0) row = 0;
    if (row >= nlines) row = nlines - 1;
    if (col > lens[row]) col = lens[row];
    if (wrap_enabled) {
        int segment = col / text_width(); left = 0;
        if (row < top) { top = row; top_segment = 0; }
        else if (row == top && segment < top_segment) top_segment = segment;
        while (cursor_screen_row() >= rows - 2) advance_view_row();
    } else {
        top_segment = 0;
        if (row < top) top = row;
        if (row >= top + rows - 2) top = row - rows + 3;
        if (col < left) left = col;
        if (col >= left + text_width()) left = col - text_width() + 1;
    }
}

int main(int argc, char **argv) {
    lines = (char (*)[MAX_COLS + 1])calloc(MAX_LINES, sizeof(*lines));
    if (!lines) { outs("vim: cannot allocate text buffer\n"); return 1; }
    if (argc > 1) { strncpy(filename, argv[1], sizeof(filename) - 1); filename[sizeof(filename) - 1] = 0; }
    detect_filetype();
    load_file(); struct winsize ws; if (ioctl(1, TIOCGWINSZ, &ws) == 0) { if (ws.ws_row > 5) rows = ws.ws_row; if (ws.ws_col > 20) cols = ws.ws_col; }
    struct termios old, raw; tcgetattr(0, &old); raw = old; raw.c_lflag &= ~(ICANON | ECHO | ISIG); tcsetattr(0, TCSANOW, &raw);
    outs("\x1b[2J");
    while (running) {
        if (help_active) render_help(); else render();
        message[0] = 0; char ch; if (read(0, &ch, 1) <= 0) continue; int c = (unsigned char)ch;
        if (help_active) {
            int page = rows - 2, max_top = HELP_LINES > page ? HELP_LINES - page : 0;
            if (c == 'q' || c == 27) help_active = 0;
            else if (c == 'j' || c == KEY_DOWN) { if (help_top < max_top) help_top++; }
            else if (c == 'k' || c == KEY_UP) { if (help_top > 0) help_top--; }
            continue;
        }
        if (mode == INSERT) {
            if (c == 27) { mode = NORMAL; if (col) col--; }
            else if (c == '\n' || c == '\r') split_line(); else if (c == 8 || c == 127) backspace();
            else if (c == KEY_LEFT) { if (col) col--; } else if (c == KEY_RIGHT) { if (col < lens[row]) col++; }
            else if (c == KEY_UP) { if (row) row--; } else if (c == KEY_DOWN) { if (row + 1 < nlines) row++; }
            else if (c == '\t') { int n = 4 - (col & 3); while (n--) insert_char(' '); } else if (c >= 32 && c < 127) insert_char((char)c);
        } else {
            if ((c >= '1' && c <= '9') || (c == '0' && count)) { count = count * 10 + c - '0'; continue; }
            int had_count = count != 0, repeat = count ? count : 1; count = 0;
            if (pending_d) { pending_d = 0; if (c == 'd') { repeat = operator_count; while (repeat--) delete_line(); } }
            else if (pending_y) { pending_y = 0; if (c == 'y') strcpy(yank, lines[row]); }
            else if (c == 'h' || c == KEY_LEFT) { while (repeat-- && col) col--; }
            else if (c == 'l' || c == KEY_RIGHT) { while (repeat-- && col + 1 < lens[row]) col++; }
            else if (c == 'j' || c == KEY_DOWN) { row += repeat; }
            else if (c == 'k' || c == KEY_UP) { row -= repeat; }
            else if (c == '0') col = 0; else if (c == '$') col = lens[row] ? lens[row] - 1 : 0;
            else if (c == 'g') { char next; if (read(0, &next, 1) > 0 && next == 'g') { row = repeat > 1 ? repeat - 1 : 0; col = 0; } }
            else if (c == 'G') { row = had_count ? repeat - 1 : nlines - 1; col = 0; }
            else if (c == 'i') mode = INSERT; else if (c == 'a') { if (col < lens[row]) col++; mode = INSERT; }
            else if (c == 'A') { col = lens[row]; mode = INSERT; } else if (c == 'I') { col = 0; mode = INSERT; }
            else if (c == 'o') { col = lens[row]; split_line(); mode = INSERT; }
            else if (c == 'O') { shift_down(row); lens[row] = 0; lines[row][0] = 0; col = 0; dirty = 1; mode = INSERT; }
            else if (c == 'x') while (repeat--) delete_char(); else if (c == 'd') { pending_d = 1; operator_count = repeat; } else if (c == 'y') { pending_y = 1; operator_count = repeat; }
            else if (c == 'p') paste_line(); else if (c == '/') { if (prompt('/', search_text, sizeof(search_text))) find_next(); }
            else if (c == 'n') find_next(); else if (c == ':') ex_command();
        }
        clamp_view();
    }
    tcsetattr(0, TCSANOW, &old); outs("\x1b[0m\x1b[?25h\x1b[2J\x1b[H"); return 0;
}