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
#define MOVE_STEP   24
#define MARGIN      12
#define CONTENT_BG  0x08111C

static wm_window_t g_wins[MAX_WIN];
static int         g_nwin, g_focus, g_move;
static uint32_t    g_scr_w, g_scr_h, g_fps;
static uint32_t   *g_back;                 /* full-screen compositor buffer      */
static boot_info_t *g_bi;

/* UI metrics chosen from the resolution in wm_run: a small framebuffer (Hyper-V's
 * fixed 1024x768) uses an 8px font so the desktop isn't cramped; a big one uses
 * 16px so text stays legible.  Title/task bars and line pitch derive from these. */
static int g_uisc   = 2;                   /* font scale: 1 (low-res) or 2 (hi-res) */
static int g_cell   = 16;                  /* glyph cell = 8 * g_uisc               */
static int g_titleh = 24;                  /* window title bar height               */
static int g_taskh  = 26;                  /* desktop task bar height               */
static int g_lineh  = 24;                  /* sysmon line pitch                     */

int wm_ui_scale(void) { return g_uisc; }
int wm_ui_cell(void)  { return g_cell; }
int wm_ui_lineh(void) { return g_lineh; }

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
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, label, 0x99CCFF, CONTENT_BG, g_uisc);
    char b[12]; int n = 0; uint32_t v = val;
    if (!v) b[n++] = '0';
    else { char tmp[12]; int m = 0; while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; } while (m) b[n++] = tmp[--m]; }
    b[n] = 0;
    fb_text_to_s(back, (uint32_t)(x + (int)slen(label) * g_cell), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG, g_uisc);
}

/* like draw_kv but renders the value as 0x-prefixed hex (for status bitfields) */
static void draw_kv_hex(uint32_t *back, int x, int y, const char *label, uint32_t val) {
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, label, 0x99CCFF, CONTENT_BG, g_uisc);
    char b[12]; int n = 0; b[n++] = '0'; b[n++] = 'x';
    for (int s = 28; s >= 0; s -= 4) { int d = (int)((val >> s) & 0xF); b[n++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); }
    b[n] = 0;
    fb_text_to_s(back, (uint32_t)(x + (int)slen(label) * g_cell), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG, g_uisc);
}

/* render "RES <w>x<h>" (the live framebuffer resolution) */
static void draw_res(uint32_t *back, int x, int y, uint32_t w, uint32_t h) {
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, "RES      ", 0x99CCFF, CONTENT_BG, g_uisc);
    char b[16]; int n = 0, m; uint32_t v; char t[6];
    v = w; m = 0; if (!v) t[m++] = '0'; while (v) { t[m++] = (char)('0' + v % 10); v /= 10; } while (m) b[n++] = t[--m];
    b[n++] = 'x';
    v = h; m = 0; if (!v) t[m++] = '0'; while (v) { t[m++] = (char)('0' + v % 10); v /= 10; } while (m) b[n++] = t[--m];
    b[n] = 0;
    fb_text_to_s(back, (uint32_t)(x + 9 * g_cell), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG, g_uisc);
}

/* render "TIME     HH:MM:SS" from the UEFI-seeded wall clock ("--:--:--" if unset) */
static void draw_clock(uint32_t *back, int x, int y) {
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, "TIME     ", 0x99CCFF, CONTENT_BG, g_uisc);
    uint64_t e = wall_epoch();
    char b[12]; int n = 0;
    if (!e) { const char *d = "--:--:--"; while (*d) b[n++] = *d++; }
    else {
        uint32_t s = (uint32_t)(e % 86400);
        int hh = (int)(s / 3600), mm = (int)((s % 3600) / 60), ss = (int)(s % 60);
        b[n++] = (char)('0' + hh / 10); b[n++] = (char)('0' + hh % 10); b[n++] = ':';
        b[n++] = (char)('0' + mm / 10); b[n++] = (char)('0' + mm % 10); b[n++] = ':';
        b[n++] = (char)('0' + ss / 10); b[n++] = (char)('0' + ss % 10);
    }
    b[n] = 0;
    fb_text_to_s(back, (uint32_t)(x + 9 * g_cell), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG, g_uisc);
}

