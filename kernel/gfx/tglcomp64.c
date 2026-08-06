/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyGL compositor (Cycle 7 G5): composite surfaces as layers with position,
 * z-order, and per-layer alpha, redrawing only the damaged region and presenting
 * through the display backend (whose scanline-diff then blits only changed rows).
 *
 * Layers point at TinyGL surfaces (an app's content, a bitmap, the desktop
 * background).  Moving or damaging a layer accumulates a damage rectangle;
 * tgl_composite() recomposes just that rectangle in z-order and presents.
 * Cross-process apps rendering into shared buffer objects come in G6. */
#include "boot64.h"
#include "tinygl.h"

#define TGL_LAYERS_MAX 16
static tgl_layer_t   g_layers[TGL_LAYERS_MAX];
static int           g_nlayers;
static int           dx0, dy0, dx1, dy1, g_dmg;   /* accumulated damage rect */
static uint32_t      g_bg;                          /* device pixel background */
static tgl_surface_t *g_out;                        /* composite target; 0 = the screen */

static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!g_dmg) { dx0 = x; dy0 = y; dx1 = x + w; dy1 = y + h; g_dmg = 1; return; }
    if (x < dx0) dx0 = x;
    if (y < dy0) dy0 = y;
    if (x + w > dx1) dx1 = x + w;
    if (y + h > dy1) dy1 = y + h;
}

void tgl_comp_clear(void) { g_nlayers = 0; g_dmg = 0; }
void tgl_comp_set_bg(uint32_t rgb) { g_bg = fb_pack(rgb); }
void tgl_comp_target(tgl_surface_t *out) { g_out = out; }

tgl_layer_t *tgl_layer_add(tgl_surface_t *surf, int x, int y, int z, uint8_t alpha) {
    if (!surf || g_nlayers >= TGL_LAYERS_MAX) return 0;
    tgl_layer_t *l = &g_layers[g_nlayers++];
    l->surf = surf; l->x = x; l->y = y; l->z = z; l->visible = 1; l->alpha = alpha;
    damage(x, y, (int)surf->w, (int)surf->h);
    return l;
}
void tgl_layer_move(tgl_layer_t *l, int x, int y) {
    if (!l) return;
    damage(l->x, l->y, (int)l->surf->w, (int)l->surf->h);   /* vacated region */
    l->x = x; l->y = y;
    damage(x, y, (int)l->surf->w, (int)l->surf->h);         /* new region */
}
void tgl_layer_damage(tgl_layer_t *l) {
    if (l) damage(l->x, l->y, (int)l->surf->w, (int)l->surf->h);
}

static uint32_t blend(uint32_t d, uint32_t s, uint8_t a) {
    if (a >= 255) return s;
    uint32_t ia = 255u - a, o = 0;
    for (int i = 0; i < 32; i += 8) {
        uint32_t sb = (s >> i) & 0xFF, db = (d >> i) & 0xFF;
        o |= (((sb * a + db * ia) / 255) & 0xFF) << i;
    }
    return o;
}

/* blit a layer, iterating only its intersection with [cx0,cy0)-[cx1,cy1) */
static void comp_blit(tgl_surface_t *out, const tgl_layer_t *l, int cx0, int cy0, int cx1, int cy1) {
    tgl_surface_t *s = l->surf;
    int x0 = l->x > cx0 ? l->x : cx0, y0 = l->y > cy0 ? l->y : cy0;
    int x1 = l->x + (int)s->w < cx1 ? l->x + (int)s->w : cx1;
    int y1 = l->y + (int)s->h < cy1 ? l->y + (int)s->h : cy1;
    for (int y = y0; y < y1; y++) {
        uint32_t *drow = &out->px[(uint32_t)y * out->w];
        const uint32_t *srow = &s->px[(uint32_t)(y - l->y) * s->w - l->x];
        for (int x = x0; x < x1; x++)
            drow[x] = (l->alpha >= 255) ? srow[x] : blend(drow[x], srow[x], l->alpha);
    }
}

/* Recompose the damaged rectangle in z-order and present it; returns the number
 * of recomposited pixels (0 when nothing changed). */
