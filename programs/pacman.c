/* Pac-Man for TinyOS — renders through the TinyGL user library (tinygl.h).
 *
 * Build off-device with musl-gcc (staged as /PACMAN.BIN) or on device:
 *     ./tcc.bin /pacman.c -o /pacman.bin ; ./pacman.bin
 *
 * A colourful, classic Pac-Man: a neon-blue maze, 254 pellets + 4 energizers,
 * smooth tile-aligned motion, and four ghosts with distinct personalities
 * (chase / scatter / frightened).  Eat an energizer to turn the tables.
 *
 * Controls: arrows / W A S D move, P pause, R restart, Q or Esc quit. */
#include <tinygl.h>
#include <unistd.h>
#include <time.h>

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_ESC 0x1B

#define COLS 28
#define ROWS 31
#define CELL 20
#define HUD  40

#define BG     0x000010
#define HUDBG  0x0A0A28

enum { RIGHT = 0, DOWN = 1, LEFT = 2, UP = 3 };
enum { GNORMAL = 0, GFRIGHT = 1, GEATEN = 2 };

#define FRIGHT_FR  420
#define SCATTER_FR 300
#define CHASE_FR   1200
#define READY_FR   90

/* The maze.  '#' wall, '.' pellet, 'o' energizer, ' ' open path, '=' ghost door.
 * Verified: every pellet is reachable from Pac's start and the side tunnel wraps. */
static const char *MAZE0[ROWS] = {
    "############################",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#o####.#####.##.#####.####o#",
    "#.####.#####.##.#####.####.#",
    "#..........................#",
    "#.####.##.########.##.####.#",
    "#.####.##.########.##.####.#",
    "#......##....##....##......#",
    "######.#####.##.#####.######",
    "######.#####.##.#####.######",
    "######.##          ##.######",
    "######.## ###==### ##.######",
    "######.## #      # ##.######",
    "      .   #      #   .      ",
    "######.## #      # ##.######",
    "######.## ######## ##.######",
    "######.##          ##.######",
    "######.##.########.##.######",
    "######.##.########.##.######",
    "#............##............#",
    "#.####.#####.##.#####.####.#",
    "#.####.#####.##.#####.####.#",
    "#o..##................##..o#",
    "###.##.##.########.##.##.###",
    "###.##.##.########.##.##.###",
    "#......##....##....##......#",
    "#.##########.##.##########.#",
    "#.##########.##.##########.#",
    "#..........................#",
    "############################",
};

static char maze[ROWS][COLS + 1];
static int  pellets;

static const int DX[4]  = { 1, 0, -1, 0 };
static const int DY[4]  = { 0, 1, 0, -1 };
static const int ANG[4] = { 0, 64, 128, 192 };   /* dir -> tgl sin/cos angle */
static const int ORD[4] = { UP, LEFT, DOWN, RIGHT };  /* tie-break preference */

typedef struct {
    int x, y, dir;
    int sc, sr;           /* start tile (col,row)   */
    int cc, cr;           /* scatter corner (col,row) */
    uint32_t color;
    int state, release, left;
} Ghost;

static Ghost gh[4];
static int   px, py, pdir, pwant;
static int   score, lives, level, over, paused, frame;
static int   fright, ghost_pts, ready, mode_timer, scatter;
static unsigned long rng;

static const int GSC[4]  = { 13, 13, 11, 16 };
static const int GSR[4]  = { 11, 14, 14, 14 };
static const int GCC[4]  = { COLS + 1, -2, COLS + 1, -2 };
static const int GCR[4]  = { -3, -3, ROWS + 2, ROWS + 2 };
static const uint32_t GCOL[4] = { 0xFF2A2A, 0xFF9CCE, 0x38E1E1, 0xFF9E3D };
static const int GREL[4] = { 0, 40, 120, 220 };
static const int GLEFT[4] = { 1, 0, 0, 0 };
static const int GDIR[4] = { LEFT, UP, UP, UP };

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
static int rev(int d) { return (d + 2) & 3; }
static int isqrt(int v) { if (v < 0) return 0; int x = 0; while ((x + 1) * (x + 1) <= v) x++; return x; }

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

