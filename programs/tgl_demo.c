/* SPDX-License-Identifier: GPL-2.0-only */

/* TinyGL demo — a userspace app that renders through the TinyGL client library
 * (programs/tcc_include/tinygl.h) and asks the kernel to present it.
 *
 * Build ON DEVICE with the self-hosting compiler:
 *     ./tcc.bin /tgl.c -o /tgl.bin
 *     ./tgl.bin
 * or off-device with musl-gcc (staged as /TGL.BIN).  Press Q or ESC to quit.
 *
 * Everything drawn here runs in the app's address space and lands directly in a
 * buffer the kernel compositor samples in place — no copy to present. */
#include <tinygl.h>
#include <unistd.h>

int main(void) {
    int sw = 1024, sh = 768;
    tgl_screen_size(&sw, &sh);

    int w = sw > 1280 ? 1280 : sw;          /* cap the software-rendered area */
    int h = sh > 800 ? 800 : sh;

    tgl_t g;
    if (tgl_open(&g, w, h) != 0) {
        static const char *e = "tgl: cannot open display\n";
        write(2, e, 25);
        return 1;
    }
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2;

    for (int frame = 0;; frame++) {
        tgl_clear(&g, 0x0E1726);                                 /* dark slate */

        for (int yy = 0; yy < g.h; yy += 2)                      /* subtle scanline sheen */
            tgl_blend(&g, 0, yy, g.w, 1, 0x1B3A5B, 40);

        int t = frame;
        int bx = (t * 4) % (g.w - 120);
        int by = ((t * 3) % (2 * (g.h - 90)));
        if (by > g.h - 90) by = 2 * (g.h - 90) - by;             /* bounce vertically */
        tgl_fill(&g, bx, by, 120, 90, 0xFF6A3D);                 /* opaque box */
        tgl_blend(&g, bx + 40, by + 30, 160, 120, 0x36A2FF, 150);/* translucent box */

        for (int i = 0; i <= g.w; i += 48)                       /* line fan */
            tgl_line(&g, g.w / 2, g.h / 2, i, (i * 7 + t * 3) % g.h, 0x2E5C86);

        tgl_fill(&g, 16, 16, 360, 62, 0x102A44);
        tgl_text(&g, 28, 26, "TinyGL  app -> UMD -> KMD -> display", 0xEAF2FF);
        tgl_text(&g, 28, 44, "zero-copy buffer object, software raster", 0x86A7C4);
        tgl_text(&g, 28, 62, "built with tcc.bin  -  press Q to quit", 0x86A7C4);

        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == 27) break;
        usleep(16000);                                           /* ~60 fps */
    }

    tgl_finish(&g);
    return 0;
}
