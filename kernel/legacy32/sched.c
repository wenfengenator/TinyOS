/* tinyos tasks & scheduler state.
 *
 * On real hardware the PIT timer IRQ calls schedule() and switch_context()
 * (entry.s) swaps kernel stacks + CR3.  Under the Unicorn emulator (no IRQs)
 * tools/emulate.py performs the equivalent save/restore over this same
 * g_tasks table — so the layout here is the contract between the two. */
#include "kernel.h"
#include "mm.h"

task_t  g_tasks[NTASK];
int32_t g_current;
int32_t g_ntask;

int task_alloc(void) {
    for (int i = 0; i < NTASK; i++)
        if (g_tasks[i].state == T_UNUSED) return i;
    return -1;
}

void task_create(int idx, uint32_t entry, uint32_t pid, const char *name, int state) {
    task_t *t = &g_tasks[idx];
    memset(t, 0, sizeof(*t));
    t->eip    = entry;
    t->esp    = USTACK_TOP(idx);
    t->ustack = USTACK_TOP(idx);
    t->eflags = 0x202;                       /* reserved + IF (preemption) */
    t->state  = state;
    t->pid    = pid;
    t->ppid   = 0;
    t->cr3    = g_kernel_pd;                  /* share the kernel address space */
    t->kstack = KSTACK_TOP - (uint32_t)idx * 0x1000u;
    t->wait_on = -1;
    unsigned n = kstrlen(name); if (n > 15) n = 15;
    memcpy(t->name, name, n); t->name[n] = 0;
}

/* hardware-path round-robin: pick the next runnable task after g_current */
int schedule(void) {
    for (int k = 1; k <= NTASK; k++) {
        int i = (g_current + k) % NTASK;
        if (g_tasks[i].state == T_READY || g_tasks[i].state == T_RUNNING) return i;
    }
    return -1;
}

/* ================= real context switching (hardware) =================
 * The int/IRQ stubs build a `struct regs` frame on the kernel stack.  A switch
 * saves the interrupted ring-3 context into the current TCB and copies the next
 * task's context into the frame, so the stub epilogue `iret`s straight into the
 * next task.  (The Unicorn emulator drives the equivalent from Python.) */
static void load_cr3(uint32_t pd) { if (pd) __asm__ volatile("mov %0, %%cr3" :: "r"(pd) : "memory"); }

void ctx_switch(struct regs *r, int next) {
    task_t *c = &g_tasks[g_current];
    if (c->state == T_RUNNING) c->state = T_READY;     /* outgoing task is runnable again */
    c->eax = r->eax; c->ebx = r->ebx; c->ecx = r->ecx; c->edx = r->edx;
    c->esi = r->esi; c->edi = r->edi; c->ebp = r->ebp;
    c->esp = r->useresp; c->eip = r->eip; c->eflags = r->eflags;

    task_t *t = &g_tasks[next];
    r->eax = t->eax; r->ebx = t->ebx; r->ecx = t->ecx; r->edx = t->edx;
    r->esi = t->esi; r->edi = t->edi; r->ebp = t->ebp;
    r->useresp = t->esp; r->eip = t->eip; r->eflags = t->eflags;
    r->cs = UCODE_SEL; r->ss = UDATA_SEL;
    r->ds = r->es = r->fs = r->gs = UDATA_SEL;
    load_cr3(t->cr3);
    if (t->state == T_READY) t->state = T_RUNNING;
    g_current = next;
}

static int ready_after(int cur) {
    for (int k = 1; k <= NTASK; k++) { int i = (cur + k) % NTASK; if (g_tasks[i].state == T_READY) return i; }
    return -1;
}
static int any_ready(void) { for (int i = 0; i < NTASK; i++) if (g_tasks[i].state == T_READY) return i; return -1; }

static void wake_waiters(uint32_t pid, uint32_t code) {
    for (int i = 0; i < NTASK; i++)
        if (g_tasks[i].state == T_BLOCKED && g_tasks[i].wait_on == (int32_t)pid) {
            g_tasks[i].wait_on = -1; g_tasks[i].eax = code; g_tasks[i].state = T_READY;
        }
}

void sched_exit(struct regs *r, uint32_t code) {
    g_tasks[g_current].state = T_EXITED; g_tasks[g_current].exit_code = code;
    wake_waiters(g_tasks[g_current].pid, code);
    int nx = any_ready();
    if (nx < 0) { kprint("[kernel] no runnable tasks; system halted.\n", KATTR); for (;;) __asm__ volatile("cli; hlt"); }
    ctx_switch(r, nx);
}
void sched_yield(struct regs *r) {
    int nx = ready_after(g_current);
    if (nx >= 0 && nx != g_current) ctx_switch(r, nx);
}
void sched_wait(struct regs *r, int pid) {
    for (int i = 0; i < NTASK; i++)
        if ((int)g_tasks[i].pid == pid && g_tasks[i].state == T_EXITED) { r->eax = g_tasks[i].exit_code; return; }
    g_tasks[g_current].state = T_BLOCKED; g_tasks[g_current].wait_on = pid;
    int nx = any_ready();
    if (nx < 0) { kprint("[kernel] wait deadlock; halted.\n", KATTR); for (;;) __asm__ volatile("cli; hlt"); }
    ctx_switch(r, nx);
}
void timer_tick(struct regs *r) {
    g_ticks++;
    if ((r->cs & 3) != 3) return;            /* only preempt a ring-3 task */
    int nx = ready_after(g_current);
    if (nx >= 0 && nx != g_current) ctx_switch(r, nx);
}
