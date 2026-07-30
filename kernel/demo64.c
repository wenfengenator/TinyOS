/* tinyos x64 graphics demo (R5) — exercises every Doom-enabling primitive:
 *   heap      : a full-screen 32-bpp back buffer allocated with kmalloc()
 *   timer     : millis()-driven animation + FPS counter
 *   keyboard  : polled arrow keys move the box; Q/ESC quits
 *   large file: the loaded TINYWAD.DAT asset is scrolled as the background
 * Renders to the back buffer and blits with fb_present() each frame. */
#include "boot64.h"

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

/* draw "label<val>" into the back buffer at pixel (x,y) */
static void hud_kv(uint32_t *back, uint32_t x, uint32_t y,
                   const char *label, uint32_t val, uint32_t fg) {
    fb_text_to(back, x, y, label, fg, 0x000814);
    char buf[12]; int n = 0; uint32_t v = val;
    if (!v) buf[n++] = '0';
    else { char t[12]; int m = 0; while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
           while (m) buf[n++] = t[--m]; }
    buf[n] = 0;
    fb_text_to(back, x + slen(label) * 8, y, buf, 0xFFFFFF, 0x000814);
}

void demo_run(boot_info_t *bi) {
    uint32_t w, h; fb_get(&w, &h);
    if (!w || !h) { kputs("[demo] no framebuffer\n"); return; }

    uint32_t *back = disp_back();
    if (!back) { kputs("[demo] back-buffer alloc failed\n"); return; }

    const uint8_t *asset = (const uint8_t *)(uintptr_t)bi->asset_base;
    uint32_t asize = (uint32_t)bi->asset_size;
    int asset_pot = (asize && (asize & (asize - 1)) == 0);   /* power of two? */
    uint32_t amask = asset_pot ? asize - 1 : 0;

    /* precompute a palette: asset byte -> device pixel (cool blue gradient) */
    uint32_t pal[256];
    for (int i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i / 3, g = (uint32_t)i / 2, b = 64 + (uint32_t)i / 2;
        pal[i] = fb_pack((r << 16) | (g << 8) | (b > 255 ? 255 : b));
    }
    uint32_t boxpx = fb_pack(0xFFCC00);
    uint32_t bgpx  = fb_pack(0x000814);

    kputs("[demo] running — arrows move, Q/ESC to quit\n");

    int bx = (int)w / 2, by = (int)h / 2, bw = 96, bh = 72, vx = 3, vy = 2;
    uint32_t fps = 0, fps_acc = 0;
    uint64_t t_start = millis(), last = t_start, next_drift = t_start;
    int running = 1;

    while (running) {
        uint64_t t = millis();

        /* ---- input (PS/2 i8042 on QEMU, VMBus synthetic keyboard on Hyper-V) ---- */
        int k;
        while ((k = kbd64_getkey()) >= 0 || (k = hvkbd_getkey()) >= 0) {
            switch (k) {
                case KEY_LEFT:  bx -= 64; break;
                case KEY_RIGHT: bx += 64; break;
                case KEY_UP:    by -= 64; break;
                case KEY_DOWN:  by += 64; break;
                case 'q': case 'Q': case KEY_ESC: running = 0; break;
                default: break;
            }
        }

        /* ---- auto drift + bounce, rate-limited to ~60 Hz so it stays a gentle
         *      background motion no matter how high the render FPS is; this lets
         *      the arrow keys (a chunky 64 px per press) clearly move the box ---- */
        if (t >= next_drift) {
            next_drift = t + 16;
            bx += vx; by += vy;
            if (bx <= 0 || bx >= (int)(w - bw)) vx = -vx;
            if (by <= 0 || by >= (int)(h - bh)) vy = -vy;
        }
        /* hard clamp each frame so arrow input can't push the box off-screen */
        if (bx < 0) bx = 0; else if (bx > (int)(w - bw)) bx = (int)(w - bw);
        if (by < 0) by = 0; else if (by > (int)(h - bh)) by = (int)(h - bh);

        /* ---- background from the scrolling asset (or an XOR fallback) ---- */
        uint32_t scroll = (uint32_t)(t / 16) * w;
        for (uint32_t y = 0; y < h; y++) {
            uint32_t *row = back + (uint64_t)y * w;
            uint32_t off = scroll + y * w;
            if (asset && asset_pot) {
                for (uint32_t x = 0; x < w; x++) row[x] = pal[asset[(off + x) & amask]];
            } else if (asset) {
                for (uint32_t x = 0; x < w; x++) row[x] = pal[asset[(off + x) % asize]];
            } else {
                for (uint32_t x = 0; x < w; x++) row[x] = pal[(uint8_t)((x ^ y) + (t >> 4))];
            }
        }

        /* ---- box ---- */
        for (int y = by; y < by + bh; y++)
            for (int x = bx; x < bx + bw; x++) back[(uint64_t)y * w + x] = boxpx;

        /* ---- HUD panel + text ---- */
        for (uint32_t y = 0; y < 48; y++)
            for (uint32_t x = 0; x < w; x++) back[(uint64_t)y * w + x] = bgpx;
        fb_text_to(back, 8, 4, "TINYOS X64 GRAPHICS DEMO  (ARROWS MOVE, Q/ESC QUIT)", 0x00FF88, 0x000814);
        hud_kv(back,   8, 16, "FPS ", fps, 0xFFFF00);
        hud_kv(back, 120, 16, "MS ",  (uint32_t)(t - t_start), 0xFFFF00);
        hud_kv(back, 320, 16, "HEAP ", (uint32_t)(heap_total() / (1024 * 1024)), 0x66CCFF);
        hud_kv(back, 480, 16, "TSCMHZ ", (uint32_t)(tsc_freq_hz() / 1000000), 0x66CCFF);
        hud_kv(back,   8, 28, "ASSET-KB ", asize / 1024, 0xFFFFFF);
        hud_kv(back, 360, 28, "CORES ", (uint32_t)smp_online_count(), 0x00FF88);
        hud_kv(back, 560, 28, "HVSTG ", (uint32_t)hvkbd_stage(), 0xFF66FF);
        hud_kv(back, 720, 28, "HVRSN ", (uint32_t)hvkbd_reason(), 0xFF66FF);

        /* ---- present + FPS bookkeeping ---- */
        disp_present();
        fps_acc++;
        if (t - last >= 1000) { fps = fps_acc; fps_acc = 0; last = t; }
    }

    kputs("[demo] exited\n");
}
