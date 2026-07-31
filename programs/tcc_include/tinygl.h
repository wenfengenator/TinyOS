/* TinyGL user API (Cycle 7 G7): a tcc-buildable client library for the TinyOS
 * kernel graphics driver.
 *
 *   app  ->  UMD (this header)  ->  KMD (kernel graphics syscalls)  ->  display
 *
 * A program links musl libc, opens a buffer object that the kernel maps
 * ZERO-COPY into the process address space, draws into it with the software
 * rasterizer below (running entirely in the app's address space), then asks the
 * kernel to composite + present it.  No pixel data crosses the syscall boundary.
 *
 * Build on-device:   ./tcc.bin /tgl.c -o /tgl.bin   (this header lives in /include)
 * or off-device with musl-gcc against programs/tcc_include/.
 *
 * The API is plain C (no compiler extensions) so the on-device TinyCC accepts it. */
#ifndef TINYGL_USER_H
#define TINYGL_USER_H

#include <stdint.h>
#include <stdlib.h>          /* malloc/free for the 3D depth buffer */
#include <unistd.h>          /* musl syscall() — the only kernel channel we need */

/* Private graphics syscalls.  Out of the way of the Linux syscall numbers;
 * MUST match kernel/arch/arch64.c. */
#define TGL_SYS_INFO       0x54474C00L      /* 'T''G''L' 0x00 */
#define TGL_SYS_BO_CREATE  0x54474C01L
#define TGL_SYS_PRESENT    0x54474C02L
#define TGL_SYS_BO_DESTROY 0x54474C03L

/* Filled by TGL_SYS_INFO / TGL_SYS_BO_CREATE — layout must match the kernel. */
struct tgl_bo_desc {
    uint64_t va;                            /* user address of the mapped pixels */
    uint32_t w, h, stride;                  /* stride in pixels (== w, tight)     */
    uint32_t rsh, gsh, bsh;                 /* channel shifts for device packing  */
    int32_t  handle;
    uint32_t _pad;
};
struct tgl_screen_info { uint32_t w, h, stride, rsh, gsh, bsh; };

/* A handle to an open window/buffer plus its zero-copy pixel mapping. */
typedef struct {
    int       handle;
    uint32_t *px;                           /* mapped device pixels, w*h tight    */
    int       w, h, stride;
    unsigned  rsh, gsh, bsh;
} tgl_t;

/* Pack an 0xRRGGBB colour into this display's device pixel format. */
static inline uint32_t tgl_rgb(const tgl_t *g, uint32_t c) {
    uint32_t r = (c >> 16) & 0xFF, gg = (c >> 8) & 0xFF, b = c & 0xFF;
    return (r << g->rsh) | (gg << g->gsh) | (b << g->bsh);
}

/* ---- lifecycle ---------------------------------------------------------- */

static inline int tgl_screen_size(int *w, int *h) {
    struct tgl_screen_info si;
    if (syscall(TGL_SYS_INFO, (long)&si) < 0) return -1;
    if (w) *w = (int)si.w;
    if (h) *h = (int)si.h;
    return 0;
}

/* Open a w*h window (buffer object) mapped zero-copy into this process.
 * Returns 0 on success (g is filled), -1 on failure. */
static inline int tgl_open(tgl_t *g, int w, int h) {
    struct tgl_bo_desc d;
    g->handle = -1; g->px = 0;
    if (syscall(TGL_SYS_BO_CREATE, (long)w, (long)h, (long)&d) < 0) return -1;
    g->handle = d.handle;
    g->px     = (uint32_t *)(uintptr_t)d.va;
    g->w = (int)d.w; g->h = (int)d.h; g->stride = (int)d.stride;
    g->rsh = d.rsh; g->gsh = d.gsh; g->bsh = d.bsh;
    return 0;
}

static inline void tgl_finish(tgl_t *g) {
    if (g->handle >= 0) syscall(TGL_SYS_BO_DESTROY, (long)g->handle);
    g->handle = -1; g->px = 0;
}

/* Composite this buffer onto the screen at (x,y) and present it.  Returns a
 * pending key code (0 = none) so a render loop can poll for input. */
static inline int tgl_present(tgl_t *g, int x, int y) {
    return (int)syscall(TGL_SYS_PRESENT, (long)g->handle, (long)x, (long)y);
}

/* ---- software rasterizer: writes straight into the mapped buffer -------- */

static inline void tgl_clear(tgl_t *g, uint32_t c) {
    uint32_t p = tgl_rgb(g, c);
    int n = g->w * g->h;
    for (int i = 0; i < n; i++) g->px[i] = p;
}

static inline void tgl_pixel(tgl_t *g, int x, int y, uint32_t c) {
    if ((unsigned)x < (unsigned)g->w && (unsigned)y < (unsigned)g->h)
        g->px[y * g->stride + x] = tgl_rgb(g, c);
}

