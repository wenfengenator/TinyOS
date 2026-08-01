/* tinyos x64 kernel console: COM1 serial + GOP linear framebuffer.
 *
 * Runs after ExitBootServices, so it touches only hardware the firmware left in
 * place: the 16550 UART (port I/O) and the linear framebuffer (MMIO).  Text is
 * mirrored to both, so the boot can be verified headlessly over serial while a
 * VM/monitor shows the same output on screen. */
#include "boot64.h"
#include "font8x8.h"

/* Freestanding memcpy/memset so the compiler always has a real symbol to call
 * (built as -fno-builtin so these loops are not turned back into self-calls). */
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d;
}

/* ---- COM1 serial ---- */
#define COM1 0x3F8
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
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
static uint32_t g_cx, g_cy;             /* text cursor (glyph cells)             */
static uint32_t g_fg = 0xC0C0C0;        /* light grey on dark blue               */
static uint32_t g_bg = 0x001020;

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
    g_cx = g_cy = 0;
    fb_fill(g_bg);
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
void fb_present(const uint32_t *back) {
    if (!g_fb) return;
    for (uint32_t y = 0; y < g_h; y++)
        memcpy((void *)(g_fb + y * g_pitch), back + y * g_w, (unsigned long)g_w * 4);
    __asm__ volatile("sfence" ::: "memory");   /* flush write-combining buffers to VRAM */
}

/* Present only the scanlines that differ from `shadow` (updated in place), so a
 * mostly-static screen skips the slow framebuffer / Hyper-V synthvid writes for
 * unchanged rows.  `back` and `shadow` are tight w*h buffers. */
void fb_present_diff(const uint32_t *back, uint32_t *shadow, uint32_t w, uint32_t h) {
    if (!g_fb) return;
    for (uint32_t y = 0; y < h; y++) {
        const uint32_t *b = back + (uint64_t)y * w;
        uint32_t *s = shadow + (uint64_t)y * w;
        uint32_t x = 0;
        while (x < w && b[x] == s[x]) x++;
        if (x == w) continue;                         /* row unchanged: skip FB write */
        memcpy((void *)(g_fb + (uint64_t)y * g_pitch), b, (unsigned long)w * 4);
        memcpy(s, b, (unsigned long)w * 4);
    }
    __asm__ volatile("sfence" ::: "memory");   /* flush write-combining buffers to VRAM */
}

/* Draw a string into a tight w-pitched buffer at pixel (x,y), each 8x8 glyph
 * magnified by an integer `scale` (nearest-neighbour, so it stays crisp). */
void fb_text_to_s(uint32_t *buf, uint32_t x, uint32_t y,
                  const char *s, uint32_t fg_rgb, uint32_t bg_rgb, uint32_t scale) {
    if (scale < 1) scale = 1;
    uint32_t fg = pack(fg_rgb), bg = pack(bg_rgb);
    for (; *s; s++) {
        unsigned uc = (unsigned char)*s;
        if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
        const unsigned char *g = font8x8[uc - FONT_FIRST];
        for (uint32_t row = 0; row < 8; row++) {
            unsigned char bits = g[row];
            for (uint32_t col = 0; col < 8; col++) {
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
        x += 8 * scale;
    }
}

/* Draw a string into a tight w-pitched buffer at pixel (x,y). */
void fb_text_to(uint32_t *buf, uint32_t x, uint32_t y,
                const char *s, uint32_t fg_rgb, uint32_t bg_rgb) {
    fb_text_to_s(buf, x, y, s, fg_rgb, bg_rgb, 1);
}

/* boot console glyph cell: the 8x8 font magnified CON_SCALE times */
#define CON_SCALE 2
#define CON_CELL  (8 * CON_SCALE)

static void draw_glyph(char c, uint32_t cx, uint32_t cy) {
    unsigned uc = (unsigned char)c;
    if (uc < FONT_FIRST || uc > FONT_LAST) uc = '?';
    const unsigned char *g = font8x8[uc - FONT_FIRST];
    uint32_t fg = pack(g_fg), bg = pack(g_bg);
    for (uint32_t row = 0; row < 8; row++) {
        unsigned char bits = g[row];
        for (uint32_t col = 0; col < 8; col++) {
            uint32_t px = (bits & (1u << col)) ? fg : bg;
            for (uint32_t sy = 0; sy < CON_SCALE; sy++)
                for (uint32_t sx = 0; sx < CON_SCALE; sx++)
                    putpx(cx * CON_CELL + col * CON_SCALE + sx, cy * CON_CELL + row * CON_SCALE + sy, px);
        }
    }
}
static void fb_scroll(void) {
    uint32_t rows = g_h / CON_CELL;
    for (uint32_t y = 0; y < (rows - 1) * CON_CELL; y++)
        for (uint32_t x = 0; x < g_w; x++)
            g_fb[y * g_pitch + x] = g_fb[(y + CON_CELL) * g_pitch + x];
    uint32_t bg = pack(g_bg);
    for (uint32_t y = (rows - 1) * CON_CELL; y < g_h; y++)
        for (uint32_t x = 0; x < g_w; x++) g_fb[y * g_pitch + x] = bg;
}
static void fb_putc(char c) {
    uint32_t cols = g_w / CON_CELL, rows = g_h / CON_CELL;
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
