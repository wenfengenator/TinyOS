/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos kernel core: freestanding libc, console, descriptor tables, boot. */
#include "kernel.h"
#include "mm.h"

/* ================= freestanding libc ================= */
void *memset(void *d, int c, unsigned n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
void *memcpy(void *d, const void *s, unsigned n) {
    unsigned char *a = d; const unsigned char *b = s; while (n--) *a++ = *b++; return d;
}
unsigned kstrlen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
int kstrncmp(const char *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) { if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i]; if (!a[i]) break; }
    return 0;
}
void kstrcpy(char *d, const char *s) { while ((*d++ = *s++)) {} }

/* ================= VGA text console + serial mirror ================= */
static volatile uint16_t *const VGA = (volatile uint16_t *)VGA_MEM;
static volatile uint32_t *const CUR = (volatile uint32_t *)CURSOR_ADDR;

static inline void outb8(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t inb8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }

#define COM1 0x3F8
void serial_init(void) {
    outb8(COM1+1, 0x00); outb8(COM1+3, 0x80); outb8(COM1+0, 0x03); outb8(COM1+1, 0x00);
    outb8(COM1+3, 0x03); outb8(COM1+2, 0xC7); outb8(COM1+4, 0x0B);
}
static void serial_putc(char c) {
    for (int i = 0; i < 100000 && !(inb8(COM1+5) & 0x20); i++) {}   /* bounded wait for THR-empty */
    outb8(COM1, (uint8_t)c);
}
int serial_rx(void) { return (inb8(COM1+5) & 1) ? (int)inb8(COM1) : -1; }

static void console_scroll(void) {
    for (int i = 0; i < 1920; i++) VGA[i] = VGA[i + 80];
    for (int i = 1920; i < 2000; i++) VGA[i] = ((uint16_t)KATTR << 8) | ' ';
}
static void move_cursor(uint32_t pos) {           /* VGA CRTC hardware cursor */
    outb8(0x3D4, 0x0F); outb8(0x3D5, (uint8_t)(pos & 0xFF));
    outb8(0x3D4, 0x0E); outb8(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}
static void putc_attr(char c, uint8_t attr) {
    if (c == '\n') { serial_putc('\r'); serial_putc('\n'); } else serial_putc(c);
    uint32_t idx = *CUR;
    if      (c == '\n') idx = (idx / 80 + 1) * 80;
    else if (c == '\b') { if (idx) idx--; }
    else              { VGA[idx] = ((uint16_t)attr << 8) | (uint8_t)c; idx++; }
    if (idx >= 2000)  { console_scroll(); idx = 1920; }
    *CUR = idx;
    move_cursor(idx);
}
void console_clear(void) {
    for (int i = 0; i < 2000; i++) VGA[i] = ((uint16_t)KATTR << 8) | ' ';
    *CUR = 0;
    move_cursor(0);
}
void console_gotoxy(int row, int col) {
    if (row < 0) row = 0;
    if (row > 24) row = 24;
    if (col < 0) col = 0;
    if (col > 79) col = 79;
    uint32_t idx = (uint32_t)row * 80 + col;
    *CUR = idx; move_cursor(idx);
}
int  console_getpos(void)     { return (int)*CUR; }
void console_setpos(int idx)  { if (idx < 0) idx = 0; if (idx > 1999) idx = 1999; *CUR = (uint32_t)idx; move_cursor((uint32_t)idx); }
int getchar_raw(void) {                         /* blocking single key, no echo */
    for (;;) {
        int c = kbd_pop(); if (c < 0) c = serial_rx();
        if (c >= 0) return c;
        __asm__ volatile("sti; hlt; cli");
    }
}
void kprint(const char *s, uint8_t attr)           { while (*s) putc_attr(*s++, attr); }
void kprint_n(const char *s, uint32_t n, uint8_t a) { while (n--) putc_attr(*s++, a); }

/* ================= GDT / IDT / TSS ================= */
struct gdt_entry { uint16_t lim_lo, base_lo; uint8_t base_mid, access, gran, base_hi; } __attribute__((packed));
struct idt_entry { uint16_t off_lo, sel; uint8_t zero, type; uint16_t off_hi; }        __attribute__((packed));
struct dt_ptr    { uint16_t limit; uint32_t base; }                                    __attribute__((packed));
struct tss_t {
    uint32_t link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct idt_entry idt[256];
static struct tss_t     tss;
static struct dt_ptr    gdtp, idtp;

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[i].base_lo = base & 0xFFFF; gdt[i].base_mid = (base >> 16) & 0xFF; gdt[i].base_hi = (base >> 24) & 0xFF;
    gdt[i].lim_lo  = limit & 0xFFFF; gdt[i].gran = ((limit >> 16) & 0x0F) | (gran & 0xF0); gdt[i].access = access;
}
static void setup_gdt(void) {
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xC0);   /* 0x08 ring0 code */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xC0);   /* 0x10 ring0 data */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xC0);   /* 0x18 ring3 code */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xC0);   /* 0x20 ring3 data */
    gdt_set(5, (uint32_t)&tss, sizeof(tss) - 1, 0x89, 0x00); /* 0x28 TSS */
    gdtp.limit = sizeof(gdt) - 1; gdtp.base = (uint32_t)&gdt;
    __asm__ volatile(
        "lgdt %0\n" "ljmp $0x08, $1f\n" "1:\n"
        "movw $0x10, %%ax\n movw %%ax,%%ds\n movw %%ax,%%es\n movw %%ax,%%fs\n movw %%ax,%%gs\n movw %%ax,%%ss\n"
        :: "m"(gdtp) : "eax", "memory");
}
static void idt_set(int n, uint32_t h, uint16_t sel, uint8_t type) {
    idt[n].off_lo = h & 0xFFFF; idt[n].off_hi = (h >> 16) & 0xFFFF; idt[n].sel = sel; idt[n].zero = 0; idt[n].type = type;
}
static void setup_idt(void) {
    memset(idt, 0, sizeof(idt));
    idt_set(0x20, (uint32_t)irq0_stub,   KCODE_SEL, 0x8E);   /* IRQ0 timer, DPL0 */
    idt_set(0x21, (uint32_t)irq1_stub,   KCODE_SEL, 0x8E);   /* IRQ1 keyboard    */
    idt_set(0x80, (uint32_t)syscall_stub, KCODE_SEL, 0xEE);  /* syscall, DPL3    */
    idtp.limit = sizeof(idt) - 1; idtp.base = (uint32_t)&idt;
    __asm__ volatile("lidt %0" :: "m"(idtp));
}
static void setup_tss(void) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0 = KDATA_SEL; tss.esp0 = KSTACK_TOP; tss.iomap_base = sizeof(tss);
    __asm__ volatile("ltr %0" :: "r"((uint16_t)TSS_SEL));
}