static inline void tgl_fill(tgl_t *g, int x, int y, int w, int h, uint32_t c) {
    uint32_t p = tgl_rgb(g, c);
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)g->h) continue;
        uint32_t *row = &g->px[yy * g->stride];
        for (int xx = x; xx < x + w; xx++)
            if ((unsigned)xx < (unsigned)g->w) row[xx] = p;
    }
}

/* Source-over alpha (a = 0..255).  Blended per byte, so it is independent of
 * the device channel order. */
static inline void tgl_blend(tgl_t *g, int x, int y, int w, int h, uint32_t c, int a) {
    if (a <= 0) return;
    if (a >= 255) { tgl_fill(g, x, y, w, h, c); return; }
    uint32_t s = tgl_rgb(g, c);
    unsigned ua = (unsigned)a, ia = 255u - ua;
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)g->h) continue;
        uint32_t *row = &g->px[yy * g->stride];
        for (int xx = x; xx < x + w; xx++) {
            if ((unsigned)xx >= (unsigned)g->w) continue;
            uint32_t d = row[xx], o = 0;
            for (int i = 0; i < 32; i += 8) {
                unsigned sb = (s >> i) & 0xFF, db = (d >> i) & 0xFF;
                o |= (((sb * ua + db * ia) / 255u) & 0xFF) << i;
            }
            row[xx] = o;
        }
    }
}

