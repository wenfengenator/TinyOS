/* tinyos x64 preemptive scheduler (Cycle 4).
 *
 * A round-robin scheduler for KERNEL threads, driven by the Local APIC timer.
 * Each thread has its own 16 KiB kernel stack; a periodic timer interrupt
 * (kernel/trap64.s:timer_isr) saves the running thread's full register frame,
 * calls sched_tick() to pick the next runnable thread, and iretq's into it.
 *
 * The whole thing is opt-in and self-contained: sched_demo() (the `sched`
 * console command) sets it up, runs several worker threads preemptively while
 * the caller renders their progress, then stops the timer and returns.  Nothing
 * else in the kernel runs with interrupts on, so the rest of the system keeps
 * its simple polled model until a thread scheduler is wired in more deeply. */
#include "boot64.h"
#include "arch64.h"

/* ---- assembly glue (kernel/trap64.s) ---- */
uint64_t timer_isr_addr(void);          /* &timer_isr (PC-relative, -mi386pep safe) */
uint64_t spurious_isr_addr(void);       /* &spurious_isr                            */
uint64_t thread_trampoline_addr(void);  /* &thread_trampoline                       */

/* ---- Local APIC (memory-mapped; same base every CPU sees its own) ---- */
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_TIMER_ICR 0x380           /* initial count  */
#define LAPIC_TIMER_CCR 0x390           /* current count  */
#define LAPIC_TIMER_DCR 0x3E0           /* divide config  */
#define LVT_MASKED      0x10000
#define LVT_PERIODIC    0x20000
#define TIMER_VEC       0x20
#define SPURIOUS_VEC    0xFF

static volatile uint8_t *g_lapic;
static inline void     lw(uint32_t r, uint32_t v) { *(volatile uint32_t *)(g_lapic + r) = v; }
static inline uint32_t lr(uint32_t r)             { return *(volatile uint32_t *)(g_lapic + r); }
static inline void outb(uint16_t p, uint8_t v)    { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }

/* ---- tasks ---- */
#define MAXTASK 8
#define KSTACK  16384
enum { T_FREE = 0, T_READY, T_RUNNING, T_DONE };
typedef struct { uint64_t rsp; uint8_t *stack; int state; } task_t;

static task_t            g_tasks[MAXTASK];
static int               g_ntask;
static int               g_cur;
static volatile uint64_t g_switches;

/* Called from timer_isr (asm) with the interrupted thread's saved rsp.  Saves it,
 * round-robins to the next runnable thread, and returns that thread's rsp. */
uint64_t sched_tick(uint64_t cur_rsp) {
    lw(LAPIC_EOI, 0);                                   /* acknowledge the IRQ */
    g_switches++;
    g_tasks[g_cur].rsp = cur_rsp;
    if (g_tasks[g_cur].state == T_RUNNING) g_tasks[g_cur].state = T_READY;
    int n = g_cur;
    for (int i = 0; i < MAXTASK; i++) {
        n = (n + 1) % MAXTASK;
        if (g_tasks[n].state == T_READY) break;
    }
    g_cur = n;
    g_tasks[g_cur].state = T_RUNNING;
    return g_tasks[g_cur].rsp;
}

/* Called from thread_trampoline (asm) if a thread function ever returns. */
void sched_exit(void) {
    g_tasks[g_cur].state = T_DONE;
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");                  /* preempted away, never picked again */
}

/* Create a ready kernel thread that runs fn(arg) on its own stack. */
int sched_spawn(void (*fn)(uint64_t), uint64_t arg) {
    if (g_ntask >= MAXTASK) return -1;
    uint8_t *st = (uint8_t *)kmalloc(KSTACK);
    if (!st) return -1;
    int id = g_ntask++;
    g_tasks[id].stack = st;

    /* Build the frame timer_isr expects to pop: [r15..rax] then the CPU interrupt
     * frame [RIP][CS][RFLAGS][RSP][SS] (long-mode iretq ALWAYS pops all five, even
     * for a same-privilege return).  rdi slot = fn, rsi slot = arg so
     * thread_trampoline calls fn(arg). */
    uint64_t top = ((uint64_t)st + KSTACK) & ~15ULL;
    uint64_t *sp = (uint64_t *)top;
    *(--sp) = 0x10;                        /* SS: kernel data             */
    *(--sp) = top;                         /* RSP: thread's stack top     */
    *(--sp) = 0x202;                       /* RFLAGS: IF=1                 */
    *(--sp) = 0x08;                        /* CS: kernel code             */
    *(--sp) = thread_trampoline_addr();    /* RIP                         */
    *(--sp) = 0;                           /* rax                         */
    *(--sp) = 0;                           /* rbx                         */
    *(--sp) = 0;                           /* rcx                         */
    *(--sp) = 0;                           /* rdx                         */
    *(--sp) = arg;                         /* rsi = arg                   */
    *(--sp) = (uint64_t)fn;                /* rdi = fn                    */
    *(--sp) = 0;                           /* rbp                         */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* r8  r9  r10                 */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* r11 r12 r13                 */
    *(--sp) = 0; *(--sp) = 0;              /* r14 r15                     */
    g_tasks[id].rsp = (uint64_t)sp;
    g_tasks[id].state = T_READY;
    return id;
}

