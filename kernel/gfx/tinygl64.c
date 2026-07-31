/* TinyGL (Cycle 7 G2): rendering + composition over the display backend.
 *
 * A surface is a render target — an offscreen pixel buffer object, or the screen
 * (a thin wrapper over the display back buffer, kernel/disp64.c).  The rendering
 * API draws into the bound target (G2: clear; G3 adds rects/blits/text); the
 * composition API blits surfaces together and presents the screen through the
 * active display backend, so TinyGL runs on any transport the seam supports. */
#include "boot64.h"
#include "tinygl.h"
#include "font8x8.h"

static tgl_surface_t  g_screen;                 /* wraps the display back buffer */
static tgl_surface_t *g_target;                 /* current render target         */

/* clip rectangle (target coords) + a small stack for nested clips */
#define TGL_CLIP_MAX 16
static int cl_x0, cl_y0, cl_x1, cl_y1;
static int g_clipst[TGL_CLIP_MAX][4], g_clipn;

/* source-over blend of device pixel `s` over `d` with coverage `a`; done per
 * byte, so it is independent of the RGBX/BGRX channel order. */
static uint32_t tgl_blend(uint32_t d, uint32_t s, uint8_t a) {
    if (a >= 255) return s;
    if (a == 0) return d;
    uint32_t ia = 255u - a, o = 0;
    for (int i = 0; i < 32; i += 8) {
        uint32_t sb = (s >> i) & 0xFF, db = (d >> i) & 0xFF;
        o |= (((sb * a + db * ia) / 255) & 0xFF) << i;
    }
    return o;
}
/* set one pixel in the bound target, clipped, blending device pixel `s` at `a` */
static void px_set(int x, int y, uint32_t s, uint8_t a) {
    tgl_surface_t *t = g_target;
    if (!t || !t->px) return;
    if (x < cl_x0 || x >= cl_x1 || y < cl_y0 || y >= cl_y1) return;
    uint32_t *p = &t->px[(uint32_t)y * t->w + (uint32_t)x];
    *p = tgl_blend(*p, s, a);
}

tgl_surface_t *tgl_screen(void) {
    g_screen.px = disp_back();
    disp_dims(&g_screen.w, &g_screen.h);
    g_screen.owned = 0;
    return &g_screen;
}

tgl_surface_t *tgl_surface_create(uint32_t w, uint32_t h) {
    if (!w || !h) return 0;
    tgl_surface_t *s = (tgl_surface_t *)kmalloc(sizeof *s);
    if (!s) return 0;
    s->px = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!s->px) { kfree(s); return 0; }
    s->w = w; s->h = h; s->owned = 1;
    return s;
}

void tgl_surface_destroy(tgl_surface_t *s) {
    if (!s) return;
    if (g_target == s) g_target = 0;
    if (s->owned && s->px) kfree(s->px);
    kfree(s);
}

void tgl_bind(tgl_surface_t *s) {
    g_target = s; g_clipn = 0;                   /* binding resets the clip to full target */
    cl_x0 = cl_y0 = 0;
    cl_x1 = s ? (int)s->w : 0;
    cl_y1 = s ? (int)s->h : 0;
}
tgl_surface_t *tgl_target(void) { return g_target; }

void tgl_clear(uint32_t rgb) {
    tgl_surface_t *t = g_target;
    if (!t || !t->px) return;
    uint32_t p = fb_pack(rgb), n = t->w * t->h;
    for (uint32_t i = 0; i < n; i++) t->px[i] = p;
}

/* Blit src into the bound target at (x,y), clipped to the target bounds. */
void tgl_compose(const tgl_surface_t *src, int x, int y) {
    tgl_surface_t *t = g_target;
    if (!t || !t->px || !src || !src->px) return;
    for (uint32_t sy = 0; sy < src->h; sy++) {
        int dy = y + (int)sy;
        if (dy < 0 || dy >= (int)t->h) continue;
        for (uint32_t sx = 0; sx < src->w; sx++) {
            int dx = x + (int)sx;
            if (dx < 0 || dx >= (int)t->w) continue;
            t->px[(uint32_t)dy * t->w + (uint32_t)dx] = src->px[sy * src->w + sx];
        }
    }
}

