/* SPDX-License-Identifier: GPL-2.0-only */

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
void     arch_set_syscall_stack(uint64_t rsp_top);  /* trap64.s: SYSCALL kstack     */

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
static inline uint64_t rdcr3(void)       { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void     wrcr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }

/* ---- tasks ---- */
#define MAXTASK 8
#define KSTACK  16384
enum { T_FREE = 0, T_READY, T_RUNNING, T_DONE, T_BLOCKED };
typedef struct { uint64_t rsp; uint8_t *stack; int state; uint64_t rsp0; int is_user; uint64_t cr3; int chan; void *proc; } task_t;

static task_t            g_tasks[MAXTASK];
static int               g_cur;
static volatile uint64_t g_switches;
static volatile int      g_sched_running;          /* 1 while the LAPIC timer drives scheduling */

/* Called from timer_isr (asm) with the interrupted thread's saved rsp.  Saves it,
 * round-robins to the next runnable thread, and returns that thread's rsp. */
uint64_t sched_tick(uint64_t cur_rsp) {
    lw(LAPIC_EOI, 0);                                   /* acknowledge the IRQ */
    if (!g_sched_running) return cur_rsp;               /* idle tick (native console): just
                                                         * ack + resume the interrupted context;
                                                         * used to wake wm_run's HLT when no
                                                         * user tasks are scheduled */
    g_switches++;
    g_tasks[g_cur].rsp = cur_rsp;
    if (g_tasks[g_cur].state == T_RUNNING) g_tasks[g_cur].state = T_READY;
    int n = g_cur, found = 0;
    for (int i = 0; i < MAXTASK; i++) {
        n = (n + 1) % MAXTASK;
        if (g_tasks[n].state == T_READY) { found = 1; break; }
    }
    if (!found) n = 0;                                  /* task 0 (driver) never blocks */
    g_cur = n;
    g_tasks[g_cur].state = T_RUNNING;
    if (g_tasks[g_cur].is_user) {                       /* ring-3 proc: point the  */
        arch_set_tss_rsp0(g_tasks[g_cur].rsp0);         /* fault + syscall stacks  */
        arch_set_syscall_stack(g_tasks[g_cur].rsp0);    /* at this proc's kstack   */
    }
    if (g_tasks[g_cur].proc) arch_switch_current(g_tasks[g_cur].proc);
    if (g_tasks[g_cur].cr3) wrcr3(g_tasks[g_cur].cr3);  /* switch address space    */
    return g_tasks[g_cur].rsp;
}

/* Reuse a finished (T_DONE) or never-used (T_FREE) task slot; slot 0 is the
 * driver task.  Lets the process model fork/exit repeatedly without exhausting
 * the fixed table. */
static int task_alloc(void) {
    for (int i = 1; i < MAXTASK; i++)
        if (g_tasks[i].state == T_FREE || g_tasks[i].state == T_DONE) {
            task_t *t = &g_tasks[i];
            t->rsp = 0; t->stack = 0; t->rsp0 = 0; t->is_user = 0;
            t->cr3 = 0; t->chan = 0; t->proc = 0;
            return i;
        }
    return -1;
}

/* Called from thread_trampoline (asm) if a thread function ever returns. */
void sched_exit(void) {
    g_tasks[g_cur].state = T_DONE;
    __asm__ volatile("sti");
    for (;;) __asm__ volatile("hlt");                  /* preempted away, never picked again */
}

/* Block the running task on a wait channel and yield the CPU.  It marks itself
 * BLOCKED (the scheduler then skips it) and halts until the timer preempts it;
 * sched_wake(chan) makes it runnable again, after which the timer reschedules it
 * and it resumes right here.  All context save/restore goes through the timer
 * path, so no separate voluntary-switch primitive is needed.  Only safe with the
 * timer running (sched_start). */
void sched_block(int chan) {
    int me = g_cur;
    g_tasks[me].chan = chan;
    g_tasks[me].state = T_BLOCKED;
    while (g_tasks[me].state == T_BLOCKED) __asm__ volatile("sti; hlt");
}

/* Wake every task blocked on `chan` (timer reschedules them). */
void sched_wake(int chan) {
    for (int i = 0; i < MAXTASK; i++)
        if (g_tasks[i].state == T_BLOCKED && g_tasks[i].chan == chan)
            g_tasks[i].state = T_READY;
}

/* Block the running task until *addr == want (e.g. a child reaching ZOMBIE).
 * The condition is tested with interrupts OFF, so a wake that lands between the
 * test and the sleep cannot be lost (the classic sleep/wakeup race): we are
 * already BLOCKED before interrupts are re-enabled at hlt.  The waker sets the
 * condition then sched_wake(chan).  Only safe with the timer running. */
