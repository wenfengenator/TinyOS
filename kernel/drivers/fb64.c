/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 kernel console: COM1 serial + GOP linear framebuffer.
 *
 * Runs after ExitBootServices, so it touches only hardware the firmware left in
 * place: the 16550 UART (port I/O) and the linear framebuffer (MMIO).  Text is
 * mirrored to both, so the boot can be verified headlessly over serial while a
 * VM/monitor shows the same output on screen. */
#include "boot64.h"
#include "arch64.h"
#include "font8x16.h"

/* Freestanding memcpy/memset so the compiler always has a real symbol to call
 * (built as -fno-builtin so these loops are not turned back into self-calls). */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d;
}
void *memmove(void *d, const void *s, unsigned long n) {
    unsigned char *dst = d; const unsigned char *src = s;
    if (dst < src) for (unsigned long i = 0; i < n; i++) dst[i] = src[i];
    else if (dst > src) while (n) { n--; dst[n] = src[n]; }
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n) {
    const unsigned char *left = a, *right = b;
    for (unsigned long i = 0; i < n; i++)
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    return 0;
}

/* ---- COM1 serial ---- */
#define COM1 0x3F8
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t r; __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
void serial_init(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

/* ---- framebuffer state ---- */
static volatile uint32_t *g_fb;         /* 0 until fb_init(): serial-only before */
static uint32_t g_w, g_h, g_pitch, g_fmt;
static uint64_t g_fb_size;
static uint32_t g_cx, g_cy;             /* text cursor (glyph cells)             */
static uint32_t g_fg = 0xC0C0C0;        /* light grey on dark blue               */
static uint32_t g_bg = 0x001020;

/* Haswell Intel display backlight + panel-power registers (IGD MMIO BAR, PCH
 * south-display block).  A real S3 resets the display engine, clearing the PWM
 * *enable* bits (in CTL1 / CPU_CTL2) and the panel-power sequencer, so restoring
 * only the duty/frequency leaves the panel dark.  Other GPUs and VMs untouched. */
#define BLC_PWM_CPU_CTL2  0x48250      /* CPU PWM enable (bit 31)              */
#define BLC_PWM_CPU_CTL   0x48254      /* CPU PWM duty                        */
#define BLC_PWM_PCH_CTL1  0xC8250      /* PCH PWM enable (bit 31)             */
#define BLC_PWM_PCH_CTL2  0xC8254      /* PCH PWM frequency + duty            */
#define PCH_PP_STATUS     0xC7200      /* bit 31 = panel powered on           */
#define PCH_PP_CONTROL    0xC7204      /* power/BLC bits + 0xABCD key in [31:16] */
#define PCH_PP_ON_DELAYS  0xC7208
#define PCH_PP_OFF_DELAYS 0xC720C
#define PCH_PP_DIVISOR    0xC7210
#define PP_UNLOCK_MASK    0xFFFF0000u
#define PP_UNLOCK_KEY     0xABCD0000u
#define PP_POWER_ON       (1u << 0)
#define PP_POWER_RESET    (1u << 1)
#define PP_BLC_ENABLE     (1u << 2)
#define PWM_ENABLE        (1u << 31)
static volatile uint8_t *g_igd;
static uint32_t g_cpu_ctl, g_pch_ctl2;   /* deep-S0 backlight blank (fb_panel_power) */
static int g_panel_saved, g_panel_off;
static uint32_t g_s3_pp[8];              /* full backlight+PPS snapshot for S3 resume */
static int g_s3_pp_saved;

static uint32_t pci_read(int bus, int dev, int fn, int off) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                    ((uint32_t)fn << 8) | (uint32_t)(off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static void panel_probe(void) {
    if (g_igd) return;
    for (int bus = 0; bus < 256 && !g_igd; bus++) for (int dev = 0; dev < 32 && !g_igd; dev++) {
        uint32_t id = pci_read(bus, dev, 0, 0);
        if ((id & 0xFFFFu) != 0x8086u) continue;
        uint32_t cls = pci_read(bus, dev, 0, 8);
        if ((cls >> 24) != 0x03) continue;
        uint32_t lo = pci_read(bus, dev, 0, 0x10);
        if ((lo & 1u) || !(lo & ~0xFu)) continue;
        uint64_t bar = lo & ~0xFu;
        if ((lo & 6u) == 4) bar |= (uint64_t)pci_read(bus, dev, 0, 0x14) << 32;
        g_igd = (volatile uint8_t *)(uintptr_t)bar;
    }
}

static uint32_t igd_read(uint32_t off) { return *(volatile uint32_t *)(g_igd + off); }
static void igd_write(uint32_t off, uint32_t value) {
    *(volatile uint32_t *)(g_igd + off) = value;
    (void)*(volatile uint32_t *)(g_igd + off);           /* posting read */
}

void fb_panel_power(int on) {
    panel_probe();
    if (!g_igd) return;
    if (!on && !g_panel_off) {
        g_cpu_ctl = igd_read(BLC_PWM_CPU_CTL);
        g_pch_ctl2 = igd_read(BLC_PWM_PCH_CTL2);
        g_panel_saved = 1;
        /* Keep the PWM clocks/controllers running.  Disabling both controllers
         * loses the firmware-programmed panel sequencing on Haswell and the
         * T440s may not light again when raw register snapshots are restored. */
        igd_write(BLC_PWM_CPU_CTL, 0);
        igd_write(BLC_PWM_PCH_CTL2, g_pch_ctl2 & 0xFFFF0000u);
        g_panel_off = 1;
    } else if (on && g_panel_off && g_panel_saved) {
        igd_write(BLC_PWM_CPU_CTL, g_cpu_ctl);
        igd_write(BLC_PWM_PCH_CTL2, g_pch_ctl2);
        g_panel_off = 0;
    }
}

void fb_s3_suspend(void) {
    panel_probe();
    if (!g_igd) return;
    g_s3_pp[0] = igd_read(BLC_PWM_CPU_CTL2);
    g_s3_pp[1] = igd_read(BLC_PWM_CPU_CTL);
    g_s3_pp[2] = igd_read(BLC_PWM_PCH_CTL1);
    g_s3_pp[3] = igd_read(BLC_PWM_PCH_CTL2);
    g_s3_pp[4] = igd_read(PCH_PP_CONTROL);
    g_s3_pp[5] = igd_read(PCH_PP_ON_DELAYS);
    g_s3_pp[6] = igd_read(PCH_PP_OFF_DELAYS);
    g_s3_pp[7] = igd_read(PCH_PP_DIVISOR);
    g_s3_pp_saved = 1;
}

/* Pack an 0xRRGGBB colour for the firmware's pixel layout.  pixel_format gives
 * the in-memory byte order; as a little-endian dword that is:
 *   fmt 0 (RGBX): byte0=R byte1=G byte2=B  ->  dword = B<<16 | G<<8 | R
 *   fmt 1 (BGRX): byte0=B byte1=G byte2=R  ->  dword = R<<16 | G<<8 | B  */
static uint32_t pack(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
    if (g_fmt == PF_BGRX8888) return (r << 16) | (g << 8) | b;
    return (b << 16) | (g << 8) | r;
}
static inline void putpx(uint32_t x, uint32_t y, uint32_t px) {
    if (x < g_w && y < g_h) g_fb[y * g_pitch + x] = px;
}

void fb_init(const boot_info_t *bi) {
    g_fb    = (volatile uint32_t *)(uintptr_t)bi->fb_base;
    g_w     = bi->width;
    g_h     = bi->height;
    g_pitch = bi->pitch_px ? bi->pitch_px : bi->width;
    g_fmt   = bi->pixel_format;
    g_fb_size = bi->fb_size;
    g_cx = g_cy = 0;
    fb_fill(g_bg);
}
/* Dump key Haswell display-engine registers as "NAME=0xVAL " pairs.  Diagnostic
 * only: a pre-S3 vs post-resume diff (written to /mnt/S3TRACE.TXT) reveals exactly
 * which pipe/plane/PLL/DDI registers the resume reset cleared, so the scanout
 * restore can be targeted instead of guessed. */
int fb_igd_dump(char *buf, int max) {
    static const uint32_t off[] = {
        0x70180, 0x7019C, 0x70188, 0x70008, 0x7001C, 0x6F400, 0x64000,
        0x130040, 0x46020, 0x46100, 0x45400, 0x45404,
        0xC7200, 0xC7204, 0xC8250, 0xC8254, 0x48250, 0x48254,
    };
    static const char *const nm[] = {
        "DSPCNTR", "DSPSURF", "DSPSTRD", "PIPECONF", "PIPESRC", "TRDDI", "DDIBUF",
        "LCPLL", "SPLL", "PORTCLK", "PWRWELL", "PWRWDRV",
        "PPSTS", "PPCTL", "PCHPWM1", "PCHPWM2", "CPUPWM2", "CPUPWM",
    };
    panel_probe();
    int p = 0;
    if (!g_igd || max < 24) { if (max > 0) buf[0] = 0; return 0; }
    for (unsigned i = 0; i < sizeof(off) / sizeof(off[0]) && p < max - 22; i++) {
        uint32_t v = igd_read(off[i]);
        for (const char *s = nm[i]; *s; s++) buf[p++] = *s;
        buf[p++] = '='; buf[p++] = '0'; buf[p++] = 'x';
        for (int sh = 28; sh >= 0; sh -= 4) buf[p++] = "0123456789ABCDEF"[(v >> sh) & 0xF];
        buf[p++] = ' ';
    }
    buf[p] = 0;
    return p;
}

static char g_igd_post[420];
const char *fb_igd_post_dump(void) { return g_igd_post; }

void fb_s3_resume(void) {
    panel_probe();
    fb_igd_dump(g_igd_post, sizeof g_igd_post);   /* raw post-reset snapshot before we touch anything */
    if (g_igd && g_s3_pp_saved) {
        igd_write(PCH_PP_ON_DELAYS, g_s3_pp[5]);
        igd_write(PCH_PP_OFF_DELAYS, g_s3_pp[6]);
        igd_write(PCH_PP_DIVISOR, g_s3_pp[7]);
        /* Power the panel back on; PP_CONTROL drops writes without the unlock key. */
        uint32_t pp = (g_s3_pp[4] & ~PP_UNLOCK_MASK) | PP_UNLOCK_KEY | PP_POWER_ON | PP_POWER_RESET;
        igd_write(PCH_PP_CONTROL, pp);
        for (int i = 0; i < 2000000 && !(igd_read(PCH_PP_STATUS) & (1u << 31)); i++)
            __asm__ volatile("pause");
        /* Re-enable the backlight PWM: the enable bits live in CTL1/CPU_CTL2, which
         * the reset cleared and the old duty-only restore never rewrote. */
        igd_write(BLC_PWM_PCH_CTL2, g_s3_pp[3]);
        igd_write(BLC_PWM_PCH_CTL1, g_s3_pp[2] | PWM_ENABLE);
        igd_write(BLC_PWM_CPU_CTL, g_s3_pp[1]);
        igd_write(BLC_PWM_CPU_CTL2, g_s3_pp[0]);
        igd_write(PCH_PP_CONTROL, pp | PP_BLC_ENABLE);
        g_panel_off = 0;
    }
    disp_present_full();
}
void fb_fill(uint32_t rgb) {
    uint32_t px = pack(rgb);
    for (uint32_t y = 0; y < g_h; y++)
        for (uint32_t x = 0; x < g_w; x++) g_fb[y * g_pitch + x] = px;
}
void fb_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t rgb) {
    uint32_t px = pack(rgb);
    for (uint32_t y = y0; y < y0 + h; y++)
        for (uint32_t x = x0; x < x0 + w; x++) putpx(x, y, px);
}
uint32_t fb_peek(uint32_t x, uint32_t y) {
    if (g_fb && x < g_w && y < g_h) return g_fb[y * g_pitch + x];
    return 0;
}

/* ---- helpers for the graphics demo (double-buffered) ---- */
uint32_t fb_pack(uint32_t rgb) { return pack(rgb); }
void fb_get(uint32_t *w, uint32_t *h) { if (w) *w = g_w; if (h) *h = g_h; }

/* Blit a tightly-packed w*h back buffer (device pixels) onto the framebuffer,
 * accounting for the hardware row stride. */
/* Copy `npx` 32-bit pixels to the (uncached / write-combining) GOP framebuffer
 * with 64-bit stores: half the bus transactions of a dword copy and an eighth of
 * the byte-wise memcpy the blit used to use — the difference between ~3 FPS and a
 * fluid desktop when the framebuffer is uncached on real hardware. */
static inline void fb_blit_row(volatile uint32_t *dst, const uint32_t *src, uint32_t npx) {
    volatile uint64_t *d64 = (volatile uint64_t *)dst;
    const uint64_t *s64 = (const uint64_t *)src;
    uint32_t n64 = npx >> 1;
    for (uint32_t i = 0; i < n64; i++) d64[i] = s64[i];
    if (npx & 1) dst[npx - 1] = src[npx - 1];      /* odd trailing pixel */
}

/* Push the CPU write-combining buffers out to the panel so a WC-mapped
 * framebuffer refreshes on its own.  sfence drains the WC buffers; the extra
 * IN evicts them too (Intel SDM 11.3) — which is precisely why the display used
 * to update only when a key was pressed (PS/2 handling does port I/O).  Doing it
 * here means every present is visible without waiting for a keystroke. */
static inline void fb_flush(void) {
    __asm__ volatile("sfence" ::: "memory");
    (void)inb(0x80);                               /* I/O-port read: forces WC eviction */
}

void fb_present(const uint32_t *back) {
    if (!g_fb) return;
    for (uint32_t y = 0; y < g_h; y++)
        fb_blit_row(g_fb + y * g_pitch, back + y * g_w, g_w);
    fb_flush();
}

/* Present only the scanlines that differ from `shadow` (updated in place), so a
 * mostly-static screen skips the slow framebuffer / Hyper-V synthvid writes for
 * unchanged rows.  Within a changed row only the [x0,x1) span that actually
 * differs is blitted, so a blinking cursor or a ticking clock costs a handful of
 * pixel writes rather than a full 1920-wide row.  `back`/`shadow` are tight w*h. */
void fb_present_diff(const uint32_t *back, uint32_t *shadow, uint32_t w, uint32_t h) {
    if (!g_fb) return;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *b = back + (uint64_t)y * w;
        uint32_t *s = shadow + (uint64_t)y * w;
        uint32_t x0 = 0;
        while (x0 < w && b[x0] == s[x0]) x0++;
        if (x0 == w) continue;                         /* row unchanged: skip FB write */
        uint32_t x1 = w;
        while (x1 > x0 && b[x1 - 1] == s[x1 - 1]) x1--;   /* trim the matching suffix */
        fb_blit_row(g_fb + (uint64_t)y * g_pitch + x0, b + x0, x1 - x0);
        for (uint32_t i = x0; i < x1; i++) s[i] = b[i];   /* shadow tracks only the change */
    }
    fb_flush();
}