/* render "DATE     YYYY-MM-DD" (civil date from the wall-clock epoch) */
static void draw_date(uint32_t *back, int x, int y) {
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, "DATE     ", 0x99CCFF, CONTENT_BG, g_uisc);
    uint64_t e = wall_epoch();
    char b[12]; int n = 0;
    if (!e) { const char *d = "----------"; while (*d) b[n++] = *d++; }
    else {
        long z = (long)(e / 86400) + 719468;                 /* Hinnant civil_from_days */
        long era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = (unsigned)(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int Y = (int)yoe + (int)(era * 400);
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned mp = (5 * doy + 2) / 153;
        unsigned D = doy - (153 * mp + 2) / 5 + 1;
        unsigned M = mp + (mp < 10 ? 3 : (unsigned)-9);
        Y += (M <= 2);
        b[n++] = (char)('0' + (Y / 1000) % 10); b[n++] = (char)('0' + (Y / 100) % 10);
        b[n++] = (char)('0' + (Y / 10) % 10);   b[n++] = (char)('0' + Y % 10);
        b[n++] = '-'; b[n++] = (char)('0' + M / 10); b[n++] = (char)('0' + M % 10);
        b[n++] = '-'; b[n++] = (char)('0' + D / 10); b[n++] = (char)('0' + D % 10);
    }
    b[n] = 0;
    fb_text_to_s(back, (uint32_t)(x + 9 * g_cell), (uint32_t)y, b, 0xFFFFFF, CONTENT_BG, g_uisc);
}

/* live system-stats window (content callback) */
static void sysmon_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch) {
    (void)win; (void)cw; (void)ch;
    int y = cy;
    fb_text_to_s(back, (uint32_t)cx, (uint32_t)y, "tinyos x64 kernel", 0x66FFCC, CONTENT_BG, g_uisc);
    y += g_lineh + 4;
    draw_kv(back, cx, y, "FPS      ", g_fps);                                      y += g_lineh;
    draw_kv(back, cx, y, "UPTIME S ", (uint32_t)(millis() / 1000));                y += g_lineh;
    draw_date(back, cx, y);                                                        y += g_lineh;
    draw_clock(back, cx, y);                                                       y += g_lineh;
    draw_kv(back, cx, y, "HEAP MB  ", (uint32_t)(heap_total() / (1024 * 1024)));    y += g_lineh;
    draw_kv(back, cx, y, "USED MB  ", (uint32_t)(heap_used()  / (1024 * 1024)));    y += g_lineh;
    draw_kv(back, cx, y, "CORES    ", (uint32_t)smp_online_count());               y += g_lineh;
    draw_kv(back, cx, y, "TSC MHZ  ", (uint32_t)(tsc_freq_hz() / 1000000));         y += g_lineh;
    draw_kv(back, cx, y, "HV STAGE ", (uint32_t)hvkbd_stage());                    y += g_lineh;
    draw_kv(back, cx, y, "STOR STG ", (uint32_t)storvsc_stage());                  y += g_lineh;
    draw_kv_hex(back, cx, y, "STOR IO  ", (uint32_t)storvsc_diag());               y += g_lineh;
    draw_kv_hex(back, cx, y, "STOR VER ", (uint32_t)storvsc_ver());                y += g_lineh;
    draw_kv(back, cx, y, "STOR BSZ ", (uint32_t)storvsc_blksz());                  y += g_lineh;
    draw_kv(back, cx, y, "NET STG  ", (uint32_t)netvsc_stage());                   y += g_lineh;
    draw_res(back, cx, y, g_scr_w, g_scr_h);                                       y += g_lineh;
    draw_kv(back, cx, y, "GOP MODES", g_bi ? g_bi->gop_modes : 0);
}

/* kernel log / trace window: renders the scrollback ring (kernel/fb64.c) with a
 * keyboard scroll offset so boot + runtime messages persist off the fb console. */
static int g_log_scroll;                    /* lines scrolled up from the tail (0 = live) */

