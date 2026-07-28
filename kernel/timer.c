/* tinyos programmable interval timer (PIT) + 8259 PIC — the hardware source of
 * preemption.  On real hardware / QEMU the PIT raises IRQ0 ~100x/second; the
 * asm stub irq0_stub (entry.s) runs on_timer() and could invoke the scheduler
 * to preempt the running task.
 *
 * The Unicorn emulator does not deliver IRQs, so under emulation these `out`
 * instructions execute harmlessly and preemption is instead driven by
 * tools/emulate.py's time-slice scheduler.  The mechanism is documented in
 * docs/DESIGN.md. */
#include "kernel.h"

volatile uint32_t g_ticks;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}

/* remap the 8259 PICs so IRQ0..15 land on vectors 0x20..0x2F */
void pic_remap(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);   /* start init (ICW1) */
    outb(0x21, 0x20); outb(0xA1, 0x28);   /* ICW2: vector offsets */
    outb(0x21, 0x04); outb(0xA1, 0x02);   /* ICW3: cascade */
    outb(0x21, 0x01); outb(0xA1, 0x01);   /* ICW4: 8086 mode */
    outb(0x21, 0xFC); outb(0xA1, 0xFF);   /* unmask IRQ0 (timer) + IRQ1 (keyboard) */
}

/* program PIT channel 0 for `hz` interrupts per second */
void pit_init(uint32_t hz) {
    uint32_t div = 1193182u / hz;
    outb(0x43, 0x36);                     /* channel 0, lobyte/hibyte, mode 3 */
    outb(0x40, (uint8_t)(div & 0xFF));
    outb(0x40, (uint8_t)((div >> 8) & 0xFF));
}

/* called from irq0_stub on every timer tick */
void on_timer(void) {
    g_ticks++;
    /* Hardware preemption point: a full kernel would here save the current
     * task, run schedule(), and switch stacks.  See docs/DESIGN.md for how the
     * emulator performs the equivalent context switch during the demo. */
}