/* Draw a string into a tight w-pitched buffer at pixel (x,y), each 8x16 glyph
 * magnified by an integer `scale` (nearest-neighbour, so it stays crisp). */
void fb_text_to_s(uint32_t *buf, uint32_t x, uint32_t y,
                  const char *s, uint32_t fg_rgb, uint32_t bg_rgb, uint32_t scale) {
    if (scale < 1) scale = 1;
    uint32_t fg = pack(fg_rgb), bg = pack(bg_rgb);
    for (; *s; s++) {
        unsigned uc = (unsigned char)*s;
        if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
        const unsigned char *g = font8x16[uc - FONT_FIRST];
        for (uint32_t row = 0; row < FONT_H; row++) {
            unsigned char bits = g[row];
            for (uint32_t col = 0; col < FONT_W; col++) {
                uint32_t px = (bits & (1u << col)) ? fg : bg;
                for (uint32_t sy = 0; sy < scale; sy++) {
                    uint32_t yy = y + row * scale + sy;
                    if (yy >= g_h) continue;
                    for (uint32_t sx = 0; sx < scale; sx++) {
                        uint32_t xx = x + col * scale + sx;
                        if (xx < g_w) buf[yy * g_w + xx] = px;
                    }
                }
            }
        }
        x += FONT_W * scale;
    }
}

