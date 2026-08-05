/* Snake for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /SNAKE.BIN) or on device:
 *     ./tcc.bin /snake.c -o /snake.bin ; ./snake.bin
 *
 * Classic snake: eat the fruit to grow, avoid the walls and yourself; the pace
 * quickens as you score.  Controls: arrows / W A S D turn, P pause, R restart,
 * Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define GW 24
#define GH 18
#define CELL 28
#define HUD 40
#define NCELL (GW * GH)

enum { RIGHT = 0, DOWN = 1, LEFT = 2, UP = 3 };
static const int DX[4] = { 1, 0, -1, 0 };
static const int DY[4] = { 0, 1, 0, -1 };

static int body[NCELL], len, dir, ndir, food, score, best, over, paused, frame;
static unsigned char occ[NCELL];
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
static int isqrt(int v) { if (v < 0) return 0; int x = 0; while ((x + 1) * (x + 1) <= v) x++; return x; }
static void disc(tgl_t *g, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) { int w = isqrt(r * r - dy * dy); tgl_fill(g, cx - w, cy + dy, 2 * w + 1, 1, c); }
}

static void place_food(void) {
    int e[NCELL], n = 0;
    for (int i = 0; i < NCELL; i++) if (!occ[i]) e[n++] = i;
    if (!n) { food = -1; over = 2; return; }
    food = e[rnd() % (unsigned)n];
}
static void reset_game(void) {
    for (int i = 0; i < NCELL; i++) occ[i] = 0;
    int hx = GW / 2, hy = GH / 2; len = 4;
    for (int i = 0; i < len; i++) { int c = hy * GW + (hx - i); body[i] = c; occ[c] = 1; }
    dir = ndir = RIGHT; score = 0; over = paused = 0; frame = 0;
    place_food();
}
static void set_dir(int d) { if (d != ((dir + 2) & 3)) ndir = d; }

static void step(void) {
    dir = ndir;
    int hx = body[0] % GW, hy = body[0] / GW;
    int nx = hx + DX[dir], ny = hy + DY[dir];
    if (nx < 0 || nx >= GW || ny < 0 || ny >= GH) { over = 1; return; }
    int nc = ny * GW + nx, grow = (nc == food), tail = body[len - 1];
    if (!grow) occ[tail] = 0;
    if (occ[nc]) { over = 1; return; }
    if (grow && len < NCELL) len++;
    for (int i = len - 1; i > 0; i--) body[i] = body[i - 1];
    body[0] = nc; occ[nc] = 1;
    if (grow) { score += 10; if (score > best) best = score; place_food(); }
}

static void seg(tgl_t *g, int cell, uint32_t col) {
    int x = (cell % GW) * CELL, y = HUD + (cell / GW) * CELL;
    tgl_fill(g, x, y, CELL, CELL, 0x0A1520);
    tgl_fill(g, x + 2, y + 2, CELL - 4, CELL - 4, col);
    tgl_blend(g, x + 2, y + 2, CELL - 4, 3, 0xFFFFFF, 45);
}
static void draw(tgl_t *g) {
    tgl_vgradient(g, 0, 0, g->w, g->h, 0x0A1A14, 0x0E2A20);
    tgl_fill(g, 0, 0, g->w, HUD, 0x0A2018);
    for (int i = 0; i < NCELL; i++) if (((i % GW) ^ (i / GW)) & 1)
        tgl_blend(g, (i % GW) * CELL, HUD + (i / GW) * CELL, CELL, CELL, 0xFFFFFF, 6);
    if (food >= 0) {
        int fx = (food % GW) * CELL + CELL / 2, fy = HUD + (food / GW) * CELL + CELL / 2;
        tgl_blend(g, fx - CELL / 2, fy - CELL / 2, CELL, CELL, 0xE0402A, 40);
        disc(g, fx, fy, CELL / 2 - 5, 0xF04030); disc(g, fx - 2, fy - 2, 3, 0xFFD0C0);
    }
    for (int i = len - 1; i >= 0; i--) {
        uint32_t col = i == 0 ? 0x60F0B0 : (0x38C888 - (uint32_t)(i * 0x000600));
        seg(g, body[i], col);
    }
    if (len) {                                     /* eyes on the head */
        int x = (body[0] % GW) * CELL, y = HUD + (body[0] / GW) * CELL;
        int ex = DX[dir], ey = DY[dir];
        disc(g, x + CELL / 2 + ex * 4 - 4, y + CELL / 2 + ey * 4 - 4, 2, 0x0A1520);
        disc(g, x + CELL / 2 + ex * 4 + 4, y + CELL / 2 + ey * 4 + 4, 2, 0x0A1520);
    }
    char b[16];
    text_s(g, 10, 12, "SNAKE", 0xEAF2FF, 2);
    text_s(g, 220, 6, "SCORE", 0x8CC7AC, 1); num(b, score); text_s(g, 220, 18, b, 0xEAF2FF, 2);
    text_s(g, 360, 6, "LEN", 0x8CC7AC, 1); num(b, len); text_s(g, 360, 18, b, 0xEAF2FF, 2);
    text_s(g, 480, 6, "BEST", 0x8CC7AC, 1); num(b, best); text_s(g, 480, 18, b, 0xEAF2FF, 2);
    if (paused && !over) { tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 120); text_c(g, g->h / 2 - 12, "PAUSED", 0xFFFFFF, 3); }
    if (over) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 150);
        text_c(g, g->h / 2 - 30, over == 2 ? "YOU WIN!" : "GAME OVER", over == 2 ? 0x60F0B0 : 0xFF6060, 3);
        text_c(g, g->h / 2 + 14, "press R to play again", 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = GW * CELL, h = HUD + GH * CELL;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "snake: no display\n"; write(2, e, 18); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    reset_game();
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'r' || key == 'R') reset_game();
        else if (key == 'p' || key == 'P') { if (!over) paused = !paused; }
        else if (key == KEY_UP || key == 'w' || key == 'W') set_dir(UP);
        else if (key == KEY_DOWN || key == 's' || key == 'S') set_dir(DOWN);
        else if (key == KEY_LEFT || key == 'a' || key == 'A') set_dir(LEFT);
        else if (key == KEY_RIGHT || key == 'd' || key == 'D') set_dir(RIGHT);
        if (!over && !paused) { int mv = 8 - score / 50; if (mv < 3) mv = 3; if (++frame % mv == 0) step(); }
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
