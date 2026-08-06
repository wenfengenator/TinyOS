/* SPDX-License-Identifier: GPL-2.0-only */

/* Sudoku for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /SUDOKU.BIN) or on device:
 *     ./tcc.bin /sudoku.c -o /sudoku.bin ; ./sudoku.bin
 *
 * A full Sudoku: a backtracking generator makes a random solved grid then digs
 * cells out while a solution-counter keeps the puzzle UNIQUELY solvable at the
 * chosen difficulty.  Peers/same-digit highlighting, pencil marks, conflict and
 * mistake checking, hints, and a timer.
 *
 * Controls: arrows / W A S D move, 1-9 place, 0 / Space / Backspace erase,
 * P pencil marks, H hint, C check mistakes, Tab cycle level, N new, R restart,
 * Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define N     9
#define CELL  50
#define BOARD (CELL * N)
#define MARGIN 16
#define HUD   48

static int  given[81], board[81], solution[81];
static unsigned char clue[81];
static unsigned short marks[81];          /* pencil candidates, bit d (1..9) */
static int  cx = 4, cy = 4, diff = 1, pencil, won, show_mistakes;
static long start_time, end_time;
static unsigned long rng;

static const int   CLUES[3] = { 42, 34, 28 };
static const char *DIFFN[3] = { "EASY", "MEDIUM", "HARD" };

static unsigned rnd(void) {
    if (!rng) rng = ((unsigned long)time(0) << 1) | 1;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 5);
}
static void shuffle(int *a, int n) {
    for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int t = a[i]; a[i] = a[j]; a[j] = t; }
}
/* 8x8 font scaled by `sc` (built on the tinygl.h glyph table). */
static void text_s(tgl_t *g, int x, int y, const char *s, uint32_t c, int sc) {
    for (; *s; s++, x += 8 * sc) {
        unsigned ch = (unsigned char)*s; if (ch < 0x20 || ch > 0x7E) continue;
        const unsigned char *gl = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) { unsigned bits = gl[row];
            for (int col = 0; col < 8; col++) if (bits & (1u << col)) tgl_fill(g, x + col * sc, y + row * sc, sc, sc, c); }
    }
}
static void text_c(tgl_t *g, int y, const char *s, uint32_t c, int sc) {
    int n = 0; for (const char *p = s; *p; p++) n++;
    text_s(g, g->w / 2 - n * 8 * sc / 2, y, s, c, sc);
}

/* ---- solver / generator ------------------------------------------------- */
static int valid(const int *g, int cell, int d) {
    int r = cell / 9, c = cell % 9;
    for (int i = 0; i < 9; i++) if (g[r * 9 + i] == d || g[i * 9 + c] == d) return 0;
    int br = r - r % 3, bc = c - c % 3;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (g[(br + i) * 9 + bc + j] == d) return 0;
    return 1;
}
/* fill the grid with a random complete solution (randomized backtracking). */
static int fill_grid(int *g, int cell) {
    while (cell < 81 && g[cell]) cell++;
    if (cell == 81) return 1;
    int order[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    shuffle(order, 9);
    for (int i = 0; i < 9; i++) { int d = order[i];
        if (valid(g, cell, d)) { g[cell] = d; if (fill_grid(g, cell + 1)) return 1; g[cell] = 0; } }
    return 0;
}
/* count solutions, stopping once `cap` is reached; leaves g unchanged. */
static int count_sol(int *g, int cell, int cap) {
    while (cell < 81 && g[cell]) cell++;
    if (cell == 81) return 1;
    int total = 0;
    for (int d = 1; d <= 9; d++) if (valid(g, cell, d)) {
        g[cell] = d; total += count_sol(g, cell + 1, cap); g[cell] = 0;
        if (total >= cap) break;
    }
    return total;
}
/* solved grid -> dig cells out, keeping a unique solution, toward `target` clues. */
static void generate(int target) {
    for (int i = 0; i < 81; i++) solution[i] = 0;
    fill_grid(solution, 0);
    for (int i = 0; i < 81; i++) given[i] = solution[i];
    int cells[81]; for (int i = 0; i < 81; i++) cells[i] = i; shuffle(cells, 81);
    int clues = 81;
    for (int k = 0; k < 81 && clues > target; k++) {
        int c = cells[k]; if (!given[c]) continue;
        int saved = given[c]; given[c] = 0;
        if (count_sol(given, 0, 2) != 1) given[c] = saved; else clues--;
    }
}

static int conflict(int cell) {
    int v = board[cell]; if (!v) return 0;
    int r = cell / 9, c = cell % 9;
    for (int i = 0; i < 9; i++) {
        int a = r * 9 + i, b = i * 9 + c;
        if (a != cell && board[a] == v) return 1;
        if (b != cell && board[b] == v) return 1;
    }
    int br = r - r % 3, bc = c - c % 3;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
        int a = (br + i) * 9 + bc + j; if (a != cell && board[a] == v) return 1;
    }
    return 0;
}
static int solved(void) {
    for (int i = 0; i < 81; i++) if (!board[i] || conflict(i)) return 0;
    return 1;
}
static void new_game(void) {
    generate(CLUES[diff]);
    for (int i = 0; i < 81; i++) { board[i] = given[i]; clue[i] = given[i] ? 1 : 0; marks[i] = 0; }
    won = show_mistakes = pencil = 0; start_time = time(0); end_time = 0;
}
static void restart(void) {
    for (int i = 0; i < 81; i++) { board[i] = given[i]; marks[i] = 0; }
    won = 0; start_time = time(0); end_time = 0;
}