/* Draw a string into a tight w-pitched buffer at pixel (x,y). */
void fb_text_to(uint32_t *buf, uint32_t x, uint32_t y,
                const char *s, uint32_t fg_rgb, uint32_t bg_rgb) {
    fb_text_to_s(buf, x, y, s, fg_rgb, bg_rgb, 1);
}

/* boot console glyph cell: the 8x16 font magnified CON_SCALE times */
#define CON_SCALE 1
#define CON_CW    (FONT_W * CON_SCALE)
#define CON_CH    (FONT_H * CON_SCALE)

static void draw_glyph(char c, uint32_t cx, uint32_t cy) {
    unsigned uc = (unsigned char)c;
    if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
    const unsigned char *g = font8x16[uc - FONT_FIRST];
    uint32_t fg = pack(g_fg), bg = pack(g_bg);
    for (uint32_t row = 0; row < FONT_H; row++) {
        unsigned char bits = g[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t px = (bits & (1u << col)) ? fg : bg;
            for (uint32_t sy = 0; sy < CON_SCALE; sy++)
                for (uint32_t sx = 0; sx < CON_SCALE; sx++)
                    putpx(cx * CON_CW + col * CON_SCALE + sx, cy * CON_CH + row * CON_SCALE + sy, px);
        }
    }
}
static void fb_scroll(void) {
    uint32_t rows = g_h / CON_CH;
    for (uint32_t y = 0; y < (rows - 1) * CON_CH; y++)
        for (uint32_t x = 0; x < g_w; x++)
            g_fb[y * g_pitch + x] = g_fb[(y + CON_CH) * g_pitch + x];
    uint32_t bg = pack(g_bg);
    for (uint32_t y = (rows - 1) * CON_CH; y < g_h; y++)
        for (uint32_t x = 0; x < g_w; x++) g_fb[y * g_pitch + x] = bg;
}
static void fb_putc(char c) {
    uint32_t cols = g_w / CON_CW, rows = g_h / CON_CH;
    if (c == '\n')       { g_cx = 0; g_cy++; }
    else if (c == '\r')  { g_cx = 0; }
    else {
        if (g_cx >= cols) { g_cx = 0; g_cy++; }
        draw_glyph(c, g_cx, g_cy);
        g_cx++;
    }
    if (g_cy >= rows) { fb_scroll(); g_cy = rows - 1; }
}

