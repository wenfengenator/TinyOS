/* tinyos x64 display layer: a swappable display backend behind a stable seam.
 *
 * The default backend is the GOP / Hyper-V synthetic framebuffer: one shared,
 * kernel-owned double buffer presented with a scanline diff (fb64), so a static
 * frame writes nothing to the slow framebuffer.  The seam (display_backend_t +
 * display_register) lets a future transport — virtio-gpu page-flip, a Hyper-V
 * synthvid path — replace it without touching the WM, login, or the games, which
 * all draw into disp_back() and call disp_present().  disp_* dispatch through the
 * active backend; the framebuffer backend is the built-in default. */
#include "boot64.h"

/* ---- built-in backend: GOP framebuffer (double buffer + scanline-diff present) ---- */
static uint32_t *g_back, *g_shadow;
static uint32_t  g_w, g_h;
static int       g_ready;

static int fbb_init(void) {
    if (g_ready) return 0;
    fb_get(&g_w, &g_h);
    if (!g_w || !g_h) return -1;
    uint64_t bytes = (uint64_t)g_w * g_h * 4;
    g_back   = (uint32_t *)kmalloc(bytes);
    g_shadow = (uint32_t *)kmalloc(bytes);
    if (!g_back || !g_shadow) { g_back = g_shadow = 0; return -1; }
    memset(g_back, 0, bytes);
    memset(g_shadow, 0xFF, bytes);          /* != back, so the first present blits all */
    g_ready = 1;
    return 0;
}
static uint32_t *fbb_back(void) { return g_back; }
static void fbb_dims(uint32_t *w, uint32_t *h) { if (w) *w = g_w; if (h) *h = g_h; }
static void fbb_present(void) { if (g_ready) fb_present_diff(g_back, g_shadow, g_w, g_h); }
static void fbb_present_full(void) {
    if (!g_ready) return;
    fb_present(g_back);
    memcpy(g_shadow, g_back, (uint64_t)g_w * g_h * 4);
}
/* single-scanout framebuffer: a "flip" is just a present (no hardware page-flip) */
static void fbb_flip(void) { fbb_present(); }

static const display_backend_t g_fb_backend = {
    "framebuffer(GOP)", 1, fbb_init, fbb_back, fbb_dims, fbb_present, fbb_present_full, fbb_flip
};

/* ---- swappable current backend + neutral API used by the WM / apps ---- */
static const display_backend_t *g_disp = &g_fb_backend;

void display_register(const display_backend_t *b) {
    if (b && b->init && b->back && b->present) {
        g_disp = b;
        kputs("[disp] backend registered: "); kputs(b->name); kputc('\n');
    }
}
const char *display_backend_name(void) { return g_disp->name; }
int         display_outputs(void)      { return g_disp->outputs; }

int       disp_init(void)                     { return g_disp->init(); }
uint32_t *disp_back(void)                     { return g_disp->back(); }
void      disp_dims(uint32_t *w, uint32_t *h) { g_disp->dims(w, h); }
void      disp_present(void)                  { g_disp->present(); }
void      disp_present_full(void)             { g_disp->present_full(); }
void      disp_flip(void)                     { g_disp->flip(); }
