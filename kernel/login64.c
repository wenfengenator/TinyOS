/* tinyos x64 login screen (Cycle 4, Phase A).
 *
 * Shown after boot, before the desktop (kernel/wm64.c).  Renders a framebuffer
 * sign-in panel, reads the username and (masked) password from either keyboard
 * (PS/2 on QEMU, VMBus on Hyper-V), and only returns once the credentials match
 * an entry in the built-in table.  It runs entirely in the kernel for now; once
 * user mode lands (Phase B+) this becomes a ring-3 `login` program on the tty. */
#include "wm64.h"

struct cred { const char *user, *pass; };
static const struct cred g_creds[] = {
    { "root", "root" },
    { "user", "user" },
};

#define L_MAX 20                      /* max chars per field                     */

static uint32_t L_w, L_h;

static void L_fill(uint32_t *bb, int x, int y, int w, int h, uint32_t rgb) {
    uint32_t px = fb_pack(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0 || x >= (int)L_w || y >= (int)L_h) return;
    if (x + w > (int)L_w) w = (int)L_w - x;
    if (y + h > (int)L_h) h = (int)L_h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = bb + (int64_t)(y + j) * L_w + x;
        for (int i = 0; i < w; i++) row[i] = px;
    }
}

static int L_streq(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
static int L_check(const char *u, const char *p) {
    for (unsigned i = 0; i < sizeof(g_creds) / sizeof(g_creds[0]); i++)
        if (L_streq(u, g_creds[i].user) && L_streq(p, g_creds[i].pass)) return 1;
    return 0;
}

void login_run(boot_info_t *bi) {
    (void)bi;
    fb_get(&L_w, &L_h);
    if (!L_w || !L_h) { kputs("[login] no framebuffer; skipping\n"); return; }
    uint32_t *bb = (uint32_t *)kmalloc((uint64_t)L_w * L_h * 4);
    if (!bb) { kputs("[login] no back buffer; skipping\n"); return; }

    char ubuf[L_MAX + 4] = {0}, pbuf[L_MAX + 4] = {0};
    int  ulen = 0, plen = 0, field = 0;             /* field 0=login 1=password */
    const char *msg = "";
    int  dirty = 1, last_blink = -1;                /* repaint only on change    */
    kputs("[login] awaiting credentials (default root/root)\n");

    for (;;) {
        int k, got = 0;
        while ((k = kbd64_getkey()) >= 0 || (k = hvkbd_getkey()) >= 0) {
            got = 1;
            if (k == '\n' || k == '\r') {
                if (field == 0) {
                    field = 1;
                } else if (L_check(ubuf, pbuf)) {
                    kputs("[login] authenticated: "); kputs(ubuf); kputc('\n');
                    kfree(bb);
                    return;                          /* -> desktop */
                } else {
                    msg = "login incorrect";
                    field = 0; ulen = plen = 0; ubuf[0] = pbuf[0] = 0;
                }
            } else if (k == '\b') {
                if (field == 0) { if (ulen > 0) ubuf[--ulen] = 0; }
                else            { if (plen > 0) pbuf[--plen] = 0; }
            } else if (k == '\t') {
                field ^= 1;
            } else if (k == KEY_ESC) {
                if (field == 0) { ulen = 0; ubuf[0] = 0; } else { plen = 0; pbuf[0] = 0; }
            } else if (k >= 0x20 && k < 0x7F) {
                if (field == 0) { if (ulen < L_MAX) { ubuf[ulen++] = (char)k; ubuf[ulen] = 0; } }
                else            { if (plen < L_MAX) { pbuf[plen++] = (char)k; pbuf[plen] = 0; } }
            }
        }

        /* Repaint only when something changed (a key or the 2 Hz cursor blink),
         * so we don't flood Hyper-V's synthetic video with a continuous full-
         * frame blit — that stalls the Virtual Machine Connection console. */
        int blink = (int)((millis() / 500) & 1);
        if (got || blink != last_blink) dirty = 1;
        last_blink = blink;
        if (!dirty) { sleep_ms(5); continue; }
        dirty = 0;

        /* ---- render the sign-in panel ---- */
        L_fill(bb, 0, 0, (int)L_w, (int)L_h, 0x0A1424);          /* backdrop     */
        L_fill(bb, 0, 0, (int)L_w, 4, 0x1E5FA8);
        L_fill(bb, 0, (int)L_h - 4, (int)L_w, 4, 0x1E5FA8);

        int pw = 380, ph = 176;
        int px = ((int)L_w - pw) / 2, py = ((int)L_h - ph) / 2;
        L_fill(bb, px - 1, py - 1, pw + 2, ph + 2, 0x3AA0FF);    /* border       */
        L_fill(bb, px, py, pw, ph, 0x101C2E);                    /* panel        */
        L_fill(bb, px, py, pw, 22, 0x1E5FA8);                    /* title bar    */
        fb_text_to(bb, px + 12, py + 7, "tinyos  -  sign in", 0xFFFFFF, 0x1E5FA8);

        int fx = px + 20, fy = py + 46;
        uint32_t abg = 0x14324F, ibg = 0x0C1420;                 /* active/idle  */
        fb_text_to(bb, fx, fy,      "login:",    0x99CCFF, 0x101C2E);
        fb_text_to(bb, fx, fy + 34, "password:", 0x99CCFF, 0x101C2E);

        int bxx = fx + 88, bw = pw - 88 - 40;
        L_fill(bb, bxx - 4, fy - 4,      bw, 24, field == 0 ? abg : ibg);
        L_fill(bb, bxx - 4, fy + 34 - 4, bw, 24, field == 1 ? abg : ibg);
        fb_text_to(bb, bxx, fy, ubuf, 0xEAF2FF, field == 0 ? abg : ibg);

        char mask[L_MAX + 4]; int i;
        for (i = 0; i < plen; i++) mask[i] = '*';
        mask[i] = 0;
        fb_text_to(bb, bxx, fy + 34, mask, 0xEAF2FF, field == 1 ? abg : ibg);

        if ((millis() / 500) & 1) {                              /* blink cursor */
            int cxp = bxx + (field == 0 ? ulen : plen) * 8;
            int cyp = fy + (field == 0 ? 0 : 34);
            fb_text_to(bb, cxp, cyp, "_", 0xFFFFFF, abg);
        }

        if (msg[0]) fb_text_to(bb, px + 20, py + ph - 42, msg, 0xFF6666, 0x101C2E);
        fb_text_to(bb, px + 20, py + ph - 22,
                   "enter = next / sign in     tab = switch     esc = clear",
                   0x6688AA, 0x101C2E);

        fb_present(bb);
    }
}
