/* SPDX-License-Identifier: GPL-2.0-only */

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

/* public wrapper so other modules (arch64 pane frames) can draw into the
 * full-screen back buffer using the same clipped fill. */
void wm_fill_rect(uint32_t *back, int x, int y, int w, int h, uint32_t rgb) {
    fill_rect(back, x, y, w, h, rgb);
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

/* like draw_kv but renders a string value (e.g. the OS version) */
static void draw_str(uint32_t *back, int x, int y, const char *label, const char *val) {
    fb_text_to_s(back, (uint32_t)x, (uint32_t)y, label, 0x99CCFF, CONTENT_BG, g_uisc);
    fb_text_to_s(back, (uint32_t)(x + (int)slen(label) * g_cell), (uint32_t)y, val, 0xFFFFFF, CONTENT_BG, g_uisc);
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

/* battery/AC for the system monitor.  The EC is polled at most every ~3 s (bounded
 * reads, safe even on an unfamiliar EC).  Percent/AC/voltage offsets were
 * calibrated from plugged-vs-unplugged EC dumps on the T440s; the raw 0xA0-0xA7
 * block is still shown to help locate the second (Power Bridge) battery. */
static acpi_batt_t g_batt;
static uint64_t g_batt_ms;
static void batt_refresh(void) {
    if (g_batt_ms && millis() - g_batt_ms < 1000) return;   /* 1s: AC/charging updates promptly */
    g_batt_ms = millis();
    acpi_battery(&g_batt);
}
/* Temporary diagnostic removed: AC confirmed at EC 0x46 bit4 on GJHT25WW. */
static const char *batt_text(char *b) {
    int n = 0;
    if (g_batt.percent >= 0) {
        int p = g_batt.percent; char t[4]; int m = 0;
        if (!p) t[m++] = '0'; else while (p) { t[m++] = (char)('0' + p % 10); p /= 10; }
        while (m) b[n++] = t[--m];
        b[n++] = '%';
    } else { b[n++] = '?'; b[n++] = '?'; }
    b[n++] = ' ';
    const char *st = !g_batt.present ? "NO BATT" : g_batt.charging ? "CHG" : g_batt.ac_online ? "AC" : "BAT";
    while (*st) b[n++] = *st++;
    b[n] = 0;
    return b;
}
static const char *volt_text(char *b) {
    int mv = g_batt.voltage_mv, n = 0;
    if (mv <= 0) { b[0] = '-'; b[1] = 0; return b; }
    int whole = mv / 1000, cents = (mv % 1000) / 10;
    char t[6]; int m = 0;
    if (!whole) t[m++] = '0'; else while (whole) { t[m++] = (char)('0' + whole % 10); whole /= 10; }
    while (m) b[n++] = t[--m];
    b[n++] = '.'; b[n++] = (char)('0' + cents / 10); b[n++] = (char)('0' + cents % 10); b[n++] = 'V';
    b[n] = 0;
    return b;
}

static const char *audio_text(char *b) {
    hda_diag_t d;
    if (hda_get_diag(&d) > 0) {
        int n = 0;
        const char *s = "READY C"; while (*s) b[n++] = *s++;
        if (d.codec >= 10) b[n++] = (char)('0' + d.codec / 10);
        b[n++] = (char)('0' + d.codec % 10); b[n++] = ' '; b[n++] = 'P';
        if (d.pin >= 10) b[n++] = (char)('0' + d.pin / 10);
        b[n++] = (char)('0' + d.pin % 10); b[n++] = ' '; b[n++] = 'V';
        if (d.volume >= 100) b[n++] = '1';
        if (d.volume >= 10) b[n++] = (char)('0' + (d.volume / 10) % 10);
        b[n++] = (char)('0' + d.volume % 10); b[n] = 0;
        return b;
    }
    const char *s = d.stage == 0 ? "NOT PROBED" :
                    d.stage == 1 ? "NO PCI" :
                    d.stage == 2 ? "BAD BAR" :
                    d.stage == 3 ? "RESET LOW" :
                    d.stage == 4 ? "RESET HIGH" :
                    d.stage == 5 && !d.codecs ? "NO CODEC" :
                    d.stage == 5 ? "NO ANALOG" : "NO DMA";
    int n = 0; while (*s) b[n++] = *s++; b[n] = 0;
    return b;
}

/* CPU-busy % over the last ~second, derived from the accumulated HLT-idle time
 * (kernel/arch64.c).  Recomputed twice a second; 0 % at an idle shell prompt. */
static uint32_t cpu_busy_pct(void) {
    static uint64_t last_ms, last_idle; static uint32_t pct;
    uint64_t now = millis(), idle = cpu_idle_ms();
    uint64_t dt = now - last_ms;
    if (dt >= 500) {
        uint64_t didle = idle - last_idle;
        if (didle > dt) didle = dt;
        pct = (uint32_t)(100 * (dt - didle) / (dt ? dt : 1));
        last_ms = now; last_idle = idle;
    }
    return pct;
}

/* live system-stats window (content callback) */
static void sysmon_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch) {
    (void)win; (void)cw; (void)ch;
    int y = cy;
    fb_text_to_s(back, (uint32_t)cx, (uint32_t)y, "tinyos x64 kernel", 0x66FFCC, CONTENT_BG, g_uisc);
    y += g_lineh + 4;
    draw_str(back, cx, y, "OS VER   ", "v" TINYOS_VERSION);                        y += g_lineh;
    draw_kv(back, cx, y, "CPU %    ", cpu_busy_pct());                             y += g_lineh;
    draw_kv(back, cx, y, "FPS      ", g_fps);                                      y += g_lineh;
    draw_kv(back, cx, y, "UPTIME S ", (uint32_t)(millis() / 1000));                y += g_lineh;
    draw_date(back, cx, y);                                                        y += g_lineh;
    draw_clock(back, cx, y);                                                       y += g_lineh;
    draw_kv(back, cx, y, "HEAP MB  ", (uint32_t)(heap_total() / (1024 * 1024)));    y += g_lineh;
    draw_kv(back, cx, y, "USED MB  ", (uint32_t)(heap_used()  / (1024 * 1024)));    y += g_lineh;
    draw_kv(back, cx, y, "CORES    ", (uint32_t)smp_online_count());               y += g_lineh;
    draw_kv(back, cx, y, "TSC MHZ  ", (uint32_t)(tsc_freq_hz() / 1000000));         y += g_lineh;
    draw_kv(back, cx, y, "HV STAGE ", (uint32_t)hvkbd_stage());                    y += g_lineh;
    draw_kv(back, cx, y, "NET STG  ", (uint32_t)netvsc_stage());                   y += g_lineh;
    { char b[20]; draw_str(back, cx, y, "AUDIO    ", audio_text(b)); }             y += g_lineh;
    draw_res(back, cx, y, g_scr_w, g_scr_h);                                       y += g_lineh;
    draw_kv(back, cx, y, "GOP MODES", g_bi ? g_bi->gop_modes : 0);                 y += g_lineh;
    batt_refresh();
    { char b[24]; draw_str(back, cx, y, "BATT     ", batt_text(b)); }              y += g_lineh;
    { char b[12]; draw_str(back, cx, y, "VOLT     ", volt_text(b)); }              y += g_lineh;
    draw_str(back, cx, y, "LID      ", acpi_lid_open() ? "OPEN" : "CLOSED");
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
               "tinyos desktop   F1 focus  F2 move  F3 sleep  F4 pane  F5 split   terminal: type help",
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

/* Window-manager hotkeys, usable both from the desktop loop and from a blocking
 * foreground app's keyboard reader (so F1/F2 + kernel-log scrolling keep working
 * while the linux shell owns stdin).  Returns 1 if the key was consumed by the
 * WM; 0 means "pass it to the focused window's content". */
int wm_hotkey(int key) {
    if (wm_is_sleeping()) { wm_sleep_set(0); return 1; }   /* any key wakes from sleep */
    if (g_nwin <= 0) return 0;
    ui_note_activity();                        /* any hotkey (focus/move/scroll) redraws */
    if (key == KEY_F3) { wm_sleep_set(1); return 1; }  /* F3: sleep now (screen off + deep idle) */
    if (key == KEY_F1) { g_focus = (g_focus + 1) % g_nwin; g_move = 0; return 1; }
    if (key == KEY_F2) { g_move = !g_move; return 1; }
    wm_window_t *wf = &g_wins[g_focus];
    if (g_move) {
        switch (key) {
            case KEY_LEFT:  wf->x -= MOVE_STEP; break;
            case KEY_RIGHT: wf->x += MOVE_STEP; break;
            case KEY_UP:    wf->y -= MOVE_STEP; break;
            case KEY_DOWN:  wf->y += MOVE_STEP; break;
            case '\n': case '\r': case KEY_ESC: g_move = 0; break;
            default: break;
        }
        clamp_window(wf);
        return 1;
    }
    if (wf->kind == WIN_LOG) { log_key(key); return 1; }   /* log focused: keys scroll it */
    return 0;                                              /* terminal focused: content gets it */
}

static void setup_windows(boot_info_t *bi) {
    g_nwin = g_focus = g_move = 0;
    int top = g_taskh + MARGIN;
    int wh  = (int)g_scr_h - top - MARGIN;                /* both windows full height */

    /* system monitor / kernel-log column.  With the compact 8px font the terminal
     * has spare width, so give the right column a comfortable ~44 cols. */
    int sw = 44 * g_cell;
    if (sw > (int)g_scr_w * 45 / 100) sw = (int)g_scr_w * 45 / 100;

    int tw = (int)g_scr_w - sw - 3 * MARGIN; if (tw < 20 * g_cell) tw = 20 * g_cell;
    int cols = (tw - 12) / g_cell, rows = (wh - g_titleh - 12) / g_lineh;
    wm_window_t *tm = &g_wins[g_nwin++];
    tm->x = MARGIN; tm->y = top; tm->w = tw; tm->h = wh; tm->title = "terminal";
    tm->user = term_create(bi, cols, rows);
    tm->kind = WIN_TERMINAL;

    int rx = MARGIN + tw + MARGIN;                       /* shared right-column x     */
    int smh = g_titleh + 20 * g_lineh + 12;              /* system monitor height */
    int minlog = g_titleh + 6 * g_lineh;                 /* keep the log usable        */
    if (smh > wh - minlog - MARGIN) smh = wh - minlog - MARGIN;

    wm_window_t *sm = &g_wins[g_nwin++];
    sm->x = rx; sm->y = top; sm->w = sw; sm->h = smh; sm->title = "system monitor";
    sm->user = 0; sm->kind = WIN_SYSMON;

    wm_window_t *lg = &g_wins[g_nwin++];                  /* kernel log below the monitor */
    lg->x = rx; lg->y = top + smh + MARGIN; lg->w = sw; lg->h = wh - smh - MARGIN;
    lg->title = "kernel log"; lg->user = 0; lg->kind = WIN_LOG;
}

/* UI activity timestamp: bumped on keypress / terminal output so the compositor
 * repaints at the full 30 FPS briefly, then idles.  Lets the desktop go quiet
 * (a couple of blits/second) when nothing is happening — the other half of the
 * weeks-on-battery goal alongside the CPU HLT in the read loops. */
static volatile uint64_t g_ui_activity_ms;
void ui_note_activity(void) { g_ui_activity_ms = millis(); }

/* ---- low-power sleep (lid close / manual) --------------------------------
 * When asleep the compositor stops (screen held black, deepest HLT idle) and,
 * on the real T440s, the EC has already cut the panel backlight in hardware.
 * Any keypress or a lid-open transition wakes it. */
static volatile int g_sleeping;
int wm_is_sleeping(void) { return g_sleeping; }
/* Timestamp of the last full-screen TinyGL present; while it is recent, wm_pump
 * stands down so the compositor does not fight the app for the framebuffer. */
static volatile uint64_t g_gfx_present_ms;
void wm_note_gfx_present(void) { g_gfx_present_ms = millis(); }
void wm_sleep_set(int on) {
    if (on == g_sleeping) return;
    g_sleeping = on ? 1 : 0;
    if (!g_back) return;
    if (on) {                                           /* blank the framebuffer once */
        for (uint32_t i = 0, n = g_scr_w * g_scr_h; i < n; i++) g_back[i] = 0;
        disp_present();
    } else {                                            /* wake: force a fresh full frame */
        ui_note_activity();
        compositor(g_back);
        disp_present();
    }
}

/* Sample the lid ~4x/s and sleep/wake on an open<->closed *transition* only, so
 * a wrong/static EC reading can never spuriously blank the screen — only a real
 * lid movement acts.  Called from the shell's input-wait loops. */
void power_poll(void) {
    static uint64_t last;
    static int prev_open = -1;
    uint64_t t = millis();
    if (t - last < 250) return;
    last = t;
    if (acpi_power_button()) wm_sleep_set(!wm_is_sleeping());   /* press toggles sleep/wake */
    int open = acpi_lid_open();
    if (prev_open < 0) { prev_open = open; return; }    /* seed without acting */
    if (prev_open && !open) wm_sleep_set(1);            /* lid just closed */
    else if (!prev_open && open) wm_sleep_set(0);       /* lid just opened */
    prev_open = open;
}

/* Composite + present one rate-limited frame.  Called from a blocking syscall
 * (e.g. SYS_READ) so the desktop stays live — cursor blink, system monitor, and
 * the line being typed — while a ring-3 program waits for keyboard input.
 * Render-on-change: after ~300 ms of no activity it only repaints on the 500 ms
 * cursor-blink / clock boundary (~2 FPS) instead of blitting 30 full frames/s. */
void wm_pump(void) {
    static uint64_t pump_last;
    static uint64_t idle_slot = (uint64_t)-1;
    if (!g_back || g_sleeping) return;                  /* asleep: hold the black screen */
    uint64_t t = millis();
    if (t - g_gfx_present_ms < 600) return;             /* a fullscreen GL app owns the screen */
    if (t - pump_last < 33) return;                     /* 30 FPS hard cap */
    if (t - g_ui_activity_ms > 300) {                   /* idle: skip redundant blits */
        uint64_t slot = t / 500;                        /* cursor blink + clock tick */
        if (slot == idle_slot) return;
        idle_slot = slot;
    }
    pump_last = t;
    compositor(g_back);
    disp_present();
}

void wm_run(boot_info_t *bi) {
    g_bi = bi;
    fb_get(&g_scr_w, &g_scr_h);
    if (!g_scr_w || !g_scr_h) { kputs("[wm] no framebuffer\n"); return; }

    g_uisc   = (g_scr_w >= 2560) ? 2 : 1;            /* 8x16 VGA font; scale up only on 4K+ panels */
    g_cell   = 8 * g_uisc;                           /* glyph / column width  */
    g_lineh  = 16 * g_uisc;                          /* glyph / row height    */
    g_titleh = g_lineh + 8;
    g_taskh  = g_lineh + 10;

    disp_init();
    g_back = disp_back();
    if (!g_back) { kputs("[wm] back-buffer alloc failed\n"); return; }

    setup_windows(bi);
    klog_wm_active(1);                       /* desktop owns the screen; log -> widget */
    kputs("[wm] desktop up: terminal + system monitor + kernel log; F1 focus, F2 move\n");

    if (g_nwin > 0 && g_wins[0].kind == WIN_TERMINAL)   /* land straight in the linux shell */
        term_autostart_shell(g_wins[0].user);

    uint64_t last = millis();
    uint32_t acc = 0;

    for (;;) {
        int k;
        while ((k = kbd64_getkey()) >= 0 || (k = hvkbd_getkey()) >= 0) {
            ui_note_activity();                             /* any key: repaint at full rate */
            if (wm_hotkey(k)) continue;                     /* F1/F2/move/log-scroll */
            wm_window_t *wf = &g_wins[g_focus];
            if (wf->kind == WIN_TERMINAL) term_key(wf, k);
        }

        wm_pump();                                          /* composite on change (respects sleep) */
        power_poll();                                       /* lid / power-button sleep triggers */

        /* Idle without pegging a core.  The native console runs with the
         * scheduler stopped, so briefly arm a LAPIC tick around the HLT (it
         * lands in sched_tick's guarded ack) and disarm on wake.  Falls back to
         * a short sleep only before the timer has ever been calibrated. */
        if (lapic_ready()) {
            uint64_t t0 = millis();
            lapic_idle_arm();
            __asm__ volatile("sti; hlt");
            lapic_idle_disarm();
            cpu_idle_add_ms(millis() - t0);                 /* count the halt toward CPU-idle %% */
        } else {
            sleep_ms(3);
        }

        uint64_t t = millis();
        acc++;
        if (t - last >= 1000) { g_fps = acc; acc = 0; last = t; }
    }
}