static void log_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch) {
    (void)win;
    int total = klog_lines();
    int vis = ch / g_lineh - 1;              /* reserve the last row for the status line */
    if (vis < 1) vis = 1;
    int W = cw / g_cell; if (W < 1) W = 1; if (W > 126) W = 126;

    if (g_log_scroll < 0) g_log_scroll = 0;
    if (g_log_scroll > total - 1) g_log_scroll = total - 1;
    int end = total - 1 - g_log_scroll;      /* newest visible log line */
    if (end < 0) end = 0;

    /* walk back from `end`, word-wrapping each line, until `vis` rows are filled */
    int start = end, rows = 0;
    for (int i = end; i >= 0; i--) {
        int len = (int)slen(klog_line(i));
        int lr = (len + W - 1) / W; if (lr < 1) lr = 1;
        if (i != end && rows + lr > vis) break;   /* keep `end` even if it alone overflows */
        rows += lr; start = i;
        if (rows >= vis) break;
    }
    if (rows > vis) rows = vis;

    int y = cy + (vis - rows) * g_lineh;     /* bottom-align: newest line at the bottom */
    int drawn = 0;
    for (int i = start; i <= end && drawn < rows; i++) {
        const char *ln = klog_line(i);
        int len = (int)slen(ln), off = 0;
        do {
            char buf[128]; int n = 0;
            while (off + n < len && n < W) { buf[n] = ln[off + n]; n++; }
            buf[n] = 0;
            fb_text_to_s(back, (uint32_t)cx, (uint32_t)y, buf, 0x9FE0A0, CONTENT_BG, g_uisc);
            y += g_lineh; drawn++; off += n;
        } while (off < len && drawn < rows);
    }

    char sb[40]; int sn = 0;
    const char *st = (g_log_scroll > 0) ? "SCROLLBACK  ESC=LIVE" : "LIVE  UP/DN,L/R SCROLL";
    while (st[sn] && sn < W && sn < 39) { sb[sn] = st[sn]; sn++; }
    sb[sn] = 0;
    fb_text_to_s(back, (uint32_t)cx, (uint32_t)(cy + ch - g_lineh), sb,
                 (g_log_scroll > 0) ? 0xFFCC44 : 0x66AAFF, CONTENT_BG, g_uisc);
}

static void log_key(int k) {
    int total = klog_lines();
    switch (k) {
        case KEY_UP:    g_log_scroll += 1;  break;
        case KEY_DOWN:  g_log_scroll -= 1;  break;
        case KEY_LEFT:  g_log_scroll += 10; break;   /* page back  */
        case KEY_RIGHT: g_log_scroll -= 10; break;   /* page fwd   */
        case KEY_ESC:   g_log_scroll = 0;   break;   /* jump to the live tail */
        default: break;
    }
    if (g_log_scroll < 0) g_log_scroll = 0;
    if (g_log_scroll > total - 1) g_log_scroll = total - 1;
}

/* draw one window (frame + content) into the back buffer */
static void draw_window(wm_window_t *w, uint32_t *back, int focused) {
    uint32_t border  = focused ? 0x3AA0FF : 0x2A4A66;
    uint32_t titlebg = focused ? 0x1E5FA8 : 0x1A2A3A;
    fill_rect(back, w->x - 1, w->y - 1, w->w + 2, w->h + 2, border);
    fill_rect(back, w->x, w->y, w->w, g_titleh, titlebg);
    fb_text_to_s(back, (uint32_t)(w->x + 6), (uint32_t)(w->y + 4), w->title, 0xFFFFFF, titlebg, g_uisc);
    if (focused && g_move)
        fb_text_to_s(back, (uint32_t)(w->x + w->w - 7 * g_cell), (uint32_t)(w->y + 4), "[MOVE]", 0xFFEE44, titlebg, g_uisc);
    int cyc = w->y + g_titleh, chc = w->h - g_titleh;
    fill_rect(back, w->x, cyc, w->w, chc, CONTENT_BG);
    /* direct dispatch by kind (no cross-object function pointers) */
    switch (w->kind) {
        case WIN_TERMINAL: term_draw(w, back, w->x + 6, cyc + 6, w->w - 12, chc - 12); break;
        case WIN_SYSMON:   sysmon_draw(w, back, w->x + 6, cyc + 6, w->w - 12, chc - 12); break;
        case WIN_LOG:      log_draw(w, back, w->x + 6, cyc + 6, w->w - 12, chc - 12); break;
        default: break;
    }
}