void tgl_present(void) { disp_present(); }

/* ---- 2D primitives (G3): all clipped and alpha-blended via px_set ---- */
void tgl_clip_push(int x, int y, int w, int h) {
    if (g_clipn >= TGL_CLIP_MAX) return;
    g_clipst[g_clipn][0] = cl_x0; g_clipst[g_clipn][1] = cl_y0;
    g_clipst[g_clipn][2] = cl_x1; g_clipst[g_clipn][3] = cl_y1; g_clipn++;
    if (x > cl_x0) cl_x0 = x;
    if (y > cl_y0) cl_y0 = y;
    if (x + w < cl_x1) cl_x1 = x + w;
    if (y + h < cl_y1) cl_y1 = y + h;
    if (cl_x1 < cl_x0) cl_x1 = cl_x0;
    if (cl_y1 < cl_y0) cl_y1 = cl_y0;
}
void tgl_clip_pop(void) {
    if (g_clipn <= 0) return;
    g_clipn--;
    cl_x0 = g_clipst[g_clipn][0]; cl_y0 = g_clipst[g_clipn][1];
    cl_x1 = g_clipst[g_clipn][2]; cl_y1 = g_clipst[g_clipn][3];
}
void tgl_pixel(int x, int y, uint32_t rgb) { px_set(x, y, fb_pack(rgb), 255); }
void tgl_fill_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t a) {
    uint32_t p = fb_pack(rgb);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) px_set(x + i, y + j, p, a);
}
void tgl_stroke_rect(int x, int y, int w, int h, uint32_t rgb) {
    tgl_fill_rect(x, y, w, 1, rgb, 255);
    tgl_fill_rect(x, y + h - 1, w, 1, rgb, 255);
    tgl_fill_rect(x, y, 1, h, rgb, 255);
    tgl_fill_rect(x + w - 1, y, 1, h, rgb, 255);
}
void tgl_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    uint32_t p = fb_pack(rgb);
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    for (;;) {
        px_set(x0, y0, p, 255);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}
void tgl_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bot >> 16) & 0xFF, bg = (bot >> 8) & 0xFF, bb = bot & 0xFF;
    int d = h > 1 ? h - 1 : 1;
    for (int j = 0; j < h; j++) {
        int r = tr + (br - tr) * j / d, g = tg + (bg - tg) * j / d, b = tb + (bb - tb) * j / d;
        uint32_t p = fb_pack(((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b);
        for (int i = 0; i < w; i++) px_set(x + i, y + j, p, 255);
    }
}
void tgl_blit_alpha(const tgl_surface_t *src, int x, int y, uint8_t a) {
    if (!src || !src->px) return;
    for (uint32_t sy = 0; sy < src->h; sy++)
        for (uint32_t sx = 0; sx < src->w; sx++)
            px_set(x + (int)sx, y + (int)sy, src->px[sy * src->w + sx], a);
}
void tgl_text(int x, int y, const char *s, uint32_t rgb) {
    uint32_t p = fb_pack(rgb);
    for (; *s; s++, x += 8) {
        unsigned char c = (unsigned char)*s;
        if (c < FONT_FIRST || c > FONT_LAST) continue;
        const unsigned char *g = font8x8[c - FONT_FIRST];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (1u << col)) px_set(x + col, y + row, p, 255);
    }
}