/* ---- kernel log scrollback -------------------------------------------------
 * Mirror every console char into a line ring buffer the window manager renders
 * in a widget.  Once the desktop owns the screen (klog_wm_active), kputc stops
 * drawing straight to the framebuffer so console text can't corrupt the WM. */
#define KLOG_COLS  128
#define KLOG_LINES 512
static char g_klog[KLOG_LINES][KLOG_COLS];
static int  g_klog_head;                    /* line currently being written        */
static int  g_klog_col;                     /* write column in that line           */
static int  g_klog_fill = 1;                /* valid lines in the ring (>= 1)      */
static int  g_wm_active;                     /* desktop up -> stop the fb console    */

static void klog_newline(void) {
    g_klog_head = (g_klog_head + 1) % KLOG_LINES;
    g_klog_col = 0;
    g_klog[g_klog_head][0] = 0;
    if (g_klog_fill < KLOG_LINES) g_klog_fill++;
}
static void klog_putc(char c) {
    if (c == '\r') return;
    if (c == '\n') { klog_newline(); return; }
    if (g_klog_col >= KLOG_COLS - 1) klog_newline();
    g_klog[g_klog_head][g_klog_col++] = c;
    g_klog[g_klog_head][g_klog_col] = 0;
}
int klog_lines(void) { return g_klog_fill; }
const char *klog_line(int i) {              /* i: 0 = oldest retained line */
    if (i < 0 || i >= g_klog_fill) return "";
    int oldest = (g_klog_head - (g_klog_fill - 1) + 2 * KLOG_LINES) % KLOG_LINES;
    return g_klog[(oldest + i) % KLOG_LINES];
}
void klog_wm_active(int on) { g_wm_active = on; }