static void draw(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0B1226, 0x10203E);
    int BX = MARGIN, BY = HUD;
    tgl_fill(g, BX - 4, BY - 4, BOARD + 8, BOARD + 8, 0x27324F);
    int selv = board[cy * 9 + cx];
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        int cell = r * 9 + c, x = BX + c * CELL, y = BY + r * CELL;
        uint32_t bg = 0x0C1428;
        int peer = (r == cy || c == cx || (r / 3 == cy / 3 && c / 3 == cx / 3));
        if (r == cy && c == cx) bg = 0x2A3E66;
        else if (peer) bg = 0x172338;
        else if (selv && board[cell] == selv) bg = 0x213A5C;
        tgl_fill(g, x, y, CELL, CELL, bg);
        int wrong = show_mistakes && board[cell] && board[cell] != solution[cell];
        if (conflict(cell) || wrong) tgl_blend(g, x, y, CELL, CELL, 0xE0402A, 70);
    }
    for (int i = 0; i <= N; i++) {                 /* grid lines, thick every 3 */
        int t = (i % 3 == 0) ? 3 : 1;
        uint32_t lc = (i % 3 == 0) ? 0x9AB0D8 : 0x33415F;
        tgl_fill(g, BX + i * CELL - t / 2, BY, t, BOARD, lc);
        tgl_fill(g, BX, BY + i * CELL - t / 2, BOARD, t, lc);
    }
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) {
        int cell = r * 9 + c, x = BX + c * CELL, y = BY + r * CELL;
        int wrong = show_mistakes && board[cell] && board[cell] != solution[cell];
        if (board[cell]) {
            char s[2] = { (char)('0' + board[cell]), 0 };
            uint32_t col = clue[cell] ? 0xEAF2FF : 0x5AC8FF;
            if (conflict(cell) || wrong) col = 0xFF7060;
            text_s(g, x + (CELL - 32) / 2, y + (CELL - 32) / 2, s, col, 4);
        } else if (marks[cell]) {
            for (int d = 1; d <= 9; d++) if (marks[cell] & (1 << d)) {
                int mr = (d - 1) / 3, mc = (d - 1) % 3;
                int sx = x + mc * (CELL / 3) + (CELL / 3 - 8) / 2, sy = y + mr * (CELL / 3) + (CELL / 3 - 8) / 2;
                char s[2] = { (char)('0' + d), 0 };
                text_s(g, sx, sy, s, 0x8CA6CC, 1);
            }
        }
    }
    text_s(g, MARGIN, 12, "SUDOKU", 0xEAF2FF, 3);
    text_s(g, 250, 8, "LEVEL", 0x8CA6CC, 1); text_s(g, 250, 22, DIFFN[diff], 0xEAF2FF, 2);
    long el = (won ? end_time : time(0)) - start_time; if (el < 0) el = 0; if (el > 5999) el = 5999;
    char tb[6]; int mm = (int)(el / 60), ss = (int)(el % 60);
    tb[0] = (char)('0' + mm / 10); tb[1] = (char)('0' + mm % 10); tb[2] = ':';
    tb[3] = (char)('0' + ss / 10); tb[4] = (char)('0' + ss % 10); tb[5] = 0;
    text_s(g, g->w - 92, 8, "TIME", 0x8CA6CC, 1); text_s(g, g->w - 92, 22, tb, 0xEAF2FF, 2);
    if (pencil) text_s(g, 400, 8, "PENCIL", 0xE0C060, 1);

    int fy = BY + BOARD + 10;
    text_s(g, MARGIN, fy, "MOVE arrows/wasd  1-9 PLACE  0 ERASE  P PENCIL", 0x6E86AC, 1);
    text_s(g, MARGIN, fy + 16, "H HINT  C CHECK  TAB LEVEL  N NEW  R RESTART  Q QUIT", 0x6E86AC, 1);

    if (won) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 150);
        text_c(g, g->h / 2 - 30, "SOLVED!", 0x50E080, 4);
        text_c(g, g->h / 2 + 16, "press N for a new puzzle", 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = MARGIN * 2 + BOARD, h = HUD + BOARD + 56;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "sudoku: no display\n"; write(2, e, 19); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    new_game();
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        int c = cy * 9 + cx;
        if (key == 'n' || key == 'N') new_game();
        else if (key == 'r' || key == 'R') restart();
        else if (key == '\t') { diff = (diff + 1) % 3; new_game(); }
        else if (key == 'p' || key == 'P') pencil = !pencil;
        else if (key == 'c' || key == 'C') show_mistakes = !show_mistakes;
        else if (key == 'h' || key == 'H') { if (!clue[c] && !won) { board[c] = solution[c]; marks[c] = 0; if (solved()) { won = 1; end_time = time(0); } } }
        else if (key == KEY_UP || key == 'w' || key == 'W') { if (cy > 0) cy--; }
        else if (key == KEY_DOWN || key == 's' || key == 'S') { if (cy < 8) cy++; }
        else if (key == KEY_LEFT || key == 'a' || key == 'A') { if (cx > 0) cx--; }
        else if (key == KEY_RIGHT || key == 'd' || key == 'D') { if (cx < 8) cx++; }
        else if (key >= '1' && key <= '9') { if (!clue[c] && !won) { int d = key - '0';
            if (pencil && board[c] == 0) marks[c] ^= (unsigned short)(1 << d);
            else { board[c] = d; marks[c] = 0; if (solved()) { won = 1; end_time = time(0); } } } }
        else if (key == '0' || key == ' ' || key == 8 || key == 127) { if (!clue[c] && !won) { board[c] = 0; marks[c] = 0; } }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
