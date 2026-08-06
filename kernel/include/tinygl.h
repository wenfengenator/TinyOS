/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyGL (Cycle 7): a small bespoke graphics API — a rendering API that draws
 * into a surface, and a composition API that places surfaces on screen and
 * presents through the display backend (kernel/disp64.c).  Not a GL/GLES subset.
 * G2 defines the surfaces + clear/compose/present seam; G3 adds 2D primitives,
 * G4 the 3D rasterizer, G5 the compositor. */
#ifndef TINYOS_TINYGL_H
#define TINYOS_TINYGL_H
#include <stdint.h>

/* A surface is a render target: an offscreen pixel buffer object, or the screen. */
typedef struct tgl_surface {
    uint32_t *px;                  /* w*h tight device pixels (fb_pack format)   */
    uint32_t  w, h;
    int       owned;               /* 1 if px was allocated by TinyGL            */
} tgl_surface_t;

/* ---- rendering API: draw into the bound target ---- */
tgl_surface_t *tgl_surface_create(uint32_t w, uint32_t h);
void           tgl_surface_destroy(tgl_surface_t *s);
tgl_surface_t *tgl_screen(void);           /* surface over the display back buffer */
void           tgl_bind(tgl_surface_t *s); /* set the current render target        */
tgl_surface_t *tgl_target(void);           /* the current render target            */
void           tgl_clear(uint32_t rgb);    /* fill the bound target (0xRRGGBB)      */

/* ---- composition API: place surfaces, present the screen ---- */
void tgl_compose(const tgl_surface_t *src, int x, int y); /* blit src into the bound target */
void tgl_present(void);                    /* flush the screen to the display backend */

/* ---- 2D rendering (G3): clipped, source-over alpha, into the bound target ----
 * Solid colours are 0xRRGGBB; `a` is 0..255 (255 = opaque). */
void tgl_clip_push(int x, int y, int w, int h);  /* intersect the clip rectangle */
void tgl_clip_pop(void);
void tgl_pixel(int x, int y, uint32_t rgb);
void tgl_fill_rect(int x, int y, int w, int h, uint32_t rgb, uint8_t a);
void tgl_stroke_rect(int x, int y, int w, int h, uint32_t rgb);
void tgl_line(int x0, int y0, int x1, int y1, uint32_t rgb);
void tgl_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bot);
void tgl_blit_alpha(const tgl_surface_t *src, int x, int y, uint8_t a);
void tgl_text(int x, int y, const char *s, uint32_t rgb);      /* 8x8 glyphs, transparent bg */

void tgl_demo(void);                       /* visible G3 scene (terminal `tgl` command) */

/* ---- 3D rasterizer (G4): fixed-point, depth-tested, perspective-correct ---- */
typedef struct { uint32_t *px; int w, h; } tgl_texture_t;   /* device pixels */
typedef struct { int x, y; int32_t iw, u, v; } tgl_v_t;     /* screen x,y (px); iw=1/z, u,v (16.16) */

void tgl_depth_enable(void);               /* alloc/attach a z-buffer for the bound target */
void tgl_depth_clear(void);
void tgl_depth_free(void);
void tgl_cull(int on);                     /* back-face culling on/off */
void tgl_tri(const tgl_v_t *a, const tgl_v_t *b, const tgl_v_t *c,
             const tgl_texture_t *tex, int shade);          /* shade 0..256 brightness */
void tgl_mesh_demo(void);                  /* spinning textured cube (terminal `tgl3d`) */
int  tgl3d_selftest(void);                 /* G4 gate: raster + depth + cull correctness */

/* ---- compositor (G5): layers with position, z-order, alpha, damage tracking ---- */
typedef struct tgl_layer { tgl_surface_t *surf; int x, y, z, visible; uint8_t alpha; } tgl_layer_t;
tgl_layer_t *tgl_layer_add(tgl_surface_t *surf, int x, int y, int z, uint8_t alpha);
void tgl_layer_move(tgl_layer_t *l, int x, int y);
void tgl_layer_damage(tgl_layer_t *l);     /* mark a layer's content changed */
void tgl_comp_set_bg(uint32_t rgb);
void tgl_comp_target(tgl_surface_t *out);  /* composite target; 0 = the screen */
void tgl_comp_clear(void);                 /* drop all layers + damage */
int  tgl_composite(void);                  /* recompose the damaged region + present */
void tgl_comp_demo(void);                  /* two composited apps (terminal `comp`) */
int  tglcomp_selftest(void);               /* G5 gate: z-order + alpha compositing */

/* ---- buffer objects (G6): GEM-lite BOs — refcounted, fenced, zero-copy ---- */
typedef struct tgl_bo {
    tgl_surface_t surf;        /* px points at the BO's frames (identity-mapped = physical) */
    uint32_t pages;
    int      handle, refcount;
    volatile uint32_t fence;   /* producer sequence number */
} tgl_bo_t;
tgl_bo_t      *tgl_bo_create(uint32_t w, uint32_t h);
void           tgl_bo_ref(tgl_bo_t *bo);
void           tgl_bo_unref(tgl_bo_t *bo);
tgl_surface_t *tgl_bo_surface(tgl_bo_t *bo);
void           tgl_bo_signal(tgl_bo_t *bo, uint32_t seq);   /* producer: frame ready */
int            tgl_bo_fenced(tgl_bo_t *bo, uint32_t seq);   /* consumer: is seq ready? */
void           tgl_bo_demo(void);          /* client renders a BO, compositor presents it */
int            tglbo_selftest(void);       /* G6 gate: BO view + fence + zero-copy compose */

int  tgl_selftest(void);                   /* G2/G3 gate: primitives yield the right pixels */

#endif /* TINYOS_TINYGL_H */