/* enter ring 3 at `entry` with stack `ustack` (never returns) */
static void enter_user(uint32_t entry, uint32_t ustack) {
    __asm__ volatile(
        "movw $0x23, %%ax\n movw %%ax,%%ds\n movw %%ax,%%es\n movw %%ax,%%fs\n movw %%ax,%%gs\n"
        "pushl $0x23\n"     /* SS  */ "pushl %0\n"      /* ESP */
        "pushl $0x202\n"    /* EFLAGS (IF set for preemption) */ "pushl $0x1B\n" /* CS */ "pushl %1\n" /* EIP */
        "iret\n" :: "r"(ustack), "r"(entry) : "eax", "memory");
}

/* ================= boot ================= */
extern void login_main(void);    /* user/login.cpp  */
extern void worker_main(void);   /* user/worker.cpp */

void kmain(void) {
    serial_init();
    console_clear();
    kprint("[kernel] tinyos online: 32-bit protected mode, ring 0 (C)\n", KATTR);
    setup_gdt(); setup_idt(); setup_tss();
    kprint("[kernel] GDT + IDT + TSS ready; syscall gate int 0x80 installed\n", KATTR);
    pic_remap(); pit_init(100);
    kprint("[kernel] PIC remapped; PIT armed at 100 Hz (preemption timer)\n", KATTR);
    mm_init(); paging_enable();
    kprint("[kernel] paging enabled; low 16 MB identity-mapped, frame pool ready\n", KATTR);
    fs_init();
    kprint("[kernel] RAM filesystem mounted (/etc/passwd, /README, /tmp/log)\n", KATTR);
    if (fat_mount() == 0) kprint("[kernel] FAT16 disk mounted at /mnt\n", KATTR);

    /* task 0 = login (ready); tasks 1,2 = workers (blocked until login OK) */
    task_create(0, (uint32_t)&login_main,  0, "login",   T_READY);
    task_create(1, (uint32_t)&worker_main, 1, "workerA", T_BLOCKED);
    task_create(2, (uint32_t)&worker_main, 2, "workerB", T_BLOCKED);
    g_ntask = 3; g_current = 0;
    kprint("[kernel] scheduler started; launching login task in ring 3...\n", KATTR);

    enter_user(g_tasks[0].eip, g_tasks[0].ustack);   /* run task 0 */
}
