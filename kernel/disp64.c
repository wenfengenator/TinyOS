/* tinyos x64 display driver: one shared, kernel-owned double buffer.
 *
 * Every full-screen renderer (login, the window manager, the console games)
 * draws into the single back buffer returned by disp_back() and calls
 * disp_present().  Present is scanline-diffed against a shadow of what is on
 * screen, so a static frame writes nothing to the slow GOP framebuffer / Hyper-V
 * synthetic video — only the rows that actually changed are copied out.  This
 * replaces the five separate kmalloc'd back buffers with one allocation. */
#include "boot64.h"

static uint32_t *g_back, *g_shadow;
static uint32_t  g_w, g_h;
static int       g_ready;

/* Allocate the shared back buffer + shadow once the framebuffer and heap exist.
 * Idempotent: safe to call from several bring-up paths.  0 on success. */
int disp_init(void) {
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

uint32_t *disp_back(void) { return g_back; }

void disp_dims(uint32_t *w, uint32_t *h) { if (w) *w = g_w; if (h) *h = g_h; }

/* Blit only the scanlines that changed since the last present. */
void disp_present(void) {
    if (g_ready) fb_present_diff(g_back, g_shadow, g_w, g_h);
}

/* Unconditional full blit (e.g. after a mode change); resyncs the shadow. */
void disp_present_full(void) {
    if (!g_ready) return;
    fb_present(g_back);
    memcpy(g_shadow, g_back, (uint64_t)g_w * g_h * 4);
}
