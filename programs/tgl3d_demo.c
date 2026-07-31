/* TinyGL 3D demo — a userspace spinning, textured, depth-tested cube rendered
 * entirely by the client library's software rasterizer (programs/tcc_include/
 * tinygl.h), straight into a zero-copy buffer the kernel then presents.
 *
 * Build ON DEVICE:  ./tcc.bin /tgl3d.c -o /tgl3d.bin  &&  ./tgl3d.bin
 * or run the prebuilt /TGL3D.BIN.  Press Q or ESC to quit. */
#include <tinygl.h>
#include <unistd.h>

static const tgl_fx cubev[8][3] = {
    {-TGL_FXONE,-TGL_FXONE,-TGL_FXONE}, { TGL_FXONE,-TGL_FXONE,-TGL_FXONE},
    { TGL_FXONE, TGL_FXONE,-TGL_FXONE}, {-TGL_FXONE, TGL_FXONE,-TGL_FXONE},
    {-TGL_FXONE,-TGL_FXONE, TGL_FXONE}, { TGL_FXONE,-TGL_FXONE, TGL_FXONE},
    { TGL_FXONE, TGL_FXONE, TGL_FXONE}, {-TGL_FXONE, TGL_FXONE, TGL_FXONE},
};
static const int cubef[6][4] = {
    {0,1,2,3}, {5,4,7,6}, {4,0,3,7}, {1,5,6,2}, {4,5,1,0}, {3,2,6,7},
};
static const tgl_fx fu[4] = { 0, TGL_FXONE, TGL_FXONE, 0 };
static const tgl_fx fv[4] = { 0, 0, TGL_FXONE, TGL_FXONE };

static uint32_t tex_px[64 * 64];

int main(void) {
    int sw = 1024, sh = 768;
    tgl_screen_size(&sw, &sh);
    int w = sw > 1280 ? 1280 : sw;
    int h = sh > 800 ? 800 : sh;

    tgl_t g;
    if (tgl_open(&g, w, h) != 0) { write(2, "tgl3d: cannot open display\n", 27); return 1; }
    tgl3d_t r;
    if (tgl3d_begin(&r, &g) != 0) { tgl_finish(&g); return 1; }

    for (int y = 0; y < 64; y++)                              /* checkerboard texture */
        for (int x = 0; x < 64; x++)
            tex_px[y * 64 + x] = tgl_rgb(&g, ((x >> 3) ^ (y >> 3)) & 1 ? 0xF0D080 : 0x9A4E24);
    tgl_tex tex = { tex_px, 64, 64 };

    int W = g.w, H = g.h, CX = W / 2, CY = H / 2;
    tgl_fx FOCAL = (tgl_fx)((H / 2) * TGL_FXONE);
    tgl_fx DIST = 4 * TGL_FXONE, NEAR = TGL_FXONE / 2;
    int ox = (sw - g.w) / 2, oy = (sh - g.h) / 2;

    for (int ang = 0;; ang = (ang + 2) & 255) {
        tgl_vgradient(&g, 0, 0, W, H, 0x0A1226, 0x000004);
        tgl_text(&g, 24, 24, "TinyGL 3D - software rasterizer  (tcc app - press Q to quit)", 0xC8E0FF);
        tgl3d_clear_depth(&r);

        tgl_fx sy = tgl_isin(ang), cy = tgl_icos(ang);
        int ax = (ang * 3 / 4) & 255;
        tgl_fx sx = tgl_isin(ax), cx = tgl_icos(ax);
        tgl_vert sv[8]; int behind[8];
        for (int i = 0; i < 8; i++) {
            tgl_fx x = cubev[i][0], y = cubev[i][1], z = cubev[i][2];
            tgl_fx x1 =  tgl_fxmul(x, cy) + tgl_fxmul(z, sy);        /* rotate about Y */
            tgl_fx z1 = -tgl_fxmul(x, sy) + tgl_fxmul(z, cy);
            tgl_fx y2 =  tgl_fxmul(y, cx) - tgl_fxmul(z1, sx);       /* rotate about X */
            tgl_fx z2 =  tgl_fxmul(y, sx) + tgl_fxmul(z1, cx) + DIST;
            behind[i] = z2 < NEAR;
            tgl_fx iw = tgl_fxdiv(TGL_FXONE, z2 < NEAR ? NEAR : z2);
            sv[i].x  = CX + (int)(tgl_fxmul(tgl_fxmul(x1, FOCAL), iw) >> TGL_FX);
            sv[i].y  = CY - (int)(tgl_fxmul(tgl_fxmul(y2, FOCAL), iw) >> TGL_FX);
            sv[i].iw = iw;
        }
        for (int f = 0; f < 6; f++) {
            const int *q = cubef[f];
            if (behind[q[0]] || behind[q[1]] || behind[q[2]] || behind[q[3]]) continue;  /* near clip */
            tgl_vert v[4];
            for (int j = 0; j < 4; j++) { v[j] = sv[q[j]]; v[j].u = fu[j]; v[j].v = fv[j]; }
            int shade = 150 + f * 16;
            tgl3d_tri(&r, &v[0], &v[1], &v[2], &tex, shade);
            tgl3d_tri(&r, &v[0], &v[2], &v[3], &tex, shade);
        }

        int key = tgl_present(&g, ox, oy);
        if (key == 'q' || key == 'Q' || key == 27) break;
        usleep(16000);
    }

    tgl3d_end(&r);
    tgl_finish(&g);
    return 0;
}