static inline void tgl_line(tgl_t *g, int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int err = dx - dy;
    for (;;) {
        tgl_pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Vertical gradient from `top` to `bot` (both 0xRRGGBB) over the rectangle. */
static inline void tgl_vgradient(tgl_t *g, int x, int y, int w, int h,
                                 uint32_t top, uint32_t bot) {
    if (h <= 0) return;
    int tr = (top >> 16) & 0xFF, tg = (top >> 8) & 0xFF, tb = top & 0xFF;
    int br = (bot >> 16) & 0xFF, bg = (bot >> 8) & 0xFF, bb = bot & 0xFF;
    for (int r = 0; r < h; r++) {
        int cr = tr + (br - tr) * r / h;
        int cg = tg + (bg - tg) * r / h;
        int cb = tb + (bb - tb) * r / h;
        tgl_fill(g, x, y + r, w, 1, ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb);
    }
}

/* 8x8 bitmap font, ASCII 0x20..0x7E; bit 0 (LSB) is the left pixel column.
 * Public-domain font8x8_basic letterforms (Daniel Hepper). */
static const unsigned char tgl_font8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
    {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},
    {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},
    {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},
    {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},
    {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},
    {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},
    {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},
    {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},
    {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},
    {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},
    {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},
    {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},
    {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},
    {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},
    {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},
    {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},
    {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00},
    {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},
    {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},
    {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},
    {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},
    {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00}, {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},
    {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* Draw a NUL-terminated string in 8x8 glyphs (transparent background). */
static inline void tgl_text(tgl_t *g, int x, int y, const char *s, uint32_t c) {
    uint32_t p = tgl_rgb(g, c);
    for (; *s; s++, x += 8) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) continue;
        const unsigned char *gl = tgl_font8x8[ch - 0x20];
        for (int row = 0; row < 8; row++) {
            int dy = y + row;
            if ((unsigned)dy >= (unsigned)g->h) continue;
            unsigned bits = gl[row];
            uint32_t *dst = &g->px[dy * g->stride];
            for (int col = 0; col < 8; col++)
                if ((bits & (1u << col)) && (unsigned)(x + col) < (unsigned)g->w)
                    dst[x + col] = p;
        }
    }
}

/* ======================================================================
 * 3D: a fixed-point software triangle rasterizer (ported from the kernel
 * G4 renderer).  Integer math only — no FPU, no libm — so it builds with
 * tcc against libc alone.  It rasterizes straight into the mapped buffer.
 * ====================================================================== */

#define TGL_FX    16
#define TGL_FXONE (1 << TGL_FX)
typedef int32_t tgl_fx;

static inline tgl_fx tgl_fxmul(tgl_fx a, tgl_fx b) { return (tgl_fx)(((int64_t)a * b) >> TGL_FX); }
static inline tgl_fx tgl_fxdiv(tgl_fx a, tgl_fx b) { return b ? (tgl_fx)(((int64_t)a << TGL_FX) / b) : 0; }

/* 16.16 quarter-sine table (Taylor); full period by symmetry.  a in 0..255 = 0..2pi. */
static const tgl_fx tgl_sinq[65] = {
    0, 1608, 3216, 4821, 6424, 8022, 9617, 11204,
    12786, 14359, 15924, 17479, 19024, 20557, 22078, 23586,
    25080, 26557, 28021, 29466, 30894, 32302, 33693, 35062,
    36410, 37736, 39040, 40320, 41576, 42806, 44012, 45191,
    46342, 47465, 48559, 49625, 50661, 51666, 52640, 53581,
    54492, 55369, 56212, 57023, 57798, 58538, 59244, 59913,
    60548, 61143, 61705, 62228, 62713, 63161, 63570, 63941,
    64274, 64568, 64822, 65039, 65215, 65352, 65451, 65508,
    65526,
};
static inline tgl_fx tgl_isin(int a) {
    a &= 255;
    if (a <= 64)  return  tgl_sinq[a];
    if (a <= 128) return  tgl_sinq[128 - a];
    if (a <= 192) return -tgl_sinq[a - 128];
    return -tgl_sinq[256 - a];
}
static inline tgl_fx tgl_icos(int a) { return tgl_isin(a + 64); }

/* Screen-space vertex: pixel x,y; iw = 1/z (16.16, larger = nearer); u,v (16.16). */
typedef struct { int x, y; tgl_fx iw, u, v; } tgl_vert;
/* Texture: device-format pixels (build with tgl_rgb); sampled tiled. */
typedef struct { uint32_t *px; int w, h; } tgl_tex;

/* Render context: a target buffer plus its depth buffer. */
typedef struct { tgl_t *fb; tgl_fx *zbuf; int w, h, cull; } tgl3d_t;

/* Attach a depth buffer to g; 0 on success, -1 on allocation failure. */
static inline int tgl3d_begin(tgl3d_t *r, tgl_t *g) {
    r->fb = g; r->w = g->w; r->h = g->h; r->cull = 0;
    r->zbuf = (tgl_fx *)malloc((size_t)g->w * g->h * sizeof(tgl_fx));
    if (!r->zbuf) return -1;
    for (int i = 0; i < g->w * g->h; i++) r->zbuf[i] = 0;   /* 0 = far */
    return 0;
}
static inline void tgl3d_clear_depth(tgl3d_t *r) {
    int n = r->w * r->h;
    for (int i = 0; i < n; i++) r->zbuf[i] = 0;
}
static inline void tgl3d_cull(tgl3d_t *r, int on) { r->cull = on; }
static inline void tgl3d_end(tgl3d_t *r) { free(r->zbuf); r->zbuf = 0; }

static inline uint32_t tgl__tex_sample(const tgl_tex *tex, tgl_fx u, tgl_fx v) {
    if (!tex || !tex->px) return 0;
    u &= (TGL_FXONE - 1); v &= (TGL_FXONE - 1);            /* tile into [0,1) */
    int tx = (int)(((int64_t)(uint32_t)u * tex->w) >> TGL_FX);
    int ty = (int)(((int64_t)(uint32_t)v * tex->h) >> TGL_FX);
    if (tx >= tex->w) tx = tex->w - 1;
    if (ty >= tex->h) ty = tex->h - 1;
    return tex->px[ty * tex->w + tx];
}
static inline uint32_t tgl__modulate(uint32_t p, int s) {  /* per-byte brightness, s in 0..256 */
    if (s >= 256) return p;
    uint32_t o = 0;
    for (int i = 0; i < 32; i += 8) o |= (((((p >> i) & 0xFF) * (uint32_t)s) >> 8) & 0xFF) << i;
    return o;
}

/* Rasterize one screen-space triangle with depth test, back-face cull, and
 * perspective-correct texture sampling. */
static inline void tgl3d_tri(tgl3d_t *r, const tgl_vert *a, const tgl_vert *b,
                             const tgl_vert *c, const tgl_tex *tex, int shade) {
    tgl_t *g = r->fb;
    int64_t area = (int64_t)(b->x - a->x) * (c->y - a->y) - (int64_t)(b->y - a->y) * (c->x - a->x);
    if (area == 0) return;
    int back = area < 0;
    if (r->cull && back) return;
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
    if (maxx >= g->w) maxx = g->w - 1;
    if (maxy >= g->h) maxy = g->h - 1;

    tgl_fx uiw_a = tgl_fxmul(a->u, a->iw), viw_a = tgl_fxmul(a->v, a->iw);
    tgl_fx uiw_b = tgl_fxmul(b->u, b->iw), viw_b = tgl_fxmul(b->v, b->iw);
    tgl_fx uiw_c = tgl_fxmul(c->u, c->iw), viw_c = tgl_fxmul(c->v, c->iw);

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
            int idx = py * g->stride + px;
            if ((tgl_fx)iw <= r->zbuf[py * r->w + px]) continue;   /* depth test (0 = far) */
            r->zbuf[py * r->w + px] = (tgl_fx)iw;

            uint64_t uiw = ((uint64_t)e0 * (uint32_t)uiw_a + (uint64_t)e1 * (uint32_t)uiw_b +
                            (uint64_t)e2 * (uint32_t)uiw_c) / uarea;
            uint64_t viw = ((uint64_t)e0 * (uint32_t)viw_a + (uint64_t)e1 * (uint32_t)viw_b +
                            (uint64_t)e2 * (uint32_t)viw_c) / uarea;
            tgl_fx u = (tgl_fx)((uiw << TGL_FX) / iw);              /* perspective divide */
            tgl_fx v = (tgl_fx)((viw << TGL_FX) / iw);
            g->px[idx] = tgl__modulate(tgl__tex_sample(tex, u, v), shade);
        }
    }
}

#endif /* TINYGL_USER_H */
