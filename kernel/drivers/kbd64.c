/* tinyos x64 polled PS/2 keyboard (i8042).
 *
 * No IDT/IRQ yet, so we poll: when the controller's output buffer is full
 * (status port 0x64 bit0), read a scan code from 0x60 and translate US set-1
 * codes to ASCII, with 0xE0-prefixed arrow keys mapped to KEY_* codes. */
#include "boot64.h"

static inline uint8_t inb(uint16_t p) { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void    outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }

/* COM1 receive: lets the serial console drive login/shell when headless.  Only
 * usable when a real 16550 is present -- on a laptop with no serial port (the
 * T440s) the 0x3F8 range is unclaimed and floats high, so inb() returns 0xFF and
 * LSR bit0 (data-ready) reads as 1 forever.  Left unguarded, serial_getc() then
 * streams phantom 0xFF "keys" that trap the cooperative input loops (login, WM,
 * DEMO, PONG) in their `while (getkey>=0)` drain and they never render -- the
 * "frozen until a real keypress" bug.  Probe the 16550 scratch register once and
 * disable serial input if it is absent. */
#define COM1 0x3F8
static int s_serial = -1;                          /* -1 unprobed, 0 absent, 1 present */
static int serial_present(void) {
    if (s_serial < 0) {
        outb(COM1 + 7, 0xAE); uint8_t a = inb(COM1 + 7);   /* 16550 scratch pad loopback */
        outb(COM1 + 7, 0x51); uint8_t b = inb(COM1 + 7);
        s_serial = (a == 0xAE && b == 0x51) ? 1 : 0;
    }
    return s_serial;
}
static int serial_getc(void) {
    if (!serial_present()) return -1;
    uint8_t lsr = inb(COM1 + 5);
    if (lsr == 0xFF || !(lsr & 0x01)) return -1;   /* open bus / no data ready */
    int c = inb(COM1);
    if (c == '\r') c = '\n';                    /* CR -> newline */
    if (c == 0x7F) c = '\b';                    /* DEL -> backspace */
    return c;
}

/* US scancode set 1 -> ASCII (unshifted / shifted), 0 = ignore */
static const char sc_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
   'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
   'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
   'b','n','m',',','.','/', 0, '*', 0, ' ', 0,
};
static const char sc_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
   'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
   'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
   'B','N','M','<','>','?', 0, '*', 0, ' ', 0,
};

static int s_shift, s_caps, s_ext, s_ctrl;

void kbd64_init(void) {
    s_shift = s_caps = s_ext = s_ctrl = 0;
    /* disable the PS/2 aux port so touchpad/TrackPoint packets can't arrive on
     * 0x60 and get mis-decoded as scancodes (spurious keys) */
    for (int g = 0; g < 100000 && (inb(0x64) & 0x02); g++) { }
    outb(0x64, 0xA7);
    /* drain any bytes the firmware left in the output buffer */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
}

int kbd64_getkey(void) {
    uint8_t st = inb(0x64);
    if (!(st & 0x01)) return serial_getc();  /* PS/2 empty -> try the serial console */
    if (st & 0x20) { (void)inb(0x60); return -1; }  /* aux (touchpad/mouse) byte -> ignore */
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { s_ext = 1; return -1; }  /* extended-key prefix */
    if (s_ext) {
        s_ext = 0;
        if (sc & 0x80) return -1;              /* extended release */
        switch (sc) {
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            default:   return -1;
        }
    }
    switch (sc) {                              /* modifiers */
        case 0x2A: case 0x36: s_shift = 1; return -1;
        case 0xAA: case 0xB6: s_shift = 0; return -1;
        case 0x1D:            s_ctrl  = 1; return -1;
        case 0x9D:            s_ctrl  = 0; return -1;
        case 0x3A:            s_caps ^= 1; return -1;
    }
    if (sc & 0x80) return -1;                  /* key release */

    switch (sc) {                              /* function keys (WM controls) */
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
    }

    char c = s_shift ? sc_shift[sc & 0x7F] : sc_ascii[sc & 0x7F];
    if (!c) return -1;
    if (s_ctrl) {                              /* Ctrl+letter -> control code (^C=3, ^D=4) */
        char l = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        return (l >= 'a' && l <= 'z') ? l - 'a' + 1 : -1;
    }
    if (s_caps) {
        if      (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return (unsigned char)c;
}