/* ---- visible G3 demo: alpha-blended, clipped 2D content at a steady frame rate ---- */
void tgl_demo(void) {
    tgl_surface_t *card = tgl_surface_create(220, 90);
    if (card) { tgl_bind(card); tgl_clear(0x1A2540); tgl_stroke_rect(0, 0, 220, 90, 0x88AAFF);
                tgl_text(12, 12, "alpha blit surface", 0xFFFFFF);
                tgl_text(12, 40, "TinyGL / Cycle 7 G3", 0x99FFAA); }
    tgl_surface_t *scr = tgl_screen();
    int W = (int)scr->w, H = (int)scr->h, t = 0;
    for (;;) {
        int k = kbd64_getkey(); if (k < 0) k = hvkbd_getkey();
        if (k == 'q' || k == 'Q' || k == 27) break;
        scr = tgl_screen(); tgl_bind(scr);
        tgl_vgradient(0, 0, W, H, 0x0C1230, 0x301040);
        tgl_text(24, 24, "TinyGL G3 - software 2D renderer   (press Q or ESC to exit)", 0xE6E6FF);
        for (int i = 0; i < 5; i++) {                          /* moving translucent rects */
            int rx = ((t * 3 + i * 160) % (W + 200)) - 100;
            tgl_fill_rect(rx, 120 + i * 70, 180, 60, (i & 1) ? 0xFF4070 : 0x40C0FF, 120);
        }
        for (int i = 0; i < 12; i++)                           /* a line fan */
            tgl_line(W / 2, H - 40, 60 + i * ((W - 120) / 11), H - 220, 0x66FFAA);
        if (card) tgl_blit_alpha(card, 40 + (t * 2) % (W - 260), H - 140, 210);
        int bx = W - 360, by = 60;                             /* clipped panel */
        tgl_clip_push(bx, by, 300, 200);
        tgl_vgradient(bx, by, 300, 200, 0x00FFA0, 0xFF00A0);
        tgl_text(bx + 12, by + 12, "clipped gradient", 0x101010);
        tgl_fill_rect(bx - 40, by + 80, 400, 40, 0xFFFFFF, 90);  /* spills, but clipped */
        tgl_clip_pop();
        tgl_stroke_rect(bx, by, 300, 200, 0xFFFFFF);
        tgl_present();
        t++;
        sleep_ms(16);                                          /* ~60 fps cap */
    }
    tgl_surface_destroy(card);
}

/* G2/G3 gate: clear/compose + fill/blend/clip/line/gradient produce the right
 * pixels — checkable headless without touching the screen. */
int tgl_selftest(void) {
    tgl_surface_t *a = tgl_surface_create(64, 64);
    tgl_surface_t *b = tgl_surface_create(128, 128);
    if (!a || !b) { tgl_surface_destroy(a); tgl_surface_destroy(b); return -1; }
    uint32_t red = fb_pack(0xFF0000), blue = fb_pack(0x0000FF), green = fb_pack(0x00FF00);
    tgl_bind(a); tgl_clear(0xFF0000);
    tgl_bind(b); tgl_clear(0x0000FF);
    tgl_compose(a, 10, 10);                      /* G2: red patch onto the blue surface */
    int ok = a->px[0] == red && b->px[0] == blue &&
             b->px[10u * 128 + 10] == red && b->px[74u * 128 + 74] == blue;
    tgl_bind(b); tgl_clear(0x000000);            /* G3: 50% white over black -> mid grey */
    tgl_fill_rect(0, 0, 128, 128, 0xFFFFFF, 128);
    int mb = (int)(b->px[0] & 0xFF); ok = ok && mb > 100 && mb < 160;
    tgl_bind(b); tgl_clear(0x000000);            /* G3: clip confines the fill */
    tgl_clip_push(10, 10, 20, 20);
    tgl_fill_rect(0, 0, 128, 128, 0x00FF00, 255);
    tgl_clip_pop();
    ok = ok && b->px[15u * 128 + 15] == green && b->px[0] == 0u;
    tgl_bind(b); tgl_clear(0x000000);            /* G3: line endpoints */
    tgl_line(0, 0, 10, 10, 0xFF0000);
    ok = ok && b->px[0] == red && b->px[10u * 128 + 10] == red;
    tgl_bind(b); tgl_vgradient(0, 0, 128, 128, 0x000000, 0xFFFFFF);  /* G3: top darker than bottom */
    ok = ok && b->px[0] < b->px[127u * 128];
    tgl_surface_destroy(a); tgl_surface_destroy(b);
    g_target = 0;
    return ok ? 0 : -2;
}
