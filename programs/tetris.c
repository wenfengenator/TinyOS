/* Tetris for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /TETRIS.BIN) or on device:
 *     ./tcc.bin /tetris.c -o /tetris.bin ; ./tetris.bin
 *
 * Controls: arrows / A D move, W or Up rotate, S or Down soft-drop,
 * Space hard-drop, R restart, Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define BW 10
#define BH 20
#define CELL 26

/* 7 tetrominoes, 4 rotations each; a 16-bit mask over a 4x4 box, bit = row*4+col. */
static const unsigned short PIECES[7][4] = {
    { 0x00F0, 0x2222, 0x00F0, 0x2222 },   /* I */
    { 0x0066, 0x0066, 0x0066, 0x0066 },   /* O */
    { 0x0072, 0x0262, 0x0270, 0x0232 },   /* T */
    { 0x0036, 0x0462, 0x0036, 0x0462 },   /* S */
    { 0x0063, 0x0264, 0x0063, 0x0264 },   /* Z */
    { 0x0071, 0x0226, 0x0470, 0x0322 },   /* J */
    { 0x0074, 0x0622, 0x0170, 0x0223 },   /* L */
};
static const uint32_t COLORS[7] = { 0x00D8D8, 0xD8D800, 0xB050E0, 0x30C830, 0xE04030, 0x3060E0, 0xE08020 };

static unsigned char board[BH][BW];
static int cur, rot, px, py, nextp, score, lines, level, over;
static unsigned long rng;

static unsigned rnd(void) {
    if (!rng) rng = ((unsigned long)time(0) << 1) | 1;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    return (unsigned)(rng >> 5);
}
static int num(char *b, long v) {
    char t[16]; int i = 0; if (v == 0) t[i++] = '0';
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    while (u) { t[i++] = (char)('0' + u % 10); u /= 10; }
    int j = 0; if (v < 0) b[j++] = '-'; while (i) b[j++] = t[--i]; b[j] = 0; return j;
}
/* 8x8 font scaled by `s` (built on the tinygl.h glyph table). */
static void text_s(tgl_t *g, int x, int y, const char *s, uint32_t c, int sc) {
    for (; *s; s++, x += 8 * sc) {
        unsigned ch = (unsigned char)*s; if (ch < 0x20 || ch > 0x7E) continue;
        const unsigned char *gl = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) { unsigned bits = gl[row];
            for (int col = 0; col < 8; col++) if (bits & (1u << col)) tgl_fill(g, x + col * sc, y + row * sc, sc, sc, c); }
    }
}

static int fits(int p, int r, int x, int y) {
    unsigned short m = PIECES[p][r];
    for (int b = 0; b < 16; b++) if (m & (1 << b)) {
        int cc = x + (b & 3), rr = y + (b >> 2);
        if (cc < 0 || cc >= BW || rr >= BH) return 0;
        if (rr >= 0 && board[rr][cc]) return 0;
    }
    return 1;
}
static void spawn(void) {
    cur = nextp; nextp = (int)(rnd() % 7); rot = 0; px = 3; py = 0;
    if (!fits(cur, rot, px, py)) over = 1;
}
static void lock_piece(void) {
    unsigned short m = PIECES[cur][rot];
    for (int b = 0; b < 16; b++) if (m & (1 << b)) {
        int cc = px + (b & 3), rr = py + (b >> 2);
        if (rr >= 0 && rr < BH && cc >= 0 && cc < BW) board[rr][cc] = (unsigned char)(cur + 1);
    }
    int cleared = 0;
    for (int r = BH - 1; r >= 0; r--) {
        int full = 1; for (int c = 0; c < BW; c++) if (!board[r][c]) { full = 0; break; }
        if (full) { cleared++;
            for (int rr = r; rr > 0; rr--) for (int c = 0; c < BW; c++) board[rr][c] = board[rr - 1][c];
            for (int c = 0; c < BW; c++) board[0][c] = 0; r++; }
    }
    if (cleared) { static const int pts[5] = { 0, 40, 100, 300, 1200 };
        score += pts[cleared] * (level + 1); lines += cleared; level = lines / 10; }
    spawn();
}
static void step_down(void) { if (fits(cur, rot, px, py + 1)) py++; else lock_piece(); }
static void reset_game(void) {
    for (int r = 0; r < BH; r++) for (int c = 0; c < BW; c++) board[r][c] = 0;
    score = lines = level = over = 0; nextp = (int)(rnd() % 7); spawn();
}