static void compositor(uint32_t *back) {
    fill_rect(back, 0, 0, (int)g_scr_w, (int)g_scr_h, 0x0A1830);      /* desktop  */
    fill_rect(back, 0, 0, (int)g_scr_w, g_taskh, 0x102A4A);           /* task bar */
    fb_text_to_s(back, 8, 5,
               "tinyos desktop    F1 focus    F2 move    terminal: type help",
               0xEAF2FF, 0x102A4A, g_uisc);
    for (int i = 0; i < g_nwin; i++) if (i != g_focus) draw_window(&g_wins[i], back, 0);
    if (g_nwin) draw_window(&g_wins[g_focus], back, 1);
}

static void clamp_window(wm_window_t *w) {
    if (w->x + w->w > (int)g_scr_w) w->x = (int)g_scr_w - w->w;
    if (w->y + w->h > (int)g_scr_h) w->y = (int)g_scr_h - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < g_taskh) w->y = g_taskh;
}

static void setup_windows(boot_info_t *bi) {
    g_nwin = g_focus = g_move = 0;
    int top = g_taskh + MARGIN;
    int wh  = (int)g_scr_h - top - MARGIN;                /* both windows full height */

    /* system monitor: just wide enough for its longest line (~26 cols); give the
     * rest to the terminal so code lines are far less likely to clip. */
    int sw = 26 * g_cell;
    if (sw > (int)g_scr_w * 40 / 100) sw = (int)g_scr_w * 40 / 100;

    int tw = (int)g_scr_w - sw - 3 * MARGIN; if (tw < 20 * g_cell) tw = 20 * g_cell;
    int cols = (tw - 12) / g_cell, rows = (wh - g_titleh - 12) / g_lineh;
    wm_window_t *tm = &g_wins[g_nwin++];
    tm->x = MARGIN; tm->y = top; tm->w = tw; tm->h = wh; tm->title = "terminal";
    tm->user = term_create(bi, cols, rows);
    tm->kind = WIN_TERMINAL;

    int rx = MARGIN + tw + MARGIN;                       /* shared right-column x     */
    int smh = g_titleh + 17 * g_lineh + 12;              /* system monitor height (16 rows) */
    int minlog = g_titleh + 6 * g_lineh;                 /* keep the log usable        */
    if (smh > wh - minlog - MARGIN) smh = wh - minlog - MARGIN;

    wm_window_t *sm = &g_wins[g_nwin++];
    sm->x = rx; sm->y = top; sm->w = sw; sm->h = smh; sm->title = "system monitor";
    sm->user = 0; sm->kind = WIN_SYSMON;

    wm_window_t *lg = &g_wins[g_nwin++];                  /* kernel log below the monitor */
    lg->x = rx; lg->y = top + smh + MARGIN; lg->w = sw; lg->h = wh - smh - MARGIN;
    lg->title = "kernel log"; lg->user = 0; lg->kind = WIN_LOG;
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
    disp_present();
}

void wm_run(boot_info_t *bi) {
    g_bi = bi;
    fb_get(&g_scr_w, &g_scr_h);
    if (!g_scr_w || !g_scr_h) { kputs("[wm] no framebuffer\n"); return; }

    g_uisc   = (g_scr_w >= 1440) ? 2 : 1;            /* 8px font on small panels (Hyper-V 1024) */
    g_cell   = 8 * g_uisc;
    g_titleh = g_cell + 8;
    g_taskh  = g_cell + 10;
    g_lineh  = g_cell + g_cell / 2;

    disp_init();
    g_back = disp_back();
    if (!g_back) { kputs("[wm] back-buffer alloc failed\n"); return; }

    setup_windows(bi);
    klog_wm_active(1);                       /* desktop owns the screen; log -> widget */
    kputs("[wm] desktop up: terminal + system monitor + kernel log; F1 focus, F2 move\n");

    if (g_nwin > 0 && g_wins[0].kind == WIN_TERMINAL)   /* land straight in the linux shell */
        term_autostart_shell(g_wins[0].user);

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
            else if (wf->kind == WIN_LOG)  log_key(k);
        }

        /* cap the compositor to ~30 FPS so the full-frame blit doesn't flood
         * Hyper-V's synthetic video channel (which stalls the VMConnect console). */
        uint64_t t = millis();
        if (t - last_frame < 33) { sleep_ms(3); continue; }
        last_frame = t;

        compositor(g_back);
        disp_present();

        acc++;
        if (t - last >= 1000) { g_fps = acc; acc = 0; last = t; }
    }
}