/* ---- unified console ---- */
void kputc(char c) {
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
    klog_putc(c);                            /* always feed the scrollback + widget */
    if (g_fb && !g_wm_active) fb_putc(c);     /* direct fb console only pre-desktop   */
}
void kputs(const char *s) { for (; *s; s++) kputc(*s); }
void kput_u64hex(uint64_t v) {
    kputs("0x");
    for (int i = 60; i >= 0; i -= 4) { int d = (v >> i) & 0xF; kputc(d < 10 ? '0' + d : 'A' + d - 10); }
}
void kput_u32dec(uint32_t v) {
    char b[10]; int n = 0;
    if (!v) { kputc('0'); return; }
    while (v) { b[n++] = '0' + (v % 10); v /= 10; }
    while (n--) kputc(b[n]);
}

/* ---- fatal-fault panic screen (kernel/arch/arch64.c:isr_c) -----------------
 * Draw the exception straight to the framebuffer, bypassing BOTH the console
 * cursor and the g_wm_active gate, so a fatal kernel fault is visible on real
 * hardware even after the desktop has taken the screen (where kputc goes only to
 * serial + the invisible scrollback — a headless laptop would otherwise just
 * freeze with no diagnostics).  Direct pixel writes only: no locks, no
 * allocation, no scrolling, so it is safe to call from an exception context. */
