/* 15-Puzzle for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /P15.BIN) or on device:
 *     ./tcc.bin /p15.c -o /p15.bin ; ./p15.bin
 *
 * Slide the tiles until they read 1..15 with the gap last.  The board is shuffled
 * by random legal slides so it is always solvable.
 *
 * Controls: arrows / W A S D slide a tile into the gap, N new shuffle,
 * Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define CELL 104
#define MARGIN 16
#define HUD 48
#define BOARD (CELL * 4)

static int tile[16], blank, moves, won;
static long start_time, end_time;
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
static void text_c(tgl_t *g, int y, const char *s, uint32_t c, int sc) {
    int n = 0; for (const char *p = s; *p; p++) n++;
    text_s(g, g->w / 2 - n * 8 * sc / 2, y, s, c, sc);
}

/* slide the tile neighbouring the gap so it moves in direction `d` (0R 1D 2L 3U). */
static int slide(int d) {
    int br = blank / 4, bc = blank % 4, src = -1;
    if (d == 2 && bc < 3) src = blank + 1;          /* LEFT: tile right of the gap slides left */
    else if (d == 0 && bc > 0) src = blank - 1;     /* RIGHT: tile left of the gap slides right */
    else if (d == 3 && br < 3) src = blank + 4;     /* UP: tile below the gap slides up */
    else if (d == 1 && br > 0) src = blank - 4;     /* DOWN: tile above the gap slides down */
    if (src < 0) return 0;
    tile[blank] = tile[src]; tile[src] = 0; blank = src; return 1;
}
static int solved(void) {
    for (int i = 0; i < 15; i++) if (tile[i] != i + 1) return 0;
    return tile[15] == 0;
}
static void new_game(void) {
    for (int i = 0; i < 15; i++) tile[i] = i + 1;
    tile[15] = 0; blank = 15;
    int last = -1;
    for (int k = 0; k < 800; k++) {                 /* shuffle by random legal slides */
        int d = (int)(rnd() % 4);
        if ((d ^ 2) == last) continue;              /* don't immediately undo */
        if (slide(d)) last = d;
    }
    moves = 0; won = 0; start_time = time(0); end_time = 0;
}
static uint32_t tilecol(int v) {
    int t = (v - 1) * 255 / 14;
    int r = 60 + t * 180 / 255, g = 170 - t * 80 / 255, b = 210 - t * 150 / 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static void draw(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0B1226, 0x10203E);
    int BX = MARGIN, BY = HUD;
    tgl_fill(g, BX - 6, BY - 6, BOARD + 12, BOARD + 12, 0x27324F);
    tgl_fill(g, BX, BY, BOARD, BOARD, 0x0C1428);
    for (int i = 0; i < 16; i++) {
        int x = BX + (i % 4) * CELL, y = BY + (i / 4) * CELL, v = tile[i];
        if (!v) continue;
        int placed = (v == i + 1);
        uint32_t c = tilecol(v);
        tgl_fill(g, x + 4, y + 4, CELL - 8, CELL - 8, 0x0A0F1E);
        tgl_fill(g, x + 6, y + 6, CELL - 12, CELL - 12, c);
        tgl_blend(g, x + 6, y + 6, CELL - 12, 4, 0xFFFFFF, 60);
        tgl_blend(g, x + 6, y + CELL - 10, CELL - 12, 4, 0x000000, 70);
        if (placed) tgl_blend(g, x + 6, y + 6, CELL - 12, CELL - 12, 0x40FF80, 40);
        char b[4]; int n = num(b, v);
        text_s(g, x + (CELL - n * 8 * 5) / 2, y + (CELL - 40) / 2, b, 0x101820, 5);
    }
    text_s(g, MARGIN, 12, "15-PUZZLE", 0xEAF2FF, 3);
    char b[16];
    text_s(g, g->w - 150, 8, "MOVES", 0x8CA6CC, 1); num(b, moves); text_s(g, g->w - 150, 22, b, 0xEAF2FF, 2);
    long el = (won ? end_time : time(0)) - start_time; if (el < 0) el = 0; if (el > 5999) el = 5999;
    char tb[6]; int mm = (int)(el / 60), ss = (int)(el % 60);
    tb[0] = (char)('0' + mm / 10); tb[1] = (char)('0' + mm % 10); tb[2] = ':';
    tb[3] = (char)('0' + ss / 10); tb[4] = (char)('0' + ss % 10); tb[5] = 0;
    text_s(g, g->w - 70, 8, "TIME", 0x8CA6CC, 1); text_s(g, g->w - 70, 22, tb, 0xEAF2FF, 2);
    text_s(g, MARGIN, BY + BOARD + 12, "arrows/wasd SLIDE   N NEW   Q QUIT", 0x6E86AC, 1);
    if (won) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 150);
        text_c(g, g->h / 2 - 30, "SOLVED!", 0x50E080, 4);
        char m[24]; int k = 0; const char *pre = "in "; while (pre[k]) { m[k] = pre[k]; k++; }
        k += num(m + k, moves); const char *suf = " moves"; for (int j = 0; suf[j]; j++) m[k++] = suf[j]; m[k] = 0;
        text_c(g, g->h / 2 + 16, m, 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = MARGIN * 2 + BOARD, h = HUD + BOARD + 44;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "p15: no display\n"; write(2, e, 16); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    new_game();
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'n' || key == 'N') new_game();
        else if (!won) {
            int d = -1;
            if (key == KEY_UP || key == 'w' || key == 'W') d = 3;
            else if (key == KEY_DOWN || key == 's' || key == 'S') d = 1;
            else if (key == KEY_LEFT || key == 'a' || key == 'A') d = 2;
            else if (key == KEY_RIGHT || key == 'd' || key == 'D') d = 0;
            if (d >= 0 && slide(d)) { moves++; if (solved()) { won = 1; end_time = time(0); } }
        }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