void sched_wait(volatile int *addr, int want, int chan) {
    for (;;) {
        __asm__ volatile("cli");
        if (*addr == want) { __asm__ volatile("sti"); return; }
        g_tasks[g_cur].chan = chan;
        g_tasks[g_cur].state = T_BLOCKED;
        __asm__ volatile("sti; hlt");        /* blocked before interrupts on */
    }
}

/* Create a ready kernel thread that runs fn(arg) on its own stack. */
int sched_spawn(void (*fn)(uint64_t), uint64_t arg) {
    int id = task_alloc();
    if (id < 0) return -1;
    uint8_t *st = (uint8_t *)kmalloc(KSTACK);
    if (!st) return -1;
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

/* Phase B: spawn a ring-3 process task.  Builds the exact frame timer_isr's
 * restore path pops, so the first time the scheduler picks this task it iretq's
 * straight into ring 3 at `entry` with IF=1 (preemptible).  rbx/r15 seed the
 * demo loop's counter id + base; rsp0 is the per-proc kernel stack. */
int usched_spawn_user(uint64_t entry, uint64_t user_rsp, uint8_t *kstack,
                      uint64_t ksize, uint64_t rbx, uint64_t r15v, uint64_t cr3, void *proc) {
    int id = task_alloc();
    if (id < 0) return -1;
    uint64_t top = ((uint64_t)kstack + ksize) & ~15ULL;
    uint64_t *sp = (uint64_t *)top;
    *(--sp) = 0x1B;                        /* SS  = udata | 3           */
    *(--sp) = user_rsp;                    /* RSP                       */
    *(--sp) = 0x202;                       /* RFLAGS: IF=1              */
    *(--sp) = 0x23;                        /* CS  = ucode | 3           */
    *(--sp) = entry;                       /* RIP                       */
    *(--sp) = 0;                           /* rax                       */
    *(--sp) = rbx;                         /* rbx = counter id          */
    *(--sp) = 0;                           /* rcx                       */
    *(--sp) = 0;                           /* rdx                       */
    *(--sp) = 0;                           /* rsi                       */
    *(--sp) = 0;                           /* rdi                       */
    *(--sp) = 0;                           /* rbp                       */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* r8  r9  r10               */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* r11 r12 r13               */
    *(--sp) = 0;                           /* r14                       */
    *(--sp) = r15v;                        /* r15 = counter base VA     */
    g_tasks[id].rsp = (uint64_t)sp;
    g_tasks[id].stack = kstack;
    g_tasks[id].rsp0 = top;
    g_tasks[id].is_user = 1;
    g_tasks[id].cr3 = cr3;
    g_tasks[id].proc = proc;
    g_tasks[id].state = T_READY;
    return id;
}

/* Spawn a forked child: like usched_spawn_user but seeds ALL the parent's
 * callee-saved registers (callee[0..5] = rbx,rbp,r12,r13,r14,r15) and returns to
 * ring 3 with rax=0 at the fork syscall's return point, so the child continues
 * exactly where the parent called fork(). */
int usched_spawn_forked(uint64_t entry, uint64_t user_rsp, uint8_t *kstack,
                        uint64_t ksize, uint64_t cr3, void *proc, const uint64_t *callee) {
    int id = task_alloc();
    if (id < 0) return -1;
    uint64_t top = ((uint64_t)kstack + ksize) & ~15ULL;
    uint64_t *sp = (uint64_t *)top;
    *(--sp) = 0x1B;                        /* SS  = udata | 3           */
    *(--sp) = user_rsp;                    /* RSP                       */
    *(--sp) = 0x202;                       /* RFLAGS: IF=1              */
    *(--sp) = 0x23;                        /* CS  = ucode | 3           */
    *(--sp) = entry;                       /* RIP = fork return          */
    *(--sp) = 0;                           /* rax = 0 (child fork return) */
    *(--sp) = callee[0];                   /* rbx                       */
    *(--sp) = 0;                           /* rcx                       */
    *(--sp) = 0;                           /* rdx                       */
    *(--sp) = 0;                           /* rsi                       */
    *(--sp) = 0;                           /* rdi                       */
    *(--sp) = callee[1];                   /* rbp                       */
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; /* r8  r9  r10               */
    *(--sp) = 0;                           /* r11                       */
    *(--sp) = callee[2];                   /* r12                       */
    *(--sp) = callee[3];                   /* r13                       */
    *(--sp) = callee[4];                   /* r14                       */
    *(--sp) = callee[5];                   /* r15                       */
    g_tasks[id].rsp = (uint64_t)sp;
    g_tasks[id].stack = kstack;
    g_tasks[id].rsp0 = top;
    g_tasks[id].is_user = 1;
    g_tasks[id].cr3 = cr3;
    g_tasks[id].proc = proc;
    g_tasks[id].state = T_READY;
    return id;
}

uint64_t sched_switches(void) { return g_switches; }

/* execve: retarget the running task's CR3 after its address space is replaced. */
void sched_set_current_cr3(uint64_t cr3) { g_tasks[g_cur].cr3 = cr3; }

/* Calibrate the LAPIC timer against millis() and start it firing at ~100 Hz. */
static uint32_t g_per_tick;                             /* calibrated period (idle-tick reuse) */
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
    g_per_tick = per_tick;
    lw(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VEC);      /* periodic, unmasked */
    lw(LAPIC_TIMER_ICR, per_tick);
}