/* filled disc, scanline */
static void disc(tgl_t *g, int cx, int cy, int r, uint32_t c) {
    for (int dy = -r; dy <= r; dy++) { int w = isqrt(r * r - dy * dy); tgl_fill(g, cx - w, cy + dy, 2 * w + 1, 1, c); }
}
static long edge(int ax, int ay, int bx, int by, int x, int y) {
    return (long)(bx - ax) * (y - ay) - (long)(by - ay) * (x - ax);
}
/* filled triangle (bounding-box barycentric — triangles here are small) */
static void tri(tgl_t *g, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    int minx = x0, maxx = x0, miny = y0, maxy = y0;
    if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
    if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
    if (y1 < miny) miny = y1; if (y1 > maxy) maxy = y1;
    if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
    long area = edge(x0, y0, x1, y1, x2, y2); if (area == 0) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        long w0 = edge(x1, y1, x2, y2, x, y), w1 = edge(x2, y2, x0, y0, x, y), w2 = edge(x0, y0, x1, y1, x, y);
        if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) tgl_pixel(g, x, y, c);
    }
}

static void draw_pac(tgl_t *g, int cx, int cy, int r, int dir, int m, uint32_t bg) {
    disc(g, cx, cy, r, 0xFFE31F);
    int a = ANG[dir];
    int x1 = cx + (int)(((long)r * tgl_icos(a - m)) >> 16), y1 = cy + (int)(((long)r * tgl_isin(a - m)) >> 16);
    int x2 = cx + (int)(((long)r * tgl_icos(a + m)) >> 16), y2 = cy + (int)(((long)r * tgl_isin(a + m)) >> 16);
    tri(g, cx, cy, x1, y1, x2, y2, bg);
}

static void draw_ghost(tgl_t *g, int i) {
    Ghost *G = &gh[i];
    int cx = G->x + CELL / 2, cy = HUD + G->y + CELL / 2, r = CELL / 2 - 1;
    int flash = (G->state == GFRIGHT && fright < 120 && ((fright / 8) & 1));
    if (G->state != GEATEN) {
        uint32_t body = G->color;
        if (G->state == GFRIGHT) body = flash ? 0xF0F0F0 : 0x2020E0;
        tgl_fill(g, cx - r, cy, 2 * r + 1, r, body);
        disc(g, cx, cy, r, body);
        int yb = cy + r;                                   /* carve wavy feet */
        for (int f = 0; f < 3; f++) { int fx = cx - r + (2 * r) * (2 * f + 1) / 6;
            tri(g, fx - r / 3, yb, fx + r / 3, yb, fx, yb - r / 2, BG); }
    }
    int ex = DX[G->dir], ey = DY[G->dir], er = r / 2, pr = r / 4;
    int lx = cx - r / 2, rx = cx + r / 2, ey0 = cy - r / 4;
    if (G->state == GFRIGHT) {
        uint32_t fc = flash ? 0xE02020 : 0xF7B8D0;
        tgl_fill(g, lx - 2, ey0 - 2, 4, 4, fc); tgl_fill(g, rx - 2, ey0 - 2, 4, 4, fc);
        for (int k = -2; k <= 2; k++) tgl_fill(g, cx + k * 3, cy + r / 2 + (k & 1 ? 2 : 0) - 1, 3, 2, fc);
    } else {
        disc(g, lx, ey0, er, 0xFFFFFF); disc(g, rx, ey0, er, 0xFFFFFF);
        disc(g, lx + ex * pr, ey0 + ey * pr, pr, 0x2222CC); disc(g, rx + ex * pr, ey0 + ey * pr, pr, 0x2222CC);
    }
}

static int blocked(int tr, int tc, int d, int ghost) {
    int nr = tr + DY[d], nc = tc + DX[d];
    if (nc < 0) nc = COLS - 1; if (nc >= COLS) nc = 0;
    if (nr < 0 || nr >= ROWS) return 1;
    char ch = maze[nr][nc];
    if (ch == '#') return 1;
    if (ch == '=' && !ghost) return 1;
    return 0;
}

static void ghost_target(int i, int *otr, int *otc) {
    Ghost *G = &gh[i];
    int tr = G->y / CELL, tc = G->x / CELL;
    if (G->state == GEATEN) { *otr = 14; *otc = 13; return; }
    if (!G->left) { *otr = 11; *otc = 13; return; }         /* steer out of the house */
    if (scatter) { *otr = G->cr; *otc = G->cc; return; }
    int pr = py / CELL, pc = px / CELL;
    if (i == 0) { *otr = pr; *otc = pc; }
    else if (i == 1) { *otr = pr + 4 * DY[pdir]; *otc = pc + 4 * DX[pdir]; }
    else if (i == 2) { int ax = pc + 2 * DX[pdir], ay = pr + 2 * DY[pdir];
        int br = gh[0].y / CELL, bc = gh[0].x / CELL; *otr = 2 * ay - br; *otc = 2 * ax - bc; }
    else { long dr = pr - tr, dc = pc - tc;
        if (dr * dr + dc * dc > 64) { *otr = pr; *otc = pc; } else { *otr = G->cr; *otc = G->cc; } }
}

