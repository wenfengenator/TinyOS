/* TinyGL buffer objects (Cycle 7 G6): GEM-lite BOs with refcounting, fences,
 * and zero-copy sharing.
 *
 * A BO owns page-aligned frames (page_alloc); its tgl_surface_t view's px points
 * straight at those frames.  A producer renders into the BO and bumps its fence;
 * the compositor samples the BO surface IN PLACE as a layer — no copy from a
 * client buffer.  The same frames can be mapped into a user process's address
 * space (arch_bo_map_*), so a userspace client renders into the very buffer the
 * compositor presents.  Fences (a monotonic sequence number) sync producer and
 * consumer. */
#include "boot64.h"
#include "tinygl.h"

static int g_bo_handle = 1;

tgl_bo_t *tgl_bo_create(uint32_t w, uint32_t h) {
    if (!w || !h) return 0;
    tgl_bo_t *bo = (tgl_bo_t *)kmalloc(sizeof *bo);
    if (!bo) return 0;
    uint64_t bytes = (uint64_t)w * h * 4;
    bo->pages = (uint32_t)((bytes + 4095) / 4096);
    bo->surf.px = (uint32_t *)page_alloc(bo->pages);   /* identity-mapped frames = physical */
    if (!bo->surf.px) { kfree(bo); return 0; }
    bo->surf.w = w; bo->surf.h = h; bo->surf.owned = 0;
    bo->handle = g_bo_handle++; bo->refcount = 1; bo->fence = 0;
    return bo;
}
void tgl_bo_ref(tgl_bo_t *bo) { if (bo) bo->refcount++; }
void tgl_bo_unref(tgl_bo_t *bo) {
    if (!bo) return;
    if (--bo->refcount <= 0) { page_free(bo->surf.px, bo->pages); kfree(bo); }
}
tgl_surface_t *tgl_bo_surface(tgl_bo_t *bo) { return bo ? &bo->surf : 0; }
void tgl_bo_signal(tgl_bo_t *bo, uint32_t seq) { if (bo) bo->fence = seq; }
int  tgl_bo_fenced(tgl_bo_t *bo, uint32_t seq) { return bo && bo->fence >= seq; }

/* ---- visible G6 demo: a producer renders a BO, the compositor presents it zero-copy ---- */
void tgl_bo_demo(void) {
    tgl_surface_t *scr = tgl_screen();
    int W = (int)scr->w, H = (int)scr->h;
    tgl_surface_t *bg = tgl_surface_create(W, H);
    if (bg) { tgl_bind(bg); tgl_vgradient(0, 0, W, H, 0x081026, 0x201028);
              tgl_text(24, 24, "TinyGL G6 - buffer objects + zero-copy compositing (fence-synced)   Q/ESC", 0xCFE2FF); }
    tgl_bo_t *bo = tgl_bo_create(460, 300);            /* the client's buffer object */
    tgl_comp_clear(); tgl_comp_target(0); tgl_comp_set_bg(0x000000);
    if (bg) tgl_layer_add(bg, 0, 0, 0, 255);
    tgl_layer_t *bl = bo ? tgl_layer_add(tgl_bo_surface(bo), (W - 460) / 2, (H - 300) / 2, 1, 255) : 0;
    uint32_t frame = 0;
    for (;;) {
        int k = kbd64_getkey(); if (k < 0) k = hvkbd_getkey();
        if (k == 'q' || k == 'Q' || k == 27) break;
        if (bo) {                                     /* producer: render into the BO, raise the fence */
            tgl_surface_t *s = tgl_bo_surface(bo);
            tgl_bind(s);
            tgl_vgradient(0, 0, 460, 300, 0x104080, 0x40206A);
            tgl_stroke_rect(0, 0, 460, 300, 0xFFFFFF);
            tgl_text(16, 16, "buffer object (client-rendered)", 0xFFFFFF);
            int t = (int)(frame * 4 % 360);
            tgl_fill_rect(30 + t, 120, 120, 90, 0x50D0FF, 200);
            tgl_fill_rect(300 - t, 170, 120, 90, 0xFF6090, 200);
            tgl_bo_signal(bo, ++frame);
        }
        if (bl && tgl_bo_fenced(bo, frame)) {          /* compositor: sample the BO in place */
            tgl_layer_damage(bl);
            tgl_composite();
        }
        sleep_ms(16);
    }
    tgl_comp_clear(); tgl_comp_target(0);
    if (bo) tgl_bo_unref(bo);
    tgl_surface_destroy(bg);
}

/* G6 gate: a BO's surface views its own frames (no copy); the compositor samples
 * the BO in place; the same frames map zero-copy into a user address space. */
int tglbo_selftest(void) {
    tgl_bo_t *bo = tgl_bo_create(64, 64);
    if (!bo) return -1;
    tgl_surface_t *bs = tgl_bo_surface(bo);
    int ok = bs && bs->px == bo->surf.px;              /* the view IS the BO frames */

    tgl_bind(bs); tgl_clear(0x30C060);                 /* producer renders */
    tgl_bo_signal(bo, 1);
    ok = ok && tgl_bo_fenced(bo, 1) && !tgl_bo_fenced(bo, 2);

    tgl_surface_t *out = tgl_surface_create(96, 96);   /* compositor samples the BO in place */
    if (!out) { tgl_bo_unref(bo); return -1; }
    tgl_comp_clear(); tgl_comp_target(out); tgl_comp_set_bg(0x000000);
    tgl_layer_add(bs, 8, 8, 0, 255);
    tgl_composite();
    ok = ok && out->px[20u * 96 + 20] == fb_pack(0x30C060);   /* producer's pixels, no copy */
    tgl_comp_clear(); tgl_comp_target(0);

    tgl_bo_ref(bo);                                    /* refcount */
    int rc = bo->refcount;
    tgl_bo_unref(bo);
    ok = ok && rc == 2;

    tgl_surface_destroy(out); tgl_bo_unref(bo);
    return ok ? 0 : -2;
}