static void cell(tgl_t *g, int x, int y, int sz, uint32_t col) {
    tgl_fill(g, x, y, sz, sz, 0x0A0F1E);
    tgl_fill(g, x + 1, y + 1, sz - 2, sz - 2, col);
    tgl_blend(g, x + 1, y + 1, sz - 2, 3, 0xFFFFFF, 55);
    tgl_blend(g, x + 1, y + 1, 3, sz - 2, 0xFFFFFF, 55);
}
static void draw(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0B1226, 0x10203E);
    int BX = 24, BY = 24;
    tgl_fill(g, BX - 3, BY - 3, BW * CELL + 6, BH * CELL + 6, 0x27324F);
    tgl_fill(g, BX, BY, BW * CELL, BH * CELL, 0x0C1428);
    for (int r = 0; r < BH; r++) for (int c = 0; c < BW; c++)
        if (board[r][c]) cell(g, BX + c * CELL, BY + r * CELL, CELL, COLORS[board[r][c] - 1]);
    if (!over) { unsigned short m = PIECES[cur][rot];
        for (int b = 0; b < 16; b++) if (m & (1 << b)) { int cc = px + (b & 3), rr = py + (b >> 2);
            if (rr >= 0) cell(g, BX + cc * CELL, BY + rr * CELL, CELL, COLORS[cur]); } }
    int SX = BX + BW * CELL + 26, SY = BY + 4;
    text_s(g, SX, SY, "TETRIS", 0xEAF2FF, 2);
    char b[16];
    text_s(g, SX, SY + 40, "SCORE", 0x8CA6CC, 1); num(b, score); text_s(g, SX, SY + 54, b, 0xEAF2FF, 2);
    text_s(g, SX, SY + 92, "LINES", 0x8CA6CC, 1); num(b, lines); text_s(g, SX, SY + 106, b, 0xEAF2FF, 2);
    text_s(g, SX, SY + 144, "LEVEL", 0x8CA6CC, 1); num(b, level); text_s(g, SX, SY + 158, b, 0xEAF2FF, 2);
    text_s(g, SX, SY + 200, "NEXT", 0x8CA6CC, 1);
    unsigned short nm = PIECES[nextp][0];
    for (int bb = 0; bb < 16; bb++) if (nm & (1 << bb))
        cell(g, SX + (bb & 3) * 20, SY + 220 + (bb >> 2) * 20, 20, COLORS[nextp]);
    if (over) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 160);
        text_s(g, g->w / 2 - 90, g->h / 2 - 20, "GAME OVER", 0xFF6060, 2);
        text_s(g, g->w / 2 - 108, g->h / 2 + 12, "press R to restart", 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = BW * CELL + 200, h = BH * CELL + 48;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "tetris: no display\n"; write(2, e, 19); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    reset_game();
    int fall = 0;
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (over) { if (key == 'r' || key == 'R') reset_game(); usleep(16000); continue; }
        if (key == KEY_LEFT || key == 'a' || key == 'A') { if (fits(cur, rot, px - 1, py)) px--; }
        else if (key == KEY_RIGHT || key == 'd' || key == 'D') { if (fits(cur, rot, px + 1, py)) px++; }
        else if (key == KEY_UP || key == 'w' || key == 'W' || key == 'x') {
            int r = (rot + 1) & 3;
            if (fits(cur, r, px, py)) rot = r;
            else if (fits(cur, r, px - 1, py)) { px--; rot = r; }
            else if (fits(cur, r, px + 1, py)) { px++; rot = r; }
        }
        else if (key == KEY_DOWN || key == 's' || key == 'S') { step_down(); score++; fall = 0; }
        else if (key == ' ') { while (fits(cur, rot, px, py + 1)) { py++; score += 2; } lock_piece(); fall = 0; }
        int dropint = 48 - level * 4; if (dropint < 6) dropint = 6;
        if (++fall >= dropint) { fall = 0; step_down(); }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