static void update_ghost(int i) {
    Ghost *G = &gh[i];
    if (G->release > 0) { G->release--; return; }
    int speed = G->state == GEATEN ? 4 : (G->state == GFRIGHT ? 1 : 2);
    if (G->x % CELL == 0 && G->y % CELL == 0) {
        int tc = G->x / CELL, tr = G->y / CELL;
        if (G->state == GEATEN && tr == 14 && tc == 13) { G->state = GNORMAL; G->left = 0; G->dir = UP; }
        if (G->state != GEATEN && !G->left && tr < 12) G->left = 1;
        int otr, otc; ghost_target(i, &otr, &otc);
        int best = -1;
        if (G->state == GFRIGHT) {
            int cand[4], n = 0;
            for (int k = 0; k < 4; k++) { int d = ORD[k];
                if (d == rev(G->dir)) continue; if (!blocked(tr, tc, d, 1)) cand[n++] = d; }
            best = n ? cand[rnd() % (unsigned)n] : rev(G->dir);
        } else {
            long bd = -1;
            for (int k = 0; k < 4; k++) { int d = ORD[k];
                if (d == rev(G->dir) || blocked(tr, tc, d, 1)) continue;
                int nr = tr + DY[d], nc = tc + DX[d]; if (nc < 0) nc = COLS - 1; if (nc >= COLS) nc = 0;
                long dist = (long)(nr - otr) * (nr - otr) + (long)(nc - otc) * (nc - otc);
                if (bd < 0 || dist < bd) { bd = dist; best = d; }
            }
            if (best < 0) best = rev(G->dir);
        }
        G->dir = best;
        if (blocked(tr, tc, G->dir, 1)) return;
    }
    G->x += DX[G->dir] * speed;
    if (G->x < 0) G->x += COLS * CELL; if (G->x >= COLS * CELL) G->x -= COLS * CELL;
    G->y += DY[G->dir] * speed;
}

static void start_fright(void) {
    fright = FRIGHT_FR; ghost_pts = 200;
    for (int i = 0; i < 4; i++) if (gh[i].state != GEATEN) { gh[i].state = GFRIGHT; gh[i].dir = rev(gh[i].dir); }
}

static void update_pac(void) {
    if (px % CELL == 0 && py % CELL == 0) {
        int tc = px / CELL, tr = py / CELL;
        char *ch = &maze[tr][tc];
        if (*ch == '.') { *ch = ' '; score += 10; pellets--; }
        else if (*ch == 'o') { *ch = ' '; score += 50; pellets--; start_fright(); }
        if (!blocked(tr, tc, pwant, 0)) pdir = pwant;
        if (blocked(tr, tc, pdir, 0)) return;
    }
    px += DX[pdir] * 2;
    if (px < 0) px += COLS * CELL; if (px >= COLS * CELL) px -= COLS * CELL;
    py += DY[pdir] * 2;
}

static void reset_positions(void) {
    for (int i = 0; i < 4; i++) {
        gh[i].x = GSC[i] * CELL; gh[i].y = GSR[i] * CELL; gh[i].dir = GDIR[i];
        gh[i].sc = GSC[i]; gh[i].sr = GSR[i]; gh[i].cc = GCC[i]; gh[i].cr = GCR[i];
        gh[i].color = GCOL[i]; gh[i].state = GNORMAL; gh[i].release = GREL[i]; gh[i].left = GLEFT[i];
    }
    px = 13 * CELL; py = 23 * CELL; pdir = LEFT; pwant = LEFT;
    fright = 0; ghost_pts = 200; scatter = 1; mode_timer = SCATTER_FR;
}
static void load_maze(void) {
    pellets = 0;
    for (int r = 0; r < ROWS; r++) { for (int c = 0; c < COLS; c++) {
        char ch = MAZE0[r][c]; maze[r][c] = ch; if (ch == '.' || ch == 'o') pellets++; } maze[r][COLS] = 0; }
}
static void die(void) {
    lives--; fright = 0;
    if (lives < 0) { over = 1; return; }
    reset_positions(); ready = READY_FR;
}
static void collisions(void) {
    for (int i = 0; i < 4; i++) {
        int dx = px - gh[i].x, dy = py - gh[i].y;
        if (dx * dx + dy * dy < CELL * CELL / 2) {
            if (gh[i].state == GFRIGHT) { score += ghost_pts; ghost_pts *= 2; gh[i].state = GEATEN; }
            else if (gh[i].state == GNORMAL) { die(); return; }
        }
    }
}
static void new_game(void) {
    score = 0; lives = 3; level = 1; over = 0; paused = 0;
    load_maze(); reset_positions(); ready = READY_FR;
}