/* Idle-tick support for the native console loop (wm_run), which runs with the
 * scheduler stopped.  Arm a periodic LAPIC tick just around a HLT so the CPU
 * halts instead of spinning at 100%; the tick lands in sched_tick's guarded
 * early-return (harmless ack).  Armed only during the HLT and disarmed on wake,
 * so it never fires while a ring-3 program is executing. */
int  lapic_ready(void)      { return g_per_tick != 0; }
void lapic_idle_arm(void)   { if (g_per_tick) { lw(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VEC); lw(LAPIC_TIMER_ICR, g_per_tick); } }
void lapic_suspend_arm(void) {
    if (!g_per_tick) return;
    uint64_t count = (uint64_t)g_per_tick * 100u;        /* calibrated 10 ms tick -> ~1 second */
    if (count > 0xFFFFFFFFu) count = 0xFFFFFFFFu;
    lw(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VEC);
    lw(LAPIC_TIMER_ICR, (uint32_t)count);
}
void lapic_idle_disarm(void){ lw(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VEC); lw(LAPIC_TIMER_ICR, 0); }

void sched_init(void) {
    uint64_t base = smp_lapic_base();
    g_lapic = (volatile uint8_t *)(uintptr_t)(base ? base : 0xFEE00000ull);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);                 /* mask legacy PIC -> only LAPIC fires */
    lw(LAPIC_SVR, 0x100 | SPURIOUS_VEC);                /* software-enable LAPIC + spurious vec */
    arch_set_irq_gate(TIMER_VEC,    timer_isr_addr());
    arch_set_irq_gate(SPURIOUS_VEC, spurious_isr_addr());
    for (int i = 0; i < MAXTASK; i++) g_tasks[i].state = T_FREE;
    g_cur = 0; g_switches = 0;
    g_tasks[0].state = T_RUNNING; g_tasks[0].stack = 0; /* task 0 = the caller's context */
    g_tasks[0].cr3   = rdcr3();                         /* resume in the kernel address space */
    g_tasks[0].proc  = arch_current_process();          /* restore `current` with task 0 */
}

static volatile int g_sched_running;               /* 1 while the LAPIC timer is firing */
int sched_is_running(void) { return g_sched_running; }
void sched_start(void) { vmbus_synic_set_masked(1); lapic_timer_start(); g_sched_running = 1; __asm__ volatile("sti"); }
void sched_stop(void)  {
    __asm__ volatile("cli");
    g_sched_running = 0;
    lw(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VEC);
    lw(LAPIC_TIMER_ICR, 0);
    vmbus_synic_set_masked(0);                     /* restore the VMBus message interrupt */
}

void sched_s3_resume(int was_running) {
    uint64_t base = smp_lapic_base();
    g_lapic = (volatile uint8_t *)(uintptr_t)(base ? base : 0xFEE00000ull);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);
    lw(LAPIC_SVR, 0x100 | SPURIOUS_VEC);
    arch_set_irq_gate(TIMER_VEC, timer_isr_addr());
    arch_set_irq_gate(SPURIOUS_VEC, spurious_isr_addr());
    if (g_tasks[g_cur].is_user) {
        arch_set_tss_rsp0(g_tasks[g_cur].rsp0);
        arch_set_syscall_stack(g_tasks[g_cur].rsp0);
    }
    if (g_tasks[g_cur].proc) arch_switch_current(g_tasks[g_cur].proc);
    g_sched_running = 0;
    if (was_running) sched_start();
    else { lw(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VEC); lw(LAPIC_TIMER_ICR, 0); }
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
