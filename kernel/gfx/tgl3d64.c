/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyGL 3D (Cycle 7 G4): a small fixed-point software triangle rasterizer.
 *
 * The kernel builds -mno-sse (no FPU), so all math is 16.16 fixed point; x86-64
 * has native 64-bit divide, so no libgcc helpers are needed.  Features: an
 * edge-function rasterizer with a per-target depth buffer, back-face culling,
 * a crude near clip, and perspective-correct texture mapping — enough for a
 * spinning, textured, depth-tested cube (the G4 gate).  Draws into the surface
 * bound via tgl_bind(); G5 will composite these surfaces. */
#include "boot64.h"
#include "tinygl.h"

#define FX     16
#define FXONE  (1 << FX)
typedef int32_t fx;

static inline fx fxmul(fx a, fx b) { return (fx)(((int64_t)a * b) >> FX); }
static inline fx fxdiv(fx a, fx b) { return b ? (fx)(((int64_t)a << FX) / b) : 0; }

/* ---- fixed-point sine: a 65-entry quarter table (Taylor), full period by symmetry ---- */
static fx  g_sinq[65];
static int g_sin_ready;
static void sin_init(void) {
    for (int i = 0; i <= 64; i++) {
        int64_t x  = (int64_t)i * 102944 / 64;   /* i*(pi/2)/64, 16.16 (pi/2 = 102944) */
        int64_t x2 = (x * x) >> FX;
        int64_t x3 = (x2 * x) >> FX;
        int64_t x5 = (((x3 * x) >> FX) * x) >> FX;
        int64_t x7 = (((x5 * x) >> FX) * x) >> FX;
        g_sinq[i] = (fx)(x - x3 / 6 + x5 / 120 - x7 / 5040);
    }
    g_sin_ready = 1;
}
static fx isin(int a) {                          /* a in 0..255 == 0..2pi */
    a &= 255;
    if (a <= 64)  return  g_sinq[a];
    if (a <= 128) return  g_sinq[128 - a];
    if (a <= 192) return -g_sinq[a - 128];
    return -g_sinq[256 - a];
}
static fx icos(int a) { return isin(a + 64); }

/* ---- per-target depth buffer + rasterizer state ---- */
static fx      *g_zbuf;
static uint32_t g_zw, g_zh;
static int      g_zon, g_cull;

void tgl_depth_enable(void) {
    tgl_surface_t *t = tgl_target();
    if (!t) return;
    if (g_zbuf && (g_zw != t->w || g_zh != t->h)) { kfree(g_zbuf); g_zbuf = 0; }
    if (!g_zbuf) { g_zbuf = (fx *)kmalloc((uint64_t)t->w * t->h * 4); g_zw = t->w; g_zh = t->h; }
    g_zon = g_zbuf ? 1 : 0;
    tgl_depth_clear();
}
void tgl_depth_clear(void) { if (g_zbuf) memset(g_zbuf, 0, (uint64_t)g_zw * g_zh * 4); }  /* 0 = far */
void tgl_depth_free(void)  { if (g_zbuf) kfree(g_zbuf); g_zbuf = 0; g_zon = 0; }
void tgl_cull(int on)      { g_cull = on; }

static uint32_t tex_sample(const tgl_texture_t *tex, fx u, fx v) {
    if (!tex || !tex->px) return 0;
    u &= (FXONE - 1); v &= (FXONE - 1);          /* tile into [0,1) */
    int tx = (int)(((int64_t)(uint32_t)u * tex->w) >> FX);
    int ty = (int)(((int64_t)(uint32_t)v * tex->h) >> FX);
    if (tx >= tex->w) tx = tex->w - 1;
    if (ty >= tex->h) ty = tex->h - 1;
    return tex->px[ty * tex->w + tx];
}
static uint32_t modulate(uint32_t p, int s) {    /* per-byte brightness, s in 0..256 */
    if (s >= 256) return p;
    uint32_t o = 0;
    for (int i = 0; i < 32; i += 8) o |= (((((p >> i) & 0xFF) * (uint32_t)s) >> 8) & 0xFF) << i;
    return o;
}

/* Rasterize one screen-space triangle (verts carry 1/z + texcoords) with the
 * depth test, back-face cull, and perspective-correct sampling. */