#define PANIC_SCALE 2
#define PANIC_ADV   (FONT_W * PANIC_SCALE)
static void panic_glyph(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    unsigned uc = (unsigned char)c;
    if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
    const unsigned char *g = font8x16[uc - FONT_FIRST];
    for (uint32_t row = 0; row < FONT_H; row++) {
        unsigned char bits = g[row];
        for (uint32_t col = 0; col < FONT_W; col++) {
            uint32_t px = (bits & (1u << col)) ? fg : bg;
            for (uint32_t sy = 0; sy < PANIC_SCALE; sy++)
                for (uint32_t sx = 0; sx < PANIC_SCALE; sx++)
                    putpx(x + col * PANIC_SCALE + sx, y + row * PANIC_SCALE + sy, px);
        }
    }
}
static uint32_t panic_str(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg) {
    for (; *s; s++) { panic_glyph(x, y, *s, fg, bg); x += PANIC_ADV; }
    return x;
}
static uint32_t panic_hex(uint32_t x, uint32_t y, uint64_t v, uint32_t fg, uint32_t bg) {
    x = panic_str(x, y, "0x", fg, bg);
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((v >> i) & 0xF);
        panic_glyph(x, y, (char)(d < 10 ? '0' + d : 'A' + d - 10), fg, bg);
        x += PANIC_ADV;
    }
    return x;
}
static uint32_t panic_soff(uint32_t x, uint32_t y, int64_t v, uint32_t fg, uint32_t bg) {
    panic_glyph(x, y, v < 0 ? '-' : '+', fg, bg); x += PANIC_ADV;
    return panic_hex(x, y, (uint64_t)(v < 0 ? -v : v), fg, bg);
}
static const char *panic_vec_name(uint64_t v) {
    switch (v) {
        case 0:  return "#DE divide error";
        case 1:  return "#DB debug";
        case 2:  return "NMI";
        case 3:  return "#BP breakpoint";
        case 6:  return "#UD invalid opcode";
        case 7:  return "#NM device-not-available";
        case 8:  return "#DF double fault";
        case 10: return "#TS invalid TSS";
        case 11: return "#NP segment not present";
        case 12: return "#SS stack-segment fault";
        case 13: return "#GP general protection";
        case 14: return "#PF page fault";
        case 16: return "#MF x87 fp error";
        case 17: return "#AC alignment check";
        case 18: return "#MC machine check";
        case 19: return "#XM simd fp";
        default: return "exception";
    }
}
void fb_panic(uint64_t vec, uint64_t err, uint64_t rip, uint64_t cr2, uint64_t cs,
              int64_t rip_off, const int64_t *bt, int nbt, int64_t mapoff) {
    if (!g_fb) return;
    uint32_t bg = pack(0x300000), fg = pack(0xFFFFFF), hd = pack(0xFFE060);
    uint32_t band = g_h < 400 ? g_h : 400;
    for (uint32_t y = 0; y < band; y++)
        for (uint32_t x = 0; x < g_w; x++) g_fb[y * g_pitch + x] = bg;
    uint32_t lh = PANIC_ADV + 6, x0 = 16, y = 12;
    panic_str(x0, y, "*** TINYOS KERNEL PANIC ***", hd, bg); y += lh;
    uint32_t x = panic_str(x0, y, "vector ", fg, bg);
    x = panic_hex(x, y, vec, fg, bg);
    panic_str(x + PANIC_ADV, y, panic_vec_name(vec), fg, bg); y += lh;
    x = panic_str(x0, y, "err ", fg, bg); x = panic_hex(x, y, err, fg, bg);
    x = panic_str(x, y, "  cs ", fg, bg); panic_hex(x, y, cs, fg, bg); y += lh;
    x = panic_str(x0, y, "rip ", fg, bg); panic_hex(x, y, rip, fg, bg); y += lh;
    x = panic_str(x0, y, "cr2 ", fg, bg); panic_hex(x, y, cr2, fg, bg); y += lh;
    x = panic_str(x0, y, "rip@isr_c ", hd, bg); panic_soff(x, y, rip_off, hd, bg); y += lh;
    x = panic_str(x0, y, "mapoff ", fg, bg); panic_soff(x, y, mapoff, fg, bg); y += lh;
    x = panic_str(x0, y, "bt ", fg, bg);
    for (int i = 0; i < nbt; i++) { x = panic_soff(x, y, bt[i], fg, bg); x += PANIC_ADV; }
    y += lh;
    panic_str(x0, y, "system halted - power-cycle to reboot", hd, bg);
    fb_flush();          /* sfence + I/O: guarantee the panel shows it under WC */
}


