/* 2048 for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /2048.BIN) or on device:
 *     ./tcc.bin /2048.c -o /2048.bin ; ./2048.bin
 *
 * Controls: arrows / W A S D slide, R restart, Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

static int grid[4][4], score, won, over;
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
    int j = 0; while (i) b[j++] = t[--i]; b[j] = 0; return j;
}
static void text_s(tgl_t *g, int x, int y, const char *s, uint32_t c, int sc) {
    for (; *s; s++, x += 8 * sc) {
        unsigned ch = (unsigned char)*s; if (ch < 0x20 || ch > 0x7E) continue;
        const unsigned char *gl = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) { unsigned bits = gl[row];
            for (int col = 0; col < 8; col++) if (bits & (1u << col)) tgl_fill(g, x + col * sc, y + row * sc, sc, sc, c); }
    }
}

static int add_tile(void) {
    int empt[16], n = 0;
    for (int i = 0; i < 16; i++) if (!grid[i / 4][i % 4]) empt[n++] = i;
    if (!n) return 0;
    int p = empt[rnd() % (unsigned)n];
    grid[p / 4][p % 4] = (rnd() % 10 == 0) ? 4 : 2;
    return 1;
}
static void reset_game(void) {
    for (int i = 0; i < 16; i++) grid[i / 4][i % 4] = 0;
    score = won = over = 0; add_tile(); add_tile();
}
/* slide+merge one line of 4 toward index 0; returns 1 if it changed. */
static int slide(int *a) {
    int tmp[4], n = 0, moved = 0;
    for (int i = 0; i < 4; i++) if (a[i]) tmp[n++] = a[i];
    for (int i = n; i < 4; i++) tmp[i] = 0;
    for (int i = 0; i < 3; i++) if (tmp[i] && tmp[i] == tmp[i + 1]) {
        tmp[i] *= 2; score += tmp[i]; if (tmp[i] == 2048) won = 1; tmp[i + 1] = 0; }
    int out[4], m = 0;
    for (int i = 0; i < 4; i++) if (tmp[i]) out[m++] = tmp[i];
    for (int i = m; i < 4; i++) out[i] = 0;
    for (int i = 0; i < 4; i++) { if (a[i] != out[i]) moved = 1; a[i] = out[i]; }
    return moved;
}
/* dir: 0 left, 1 right, 2 up, 3 down */
static int move_dir(int dir) {
    int moved = 0;
    for (int r = 0; r < 4; r++) {
        int line[4], rr, cc;
        for (int i = 0; i < 4; i++) {
            if (dir == 0) { rr = r; cc = i; } else if (dir == 1) { rr = r; cc = 3 - i; }
            else if (dir == 2) { rr = i; cc = r; } else { rr = 3 - i; cc = r; }
            line[i] = grid[rr][cc];
        }
        if (slide(line)) moved = 1;
        for (int i = 0; i < 4; i++) {
            if (dir == 0) { rr = r; cc = i; } else if (dir == 1) { rr = r; cc = 3 - i; }
            else if (dir == 2) { rr = i; cc = r; } else { rr = 3 - i; cc = r; }
            grid[rr][cc] = line[i];
        }
    }
    return moved;
}
static int can_move(void) {
    for (int i = 0; i < 16; i++) if (!grid[i / 4][i % 4]) return 1;
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        if (c < 3 && grid[r][c] == grid[r][c + 1]) return 1;
        if (r < 3 && grid[r][c] == grid[r + 1][c]) return 1;
    }
    return 0;
}
static uint32_t tilecol(int v) {
    switch (v) {
        case 2: return 0xEEE4DA; case 4: return 0xEDE0C8; case 8: return 0xF2B179;
        case 16: return 0xF59563; case 32: return 0xF67C5F; case 64: return 0xF65E3B;
        case 128: return 0xEDCF72; case 256: return 0xEDCC61; case 512: return 0xEDC850;
        case 1024: return 0xEDC53F; default: return 0xEDC22E;
    }
}

#define TS 100
#define GAP 12
#define OX 24
#define OY 96

static void draw(tgl_t *g) {
    tgl_clear(g, 0xFAF8EF);
    char b[16];
    text_s(g, OX, 22, "2048", 0x776E65, 4);
    text_s(g, OX + 260, 24, "SCORE", 0x8F8577, 1);
    int n = num(b, score); text_s(g, OX + 260 + (56 - n * 8), 40, b, 0x5C5449, 2);
    int span = 4 * TS + 3 * GAP;
    tgl_fill(g, OX - 10, OY - 10, span + 20, span + 20, 0xBBADA0);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        int x = OX + c * (TS + GAP), y = OY + r * (TS + GAP), v = grid[r][c];
        tgl_fill(g, x, y, TS, TS, v ? tilecol(v) : 0xCDC1B4);
        if (v) { int len = num(b, v); int sc = len <= 2 ? 4 : (len == 3 ? 3 : 2);
            text_s(g, x + (TS - len * 8 * sc) / 2, y + (TS - 8 * sc) / 2, b, v <= 4 ? 0x776E65 : 0xF9F6F2, sc); }
    }
    if (won && !over) text_s(g, OX, OY - 34, "YOU MADE 2048!", 0xF67C5F, 2);
    if (over) {
        tgl_blend(g, OX - 10, OY - 10, span + 20, span + 20, 0xEEE4DA, 150);
        text_s(g, OX + 90, OY + span / 2 - 30, "GAME OVER", 0x776E65, 3);
        text_s(g, OX + 96, OY + span / 2 + 8, "press R to restart", 0x776E65, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = OX * 2 + 4 * TS + 3 * GAP, h = OY + 4 * TS + 3 * GAP + 24;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "2048: no display\n"; write(2, e, 17); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    reset_game();
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'r' || key == 'R') { reset_game(); usleep(16000); continue; }
        if (!over) {
            int dir = -1;
            if (key == KEY_LEFT || key == 'a' || key == 'A') dir = 0;
            else if (key == KEY_RIGHT || key == 'd' || key == 'D') dir = 1;
            else if (key == KEY_UP || key == 'w' || key == 'W') dir = 2;
            else if (key == KEY_DOWN || key == 's' || key == 'S') dir = 3;
            if (dir >= 0 && move_dir(dir)) { add_tile(); if (!can_move()) over = 1; }
        }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