void tgl_tri(const tgl_v_t *a, const tgl_v_t *b, const tgl_v_t *c,
             const tgl_texture_t *tex, int shade) {
    tgl_surface_t *t = tgl_target();
    if (!t || !t->px) return;
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    int back = area < 0;
    if (g_cull && back) return;
    uint64_t uarea = back ? (uint64_t)(-area) : (uint64_t)area;

    int minx = a->x, maxx = a->x, miny = a->y, maxy = a->y;
    if (b->x < minx) minx = b->x;
    if (b->x > maxx) maxx = b->x;
    if (c->x < minx) minx = c->x;
    if (c->x > maxx) maxx = c->x;
    if (b->y < miny) miny = b->y;
    if (b->y > maxy) maxy = b->y;
    if (c->y < miny) miny = c->y;
    if (c->y > maxy) maxy = c->y;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= (int)t->w) maxx = (int)t->w - 1;
    if (maxy >= (int)t->h) maxy = (int)t->h - 1;

    fx uiw_a = fxmul(a->u, a->iw), viw_a = fxmul(a->v, a->iw);
    fx uiw_b = fxmul(b->u, b->iw), viw_b = fxmul(b->v, b->iw);
    fx uiw_c = fxmul(c->u, c->iw), viw_c = fxmul(c->v, c->iw);

    for (int py = miny; py <= maxy; py++) {
        for (int px = minx; px <= maxx; px++) {
            int64_t e0 = (int64_t)(c->x - b->x) * (py - b->y) - (int64_t)(c->y - b->y) * (px - b->x);
            int64_t e1 = (int64_t)(a->x - c->x) * (py - c->y) - (int64_t)(a->y - c->y) * (px - c->x);
            int64_t e2 = (int64_t)(b->x - a->x) * (py - a->y) - (int64_t)(b->y - a->y) * (px - a->x);
            if (back) { e0 = -e0; e1 = -e1; e2 = -e2; }
            if (e0 < 0 || e1 < 0 || e2 < 0) continue;

            uint64_t iw = ((uint64_t)e0 * (uint32_t)a->iw + (uint64_t)e1 * (uint32_t)b->iw +
                           (uint64_t)e2 * (uint32_t)c->iw) / uarea;
            if (!iw) continue;
            uint32_t idx = (uint32_t)py * t->w + (uint32_t)px;
            if (g_zon) { if ((fx)iw <= g_zbuf[idx]) continue; g_zbuf[idx] = (fx)iw; }

            uint64_t uiw = ((uint64_t)e0 * (uint32_t)uiw_a + (uint64_t)e1 * (uint32_t)uiw_b +
                            (uint64_t)e2 * (uint32_t)uiw_c) / uarea;
            uint64_t viw = ((uint64_t)e0 * (uint32_t)viw_a + (uint64_t)e1 * (uint32_t)viw_b +
                            (uint64_t)e2 * (uint32_t)viw_c) / uarea;
            fx u = (fx)((uiw << FX) / iw);        /* perspective divide */
            fx v = (fx)((viw << FX) / iw);
            t->px[idx] = modulate(tex_sample(tex, u, v), shade);
        }
    }
}

/* ---- spinning textured cube demo (terminal `tgl3d`) ---- */
static const fx cubev[8][3] = {
    {-FXONE,-FXONE,-FXONE}, { FXONE,-FXONE,-FXONE}, { FXONE, FXONE,-FXONE}, {-FXONE, FXONE,-FXONE},
    {-FXONE,-FXONE, FXONE}, { FXONE,-FXONE, FXONE}, { FXONE, FXONE, FXONE}, {-FXONE, FXONE, FXONE},
};
static const int cubef[6][4] = {
    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
};