/* Calibrate the LAPIC timer against millis() and start it firing at ~100 Hz. */
static void lapic_timer_start(void) {
    lw(LAPIC_TIMER_DCR, 0x3);                           /* divide by 16 */
    lw(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VEC);        /* masked, one-shot for calibration */
    lw(LAPIC_TIMER_ICR, 0xFFFFFFFFu);
    uint64_t t0 = millis();
    while (millis() - t0 < 50) __asm__ volatile("pause");
    uint32_t ticks_50ms = 0xFFFFFFFFu - lr(LAPIC_TIMER_CCR);
    lw(LAPIC_TIMER_ICR, 0);                             /* stop */
    uint32_t per_tick = ticks_50ms / 5;                 /* 10 ms => ~100 Hz */
    if (per_tick < 1000) per_tick = 1000;               /* guard against a stalled clock */
    lw(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VEC);      /* periodic, unmasked */
    lw(LAPIC_TIMER_ICR, per_tick);
}

void sched_init(void) {
    uint64_t base = smp_lapic_base();
    g_lapic = (volatile uint8_t *)(uintptr_t)(base ? base : 0xFEE00000ull);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);                 /* mask legacy PIC -> only LAPIC fires */
    lw(LAPIC_SVR, 0x100 | SPURIOUS_VEC);                /* software-enable LAPIC + spurious vec */
    arch_set_irq_gate(TIMER_VEC,    timer_isr_addr());
    arch_set_irq_gate(SPURIOUS_VEC, spurious_isr_addr());
    for (int i = 0; i < MAXTASK; i++) g_tasks[i].state = T_FREE;
    g_ntask = 1; g_cur = 0; g_switches = 0;
    g_tasks[0].state = T_RUNNING; g_tasks[0].stack = 0; /* task 0 = the caller's context */
}

void sched_start(void) { vmbus_synic_set_masked(1); lapic_timer_start(); __asm__ volatile("sti"); }
void sched_stop(void)  {
    __asm__ volatile("cli");
    lw(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VEC);
    lw(LAPIC_TIMER_ICR, 0);
    vmbus_synic_set_masked(0);                     /* restore the VMBus message interrupt */
}

/* ---- the `sched` console demo ---- */
static volatile uint64_t g_counter[MAXTASK];

static void worker(uint64_t id) {
    for (;;) {
        g_counter[id]++;
        for (volatile int i = 0; i < 1500; i++) __asm__ volatile("pause");  /* pace it */
    }
}

static void put_u64(char *b, uint64_t v) {
    int n = 0; char t[24];
    if (!v) { b[0] = '0'; b[1] = 0; return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    int k = 0; while (n) b[k++] = t[--n]; b[k] = 0;
}

void sched_demo(void) {
    uint32_t w, h; fb_get(&w, &h);
    if (!w || !h) return;
    uint32_t *back = disp_back();
    if (!back) return;

    const int NW = 4;                                   /* worker threads */
    for (int i = 0; i < MAXTASK; i++) g_counter[i] = 0;
    sched_init();
    for (int i = 1; i <= NW; i++) sched_spawn(worker, (uint64_t)i);
    sched_start();

    uint32_t bg  = fb_pack(0x0C1420);
    uint32_t barc = fb_pack(0x2277FF);
    int running = 1;
    while (running) {
        for (uint32_t i = 0; i < w * h; i++) back[i] = bg;
        fb_text_to(back, 16, 14, "TINYOS PREEMPTIVE SCHEDULER   LAPIC TIMER, ROUND-ROBIN", 0x66FFCC, 0x0C1420);
        fb_text_to(back, 16, 26, "4 KERNEL THREADS TIME-SLICED CONCURRENTLY.   Q / ESC QUITS.", 0xAACCFF, 0x0C1420);

        char num[24];
        put_u64(num, g_switches);
        fb_text_to(back, 16, 44, "CONTEXT SWITCHES:", 0xFFAA44, 0x0C1420);
        fb_text_to(back, 168, 44, num, 0xFFFFFF, 0x0C1420);

        for (int i = 1; i <= NW; i++) {
            uint32_t y = 74 + (uint32_t)(i - 1) * 40;
            char lbl[16]; lbl[0] = 'T'; lbl[1] = 'H'; lbl[2] = 'R'; lbl[3] = 'E';
            lbl[4] = 'A'; lbl[5] = 'D'; lbl[6] = ' '; lbl[7] = (char)('0' + i); lbl[8] = 0;
            fb_text_to(back, 16, y, lbl, 0xFFFF66, 0x0C1420);
            put_u64(num, g_counter[i]);
            fb_text_to(back, 96, y, num, 0xFFFFFF, 0x0C1420);
            uint32_t bw = (uint32_t)((g_counter[i] / 40) % (w > 260 ? w - 260 : 1));
            for (uint32_t yy = 0; yy < 16 && y + yy < h; yy++) {
                uint32_t *row = back + (uint64_t)(y + yy) * w + 240;
                for (uint32_t xx = 0; xx < bw; xx++) row[xx] = barc;
            }
        }
        disp_present();

        int k = kbd64_getkey();
        if (k < 0) k = hvkbd_getkey();
        if (k == 'q' || k == 'Q' || k == KEY_ESC) running = 0;
    }

    sched_stop();
}
