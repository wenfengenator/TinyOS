/* tinyos x64 tiny windowing system (Cycle 3).
 *
 * A keyboard-driven desktop compositor over the GOP framebuffer.  Every frame it
 * repaints a full-screen back buffer — desktop background, a top task bar, then
 * each window bottom-to-top with the focused window drawn last so it sits on top
 * — and blits it with fb_present().  Keystrokes are polled from both the PS/2
 * (QEMU) and VMBus (Hyper-V) keyboards and routed to the focused window, except
 * for the reserved desktop keys:
 *
 *   F1              cycle focus to the next window (which raises it)
 *   F2              toggle "move mode" for the focused window
 *   arrows (move)   nudge the focused window while move mode is on
 *
 * The window manager knows nothing about what a window contains: each window
 * supplies a draw() and an on_key() callback (see kernel/wm64.h).  This file
 * wires up two windows — an interactive TERMINAL and a live SYSTEM MONITOR. */
#include "wm64.h"

#define MAX_WIN     8
#define TITLE_H     16
#define TASKBAR_H   18
#define MOVE_STEP   24
#define MARGIN      12
#define CONTENT_BG  0x08111C

static wm_window_t g_wins[MAX_WIN];
static int         g_nwin, g_focus, g_move;
static uint32_t    g_scr_w, g_scr_h, g_fps;
static uint32_t   *g_back;                 /* full-screen compositor buffer      */
static boot_info_t *g_bi;

int         wm_window_count(void)       { return g_nwin; }
const char *wm_window_title(int i)      { return (i >= 0 && i < g_nwin) ? g_wins[i].title : ""; }
int         wm_focused_index(void)      { return g_focus; }

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

/* clipped solid rectangle into the tight full-screen back buffer (stride g_scr_w) */
static void fill_rect(uint32_t *back, int x, int y, int w, int h, uint32_t rgb) {
    uint32_t px = fb_pack(rgb);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0 || x >= (int)g_scr_w || y >= (int)g_scr_h) return;
    if (x + w > (int)g_scr_w) w = (int)g_scr_w - x;
    if (y + h > (int)g_scr_h) h = (int)g_scr_h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = back + (int64_t)(y + j) * g_scr_w + x;
        for (int i = 0; i < w; i++) row[i] = px;
    }
}

/* "label<decimal>" at (x,y), label dim-blue and value white */
static void draw_kv(uint32_t *back, int x, int y, const char *label, uint32_t val) {
    fb_text_to(back, (uint32_t)x, (uint32_t)y, label, 0x99CCFF, CONTENT_BG);
    char b[12]; int n = 0; uint32_t v = val;
    if (!v) b[n++] = '0';
    else { char tmp[12]; int m = 0; while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; } while (m) b[n++] = tmp[--m]; }
    b[n] = 0;
    fb_text_to(back, (uint32_t)(x + (int)slen(label) * 8), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG);
}

/* live system-stats window (content callback) */
static void sysmon_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch) {
    (void)win; (void)cw; (void)ch;
    fb_text_to(back, (uint32_t)cx, (uint32_t)cy, "tinyos x64 kernel", 0x66FFCC, CONTENT_BG);
    draw_kv(back, cx, cy + 16, "FPS      ", g_fps);
    draw_kv(back, cx, cy + 28, "UPTIME S ", (uint32_t)(millis() / 1000));
    draw_kv(back, cx, cy + 40, "HEAP MB  ", (uint32_t)(heap_total() / (1024 * 1024)));
    draw_kv(back, cx, cy + 52, "USED MB  ", (uint32_t)(heap_used()  / (1024 * 1024)));
    draw_kv(back, cx, cy + 64, "CORES    ", (uint32_t)smp_online_count());
    draw_kv(back, cx, cy + 76, "TSC MHZ  ", (uint32_t)(tsc_freq_hz() / 1000000));
    draw_kv(back, cx, cy + 88, "HV STAGE ", (uint32_t)hvkbd_stage());
}

/* draw one window (frame + content) into the back buffer */
static void draw_window(wm_window_t *w, uint32_t *back, int focused) {
    uint32_t border  = focused ? 0x3AA0FF : 0x2A4A66;
    uint32_t titlebg = focused ? 0x1E5FA8 : 0x1A2A3A;
    fill_rect(back, w->x - 1, w->y - 1, w->w + 2, w->h + 2, border);
    fill_rect(back, w->x, w->y, w->w, TITLE_H, titlebg);
    fb_text_to(back, (uint32_t)(w->x + 4), (uint32_t)(w->y + 4), w->title, 0xFFFFFF, titlebg);
    if (focused && g_move)
        fb_text_to(back, (uint32_t)(w->x + w->w - 56), (uint32_t)(w->y + 4), "[MOVE]", 0xFFEE44, titlebg);
    int cyc = w->y + TITLE_H, chc = w->h - TITLE_H;
    fill_rect(back, w->x, cyc, w->w, chc, CONTENT_BG);
    /* direct dispatch by kind (no cross-object function pointers) */
    switch (w->kind) {
        case WIN_TERMINAL: term_draw(w, back, w->x + 6, cyc + 6, w->w - 12, chc - 12); break;
        case WIN_SYSMON:   sysmon_draw(w, back, w->x + 6, cyc + 6, w->w - 12, chc - 12); break;
        default: break;
    }
}