void tgl_mesh_demo(void) {
    if (!g_sin_ready) sin_init();
    tgl_texture_t tex; tex.w = 64; tex.h = 64;
    tex.px = (uint32_t *)kmalloc((uint64_t)tex.w * tex.h * 4);
    if (tex.px) for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++)
        tex.px[y * 64 + x] = fb_pack(((x >> 3) ^ (y >> 3)) & 1 ? 0xF0D080 : 0x9A4E24);

    tgl_surface_t *scr = tgl_screen();
    int W = (int)scr->w, H = (int)scr->h, CX = W / 2, CY = H / 2;
    fx FOCAL = (fx)((int64_t)(H / 2) * FXONE);
    fx DIST  = 4 * FXONE, NEAR = FXONE / 2;
    int ang = 0;

    for (;;) {
        int k = kbd64_getkey(); if (k < 0) k = hvkbd_getkey();
        if (k == 'q' || k == 'Q' || k == 27) break;
        scr = tgl_screen(); tgl_bind(scr);
        tgl_vgradient(0, 0, W, H, 0x0A1226, 0x000004);
        tgl_text(24, 24, "TinyGL G4 - software 3D rasterizer   (press Q or ESC to exit)", 0xC8E0FF);
        tgl_depth_enable();

        fx sy = isin(ang), cy = icos(ang);
        int ax = (ang * 3 / 4) & 255;
        fx sx = isin(ax), cx = icos(ax);
        tgl_v_t sv[8]; int behind[8];
        for (int i = 0; i < 8; i++) {
            fx x = cubev[i][0], y = cubev[i][1], z = cubev[i][2];
            fx x1 =  fxmul(x, cy) + fxmul(z, sy);       /* rotate about Y */
            fx z1 = -fxmul(x, sy) + fxmul(z, cy);
            fx y2 =  fxmul(y, cx) - fxmul(z1, sx);       /* rotate about X */
            fx z2 =  fxmul(y, sx) + fxmul(z1, cx) + DIST;
            behind[i] = z2 < NEAR;
            fx iw = fxdiv(FXONE, z2 < NEAR ? NEAR : z2);
            sv[i].x  = CX + (int)(fxmul(fxmul(x1, FOCAL), iw) >> FX);
            sv[i].y  = CY - (int)(fxmul(fxmul(y2, FOCAL), iw) >> FX);
            sv[i].iw = iw;
        }
        static const fx fu[4] = {0, FXONE, FXONE, 0}, fv[4] = {0, 0, FXONE, FXONE};
        for (int f = 0; f < 6; f++) {
            int *q = (int *)cubef[f];
            if (behind[q[0]] || behind[q[1]] || behind[q[2]] || behind[q[3]]) continue;  /* near clip */
            tgl_v_t v[4];
            for (int j = 0; j < 4; j++) { v[j] = sv[q[j]]; v[j].u = fu[j]; v[j].v = fv[j]; }
            int shade = 150 + f * 16;
            tgl_tri(&v[0], &v[1], &v[2], &tex, shade);
            tgl_tri(&v[0], &v[2], &v[3], &tex, shade);
        }
        tgl_present();
        ang = (ang + 2) & 255;
        sleep_ms(20);
    }
    tgl_depth_free();
    if (tex.px) kfree(tex.px);
}

/* G4 gate: raster + depth test + back-face cull produce the right pixels
 * (checkable headless, offscreen). */
int tgl3d_selftest(void) {
    if (!g_sin_ready) sin_init();
    tgl_surface_t *s = tgl_surface_create(64, 64);
    if (!s) return -1;
    uint32_t red = fb_pack(0xFF0000), grn = fb_pack(0x00FF00);
    uint32_t rp = red, gp = grn;
    tgl_texture_t rtex = { &rp, 1, 1 }, gtex = { &gp, 1, 1 };
    tgl_v_t na = {2,2,FXONE,0,0}, nb = {61,2,FXONE,0,0}, nc = {30,61,FXONE,0,0};   /* iw=1 near */
    tgl_v_t fa = {2,2,FXONE/2,0,0}, fb2 = {61,2,FXONE/2,0,0}, fc = {30,61,FXONE/2,0,0}; /* iw=1/2 far */
    int cx = 30, cy = 20, ci = cy * 64 + cx;

    tgl_bind(s); tgl_clear(0x000000); tgl_depth_enable(); tgl_cull(0);
    tgl_tri(&na, &nb, &nc, &rtex, 256);                 /* near red */
    int ok = s->px[ci] == red;
    tgl_tri(&fa, &fb2, &fc, &gtex, 256);                /* far green loses depth test */
    ok = ok && s->px[ci] == red;

    tgl_clear(0x000000); tgl_depth_clear();
    tgl_tri(&fa, &fb2, &fc, &gtex, 256);                /* far first */
    tgl_tri(&na, &nb, &nc, &rtex, 256);                 /* near wins regardless of order */
    ok = ok && s->px[ci] == red;

    tgl_clear(0x000000); tgl_depth_clear(); tgl_cull(1);
    tgl_tri(&nc, &nb, &na, &rtex, 256);                 /* reversed winding -> culled */
    ok = ok && s->px[ci] == 0u;
    tgl_tri(&na, &nb, &nc, &rtex, 256);                 /* front-facing -> drawn */
    ok = ok && s->px[ci] == red;
    tgl_cull(0);

    tgl_depth_free(); tgl_surface_destroy(s);
    return ok ? 0 : -2;
}