static void draw(tgl_t *g) {
    tgl_fill(g, 0, 0, g->w, g->h, BG);
    tgl_fill(g, 0, 0, g->w, HUD, HUDBG);
    tgl_blend(g, 0, HUD - 2, g->w, 2, 0x3A3AF6, 120);
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        int x = c * CELL, y = HUD + r * CELL; char ch = maze[r][c];
        if (ch == '#') { tgl_vgradient(g, x, y, CELL, CELL, 0x3A3AF6, 0x1414B0);
            tgl_blend(g, x, y, CELL, 1, 0x9A9AFF, 110); tgl_blend(g, x, y + CELL - 1, CELL, 1, 0x000030, 120); }
        else if (ch == '=') tgl_fill(g, x, y + CELL / 2 - 2, CELL, 4, 0xF7B8D0);
        else if (ch == '.') tgl_fill(g, x + CELL / 2 - 1, y + CELL / 2 - 1, 3, 3, 0xF7E3A1);
        else if (ch == 'o') { if (((frame >> 4) & 1) == 0) disc(g, x + CELL / 2, y + CELL / 2, CELL / 2 - 3, 0xFFD6F0); }
    }
    int ph = frame % 16, tw = ph < 8 ? ph : 16 - ph, m = 4 + tw * 3;
    if (paused || ready > 0) m = 8;
    draw_pac(g, px + CELL / 2, HUD + py + CELL / 2, CELL / 2 - 1, pdir, m, BG);
    for (int i = 0; i < 4; i++) draw_ghost(g, i);

    char b[16];
    text_s(g, 8, 4, "SCORE", 0x8FA8D6, 1); num(b, score); text_s(g, 8, 16, b, 0xFFFFFF, 2);
    text_s(g, g->w / 2 - 56, 6, "PAC-MAN", 0xFFE31F, 2);
    text_c(g, 26, "LEVEL", 0x8FA8D6, 1);
    for (int i = 0; i < lives && i < 5; i++) draw_pac(g, g->w - 16 - i * 22, HUD / 2, 8, LEFT, 7, HUDBG);

    if (ready > 0 && !over) text_c(g, HUD + 17 * CELL, "READY!", 0xFFE31F, 2);
    if (paused && !over) { tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 120); text_c(g, g->h / 2 - 12, "PAUSED", 0xFFFFFF, 3); }
    if (over) {
        tgl_blend(g, 0, 0, g->w, g->h, 0x000000, 160);
        text_c(g, g->h / 2 - 30, "GAME OVER", 0xFF5050, 3);
        text_c(g, g->h / 2 + 14, "PRESS R TO RESTART", 0xEAF2FF, 1);
    }
}

int main(void) {
    int sw = 1024, sh = 768; tgl_screen_size(&sw, &sh);
    int w = COLS * CELL, h = HUD + ROWS * CELL;
    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { static const char *e = "pacman: no display\n"; write(2, e, 19); return 1; }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2; if (ox < 0) ox = 0; if (oy < 0) oy = 0;
    new_game();
    for (;;) {
        draw(&g);
        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == KEY_ESC) break;
        if (key == 'r' || key == 'R') new_game();
        else if (key == 'p' || key == 'P') { if (!over) paused = !paused; }
        else if (key == KEY_UP || key == 'w' || key == 'W') pwant = UP;
        else if (key == KEY_DOWN || key == 's' || key == 'S') pwant = DOWN;
        else if (key == KEY_LEFT || key == 'a' || key == 'A') pwant = LEFT;
        else if (key == KEY_RIGHT || key == 'd' || key == 'D') pwant = RIGHT;

        if (!over && !paused) {
            if (ready > 0) ready--;
            else {
                update_pac();
                for (int i = 0; i < 4; i++) update_ghost(i);
                collisions();
                if (!over) {
                    if (fright > 0) { if (--fright == 0) for (int i = 0; i < 4; i++) if (gh[i].state == GFRIGHT) gh[i].state = GNORMAL; }
                    else if (--mode_timer <= 0) { scatter = !scatter; mode_timer = scatter ? SCATTER_FR : CHASE_FR;
                        for (int i = 0; i < 4; i++) if (gh[i].state == GNORMAL) gh[i].dir = rev(gh[i].dir); }
                    if (pellets == 0) { level++; load_maze(); reset_positions(); ready = READY_FR; }
                }
            }
        }
        frame++;
        usleep(16000);
    }
    tgl_finish(&g);
    return 0;
}