static void compositor(uint32_t *back) {
    fill_rect(back, 0, 0, (int)g_scr_w, (int)g_scr_h, 0x0A1830);      /* desktop  */
    fill_rect(back, 0, 0, (int)g_scr_w, TASKBAR_H, 0x102A4A);         /* task bar */
    fb_text_to(back, 6, 5,
               "tinyos desktop    F1 = focus    F2 = move + arrows    terminal: type help",
               0xEAF2FF, 0x102A4A);
    for (int i = 0; i < g_nwin; i++) if (i != g_focus) draw_window(&g_wins[i], back, 0);
    if (g_nwin) draw_window(&g_wins[g_focus], back, 1);
}

static void clamp_window(wm_window_t *w) {
    if (w->x + w->w > (int)g_scr_w) w->x = (int)g_scr_w - w->w;
    if (w->y + w->h > (int)g_scr_h) w->y = (int)g_scr_h - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < TASKBAR_H) w->y = TASKBAR_H;
}

static void setup_windows(boot_info_t *bi) {
    g_nwin = g_focus = g_move = 0;
    int top = TASKBAR_H + MARGIN;

    /* terminal fills the left ~62% of the desktop */
    int tw = (int)g_scr_w * 62 / 100; if (tw < 320) tw = 320;
    int th = (int)g_scr_h - top - MARGIN;
    int cols = (tw - 12) / 8;
    int rows = (th - TITLE_H - 12) / 8;
    wm_window_t *tm = &g_wins[g_nwin++];
    tm->x = MARGIN; tm->y = top; tm->w = tw; tm->h = th; tm->title = "terminal";
    tm->user = term_create(bi, cols, rows);
    tm->kind = WIN_TERMINAL;

    /* system monitor sits to the right */
    int sx = MARGIN + tw + MARGIN;
    int sw = (int)g_scr_w - sx - MARGIN; if (sw < 200) sw = 200;
    wm_window_t *sm = &g_wins[g_nwin++];
    sm->x = sx; sm->y = top; sm->w = sw; sm->h = 140; sm->title = "system monitor";
    sm->user = 0; sm->kind = WIN_SYSMON;
}

/* Composite + present one rate-limited frame.  Called from a blocking syscall
 * (e.g. SYS_READ) so the desktop stays live — cursor blink, system monitor, and
 * the line being typed — while a ring-3 program waits for keyboard input. */
void wm_pump(void) {
    static uint64_t pump_last;
    if (!g_back) return;
    uint64_t t = millis();
    if (t - pump_last < 33) return;
    pump_last = t;
    compositor(g_back);
    fb_present(g_back);
}

void wm_run(boot_info_t *bi) {
    g_bi = bi;
    fb_get(&g_scr_w, &g_scr_h);
    if (!g_scr_w || !g_scr_h) { kputs("[wm] no framebuffer\n"); return; }

    g_back = (uint32_t *)kmalloc((uint64_t)g_scr_w * g_scr_h * 4);
    if (!g_back) { kputs("[wm] back-buffer alloc failed\n"); return; }

    setup_windows(bi);
    kputs("[wm] desktop up — terminal + system monitor; F1 focus, F2 move\n");

    uint64_t last = millis();
    uint64_t last_frame = 0;
    uint32_t acc = 0;

    for (;;) {
        int k;
        while ((k = kbd64_getkey()) >= 0 || (k = hvkbd_getkey()) >= 0) {
            if (k == KEY_F1) { g_focus = (g_focus + 1) % g_nwin; g_move = 0; continue; }
            if (k == KEY_F2) { g_move = !g_move; continue; }
            if (g_move) {
                wm_window_t *wf = &g_wins[g_focus];
                switch (k) {
                    case KEY_LEFT:  wf->x -= MOVE_STEP; break;
                    case KEY_RIGHT: wf->x += MOVE_STEP; break;
                    case KEY_UP:    wf->y -= MOVE_STEP; break;
                    case KEY_DOWN:  wf->y += MOVE_STEP; break;
                    case '\n': case '\r': case KEY_ESC: g_move = 0; break;
                    default: break;
                }
                clamp_window(wf);
                continue;
            }
            wm_window_t *wf = &g_wins[g_focus];
            if (wf->kind == WIN_TERMINAL) term_key(wf, k);
        }

        /* cap the compositor to ~30 FPS so the full-frame blit doesn't flood
         * Hyper-V's synthetic video channel (which stalls the VMConnect console). */
        uint64_t t = millis();
        if (t - last_frame < 33) { sleep_ms(3); continue; }
        last_frame = t;

        compositor(g_back);
        fb_present(g_back);

        acc++;
        if (t - last >= 1000) { g_fps = acc; acc = 0; last = t; }
    }
}
