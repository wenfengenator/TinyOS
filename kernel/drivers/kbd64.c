/* tinyos x64 polled PS/2 keyboard (i8042).
 *
 * No IDT/IRQ yet, so we poll: when the controller's output buffer is full
 * (status port 0x64 bit0), read a scan code from 0x60 and translate US set-1
 * codes to ASCII, with 0xE0-prefixed arrow keys mapped to KEY_* codes. */
#include "boot64.h"

static inline uint8_t inb(uint16_t p) { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }

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
    /* drain any bytes the firmware left in the output buffer */
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
}

int kbd64_getkey(void) {
    if (!(inb(0x64) & 0x01)) return -1;        /* output buffer empty */
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
