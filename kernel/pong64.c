/* tinyos x64 "pong" — a classic two-paddle ball game rendered to a full-screen
 * 32-bpp back buffer, in the same style as kernel/demo64.c.  Launched from the
 * terminal console with the `pong` command.
 *
 *   left paddle : player  (Up/Down arrows, or W/S)
 *   right paddle: a simple AI that tracks the ball
 *   ball        : bounces off the top/bottom walls and the paddles; the angle
 *                 depends on where it strikes the paddle
 *   scoring     : a point when the ball passes a paddle; Q/ESC quits
 *
 * Physics run on a fixed ~60 Hz tick (independent of render FPS) so play speed is
 * the same however fast the framebuffer blits. */
#include "boot64.h"

/* fill an axis-aligned rectangle into the back buffer, clipped to the screen */
static void fill_rect(uint32_t *b, uint32_t w, uint32_t h,
                      int x, int y, int rw, int rh, uint32_t px) {
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > (int)w) rw = (int)w - x;
    if (y + rh > (int)h) rh = (int)h - y;
    for (int yy = 0; yy < rh; yy++) {
        uint32_t *row = b + (uint64_t)(y + yy) * w + x;
        for (int xx = 0; xx < rw; xx++) row[xx] = px;
    }
}

/* draw an unsigned decimal at (x,y) */
static void draw_num(uint32_t *b, uint32_t x, uint32_t y, uint32_t v, uint32_t fg, uint32_t bg) {
    char buf[12]; int n = 0;
    if (!v) buf[n++] = '0';
    else { char t[12]; int m = 0; while (v) { t[m++] = (char)('0' + v % 10); v /= 10; }
           while (m) buf[n++] = t[--m]; }
    buf[n] = 0;
    fb_text_to(b, x, y, buf, fg, bg);
}

void pong_run(boot_info_t *bi) {
    (void)bi;
    uint32_t w, h; fb_get(&w, &h);
    if (!w || !h) { kputs("[pong] no framebuffer\n"); return; }

    uint32_t *back = (uint32_t *)kmalloc((uint64_t)w * h * 4);
    if (!back) { kputs("[pong] back-buffer alloc failed\n"); return; }

    const uint32_t BG_RGB = 0x001018;
    uint32_t bg   = fb_pack(BG_RGB);
    uint32_t net  = fb_pack(0x22333A);
    uint32_t lpx  = fb_pack(0x33FF66);          /* player paddle (green)  */
    uint32_t rpx  = fb_pack(0xFFCC00);          /* AI paddle (amber)      */
    uint32_t ball = fb_pack(0xFFFFFF);

    const int PW = 16, PH = 110, BS = 16, MG = 44;
    int lx = MG,               ly = (int)h / 2 - PH / 2;
    int rx = (int)w - MG - PW, ry = (int)h / 2 - PH / 2;
    int bx = (int)w / 2 - BS / 2, by = (int)h / 2 - BS / 2;
    int bvx = 6, bvy = 4;
    int lscore = 0, rscore = 0;

    kputs("[pong] running — Up/Down or W/S move, Q/ESC quits\n");

    uint64_t t_start = millis(), last = t_start, next_tick = t_start;
    uint32_t fps = 0, fps_acc = 0;
    int running = 1;

    while (running) {
        uint64_t t = millis();

        /* ---- input ---- */
        int k;
        while ((k = kbd64_getkey()) >= 0 || (k = hvkbd_getkey()) >= 0) {
            switch (k) {
                case KEY_UP:   case 'w': case 'W': ly -= 48; break;
                case KEY_DOWN: case 's': case 'S': ly += 48; break;
                case 'q': case 'Q': case KEY_ESC:  running = 0; break;
                default: break;
            }
        }
        if (ly < 0) ly = 0; else if (ly > (int)h - PH) ly = (int)h - PH;

        /* ---- fixed-step physics (~60 Hz) ---- */
        while (t >= next_tick) {
            next_tick += 16;
            bx += bvx; by += bvy;

            if (by <= 0)              { by = 0;             bvy = -bvy; }
            else if (by >= (int)h - BS) { by = (int)h - BS; bvy = -bvy; }

            /* left (player) paddle */
            if (bvx < 0 && bx < lx + PW && bx + BS > lx && by < ly + PH && by + BS > ly) {
                bx = lx + PW; bvx = -bvx; bvy += ((by + BS / 2) - (ly + PH / 2)) / 16;
            }
            /* right (AI) paddle */
            if (bvx > 0 && bx < rx + PW && bx + BS > rx && by < ry + PH && by + BS > ry) {
                bx = rx - BS; bvx = -bvx; bvy += ((by + BS / 2) - (ry + PH / 2)) / 16;
            }
            if (bvy > 10) bvy = 10; else if (bvy < -10) bvy = -10;

            /* scoring + serve */
            if (bx < -BS)         { rscore++; bx = (int)w / 2 - BS / 2; by = (int)h / 2 - BS / 2; bvx =  6; bvy =  4; }
            else if (bx > (int)w) { lscore++; bx = (int)w / 2 - BS / 2; by = (int)h / 2 - BS / 2; bvx = -6; bvy = -4; }

            /* AI tracks the ball with a small dead zone (beatable) */
            int target = by + BS / 2 - PH / 2;
            if (ry < target - 4) ry += 5; else if (ry > target + 4) ry -= 5;
            if (ry < 0) ry = 0; else if (ry > (int)h - PH) ry = (int)h - PH;
        }

        /* ---- render ---- */
        for (uint32_t i = 0; i < w * h; i++) back[i] = bg;
        for (int y = 0; y < (int)h; y += 28) fill_rect(back, w, h, (int)w / 2 - 3, y, 6, 16, net);
        fill_rect(back, w, h, lx, ly, PW, PH, lpx);
        fill_rect(back, w, h, rx, ry, PW, PH, rpx);
        fill_rect(back, w, h, bx, by, BS, BS, ball);

        draw_num(back, (uint32_t)w / 2 - 72, 20, (uint32_t)lscore, 0x33FF66, BG_RGB);
        draw_num(back, (uint32_t)w / 2 + 56, 20, (uint32_t)rscore, 0xFFCC00, BG_RGB);
        fb_text_to(back, 8, 4, "TINYOS PONG   UP/DOWN OR W/S = MOVE   Q/ESC = QUIT", 0x00FF88, BG_RGB);
        draw_num(back, (uint32_t)w - 64, 4, fps, 0xFFFF00, BG_RGB);

        fb_present(back);
        fps_acc++;
        if (t - last >= 1000) { fps = fps_acc; fps_acc = 0; last = t; }
    }

    kfree(back);
    kputs("[pong] exited\n");
}
