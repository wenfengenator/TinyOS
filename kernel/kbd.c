/* tinyos PS/2 keyboard driver (IRQ1).
 *
 * On real hardware the keyboard controller raises IRQ1; irq1_stub (entry.s)
 * calls on_keyboard(), which reads the scancode from port 0x60, translates it,
 * and pushes an ASCII byte into the g_kbd ring buffer.  sys_read(STDIN) drains
 * that buffer.  Under the Unicorn emulator (no IRQ1) tools/emulate.py pushes the
 * user's scripted keystrokes into the very same ring buffer, so the stdin path
 * is identical. */
#include "kernel.h"

#define KBD_BUF 256
volatile uint8_t  g_kbd[KBD_BUF];
volatile uint32_t g_kbd_head;
volatile uint32_t g_kbd_tail;

static inline uint8_t inb(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

/* US scancode set 1 -> ASCII (unshifted), 0 = ignore */
static const char sc_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
   'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0, 'a','s',
   'd','f','g','h','j','k','l',';','\'','`', 0,'\\','z','x','c','v',
   'b','n','m',',','.','/', 0, '*', 0, ' ', 0,
};
/* US scancode set 1 -> ASCII (shifted), 0 = ignore */
static const char sc_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
   'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0, 'A','S',
   'D','F','G','H','J','K','L',':','"','~', 0, '|','Z','X','C','V',
   'B','N','M','<','>','?', 0, '*', 0, ' ', 0,
};

void kbd_push(uint8_t c) {
    uint32_t n = (g_kbd_head + 1) % KBD_BUF;
    if (n != g_kbd_tail) { g_kbd[g_kbd_head] = c; g_kbd_head = n; }
}
int kbd_pop(void) {
    if (g_kbd_head == g_kbd_tail) return -1;
    uint8_t c = g_kbd[g_kbd_tail];
    g_kbd_tail = (g_kbd_tail + 1) % KBD_BUF;
    return (int)c;
}

/* called from irq1_stub on every keypress (hardware path) */
void on_keyboard(void) {
    static int shift = 0, caps = 0, ext = 0;
    uint8_t sc = inb(0x60);

    if (sc == 0xE0) { ext = 1; return; }       /* extended-key (arrows, del) prefix */
    if (ext) {
        ext = 0;
        if (sc & 0x80) return;                 /* extended key release */
        uint8_t k = 0;
        switch (sc) {
            case 0x48: k = KEY_UP;    break;
            case 0x50: k = KEY_DOWN;  break;
            case 0x4B: k = KEY_LEFT;  break;
            case 0x4D: k = KEY_RIGHT; break;
            case 0x53: k = KEY_DEL;   break;   /* Delete (forward) */
            case 0x47: k = KEY_HOME;  break;
            case 0x4F: k = KEY_END;   break;
            case 0x49: k = KEY_PGUP;  break;
            case 0x51: k = KEY_PGDN;  break;
            default: return;
        }
        kbd_push(k);
        return;
    }

    switch (sc) {                              /* modifier make/break codes */
        case 0x2A: case 0x36: shift = 1; return;   /* L/R shift pressed  */
        case 0xAA: case 0xB6: shift = 0; return;   /* L/R shift released */
        case 0x3A:            caps ^= 1; return;   /* caps lock toggles  */
    }
    if (sc & 0x80) return;                     /* any other key release: ignore */

    char c = shift ? sc_shift[(int)sc] : sc_ascii[(int)sc];
    if (!c) return;
    if (caps) {                                /* caps lock inverts letter case */
        if      (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    kbd_push((uint8_t)c);
}