int tgl_composite(void) {
    if (!g_dmg) return 0;
    tgl_surface_t *out = g_out ? g_out : tgl_screen();
    if (!out || !out->px) { g_dmg = 0; return 0; }
    int x0 = dx0 < 0 ? 0 : dx0, y0 = dy0 < 0 ? 0 : dy0;
    int x1 = dx1 > (int)out->w ? (int)out->w : dx1, y1 = dy1 > (int)out->h ? (int)out->h : dy1;
    g_dmg = 0;
    if (x1 <= x0 || y1 <= y0) return 0;

    for (int y = y0; y < y1; y++)                 /* background under the damage */
        for (int x = x0; x < x1; x++) out->px[(uint32_t)y * out->w + x] = g_bg;

    int order[TGL_LAYERS_MAX];                     /* z-order (ascending), insertion sort */
    for (int i = 0; i < g_nlayers; i++) {
        int j = i; order[j] = i;
        while (j > 0 && g_layers[order[j - 1]].z > g_layers[i].z) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    for (int i = 0; i < g_nlayers; i++) {
        const tgl_layer_t *l = &g_layers[order[i]];
        if (l->visible) comp_blit(out, l, x0, y0, x1, y1);
    }
    if (!g_out) tgl_present();                     /* scanline-diff to the framebuffer */
    return (x1 - x0) * (y1 - y0);
}

/* ---- visible G5 demo: two "app" surfaces composited, damage-limited ---- */
void tgl_comp_demo(void) {
    tgl_surface_t *scr = tgl_screen();
    int W = (int)scr->w, H = (int)scr->h;
    tgl_surface_t *bg = tgl_surface_create(W, H);
    if (bg) { tgl_bind(bg); tgl_vgradient(0, 0, W, H, 0x0A1430, 0x2A0A30);
              tgl_text(24, 24, "TinyGL G5 - compositor: z-order + alpha + damage tracking   (Q/ESC to exit)", 0xD0E0FF); }
    tgl_surface_t *a = tgl_surface_create(240, 150);
    if (a) { tgl_bind(a); tgl_clear(0x1E3E78); tgl_stroke_rect(0, 0, 240, 150, 0xFFFFFF);
             tgl_text(16, 16, "app A  (opaque)", 0xFFFFFF); tgl_fill_rect(20, 48, 200, 84, 0x40C0FF, 255); }
    tgl_surface_t *b = tgl_surface_create(220, 140);
    if (b) { tgl_bind(b); tgl_clear(0x8A2246); tgl_stroke_rect(0, 0, 220, 140, 0xFFFFFF);
             tgl_text(16, 16, "app B  (55% alpha)", 0xFFFFFF); tgl_fill_rect(20, 48, 180, 74, 0xFF80A0, 255); }

    tgl_comp_clear(); tgl_comp_target(0); tgl_comp_set_bg(0x000000);
    if (bg) tgl_layer_add(bg, 0, 0, 0, 255);
    tgl_layer_t *la = tgl_layer_add(a, 120, 160, 1, 255);
    tgl_layer_t *lb = tgl_layer_add(b, 520, 300, 2, 140);
    int ax = 120, ay = 160, adx = 5, ady = 3, bx = 520, by = 300, bdx = -4, bdy = 5;
    for (;;) {
        int k = kbd64_getkey(); if (k < 0) k = hvkbd_getkey();
        if (k == 'q' || k == 'Q' || k == 27) break;
        ax += adx; if (ax < 0 || ax > W - 240) { adx = -adx; ax += adx; }
        ay += ady; if (ay < 40 || ay > H - 150) { ady = -ady; ay += ady; }
        bx += bdx; if (bx < 0 || bx > W - 220) { bdx = -bdx; bx += bdx; }
        by += bdy; if (by < 40 || by > H - 140) { bdy = -bdy; by += bdy; }
        tgl_layer_move(la, ax, ay);
        tgl_layer_move(lb, bx, by);
        tgl_composite();                           /* only the moved boxes' rects redraw */
        sleep_ms(16);
    }
    tgl_comp_clear(); tgl_comp_target(0);
    tgl_surface_destroy(bg); tgl_surface_destroy(a); tgl_surface_destroy(b);
}

/* G5 gate: two layers composited with z-order + alpha into an offscreen target,
 * verified by the resulting pixels (headless-checkable). */
int tglcomp_selftest(void) {
    tgl_surface_t *out = tgl_surface_create(128, 128);
    tgl_surface_t *la  = tgl_surface_create(40, 40);
    tgl_surface_t *lb  = tgl_surface_create(40, 40);
    if (!out || !la || !lb) { tgl_surface_destroy(out); tgl_surface_destroy(la); tgl_surface_destroy(lb); return -1; }
    uint32_t red = fb_pack(0xFF0000), blue = fb_pack(0x0000FF);
    tgl_bind(la); tgl_clear(0xFF0000);
    tgl_bind(lb); tgl_clear(0x0000FF);

    tgl_comp_clear(); tgl_comp_target(out); tgl_comp_set_bg(0x000000);
    tgl_layer_add(la, 10, 10, 0, 255);            /* red z0 opaque, covers 10..50 */
    tgl_layer_add(lb, 30, 30, 1, 128);            /* blue z1 50%, overlaps 30..50 */
    int painted = tgl_composite();
    uint32_t only = out->px[15u * 128 + 15];      /* red-only */
    uint32_t over = out->px[35u * 128 + 35];      /* blend of blue over red */
    uint32_t bgpx = out->px[60u * 128 + 12];      /* inside damage, covered by no layer -> bg */
    int ok = painted > 0 && only == red && bgpx == 0u && over != red && over != blue;

    tgl_comp_clear();                             /* z-order: opaque red on top wins */
    tgl_layer_add(lb, 30, 30, 0, 255);
    tgl_layer_add(la, 10, 10, 1, 255);
    tgl_composite();
    ok = ok && out->px[35u * 128 + 35] == red;

    tgl_comp_clear(); tgl_comp_target(0);
    tgl_surface_destroy(out); tgl_surface_destroy(la); tgl_surface_destroy(lb);
    return ok ? 0 : -2;
}
