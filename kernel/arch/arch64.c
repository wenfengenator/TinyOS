/* tinyos x64 arch layer (Cycle 4, Phase B): ring-0/ring-3 privilege split.
 *
 * arch_init() installs our own GDT (kernel + user code/data), a TSS (so faults
 * from ring 3 land on a kernel stack), and an IDT whose exception gates dump to
 * the serial console.  arch_usermode_selftest() then proves the whole path: it
 * maps a tiny user program (kernel/uprog.s) into a fresh, user-accessible region
 * of the current address space, enables SYSCALL, drops to ring 3 via iretq, and
 * services the program's SYS_WRITE / SYS_EXIT before returning to the caller.
 *
 * This is the foundation for real processes (Phase C): per-process page tables,
 * a scheduler, and exec/fork.  For now it runs one program to completion. */
#include "arch64.h"
#include "proc64.h"            /* struct proc + process table (Tier 1 Phase A) */
#include "fat64.h"             /* RAM-disk FAT16: readdir/readfile/writefile */
#include "tinygl.h"            /* buffer objects (Cycle 7 G6) for the zero-copy map test */
#include "uhello.h"            /* static const unsigned char hello_prog[] */
#include "ucount.h"            /* static const unsigned char count_prog[] */
#include "ugreet.h"            /* static const unsigned char greet_prog[] */
#include "ush.h"               /* static const unsigned char sh_prog[]    */
#include "uvi.h"               /* static const unsigned char vi_prog[]    */
#include "uexec_probe.h"       /* TinyOS parent for nested filesystem ELF */
#include "linux_hello.h"       /* static ELF64 Linux-ABI boot probe       */
#include "linux_fork.h"        /* fork()+wait4() Linux-ABI self-test      */
#include "linux_forkexec.h"    /* fork()+execve()+wait4() self-test       */
#include "linux_forkstorm.h"   /* fork/wait4 storm: task-slot reclamation  */
#include "linux_pipe.h"        /* pipe()+dup2()+fork() self-test          */
#include "linux_sig.h"         /* signal delivery self-test               */
#include "linux_sigh.h"        /* custom signal handler + rt_sigreturn     */
#include "linux_tty.h"         /* tty termios + process groups self-test  */
#include "linux_proc.h"        /* syscall tail + procfs self-test         */
#include "musl_probe.h"        /* real static musl libc probe             */

/* ---- assembly helpers (kernel/trap64.s) ---- */
void     arch_load_gdt(void *gdtr);
void     arch_ltr(uint16_t sel);
void     arch_set_syscall_stack(uint64_t rsp_top);
void     arch_setup_syscall_msrs(void);
uint64_t arch_isr_addr(unsigned i);
void     enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);
void     enter_user_exec(uint64_t rip, uint64_t rsp, uint64_t cr3);   /* execve: iretq into a replaced image */
void     sys_exit_resume(void);
uint64_t syscall_user_rip(void);            /* trap64.s: user rip at the last syscall */
uint64_t syscall_user_rsp(void);            /* trap64.s: user rsp at the last syscall */
uint64_t syscall_user_callee(void);         /* trap64.s: &{rbx,rbp,r12,r13,r14,r15} at last syscall */
static uint64_t do_demo_fork(void);         /* fork() for demo procs (defined later)  */
static uint64_t do_linux_fork(void);        /* fork() for scheduled Linux procs       */
static int      linux_wait4(int wpid, int *wstatus, int options);
static void     write_fs_base(uint64_t base);
static uint64_t read_fs_base(void);
static void     free_user_pages(uint64_t *pml4);   /* reclaim a dead process's frames */

/* ---- GDT + TSS ---- */
struct gdtr   { uint16_t limit; uint64_t base; } __attribute__((packed));
struct tss64  {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static uint64_t     g_gdt[7];               /* 0 null,1 kcode,2 kdata,3 udata,4 ucode,5-6 TSS */
static struct tss64 g_tss;
static uint8_t      g_kstack0[16384] __attribute__((aligned(16)));   /* TSS RSP0 (faults)     */
static uint8_t      g_ksys0[16384]   __attribute__((aligned(16)));   /* SYSCALL kstack level 0 */
static uint8_t      g_ksys1[16384]   __attribute__((aligned(16)));   /* SYSCALL kstack level 1 */
static uint8_t      g_ksys2[16384]   __attribute__((aligned(16)));   /* execve replacement     */

/* ---- IDT ---- */
struct idt_entry {
    uint16_t off_lo; uint16_t sel; uint8_t ist; uint8_t type;
    uint16_t off_mid; uint32_t off_hi; uint32_t zero;
} __attribute__((packed));
static struct idt_entry g_idt[256];

/* ---- process table (Tier 1 Phase A): per-process state lives in `current` ---- */
static struct proc  g_proc[PROC_MAX];
static struct proc *current;                    /* the running process           */
static int          g_next_pid;

/* The former per-program globals now address fields of the running process, so
 * the existing call sites keep compiling while the storage moves per-proc. */
#define g_user_abi         (current->abi)
#define g_cur_pid          (current->pid)
#define g_user_exit_code   (current->exit_status)
#define g_linux_fs_base    (current->fs_base)
#define g_linux_sigmask    (current->sigmask)
#define g_linux_clear_tid  (current->clear_tid)
#define g_linux_vm         (current->vm)
#define g_linux_fds        (current->fds)
#define g_linux_code       (current->code)
#define g_linux_stk        (current->stk)
#define g_linux_kstack_top (current->kstack_top)
#define LINUX_FD_MAX       PROC_FD_MAX

/* ---- user address space layout (high canonical, clear of physical RAM) ---- */
#define USER_CODE_VA    0x0000700000000000ULL
#define USER_STK_TOP    0x0000700000100000ULL
#define LINUX_LOAD_VA   0x0000000000400000ULL
#define LINUX_LOAD_LIMIT 0x0000000000600000ULL
#define LINUX_STK_TOP   0x00007FFFFFF00000ULL
#define LINUX_BRK_BASE  0x0000500000000000ULL
#define LINUX_BRK_LIMIT 0x0000500010000000ULL
#define LINUX_MMAP_BASE 0x0000600000000000ULL
#define LINUX_MMAP_LIMIT 0x0000600100000000ULL
#define USER_CODE_PAGES 16

/* TinyGL graphics client ABI (Cycle 7 G7): private syscalls a userspace app
 * uses to allocate a zero-copy buffer object and present it.  Must match
 * programs/tcc_include/tinygl.h. */
#define TGL_SYS_INFO       0x54474C00ULL
#define TGL_SYS_BO_CREATE  0x54474C01ULL
#define TGL_SYS_PRESENT    0x54474C02ULL
#define TGL_SYS_BO_DESTROY 0x54474C03ULL
#define USER_STK_PAGES  4
#define LINUX_LOAD_PAGES ((LINUX_LOAD_LIMIT - LINUX_LOAD_VA) / 4096)
#define USER_ARG_OFF    (USER_CODE_PAGES * 4096 - 256)   /* argv[0] staged here */
#define PTE_P   0x1
#define PTE_RW  0x2
#define PTE_US  0x4
#define PTE_PS  0x80

/* C dumper for the exception stubs (kernel/trap64.s). */
void isr_c(uint64_t vec, uint64_t err, uint64_t rip) {
    uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kputs("\n[EXC] vector "); kput_u32dec((uint32_t)vec);
    kputs(" err ");  kput_u64hex(err);
    kputs(" rip ");  kput_u64hex(rip);
    kputs(" cr2 ");  kput_u64hex(cr2);
    kputs("\n[EXC] halted.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static void set_gate(int i, uint64_t handler) {
    g_idt[i].off_lo  = handler & 0xFFFF;
    g_idt[i].sel     = 0x08;                 /* kernel code               */
    g_idt[i].ist     = 0;
    g_idt[i].type    = 0x8E;                 /* present, DPL0, int gate   */
    g_idt[i].off_mid = (handler >> 16) & 0xFFFF;
    g_idt[i].off_hi  = (handler >> 32) & 0xFFFFFFFF;
    g_idt[i].zero    = 0;
}

static void set_tss_desc(int idx, uint64_t base, uint32_t limit) {
    uint64_t lo = 0;
    lo |= (limit & 0xFFFFULL);
    lo |= (base & 0xFFFFFFULL) << 16;
    lo |= (uint64_t)0x89 << 40;              /* present, 64-bit avail TSS */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= ((base >> 24) & 0xFFULL) << 56;
    g_gdt[idx]     = lo;
    g_gdt[idx + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

void arch_init(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;        /* kernel code (DPL0, L=1)   */
    g_gdt[2] = 0x00CF92000000FFFFULL;        /* kernel data (DPL0)        */
    g_gdt[3] = 0x00CFF2000000FFFFULL;        /* user   data (DPL3)        */
    g_gdt[4] = 0x00AFFA000000FFFFULL;        /* user   code (DPL3, L=1)   */

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0       = (uint64_t)(g_kstack0 + sizeof(g_kstack0));
    g_tss.iomap_base = sizeof(g_tss);
    set_tss_desc(5, (uint64_t)&g_tss, sizeof(g_tss) - 1);

    static struct gdtr gp;
    gp.limit = sizeof(g_gdt) - 1;
    gp.base  = (uint64_t)g_gdt;
    kputs("[arch] loading gdt+tss\n");
    arch_load_gdt(&gp);
    arch_ltr(0x28);
    kputs("[arch] gdt+tss ok; loading idt\n");

    for (int i = 0; i < 256; i++) set_gate(i, arch_isr_addr((unsigned)i));
    static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    idtr.limit = sizeof(g_idt) - 1;
    idtr.base  = (uint64_t)g_idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));

    kputs("[arch] gdt + tss + idt installed (ring 0/3 ready)\n");
}

/* Install an interrupt gate at runtime.  The IDT is already loaded (arch_init),
 * so this takes effect immediately; used by the scheduler for the LAPIC timer. */
void arch_set_irq_gate(int vec, uint64_t handler) {
    if (vec >= 0 && vec < 256) set_gate(vec, handler);
}

/* Scheduler: repoint TSS.RSP0 so the next ring-3 -> ring-0 transition lands on
 * the running proc's own kernel stack (kernel/sched64.c:sched_tick). */
void arch_set_tss_rsp0(uint64_t rsp0) { g_tss.rsp0 = rsp0; }

/* Scheduler: make `current` follow the running proc so its syscalls act on it. */
void arch_switch_current(void *proc) {
    struct proc *p = (struct proc *)proc;
    if (!p || p == current) return;
    if (current) current->fs_base = read_fs_base();   /* save outgoing TLS base */
    current = p;
    write_fs_base(p->fs_base);                         /* restore incoming TLS base */
}

/* ---- syscall dispatch (called from kernel/trap64.s:syscall_entry) ---- */
static int arch_exec_named(const char *name, const char *arg);  /* nested child */
static int proc_info(int i, int *pid, int *ring, int *state, char *name); /* ps */
static int name_eq(const char *a, const char *b);
static uint64_t linux_brk(uint64_t requested);
static uint64_t linux_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                           uint64_t flags, uint64_t fd, uint64_t offset);
static int linux_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
static int linux_munmap(uint64_t addr, uint64_t len);
static int linux_openat(int dirfd, const char *path, int flags);
static int linux_resolve_at(int dirfd, const char *path, char *full);
static int linux_close(int fd);
static void linux_close_all_fds(void);
static int linux_pipe2(int *fds, int flags);
static int linux_dup(int oldfd);
static int linux_dup2(int oldfd, int newfd);
static int linux_fcntl(int fd, int cmd, uint64_t arg);
static int linux_read(int fd, void *buf, uint32_t len);
static int linux_write(int fd, const void *buf, uint32_t len);
static int64_t linux_lseek(int fd, int64_t offset, int whence);
static int linux_fstat(int fd, void *statbuf);
static int linux_ftruncate(int fd, uint64_t size);
static int linux_newfstatat(int dirfd, const char *path, void *statbuf, int flags);
static int linux_getdents64(int fd, void *buf, uint32_t len);
static int linux_mkdirat(int dirfd, const char *path);
static int linux_unlinkat(int dirfd, const char *path, int flags);
static int linux_rmdir(const char *path);
static int linux_rename(const char *oldp, const char *newp);
static uint64_t linux_getcwd(char *buf, uint64_t size);
static int linux_arch_prctl(int code, uint64_t addr);
static int linux_clock_gettime(int clock_id, void *timespec);
static int linux_getrandom(void *buf, uint32_t len, uint32_t flags);
static int linux_writev(int fd, const void *iov, int count);
static int linux_ioctl(int fd, uint64_t request, void *arg);
static int64_t gfx_info(void *user_info);
static int64_t gfx_bo_create(uint32_t w, uint32_t h, void *user_desc);
static int64_t gfx_present(int handle, int x, int y);
static int64_t gfx_bo_destroy(int handle);
struct linux_exec_args;
static int linux_execve(const char *path, const char *const *argv,
                        const char *const *envp);
static int run_linux_path(const char *path, uint8_t *stk, uint8_t *kstack_top,
                          const struct linux_exec_args *args, int on_sched);
static int arch_user_run(const char *name, const uint8_t *img, uint64_t size,
                         const char *arg);

static uint64_t g_kernel_cr3;

#define LINUX_EXEC_MAX 8
#define LINUX_EXEC_STR 128
struct linux_exec_args {
    int argc, envc;
    char argv[LINUX_EXEC_MAX][LINUX_EXEC_STR];
    char envp[LINUX_EXEC_MAX][LINUX_EXEC_STR];
};

static void write_fs_base(uint64_t base) {
    uint32_t lo = (uint32_t)base, hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(lo), "d"(hi));
    g_linux_fs_base = base;
}

static uint64_t read_fs_base(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000100U));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- process table helpers ---- */
static void proc_set_name(struct proc *p, const char *name) {
    int j = 0;
    if (name) while (name[j] && j < 15) { p->name[j] = name[j]; j++; }
    p->name[j] = 0;
}

/* proc[0] is the kernel context; user programs get proc[1..].  `current` always
 * points at a valid proc so the syscall layer can dereference it unconditionally. */
static void proc_table_init(void) {
    for (int i = 0; i < PROC_MAX; i++) g_proc[i].state = PROC_UNUSED;
    memset(&g_proc[0], 0, sizeof(g_proc[0]));
    g_proc[0].pid = 0;
    g_proc[0].abi = USER_ABI_TINYOS;
    g_proc[0].state = PROC_RUNNING;
    proc_set_name(&g_proc[0], "kernel");
    g_next_pid = 1;
    current = &g_proc[0];
}

static struct proc *proc_alloc(void) {
    for (int i = 1; i < PROC_MAX; i++) {
        if (g_proc[i].state != PROC_UNUSED) continue;
        struct proc *p = &g_proc[i];
        memset(p, 0, sizeof(*p));
        p->pid = g_next_pid++;
        p->state = PROC_EMBRYO;
        return p;
    }
    return 0;
}

static void proc_free(struct proc *p) {
    if (!p || p == &g_proc[0]) return;
    p->state = PROC_UNUSED;
}

/* Saved user context for signal delivery / rt_sigreturn (field offsets must match
 * kernel/trap64.s:resume_context). */
struct sigframe_ctx {
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t rax;
    uint64_t rip, rflags, rsp;
    uint64_t mask;
};
void sig_set_frame(uint64_t rsp, uint64_t signo);   /* trap64.s: stash delivery frame */
void resume_context(struct sigframe_ctx *ctx);      /* trap64.s: iretq back (no return) */

/* Terminate the running process (exit or a fatal signal): close fds, notify the
 * parent (SIGCHLD + wake), become a zombie, and retire the scheduler task. */
static void proc_terminate(int status) {
    current->exit_status = status;
    linux_close_all_fds();
    write_fs_base(0);
    if (current->on_sched) {
        for (int i = 0; i < PROC_MAX; i++)
            if (g_proc[i].state != PROC_UNUSED && g_proc[i].pid == current->ppid) {
                g_proc[i].sigpending |= (1ULL << (17 - 1));   /* SIGCHLD to the parent */
                break;
            }
        current->state = PROC_ZOMBIE;
        sched_wake(current->pid);
        sched_wake(current->ppid);
        sched_exit();                        /* never returns */
    }
    sys_exit_resume();                       /* run-to-completion path */
}

/* rt_sigaction(13): install a per-process handler (0=default, 1=ignore) + the
 * SA_RESTORER trampoline; report the previous handler in oldact. */
static int linux_rt_sigaction(int sig, const void *act, void *oldact, uint64_t sigsetsize) {
    if (sig < 1 || sig > 64 || sigsetsize != 8) return -22;
    const uint64_t *a = (const uint64_t *)act;   /* {handler, flags, restorer, mask} */
    uint64_t *o = (uint64_t *)oldact;
    if (o) { o[0] = current->sighandler[sig]; o[1] = 0; o[2] = current->sigrestorer; o[3] = 0; }
    if (a) {
        current->sighandler[sig] = a[0];
        if (a[1] & 0x04000000ULL) current->sigrestorer = a[2];   /* SA_RESTORER */
    }
    return 0;
}

/* kill(62): post a signal to a process (sig 0 = existence check); wake it so a
 * blocked target delivers on its next syscall return. */
static int linux_kill(int pid, int sig) {
    if (sig < 0 || sig > 64) return -22;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PROC_UNUSED && g_proc[i].pid == pid) {
            if (sig == 0) return 0;
            g_proc[i].sigpending |= (1ULL << (sig - 1));
            sched_wake(pid);
            return 0;
        }
    return -3;   /* ESRCH */
}

/* Signals whose SIG_DFL kills the process (vs. ignore/stop). */
static int sig_default_terminate(int sig) {
    switch (sig) {
    case 17: case 18: case 20: case 21: case 22: case 23: case 28:  /* CHLD/CONT/TSTP/TTIN/TTOU/URG/WINCH */
        return 0;
    default:
        return 1;
    }
}

/* Called from syscall_entry after every Linux syscall.  Acts on a pending signal:
 * default terminate/ignore, or deliver a custom handler by building a signal frame
 * on the user stack (return the handler VA; enter_user_signal jumps to it). */
uint64_t signal_check(uint64_t retval) {
    if (current->abi != USER_ABI_LINUX) return 0;
    uint64_t deliver = current->sigpending & ~current->sigmask;
    if (!deliver) return 0;
    int sig = 1;
    while (sig <= 64 && !(deliver & (1ULL << (sig - 1)))) sig++;
    if (sig > 64) return 0;
    current->sigpending &= ~(1ULL << (sig - 1));
    uint64_t h = current->sighandler[sig];
    if (sig == 9 || (h == 0 && sig_default_terminate(sig)))    /* SIGKILL or fatal default */
        proc_terminate(128 + sig);                             /* never returns */
    if (h == 0 || h == 1) return 0;                            /* default ignore or SIG_IGN */
    /* custom handler: build a signal frame on the user stack and enter it */
    uint64_t ursp = syscall_user_rsp();
    uint64_t sp = (ursp - 128) & ~15ULL;                       /* skip the red zone */
    sp -= sizeof(struct sigframe_ctx);
    sp &= ~15ULL;
    struct sigframe_ctx *ctx = (struct sigframe_ctx *)sp;
    const uint64_t *cal = (const uint64_t *)syscall_user_callee();
    ctx->rbx = cal[0]; ctx->rbp = cal[1];
    ctx->r12 = cal[2]; ctx->r13 = cal[3]; ctx->r14 = cal[4]; ctx->r15 = cal[5];
    ctx->rax = retval;
    ctx->rip = syscall_user_rip();
    ctx->rflags = 0x202;
    ctx->rsp = ursp;
    ctx->mask = current->sigmask;
    uint64_t hsp = sp - 8;
    *(uint64_t *)hsp = current->sigrestorer;                   /* handler's return addr */
    sig_set_frame(hsp, (uint64_t)sig);
    current->sigmask |= (1ULL << (sig - 1));                   /* block during the handler */
    return h;
}

/* rt_sigreturn(15): restore the signal mask + interrupted context and resume. */
static uint64_t sig_return(void) {
    struct sigframe_ctx *ctx = (struct sigframe_ctx *)syscall_user_rsp();
    current->sigmask = ctx->mask;
    resume_context(ctx);                                       /* never returns */
    return 0;
}

/* ---- controlling tty (Phase F): one console tty with termios + a foreground
 * process group.  The line discipline turns Ctrl-C into SIGINT for that group. */
#define TTY_ISIG   0x0001
#define TTY_ICANON 0x0002
#define TTY_ECHO   0x0008
static struct { int fg_pgrp; uint32_t lflag; } g_tty = { 0, TTY_ISIG | TTY_ICANON | TTY_ECHO };

/* Post a signal to every member of a process group. */
static void signal_pgrp(int pgrp, int sig) {
    if (pgrp <= 0) return;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PROC_UNUSED && g_proc[i].pgid == pgrp) {
            g_proc[i].sigpending |= (1ULL << (sig - 1));
            sched_wake(g_proc[i].pid);
        }
}

static struct proc *proc_by_pid(int pid) {
    if (!pid) return current;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proc[i].state != PROC_UNUSED && g_proc[i].pid == pid) return &g_proc[i];
    return 0;
}

static int linux_setpgid(int pid, int pgid) {
    struct proc *t = proc_by_pid(pid);
    if (!t) return -3;
    t->pgid = pgid ? pgid : t->pid;
    return 0;
}
static int linux_getpgid(int pid) {
    struct proc *t = proc_by_pid(pid);
    if (!t) return -3;
    return t->pgid ? t->pgid : t->pid;
}
static int linux_setsid(void) {
    current->sid = current->pid;
    current->pgid = current->pid;
    return current->pid;
}
static int linux_getsid(int pid) {
    struct proc *t = proc_by_pid(pid);
    if (!t) return -3;
    return t->sid ? t->sid : t->pid;
}

/* ---- Phase G: syscall long tail + minimal procfs -------------------------- */
static int strpfx(const char *s, const char *p) { int i = 0; while (p[i]) { if (s[i] != p[i]) return 0; i++; } return 1; }
static int str_ends(const char *s, const char *suf) {
    int ls = 0; while (s[ls]) ls++; int lf = 0; while (suf[lf]) lf++;
    return lf <= ls && name_eq(s + ls - lf, suf);
}
static int g_app(char *b, int n, const char *s) { for (int i = 0; s[i]; i++) b[n++] = s[i]; return n; }
static int g_dec(char *b, int n, uint64_t v) { char t[24]; int i = 0; if (!v) { b[n++] = '0'; return n; } while (v) { t[i++] = (char)('0' + v % 10); v /= 10; } while (i) b[n++] = t[--i]; return n; }

/* Synthesize a /proc file's contents into out (<512 bytes); -1 if unknown. */
static int proc_synth(const char *path, char *out) {
    int n = 0;
    if (name_eq(path, "/proc/meminfo")) {
        n = g_app(out, n, "MemTotal:       "); n = g_dec(out, n, heap_total() / 1024);
        n = g_app(out, n, " kB\nMemFree:        "); n = g_dec(out, n, (heap_total() - heap_used()) / 1024);
        n = g_app(out, n, " kB\n");
    } else if (name_eq(path, "/proc/mounts")) {
        n = g_app(out, n, "rootfs / rootfs rw 0 0\ndev /dev devtmpfs rw 0 0\n");
    } else if (str_ends(path, "/stat")) {
        n = g_dec(out, n, (uint64_t)current->pid); n = g_app(out, n, " (");
        n = g_app(out, n, current->name); n = g_app(out, n, ") R ");
        n = g_dec(out, n, (uint64_t)current->ppid); n = g_app(out, n, " ");
        n = g_dec(out, n, (uint64_t)(current->pgid ? current->pgid : current->pid)); n = g_app(out, n, " ");
        n = g_dec(out, n, (uint64_t)(current->sid ? current->sid : current->pid));
        n = g_app(out, n, " 0 -1 0 0 0 0 0 0 0 0 0\n");
    } else return -1;
    return n;
}

static int proc_read(const char *path, char *buf, uint32_t len, uint32_t off) {
    char tmp[512]; int n = proc_synth(path, tmp);
    if (n < 0) return -2;
    if (off >= (uint32_t)n) return 0;
    uint32_t c = (uint32_t)n - off; if (c > len) c = len;
    for (uint32_t i = 0; i < c; i++) buf[i] = tmp[off + i];
    return (int)c;
}

static int linux_uname(void *buf) {
    if (!buf) return -14;
    char *u = (char *)buf;                     /* utsname: 6 x 65-byte NUL-padded fields */
    memset(u, 0, 6 * 65);
    const char *f[6] = { "TinyOS", "tinyos", "6.1.0-tinyos", "#1 TinyOS x64", "x86_64", "(none)" };
    for (int i = 0; i < 6; i++) { int j = 0; while (f[i][j] && j < 64) { u[i * 65 + j] = f[i][j]; j++; } }
    return 0;
}

static int linux_nanosleep(const void *req, void *rem) {
    if (!req) return -14;
    const uint64_t *ts = (const uint64_t *)req;   /* {tv_sec, tv_nsec} */
    uint64_t ms = ts[0] * 1000 + ts[1] / 1000000;
    uint64_t t0 = millis();
    while (millis() - t0 < ms) __asm__ volatile("pause");
    if (rem) { ((uint64_t *)rem)[0] = 0; ((uint64_t *)rem)[1] = 0; }
    return 0;
}

static int linux_access(int dirfd, const char *path) {
    char full[80]; if (linux_resolve_at(dirfd, path, full) < 0) return -2;
    if (strpfx(full, "/proc/")) return 0;
    uint8_t attr; uint16_t cl; uint32_t sz;
    return fat_resolve(full, &attr, &cl, &sz) == 0 ? 0 : -2;
}

static int linux_readlinkat(int dirfd, const char *path, char *buf, uint32_t bufsiz) {
    char full[80]; if (linux_resolve_at(dirfd, path, full) < 0) return -2;
    if (name_eq(full, "/proc/self/exe")) {
        int n = g_app(buf, 0, "/bin/");
        n = g_app(buf, n, current->name);
        return n > (int)bufsiz ? (int)bufsiz : n;
    }
    return -22;                                /* not a symlink */
}

static int linux_sysinfo(void *buf) {
    if (!buf) return -14;
    memset(buf, 0, 112);
    uint64_t *si = (uint64_t *)buf;            /* uptime, loads[3], totalram, freeram, ... */
    si[0] = millis() / 1000;
    si[4] = heap_total();
    si[5] = heap_total() - heap_used();
    return 0;
}

static int linux_poll(void *fds, int nfds) {
    struct pollfd { int fd; short events, revents; } *p = (struct pollfd *)fds;
    for (int i = 0; i < nfds; i++) p[i].revents = p[i].events;   /* claim every fd ready */
    return nfds;
}

static int linux_prlimit(void *old) {
    if (old) { ((uint64_t *)old)[0] = 0x800000; ((uint64_t *)old)[1] = ~0ULL; }  /* cur, max */
    return 0;
}

uint64_t syscall_c(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (g_user_abi == USER_ABI_DEMO) {          /* scheduler self-test procs */
        if (num == 0x2000) sched_block((int)a0); /* blocking "read" -> yield the CPU     */
        else if (num == 0x2001) {                /* exit(status): zombie + retire task   */
            current->exit_status = (int)a0;
            current->state = PROC_ZOMBIE;
            sched_wake(current->pid);            /* wake a parent blocked in wait         */
            sched_exit();                        /* task DONE; never returns */
        }
        else if (num == 0x2002) return do_demo_fork();  /* fork -> child pid to parent   */
        return 0;
    }
    if (g_user_abi == USER_ABI_LINUX) {
        switch (num) {
        case 0:                             /* read                       */
            return (uint64_t)(int64_t)linux_read((int)a0, (void *)a1, (uint32_t)a2);
        case 1:                             /* write                      */
            return (uint64_t)(int64_t)linux_write((int)a0, (const void *)a1, (uint32_t)a2);
        case 2:                             /* open                       */
            return (uint64_t)(int64_t)linux_openat(-100, (const char *)a0, (int)a1);
        case 3:                             /* close                      */
            return (uint64_t)(int64_t)linux_close((int)a0);
        case 5:                             /* fstat                      */
            return (uint64_t)(int64_t)linux_fstat((int)a0, (void *)a1);
        case 8:                             /* lseek                      */
            return (uint64_t)linux_lseek((int)a0, (int64_t)a1, (int)a2);
        case 9:                             /* mmap                       */
            return linux_mmap(a0, a1, a2, a3, a4, a5);
        case 10:                            /* mprotect                   */
            return (uint64_t)(int64_t)linux_mprotect(a0, a1, a2);
        case 11:                            /* munmap                     */
            return (uint64_t)(int64_t)linux_munmap(a0, a1);
        case 12:                            /* brk                        */
            return linux_brk(a0);
        case TGL_SYS_INFO:                  /* TinyGL: query the display   */
            return (uint64_t)gfx_info((void *)a0);
        case TGL_SYS_BO_CREATE:             /* TinyGL: zero-copy buffer    */
            return (uint64_t)gfx_bo_create((uint32_t)a0, (uint32_t)a1, (void *)a2);
        case TGL_SYS_PRESENT:               /* TinyGL: composite + present */
            return (uint64_t)gfx_present((int)a0, (int)a1, (int)a2);
        case TGL_SYS_BO_DESTROY:            /* TinyGL: free a buffer       */
            return (uint64_t)gfx_bo_destroy((int)a0);
        case 13:                            /* rt_sigaction               */
            return (uint64_t)(int64_t)linux_rt_sigaction((int)a0, (const void *)a1, (void *)a2, a3);
        case 15:                            /* rt_sigreturn               */
            return sig_return();
        case 62:                            /* kill                       */
            return (uint64_t)(int64_t)linux_kill((int)a0, (int)a1);
        case 234:                           /* tgkill                     */
            return (uint64_t)(int64_t)linux_kill((int)a1, (int)a2);
        case 109:                           /* setpgid                    */
            return (uint64_t)(int64_t)linux_setpgid((int)a0, (int)a1);
        case 121:                           /* getpgid                    */
            return (uint64_t)(int64_t)linux_getpgid((int)a0);
        case 111:                           /* getpgrp                    */
            return (uint64_t)(current->pgid ? current->pgid : current->pid);
        case 112:                           /* setsid                     */
            return (uint64_t)(int64_t)linux_setsid();
        case 124:                           /* getsid                     */
            return (uint64_t)(int64_t)linux_getsid((int)a0);
        case 63:                            /* uname                      */
            return (uint64_t)(int64_t)linux_uname((void *)a0);
        case 35:                            /* nanosleep                  */
            return (uint64_t)(int64_t)linux_nanosleep((const void *)a0, (void *)a1);
        case 230:                           /* clock_nanosleep            */
            return (uint64_t)(int64_t)linux_nanosleep((const void *)a2, (void *)a3);
        case 21:                            /* access                     */
            return (uint64_t)(int64_t)linux_access(-100, (const char *)a0);
        case 269:                           /* faccessat                  */
            return (uint64_t)(int64_t)linux_access((int)a0, (const char *)a1);
        case 89:                            /* readlink                   */
            return (uint64_t)(int64_t)linux_readlinkat(-100, (const char *)a0, (char *)a1, (uint32_t)a2);
        case 267:                           /* readlinkat                 */
            return (uint64_t)(int64_t)linux_readlinkat((int)a0, (const char *)a1, (char *)a2, (uint32_t)a3);
        case 99:                            /* sysinfo                    */
            return (uint64_t)(int64_t)linux_sysinfo((void *)a0);
        case 7:                             /* poll                       */
            return (uint64_t)(int64_t)linux_poll((void *)a0, (int)a1);
        case 96:                            /* gettimeofday               */
            if (a0) { uint64_t ms = millis(), w = wall_epoch();
                      ((uint64_t *)a0)[0] = w ? w : ms / 1000; ((uint64_t *)a0)[1] = (ms % 1000) * 1000; }
            return 0;
        case 110:                           /* getppid                    */
            return (uint64_t)current->ppid;
        case 102: case 104: case 107: case 108:   /* getuid/getgid/geteuid/getegid = root */
            return 0;
        case 95:                            /* umask                      */
            return 0;
        case 98:                            /* getrusage                  */
            if (a1) memset((void *)a1, 0, 144);
            return 0;
        case 97:                            /* getrlimit                  */
            return (uint64_t)(int64_t)linux_prlimit((void *)a1);
        case 302:                           /* prlimit64                  */
            return (uint64_t)(int64_t)linux_prlimit((void *)a3);
        case 202:                           /* futex (stub)               */
            return 0;
        case 204:                           /* sched_getaffinity          */
            if (a2) *(uint64_t *)a2 = 1;
            return 8;
        case 332:                           /* statx -> ENOSYS (fall back to fstatat) */
            return (uint64_t)-38;
        case 14: {                          /* rt_sigprocmask             */
            if (a3 != 8) return (uint64_t)-22;
            if (a2) *(uint64_t *)a2 = g_linux_sigmask;
            if (a1) {
                uint64_t mask = *(const uint64_t *)a1;
                if (a0 == 0) g_linux_sigmask |= mask;
                else if (a0 == 1) g_linux_sigmask &= ~mask;
                else if (a0 == 2) g_linux_sigmask = mask;
                else return (uint64_t)-22;
            }
            return 0;
        }
        case 16:                            /* ioctl                      */
            return (uint64_t)(int64_t)linux_ioctl((int)a0, a1, (void *)a2);
        case 20:                            /* writev                     */
            return (uint64_t)(int64_t)linux_writev((int)a0, (const void *)a1, (int)a2);
        case 22:                            /* pipe                       */
            return (uint64_t)(int64_t)linux_pipe2((int *)a0, 0);
        case 293:                           /* pipe2                      */
            return (uint64_t)(int64_t)linux_pipe2((int *)a0, (int)a1);
        case 32:                            /* dup                        */
            return (uint64_t)(int64_t)linux_dup((int)a0);
        case 33:                            /* dup2                       */
            return (uint64_t)(int64_t)linux_dup2((int)a0, (int)a1);
        case 292:                           /* dup3                       */
            return (uint64_t)(int64_t)linux_dup2((int)a0, (int)a1);
        case 72:                            /* fcntl                      */
            return (uint64_t)(int64_t)linux_fcntl((int)a0, (int)a1, a2);
        case 39:                            /* getpid                     */
        case 186:                           /* gettid                     */
            return (uint64_t)g_cur_pid;
        case 56:                            /* clone(flags, stack, ...) — fork when no new stack */
            return a1 ? (uint64_t)-38 : do_linux_fork();
        case 57:                            /* fork                       */
            return do_linux_fork();
        case 59:                            /* execve                     */
            return (uint64_t)(int64_t)linux_execve((const char *)a0,
                (const char *const *)a1, (const char *const *)a2);
        case 61:                            /* wait4                      */
            return (uint64_t)(int64_t)linux_wait4((int)a0, (int *)a1, (int)a2);
        case 77:                            /* ftruncate                  */
            return (uint64_t)(int64_t)linux_ftruncate((int)a0, a1);
        case 79:                            /* getcwd                     */
            return linux_getcwd((char *)a0, a1);
        case 80:                            /* chdir                      */
            return (uint64_t)(int64_t)fs_chdir((const char *)a0);
        case 158:                           /* arch_prctl                 */
            return (uint64_t)(int64_t)linux_arch_prctl((int)a0, a1);
        case 218:                           /* set_tid_address            */
            g_linux_clear_tid = a0;
            return (uint64_t)g_cur_pid;
        case 217:                           /* getdents64                 */
            return (uint64_t)(int64_t)linux_getdents64((int)a0, (void *)a1, (uint32_t)a2);
        case 228:                           /* clock_gettime              */
            return (uint64_t)(int64_t)linux_clock_gettime((int)a0, (void *)a1);
        case 201: {                         /* time(tloc)                 */
            uint64_t w = wall_epoch(); if (!w) w = millis() / 1000;
            if (a0) *(int64_t *)a0 = (int64_t)w;
            return w;
        }
        case 257:                           /* openat                     */
            return (uint64_t)(int64_t)linux_openat((int)a0, (const char *)a1, (int)a2);
        case 258:                           /* mkdirat                    */
            return (uint64_t)(int64_t)linux_mkdirat((int)a0, (const char *)a1);
        case 83:                            /* mkdir(path, mode)  (musl legacy) */
            return (uint64_t)(int64_t)linux_mkdirat(-100, (const char *)a0);
        case 84:                            /* rmdir(path)                */
            return (uint64_t)(int64_t)linux_rmdir((const char *)a0);
        case 87:                            /* unlink(path)               */
            return (uint64_t)(int64_t)linux_unlinkat(-100, (const char *)a0, 0);
        case 4:                             /* stat(path, statbuf)        */
        case 6:                             /* lstat(path, statbuf) — FAT has no symlinks */
            return (uint64_t)(int64_t)linux_newfstatat(-100, (const char *)a0, (void *)a1, 0);
        case 82:                            /* rename(old, new)           */
            return (uint64_t)(int64_t)linux_rename((const char *)a0, (const char *)a1);
        case 85:                            /* creat = open O_CREAT|O_WRONLY|O_TRUNC */
            return (uint64_t)(int64_t)linux_openat(-100, (const char *)a0, 0100 | 1 | 01000);
        case 86:                            /* link    -> ENOSYS (FAT)    */
        case 88:                            /* symlink -> ENOSYS (FAT)    */
            return (uint64_t)-38;
        case 90:                            /* chmod  -> no-op (FAT perms)*/
        case 91:                            /* fchmod -> no-op            */
            return 0;
        case 262:                           /* newfstatat                 */
            return (uint64_t)(int64_t)linux_newfstatat((int)a0, (const char *)a1, (void *)a2, (int)a3);
        case 263:                           /* unlinkat                   */
            return (uint64_t)(int64_t)linux_unlinkat((int)a0, (const char *)a1, (int)a2);
        case 273:                           /* set_robust_list            */
            return 0;
        case 318:                           /* getrandom                  */
            return (uint64_t)(int64_t)linux_getrandom((void *)a0, (uint32_t)a1, (uint32_t)a2);
        case 60:                            /* exit(status)               */
        case 231:                           /* exit_group(status)         */
            g_user_exit_code = (int)a0;
            linux_close_all_fds();          /* release pipe/file ends (EOF to peers) */
            write_fs_base(0);
            if (current->on_sched) {        /* scheduled proc: zombie + wake parent, retire task */
                current->state = PROC_ZOMBIE;
                sched_wake(current->pid);   /* waiters blocked on the child's pid   */
                sched_wake(current->ppid);  /* a parent blocked in wait4 (own pid)  */
                sched_exit();               /* never returns */
            }
            sys_exit_resume();
            return 0;
        default:
            return (uint64_t)-38;           /* -ENOSYS                    */
        }
    }
    switch (num) {
    case 4:                                   /* write(fd, buf, len)       */
        (void)a0;
        usys_write((const char *)a1, (unsigned long)a2);
        return a2;
    case 3: {                                 /* read(fd, buf, max) -> line */
        (void)a0;
        char *buf = (char *)a1; int max = (int)a2, n = 0;
        for (;;) {
            int k = kbd64_getkey();
            if (k < 0) k = hvkbd_getkey();
            if (k >= 0) {
                if (k == '\n' || k == '\r') { char nl = '\n'; usys_write(&nl, 1); break; }
                else if (k == '\b') { if (n > 0) { n--; char b = '\b'; usys_write(&b, 1); } }
                else if (k >= 0x20 && k < 0x7F && n < max) { char c = (char)k; buf[n++] = c; usys_write(&c, 1); }
            }
            wm_pump();                        /* keep the desktop live while blocked */
        }
        return (uint64_t)n;
    }
    case 9:                                   /* exec(name, arg) -> code   */
        return (uint64_t)arch_exec_named((const char *)a0, (const char *)a1);
    case 19: {                                /* getchar() -> one keycode  */
        for (;;) {
            int k = kbd64_getkey();
            if (k < 0) k = hvkbd_getkey();
            if (k >= 0) return (uint64_t)k;
            wm_pump();                        /* keep the desktop live      */
        }
    }
    case 20:                                  /* clear() the terminal grid */
        term_clear_active();
        return 0;
    case 21:                                  /* gotoxy(row, col)          */
        term_gotoxy_active((int)a0, (int)a1);
        return 0;
    case 8:                                   /* getpid()                  */
        return (uint64_t)g_cur_pid;
    case 1:                                   /* exit(code)                */
        g_user_exit_code = (int)a0;
        sys_exit_resume();                    /* does not return           */
        return 0;
    case 15: {                                /* readdir(dir,i,name)       */
        char full[80]; fs_resolve((const char *)a0, full);
        int idx = (int)a1; char *name = (char *)a2;
        uint16_t dcl = 0; uint8_t at; uint16_t cl; uint32_t sz;
        if (!(full[0] == '/' && full[1] == 0)) {
            if (fat_resolve(full, &at, &cl, &sz) < 0 || !(at & 0x10)) return (uint64_t)-1;
            dcl = cl;
        }
        if (fat_list(dcl, idx, name, &at, &cl, &sz) < 0) return (uint64_t)-1;
        return (uint64_t)((at & 0x10) ? 1 : 0);
    }
    case 16: {                                /* taskinfo(i, buf) -> ps    */
        struct pinfo { int pid, ring, state; char name[16]; } *o = (struct pinfo *)a1;
        return (uint64_t)(int)proc_info((int)a0, &o->pid, &o->ring, &o->state, o->name);
    }
    case 18: {                                /* readfile(path,buf,max)    */
        char full[80]; fs_resolve((const char *)a0, full);
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_resolve(full, &at, &cl, &sz) < 0 || (at & 0x10)) return (uint64_t)-1;
        return (uint64_t)(int)fat_read(cl, sz, (void *)a1, (uint32_t)a2);
    }
    case 22: {                                /* writefile(path,buf,n)     */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_write(full, (const void *)a1, (uint32_t)a2);
    }
    case 23: {                                /* mkdir(path)               */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_mkdir(full);
    }
    case 24: {                                /* unlink(path)              */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_unlink(full);
    }
    case 25:                                  /* chdir(path)               */
        return (uint64_t)(int)fs_chdir((const char *)a0);
    case 26:                                  /* getcwd(buf)               */
        fs_getcwd((char *)a0);
        return 0;
    case 27: {                                /* winsize() -> (cols<<16)|rows */
        int c = 0, r = 0; term_active_size(&c, &r);
        return (uint64_t)(((unsigned)c << 16) | (unsigned)(r & 0xFFFF));
    }
    case 28: {                                /* copy(src, dst) -> bytes/-1 */
        char src[80], dst[80];
        fs_resolve((const char *)a0, src);
        fs_resolve((const char *)a1, dst);
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_resolve(src, &at, &cl, &sz) < 0 || (at & 0x10)) return (uint64_t)-1;
        uint8_t *buf = (uint8_t *)kmalloc(sz ? sz : 1);
        if (!buf) return (uint64_t)-1;
        int r = fat_read(cl, sz, buf, sz);
        int w = (r == (int)sz) ? fat_write(dst, buf, sz) : -1;
        kfree(buf);
        return (uint64_t)(int)w;
    }
    default:
        return (uint64_t)-1;
    }
}

/* ---- minimal page-table walk: add a user page to the live address space ---- */
static uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static void flush_tlb(void)    { uint64_t v; __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(v) :: "memory"); }

/* Firmware write-protects its own page tables (CR0.WP), so temporarily allow
 * supervisor writes to read-only pages while we splice in the user mappings. */
static uint64_t wp_disable(void) {
    uint64_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 & ~(1ULL << 16)) : "memory");
    return cr0;
}
static void wp_restore(uint64_t cr0) { __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory"); }

static uint64_t *next_level(uint64_t *tbl, int i) {
    if (!(tbl[i] & PTE_P)) {
        uint64_t *n = (uint64_t *)page_alloc(1);          /* identity-mapped, zeroed */
        tbl[i] = (uint64_t)n | PTE_P | PTE_RW | PTE_US;
        return n;
    }
    tbl[i] |= PTE_US;                                  /* open the path to ring 3 */
    return (uint64_t *)(tbl[i] & ~0xFFFULL);
}

static void map_user_page(uint64_t pml4_phys, uint64_t va, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;            /* CR3 is identity-mapped  */
    uint64_t *pdpt = next_level(pml4, (int)((va >> 39) & 0x1FF));
    uint64_t *pd   = next_level(pdpt, (int)((va >> 30) & 0x1FF));
    uint64_t *pt   = next_level(pd,   (int)((va >> 21) & 0x1FF));
    pt[(va >> 12) & 0x1FF] = (phys & ~0xFFFULL) | flags | PTE_P;
}

/* ---- reusable ring-3 program regions ------------------------------------
 * Two frame sets share the single USER_CODE_VA / USER_STK_TOP window so a shell
 * (level 0) can exec a child (level 1): only one is mapped at a time, swapped by
 * rewriting our own (writable) leaf page table.  A separate kernel syscall stack
 * per level keeps a suspended parent's syscall frame from being clobbered by the
 * child (see kernel/trap64.s). */
static uint8_t  *g_ucode, *g_ustk;             /* level 0 (console / shell)     */
static uint8_t  *g_ccode, *g_cstk;             /* level 1 (exec'd child)        */
static uint64_t *g_pt;                         /* cached leaf PT for the window */
static int       g_depth;                      /* 0 primary, 1 child running    */

static uint64_t *walk_pt(uint64_t pml4_phys, uint64_t va) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(pml4[(va >> 39) & 0x1FF] & ~0xFFFULL);
    uint64_t *pd   = (uint64_t *)(pdpt[(va >> 30) & 0x1FF] & ~0xFFFULL);
    return (uint64_t *)(pd[(va >> 21) & 0x1FF] & ~0xFFFULL);
}

/* point the code+stack window at a given pair of physical frame runs */
static void map_region(uint8_t *code, uint8_t *stk) {
    int ci = (int)((USER_CODE_VA >> 12) & 0x1FF);
    int si = (int)((USER_STK_TOP  >> 12) & 0x1FF);
    for (int i = 0; i < USER_CODE_PAGES; i++)
        g_pt[ci + i]     = ((uint64_t)(code + (uint64_t)i * 4096)) | PTE_P | PTE_RW | PTE_US;
    for (int i = 0; i < USER_STK_PAGES; i++)
        g_pt[si - 1 - i] = ((uint64_t)(stk  + (uint64_t)i * 4096)) | PTE_P | PTE_RW | PTE_US;
    flush_tlb();
}

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static uint64_t *copy_page_table(const uint64_t *source) {
    uint64_t *copy = (uint64_t *)page_alloc(1);
    if (!copy) return 0;
    if (source) for (int i = 0; i < 512; i++) copy[i] = source[i];
    return copy;
}

static uint64_t make_table_entry(uint64_t *table) {
    return (uint64_t)table | PTE_P | PTE_RW | PTE_US;
}

/* struct proc_vm / struct linux_fd / enum linux_fd_kind now live in proc64.h;
 * the running process's VM arena and fd table are reached through `current`. */
#define LINUX_O_ACCMODE 3
#define LINUX_O_CREAT 0100
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_DIRECTORY 0200000

static int linux_resolve_at(int dirfd, const char *path, char *full) {
    if (!path || !path[0]) return -2;
    if (path[0] == '/' || dirfd == -100) { fs_resolve(path, full); return 0; }
    if (dirfd < 3 || dirfd >= LINUX_FD_MAX || !g_linux_fds[dirfd].used ||
        g_linux_fds[dirfd].kind != LINUX_FD_DIR) return -9;
    int n = 0;
    while (g_linux_fds[dirfd].path[n] && n < 79) { full[n] = g_linux_fds[dirfd].path[n]; n++; }
    if (n > 1 && n < 79) full[n++] = '/';
    int i = 0; while (path[i] && n < 79) full[n++] = path[i++];
    full[n] = 0;
    return path[i] ? -36 : 0;
}

static int linux_fd_alloc(int kind, int flags, const char *path, uint16_t dir_cluster, uint32_t offset) {
    for (int fd = 3; fd < LINUX_FD_MAX; fd++) {
        if (g_linux_fds[fd].used) continue;
        g_linux_fds[fd].used = 1;
        g_linux_fds[fd].kind = kind;
        g_linux_fds[fd].flags = flags;
        g_linux_fds[fd].offset = offset;
        g_linux_fds[fd].dir_cluster = dir_cluster;
        int i = 0; while (path && path[i] && i < 79) { g_linux_fds[fd].path[i] = path[i]; i++; }
        g_linux_fds[fd].path[i] = 0;
        return fd;
    }
    return -24;
}

static int linux_openat(int dirfd, const char *path, int flags) {
    char full[80]; int resolved = linux_resolve_at(dirfd, path, full);
    if (resolved < 0) return resolved;
    if (strpfx(full, "/proc/")) {               /* synthetic procfs */
        char tmp[512]; if (proc_synth(full, tmp) < 0) return -2;
        return linux_fd_alloc(LINUX_FD_PROC, flags, full, 0, 0);
    }
    if (name_eq(full, "/dev/null")) return linux_fd_alloc(LINUX_FD_NULL, flags, full, 0, 0);
    uint8_t attr; uint16_t cluster; uint32_t size;
    int exists = fat_resolve(full, &attr, &cluster, &size) == 0;
    if (exists && (attr & 0x10)) {
        if (flags & LINUX_O_ACCMODE) return -21;
        return linux_fd_alloc(LINUX_FD_DIR, flags, full, cluster, 0);
    }
    if (flags & LINUX_O_DIRECTORY) return -20;
    if (!exists) {
        if (!(flags & LINUX_O_CREAT)) return -2;
        if (fat_write(full, "", 0) < 0) return -28;
        size = 0;
    } else if ((flags & LINUX_O_TRUNC) && (flags & LINUX_O_ACCMODE)) {
        if (fat_write(full, "", 0) < 0) return -5;
        size = 0;
    }
    return linux_fd_alloc(LINUX_FD_FILE, flags, full, 0, (flags & LINUX_O_APPEND) ? size : 0);
}

/* ---- pipes (Phase D): a kernel ring buffer with reference-counted ends ------
 * A pipe is shared between its read and write fds (which may live in different
 * processes after fork), so pipes are a small global pool; each fd holds the
 * pool index.  Blocking read/write use the scheduler's sleep/wake, race-free
 * because a syscall runs with interrupts off. */
#define NPIPE     16
#define PIPE_SIZE 4096
struct pipe { uint8_t buf[PIPE_SIZE]; uint32_t r, w, count; int readers, writers; int used; };
static struct pipe g_pipes[NPIPE];
#define PIPE_RCHAN(i) (0x40000000 + (i) * 2)       /* a blocked reader waits here */
#define PIPE_WCHAN(i) (0x40000000 + (i) * 2 + 1)   /* a blocked writer waits here */

static int linux_close(int fd) {
    if (fd < 0 || fd >= LINUX_FD_MAX) return -9;
    if (!g_linux_fds[fd].used) return fd < 3 ? 0 : -9;   /* default console fd: no-op */
    int k = g_linux_fds[fd].kind, idx = g_linux_fds[fd].pipe;
    if (k == LINUX_FD_PIPE_R) { if (g_pipes[idx].readers > 0) g_pipes[idx].readers--; sched_wake(PIPE_WCHAN(idx)); }
    else if (k == LINUX_FD_PIPE_W) { if (g_pipes[idx].writers > 0) g_pipes[idx].writers--; sched_wake(PIPE_RCHAN(idx)); }
    if ((k == LINUX_FD_PIPE_R || k == LINUX_FD_PIPE_W) &&
        g_pipes[idx].readers == 0 && g_pipes[idx].writers == 0) g_pipes[idx].used = 0;
    g_linux_fds[fd].used = 0;
    return 0;
}

/* Close every open fd (process exit): decrements pipe ends so peers see EOF. */
static void linux_close_all_fds(void) {
    for (int fd = 0; fd < LINUX_FD_MAX; fd++)
        if (g_linux_fds[fd].used) linux_close(fd);
}

/* Blocking pipe read: sleep while empty (a writer wakes us); 0 (EOF) once every
 * write end is closed. */
static int pipe_read(int idx, void *buf, uint32_t len) {
    struct pipe *pp = &g_pipes[idx];
    char *out = (char *)buf; uint32_t n = 0;
    for (;;) {
        __asm__ volatile("cli");
        if (pp->count > 0) break;
        if (pp->writers == 0) { __asm__ volatile("sti"); return 0; }
        sched_block(PIPE_RCHAN(idx));
    }
    while (n < len && pp->count > 0) {
        out[n++] = (char)pp->buf[pp->r]; pp->r = (pp->r + 1) % PIPE_SIZE; pp->count--;
    }
    sched_wake(PIPE_WCHAN(idx));
    __asm__ volatile("sti");
    return (int)n;
}

/* Blocking pipe write: sleep while full (a reader wakes us); -EPIPE once every
 * read end is closed. */
static int pipe_write(int idx, const void *buf, uint32_t len) {
    struct pipe *pp = &g_pipes[idx];
    const char *src = (const char *)buf; uint32_t n = 0;
    for (;;) {
        __asm__ volatile("cli");
        if (pp->readers == 0) { __asm__ volatile("sti"); return n ? (int)n : -32; }
        while (n < len && pp->count < PIPE_SIZE) {
            pp->buf[pp->w] = (uint8_t)src[n++]; pp->w = (pp->w + 1) % PIPE_SIZE; pp->count++;
        }
        sched_wake(PIPE_RCHAN(idx));
        if (n >= len) { __asm__ volatile("sti"); return (int)n; }
        sched_block(PIPE_WCHAN(idx));
    }
}

/* pipe2(fds,flags): allocate a pipe object plus a read fd and a write fd. */
static int linux_pipe2(int *fds, int flags) {
    if (!fds) return -14;
    int idx = -1;
    for (int i = 0; i < NPIPE; i++) if (!g_pipes[i].used) { idx = i; break; }
    if (idx < 0) return -24;
    int rfd = linux_fd_alloc(LINUX_FD_PIPE_R, flags, 0, 0, 0);
    if (rfd < 0) return rfd;
    int wfd = linux_fd_alloc(LINUX_FD_PIPE_W, flags | 1, 0, 0, 0);   /* O_WRONLY end */
    if (wfd < 0) { g_linux_fds[rfd].used = 0; return wfd; }
    struct pipe *pp = &g_pipes[idx];
    pp->r = pp->w = pp->count = 0; pp->readers = 1; pp->writers = 1; pp->used = 1;
    g_linux_fds[rfd].pipe = idx; g_linux_fds[wfd].pipe = idx;
    fds[0] = rfd; fds[1] = wfd;
    return 0;
}

/* dup2(old,new): make newfd share oldfd's file/pipe, closing newfd first if open. */
static int linux_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= LINUX_FD_MAX || !g_linux_fds[oldfd].used) return -9;
    if (newfd < 0 || newfd >= LINUX_FD_MAX) return -9;
    if (oldfd == newfd) return newfd;
    if (g_linux_fds[newfd].used) linux_close(newfd);
    g_linux_fds[newfd] = g_linux_fds[oldfd];
    int k = g_linux_fds[newfd].kind;
    if (k == LINUX_FD_PIPE_R) g_pipes[g_linux_fds[newfd].pipe].readers++;
    else if (k == LINUX_FD_PIPE_W) g_pipes[g_linux_fds[newfd].pipe].writers++;
    return newfd;
}

static int linux_dup(int oldfd) {
    if (oldfd < 0 || oldfd >= LINUX_FD_MAX || !g_linux_fds[oldfd].used) return -9;
    for (int fd = 3; fd < LINUX_FD_MAX; fd++)
        if (!g_linux_fds[fd].used) return linux_dup2(oldfd, fd);
    return -24;
}

/* Minimal fcntl(72): F_DUPFD, plus F_GET/SETFD (no close-on-exec tracking yet)
 * and F_GET/SETFL so libc probes succeed. */
static int linux_fcntl(int fd, int cmd, uint64_t arg) {
    if (fd < 0 || fd >= LINUX_FD_MAX) return -9;
    switch (cmd) {
    case 0:                                         /* F_DUPFD */
        for (int nf = (int)arg; nf >= 0 && nf < LINUX_FD_MAX; nf++)
            if (!g_linux_fds[nf].used) return linux_dup2(fd, nf);
        return -24;
    case 1: return 0;                               /* F_GETFD */
    case 2: return 0;                               /* F_SETFD */
    case 3: return g_linux_fds[fd].used ? g_linux_fds[fd].flags : 0;   /* F_GETFL */
    case 4: if (g_linux_fds[fd].used) g_linux_fds[fd].flags = (int)arg; return 0; /* F_SETFL */
    default: return 0;
    }
}

/* ---- canonical tty line editor: Up/Down history + Left/Right cursor edit ----
 * Reads one keyboard line, rendering edits to the active terminal with ANSI
 * cursor sequences (ESC[C/D/K).  Returns the length incl. the trailing '\n', or
 * 0 for EOF (Ctrl-D on an empty line). */
#define TTY_HIST_N 16
static char g_tty_hist[TTY_HIST_N][256];
static int  g_tty_histn;

static void tty_emit(const char *s, int n) { usys_write(s, (unsigned long)n); }
static void tty_bs(int n) { while (n-- > 0) tty_emit("\x1b[D", 3); }   /* cursor left  */
static void tty_fw(int n) { while (n-- > 0) tty_emit("\x1b[C", 3); }   /* cursor right */

static void tty_hist_push(const char *s, int n) {
    if (n <= 0) return;
    if (n > 255) n = 255;
    if (g_tty_histn == TTY_HIST_N) {                       /* full: drop the oldest */
        for (int i = 1; i < TTY_HIST_N; i++)
            for (int j = 0; j < 256; j++) g_tty_hist[i - 1][j] = g_tty_hist[i][j];
        g_tty_histn--;
    }
    int i = 0; for (; i < n; i++) g_tty_hist[g_tty_histn][i] = s[i];
    g_tty_hist[g_tty_histn][i] = 0; g_tty_histn++;
}

static int tty_read_line(char *out, int max) {
    char line[256]; int len = 0, pos = 0;
    int hidx = g_tty_histn;                                 /* == histn: a fresh (unsaved) line */
    for (;;) {
        int key = kbd64_getkey(); if (key < 0) key = hvkbd_getkey();
        if (key < 0) { wm_pump(); continue; }
        if (key == 3 && (g_tty.lflag & TTY_ISIG)) {         /* Ctrl-C: discard line, reprompt */
            signal_pgrp(g_tty.fg_pgrp, 2);
            tty_emit("\n", 1); out[0] = '\n'; return 1;
        }
        if (key == 4) { if (len == 0) return 0; continue; } /* Ctrl-D: EOF only on an empty line */
        if (key == '\r' || key == '\n') {
            tty_emit("\n", 1);
            tty_hist_push(line, len);
            int n = 0; for (int i = 0; i < len && n < max - 2; i++) out[n++] = line[i];
            out[n++] = '\n'; return n;
        }
        if (key == KEY_LEFT)  { if (pos > 0) { pos--; tty_bs(1); } continue; }
        if (key == KEY_RIGHT) { if (pos < len) { pos++; tty_fw(1); } continue; }
        if (key == KEY_UP || key == KEY_DOWN) {             /* history: replace the whole line */
            int ni = hidx + (key == KEY_UP ? -1 : 1);
            if (ni < 0) ni = 0;
            if (ni > g_tty_histn) ni = g_tty_histn;
            if (ni == hidx) continue;
            hidx = ni;
            tty_bs(pos); tty_emit("\x1b[K", 3);             /* cursor to start, clear to EOL */
            len = 0;
            if (hidx < g_tty_histn)
                while (g_tty_hist[hidx][len] && len < 255) { line[len] = g_tty_hist[hidx][len]; len++; }
            tty_emit(line, len); pos = len;
            continue;
        }
        if (key == '\b' || key == 0x7f) {                   /* backspace: delete before cursor */
            if (pos > 0) {
                for (int i = pos - 1; i < len - 1; i++) line[i] = line[i + 1];
                len--; pos--;
                tty_bs(1); tty_emit(&line[pos], len - pos); tty_emit("\x1b[K", 3); tty_bs(len - pos);
            }
            continue;
        }
        if (key >= 0x20 && key < 0x7f) {                    /* printable: insert at the cursor */
            if (len < 255 && len < max - 2) {
                for (int i = len; i > pos; i--) line[i] = line[i - 1];
                line[pos] = (char)key; len++;
                tty_emit(&line[pos], len - pos); tty_bs(len - pos - 1); pos++;
            }
            continue;
        }
    }
}

static int linux_read(int fd, void *buf, uint32_t len) {
    if (fd < 0 || fd >= LINUX_FD_MAX) return -9;
    if (g_linux_fds[fd].used) {
        int k = g_linux_fds[fd].kind;
        if (k == LINUX_FD_PIPE_R) return pipe_read(g_linux_fds[fd].pipe, buf, len);
        if (k == LINUX_FD_PIPE_W) return -9;
        if (k == LINUX_FD_PROC) {
            int r = proc_read(g_linux_fds[fd].path, (char *)buf, len, g_linux_fds[fd].offset);
            if (r > 0) g_linux_fds[fd].offset += (uint32_t)r;
            return r;
        }
        if (k == LINUX_FD_NULL) return 0;
        if (k != LINUX_FD_FILE) return -21;
        if ((g_linux_fds[fd].flags & LINUX_O_ACCMODE) == 1) return -9;
        int result = fat_pread(g_linux_fds[fd].path, buf, len, g_linux_fds[fd].offset);
        if (result > 0) g_linux_fds[fd].offset += (uint32_t)result;
        return result < 0 ? -5 : result;
    }
    if (fd == 0) {                                     /* stdin: keyboard */
        if (!(g_tty.lflag & TTY_ICANON)) {             /* raw mode (vi etc.): keys as they arrive, no echo */
            char *out = (char *)buf; uint32_t count = 0;
            while (count < len) {
                int key = kbd64_getkey(); if (key < 0) key = hvkbd_getkey();
                if (key < 0) { if (count) break; wm_pump(); continue; }   /* block for the first byte */
                if (key == 3 && (g_tty.lflag & TTY_ISIG)) { signal_pgrp(g_tty.fg_pgrp, 2); break; }
                if (key > KEY_RIGHT) continue;         /* drop F-keys; keep arrows (0x80-0x83) */
                if (key == '\r') key = '\n';
                out[count++] = (char)key;
            }
            return (int)count;
        }
        static char cook[256]; static int cook_len, cook_pos;   /* canonical cooked line */
        if (cook_pos >= cook_len) {                    /* buffer drained: edit a fresh line */
            cook_pos = 0;
            cook_len = tty_read_line(cook, sizeof cook);
            if (cook_len <= 0) { cook_len = 0; return 0; }   /* Ctrl-D EOF */
        }
        char *out = (char *)buf; uint32_t count = 0;
        while (count < len && cook_pos < cook_len) out[count++] = cook[cook_pos++];
        return (int)count;
    }
    return -9;
}

static int linux_write(int fd, const void *buf, uint32_t len) {
    if (fd < 0 || fd >= LINUX_FD_MAX) return -9;
    if (g_linux_fds[fd].used) {
        int k = g_linux_fds[fd].kind;
        if (k == LINUX_FD_PIPE_W) return pipe_write(g_linux_fds[fd].pipe, buf, len);
        if (k == LINUX_FD_PIPE_R) return -9;
        if (k == LINUX_FD_NULL) return (int)len;
        if (k != LINUX_FD_FILE) return -21;
        if (!(g_linux_fds[fd].flags & LINUX_O_ACCMODE)) return -9;
        if (g_linux_fds[fd].flags & LINUX_O_APPEND) {
            uint8_t attr; uint16_t cluster; uint32_t size;
            if (fat_resolve(g_linux_fds[fd].path, &attr, &cluster, &size) < 0) return -5;
            g_linux_fds[fd].offset = size;
        }
        int result = fat_pwrite(g_linux_fds[fd].path, buf, len, g_linux_fds[fd].offset);
        if (result > 0) g_linux_fds[fd].offset += (uint32_t)result;
        return result < 0 ? -5 : result;
    }
    if (fd == 1 || fd == 2) { usys_write((const char *)buf, len); return (int)len; }
    return -9;
}

static int64_t linux_lseek(int fd, int64_t offset, int whence) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind == LINUX_FD_DIR) return -29;
    if (g_linux_fds[fd].kind == LINUX_FD_NULL) return 0;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = g_linux_fds[fd].offset;
    else if (whence == 2) {
        uint8_t attr; uint16_t cluster; uint32_t size;
        if (fat_resolve(g_linux_fds[fd].path, &attr, &cluster, &size) < 0) return -5;
        base = size;
    } else return -22;
    if (offset < -base || base + offset > 0xFFFFFFFFLL) return -22;
    g_linux_fds[fd].offset = (uint32_t)(base + offset);
    return base + offset;
}

struct linux_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
};

static void linux_fill_stat(struct linux_stat *st, int kind, uint16_t cluster, uint32_t size) {
    memset(st, 0, sizeof(*st));
    st->ino = cluster ? cluster : 1;
    st->nlink = kind == LINUX_FD_DIR ? 2 : 1;
    st->mode = kind == LINUX_FD_DIR ? 0040755 : kind == LINUX_FD_FILE ? 0100644 : 0020666;
    st->size = kind == LINUX_FD_FILE ? size : 0;
    st->blksize = 512;
    st->blocks = (size + 511) / 512;
}

static int linux_stat_path(const char *path, struct linux_stat *st) {
    if (name_eq(path, "/dev/null")) { linux_fill_stat(st, LINUX_FD_NULL, 0, 0); return 0; }
    uint8_t attr; uint16_t cluster; uint32_t size;
    if (fat_resolve(path, &attr, &cluster, &size) < 0) return -2;
    linux_fill_stat(st, (attr & 0x10) ? LINUX_FD_DIR : LINUX_FD_FILE, cluster, size);
    return 0;
}

static int linux_fstat(int fd, void *statbuf) {
    if (!statbuf) return -14;
    if (fd >= 0 && fd <= 2) { linux_fill_stat((struct linux_stat *)statbuf, LINUX_FD_NULL, 0, 0); return 0; }
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    return linux_stat_path(g_linux_fds[fd].path, (struct linux_stat *)statbuf);
}

static int linux_ftruncate(int fd, uint64_t size) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind != LINUX_FD_FILE) return -22;
    if (!(g_linux_fds[fd].flags & LINUX_O_ACCMODE)) return -22;
    if (size > 0xFFFFFFFFULL) return -27;
    return fat_truncate(g_linux_fds[fd].path, (uint32_t)size) < 0 ? -5 : 0;
}

static int linux_newfstatat(int dirfd, const char *path, void *statbuf, int flags) {
    (void)flags;
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    return linux_stat_path(full, (struct linux_stat *)statbuf);
}

static int linux_getdents64(int fd, void *buf, uint32_t len) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind != LINUX_FD_DIR) return -20;
    uint8_t *out = (uint8_t *)buf; uint32_t used = 0;
    for (;;) {
        char name[16]; uint8_t attr; uint16_t cluster; uint32_t size;
        if (fat_list(g_linux_fds[fd].dir_cluster, (int)g_linux_fds[fd].offset,
                     name, &attr, &cluster, &size) < 0) break;
        (void)size;
        uint32_t name_len = 0; while (name[name_len]) name_len++;
        uint16_t reclen = (uint16_t)((19 + name_len + 1 + 7) & ~7U);
        if (used + reclen > len) break;
        uint8_t *entry = out + used; memset(entry, 0, reclen);
        *(uint64_t *)(entry + 0) = cluster ? cluster : 1;
        *(int64_t *)(entry + 8) = (int64_t)g_linux_fds[fd].offset + 1;
        *(uint16_t *)(entry + 16) = reclen;
        entry[18] = (attr & 0x10) ? 4 : 8;
        for (uint32_t i = 0; i <= name_len; i++) entry[19 + i] = (uint8_t)name[i];
        used += reclen; g_linux_fds[fd].offset++;
    }
    return (int)used;
}

static int linux_mkdirat(int dirfd, const char *path) {
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    return fat_mkdir(full) < 0 ? -17 : 0;
}

/* rmdir(84): remove an empty directory.  fat_rmdir codes -> Linux errnos. */
static int linux_rmdir(const char *path) {
    char full[80]; int result = linux_resolve_at(-100, path, full);
    if (result < 0) return result;
    result = fat_rmdir(full);
    return result == 0 ? 0 : result == -2 ? -20 : result == -3 ? -39 : -2;
}

/* rename(82): FAT has no atomic rename, so copy-then-unlink for files; refuse
 * directory renames with -EXDEV so userland can fall back to recursive copy. */
static int linux_rename(const char *oldp, const char *newp) {
    char fo[80], fn[80];
    int a = linux_resolve_at(-100, oldp, fo); if (a < 0) return a;
    int b = linux_resolve_at(-100, newp, fn); if (b < 0) return b;
    uint8_t attr; uint16_t cluster; uint32_t size;
    if (fat_resolve(fo, &attr, &cluster, &size) < 0) return -2;   /* ENOENT */
    if (attr & 0x10) return -18;                                  /* EXDEV: dir rename n/a */
    uint8_t *buf = (uint8_t *)kmalloc(size ? size : 1);
    if (!buf) return -12;
    int r = size ? fat_read(cluster, size, buf, size) : 0;
    int w = (r == (int)size) ? fat_write(fn, buf, size) : -1;
    kfree(buf);
    if (w < 0) return -5;
    fat_unlink(fo);
    return 0;
}

static int linux_unlinkat(int dirfd, const char *path, int flags) {
    if (flags) return -22;
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    result = fat_unlink(full);
    return result == -2 ? -21 : result < 0 ? -2 : 0;
}

static uint64_t linux_getcwd(char *buf, uint64_t size) {
    if (!buf || !size) return (uint64_t)-22;
    char cwd[80]; fs_getcwd(cwd);
    uint64_t len = 0; while (cwd[len]) len++;
    if (len + 1 > size) return (uint64_t)-34;
    for (uint64_t i = 0; i <= len; i++) buf[i] = cwd[i];
    return (uint64_t)buf;
}

static int linux_arch_prctl(int code, uint64_t addr) {
    if (code == 0x1002) {                   /* ARCH_SET_FS */
        if (addr >= 0x0000800000000000ULL) return -22;
        write_fs_base(addr);
        return 0;
    }
    if (code == 0x1003) {                   /* ARCH_GET_FS */
        if (!addr) return -14;
        *(uint64_t *)addr = g_linux_fs_base;
        return 0;
    }
    return -22;
}

struct linux_timespec { int64_t sec, nsec; };

static int linux_clock_gettime(int clock_id, void *timespec) {
    if (!timespec) return -14;
    if (clock_id != 0 && clock_id != 1 && clock_id != 4) return -22;
    uint64_t ms = millis();
    struct linux_timespec *ts = (struct linux_timespec *)timespec;
    if (clock_id == 0) {                        /* CLOCK_REALTIME: wall clock */
        uint64_t w = wall_epoch();
        ts->sec = (int64_t)(w ? w : ms / 1000);
    } else {                                    /* CLOCK_MONOTONIC(_RAW): uptime */
        ts->sec = (int64_t)(ms / 1000);
    }
    ts->nsec = (int64_t)((ms % 1000) * 1000000);
    return 0;
}

static uint64_t linux_random_state(void) {
    static uint64_t state;
    if (!state) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        state = ((uint64_t)hi << 32) | lo;
        state ^= (uint64_t)(uintptr_t)&state ^ millis();
    }
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}

static int linux_getrandom(void *buf, uint32_t len, uint32_t flags) {
    if (!buf || (flags & ~3U)) return -22;
    uint8_t *out = (uint8_t *)buf;
    uint64_t random = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!(i & 7)) random = linux_random_state();
        out[i] = (uint8_t)(random >> ((i & 7) * 8));
    }
    return (int)len;
}

struct linux_iovec { const void *base; uint64_t len; };

static int linux_writev(int fd, const void *iov, int count) {
    if (!iov || count < 0 || count > 1024) return -22;
    const struct linux_iovec *vec = (const struct linux_iovec *)iov;
    int total = 0;
    for (int i = 0; i < count; i++) {
        if (vec[i].len > 0x7FFFFFFFULL) return total ? total : -22;
        int result = linux_write(fd, vec[i].base, (uint32_t)vec[i].len);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result != vec[i].len) break;
    }
    return total;
}

static int linux_ioctl(int fd, uint64_t request, void *arg) {
    if (fd < 0 || fd > 2) return -25;
    switch (request) {
    case 0x5413: {                          /* TIOCGWINSZ */
        if (!arg) return -14;
        struct { uint16_t rows, cols, xpixels, ypixels; } *ws = arg;
        int cols = 0, rows = 0; term_active_size(&cols, &rows);
        ws->rows = (uint16_t)rows; ws->cols = (uint16_t)cols;
        ws->xpixels = 0; ws->ypixels = 0;
        return 0;
    }
    case 0x5401: {                          /* TCGETS */
        if (!arg) return -14;
        memset(arg, 0, 60);
        uint32_t *t = (uint32_t *)arg;      /* c_iflag,c_oflag,c_cflag,c_lflag */
        t[0] = 0x0500; t[1] = 0x0005; t[2] = 0x00BF; t[3] = g_tty.lflag;
        ((uint8_t *)arg)[17] = 3;           /* c_cc[VINTR] = Ctrl-C */
        ((uint8_t *)arg)[21] = 4;           /* c_cc[VEOF]  = Ctrl-D */
        return 0;
    }
    case 0x5402: case 0x5403: case 0x5404:  /* TCSETS / TCSETSW / TCSETSF */
        if (!arg) return -14;
        g_tty.lflag = ((const uint32_t *)arg)[3];
        return 0;
    case 0x540F:                            /* TIOCGPGRP */
        if (!arg) return -14;
        *(int *)arg = g_tty.fg_pgrp ? g_tty.fg_pgrp : (current->pgid ? current->pgid : current->pid);
        return 0;
    case 0x5410:                            /* TIOCSPGRP */
        if (!arg) return -14;
        g_tty.fg_pgrp = *(const int *)arg;
        return 0;
    default:
        return -25;
    }
}

static uint64_t *linux_vm_pt(uint64_t va, int create) {
    if (!g_linux_vm.pml4) return 0;
    uint64_t *table = g_linux_vm.pml4;
    int shifts[3] = { 39, 30, 21 };
    for (int level = 0; level < 3; level++) {
        int index = (int)((va >> shifts[level]) & 0x1FF);
        if (!(table[index] & PTE_P)) {
            if (!create) return 0;
            uint64_t *next = copy_page_table(0);
            if (!next) return 0;
            table[index] = make_table_entry(next);
        } else if (table[index] & PTE_PS) {
            return 0;
        }
        table = (uint64_t *)(table[index] & ~0xFFFULL);
    }
    return table;
}

static uint64_t *linux_vm_pte(uint64_t va, int create) {
    uint64_t *pt = linux_vm_pt(va, create);
    return pt ? &pt[(va >> 12) & 0x1FF] : 0;
}

static int linux_map_page(uint64_t va, uint64_t prot) {
    uint64_t *pte = linux_vm_pte(va, 1);
    if (!pte || (*pte & ~0xFFFULL)) return -1;
    void *frame = page_alloc(1);
    if (!frame) return -1;
    *pte = (uint64_t)frame | PTE_US | (prot ? PTE_P : 0) | ((prot & 2) ? PTE_RW : 0);
    return 0;
}

/* G6: map a buffer object's existing frames into the Linux address space so a
 * userspace client renders straight into the buffer the compositor samples —
 * zero copy.  Returns the user VA (0 on failure). */
uint64_t arch_bo_map_user(void *kva, uint32_t bytes) {
    if (!g_linux_vm.pml4 || !kva || !bytes) return 0;
    uint32_t pages = (bytes + 4095) / 4096;
    uint64_t va = g_linux_vm.mmap_next;
    if (va + (uint64_t)pages * 4096 > LINUX_MMAP_LIMIT) return 0;
    uint64_t phys = (uint64_t)(uintptr_t)kva;          /* frames are identity-mapped */
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t *pte = linux_vm_pte(va + (uint64_t)i * 4096, 1);
        if (!pte) return 0;
        *pte = (phys + (uint64_t)i * 4096) | PTE_P | PTE_RW | PTE_US;
    }
    g_linux_vm.mmap_next += (uint64_t)pages * 4096;
    return va;
}

/* Create a BO and build a fresh user page-table hierarchy mapping a user VA to
 * the BO's frames, then walk it back: the user VA resolves to the BO's own
 * physical frames (= zero copy).  Self-contained (no active AS).  0 ok, -1 fail. */
int arch_bo_zerocopy_test(void) {
    tgl_bo_t *bo = tgl_bo_create(16, 16);
    if (!bo) return -1;
    uint64_t phys = (uint64_t)(uintptr_t)bo->surf.px;
    uint64_t va = LINUX_MMAP_BASE;
    uint64_t *pml4 = (uint64_t *)page_alloc(1), *pdpt = (uint64_t *)page_alloc(1);
    uint64_t *pd = (uint64_t *)page_alloc(1), *pt = (uint64_t *)page_alloc(1);
    int r = -1;
    if (pml4 && pdpt && pd && pt) {                    /* frames are identity-mapped: VA == phys */
        pml4[(va >> 39) & 0x1FF] = (uint64_t)pdpt | PTE_P | PTE_RW | PTE_US;
        pdpt[(va >> 30) & 0x1FF] = (uint64_t)pd   | PTE_P | PTE_RW | PTE_US;
        pd[(va >> 21) & 0x1FF]   = (uint64_t)pt   | PTE_P | PTE_RW | PTE_US;
        pt[(va >> 12) & 0x1FF]   = phys | PTE_P | PTE_RW | PTE_US;
        uint64_t *t = pml4;
        t = (uint64_t *)(t[(va >> 39) & 0x1FF] & ~0xFFFULL);
        t = (uint64_t *)(t[(va >> 30) & 0x1FF] & ~0xFFFULL);
        t = (uint64_t *)(t[(va >> 21) & 0x1FF] & ~0xFFFULL);
        r = ((t[(va >> 12) & 0x1FF] & ~0xFFFULL) == phys) ? 0 : -1;
    }
    if (pt) page_free(pt, 1);
    if (pd) page_free(pd, 1);
    if (pdpt) page_free(pdpt, 1);
    if (pml4) page_free(pml4, 1);
    tgl_bo_unref(bo);
    return r;
}

/* ---- G7: graphics client (KMD) ----------------------------------------
 * Back the TinyGL user API (programs/tcc_include/tinygl.h): allocate buffer
 * objects, map them zero-copy into the calling process, and composite+present
 * them through the display backend.  An app renders into the mapped BO and the
 * compositor samples it in place — no pixel copy crosses the syscall boundary. */
#define GFX_MAX_BO 16
static struct gfx_slot { tgl_bo_t *bo; uint64_t uva, bytes; uint64_t *owner; } g_gfx[GFX_MAX_BO];

/* Channel shifts that reproduce fb64's device packing (probed via fb_pack). */
static void gfx_shifts(uint32_t *rsh, uint32_t *gsh, uint32_t *bsh) {
    int bgrx = (fb_pack(0xFF0000) == 0x00FF0000u);   /* R in the high byte => BGRX */
    *rsh = bgrx ? 16 : 0; *gsh = 8; *bsh = bgrx ? 0 : 16;
}

static int64_t gfx_info(void *user_info) {
    if (!user_info) return -14;                      /* EFAULT */
    struct { uint32_t w, h, stride, rsh, gsh, bsh; } inf;
    uint32_t w = 0, h = 0;
    disp_dims(&w, &h);
    inf.w = w; inf.h = h; inf.stride = w;
    gfx_shifts(&inf.rsh, &inf.gsh, &inf.bsh);
    for (uint32_t i = 0; i < sizeof inf; i++) ((uint8_t *)user_info)[i] = ((uint8_t *)&inf)[i];
    return 0;
}

static int64_t gfx_bo_create(uint32_t w, uint32_t h, void *user_desc) {
    if (!user_desc || !w || !h || w > 4096 || h > 4096) return -22;   /* EINVAL */
    int slot = -1;
    for (int i = 0; i < GFX_MAX_BO; i++) if (!g_gfx[i].bo) { slot = i; break; }
    if (slot < 0) return -24;                        /* EMFILE */
    tgl_bo_t *bo = tgl_bo_create(w, h);
    if (!bo) return -12;                             /* ENOMEM */
    uint64_t bytes = (uint64_t)bo->pages * 4096;
    uint64_t uva = arch_bo_map_user(bo->surf.px, (uint32_t)bytes);
    if (!uva) { tgl_bo_unref(bo); return -12; }
    g_gfx[slot].bo = bo; g_gfx[slot].uva = uva; g_gfx[slot].bytes = bytes;
    g_gfx[slot].owner = current->vm.pml4;
    struct { uint64_t va; uint32_t w, h, stride, rsh, gsh, bsh; int32_t handle; uint32_t pad; } d;
    d.va = uva; d.w = bo->surf.w; d.h = bo->surf.h; d.stride = bo->surf.w;
    gfx_shifts(&d.rsh, &d.gsh, &d.bsh);
    d.handle = slot; d.pad = 0;
    for (uint32_t i = 0; i < sizeof d; i++) ((uint8_t *)user_desc)[i] = ((uint8_t *)&d)[i];
    return 0;
}

static int64_t gfx_present(int handle, int x, int y) {
    if (handle < 0 || handle >= GFX_MAX_BO || !g_gfx[handle].bo) return -22;
    tgl_bind(tgl_screen());                          /* composite BO -> back buffer -> display */
    tgl_compose(&g_gfx[handle].bo->surf, x, y);
    tgl_present();
    int k = kbd64_getkey();
    if (k < 0) k = hvkbd_getkey();
    return k < 0 ? 0 : k;
}

static int64_t gfx_bo_destroy(int handle) {
    if (handle < 0 || handle >= GFX_MAX_BO || !g_gfx[handle].bo) return -22;
    if (g_gfx[handle].owner == current->vm.pml4) {   /* unmap only from the owning AS */
        for (uint64_t o = 0; o < g_gfx[handle].bytes; o += 4096) {
            uint64_t *pte = linux_vm_pte(g_gfx[handle].uva + o, 0);
            if (pte) *pte = 0;
        }
        write_cr3((uint64_t)current->vm.pml4);       /* flush the stale TLB entries */
    }
    tgl_bo_unref(g_gfx[handle].bo);
    g_gfx[handle].bo = 0; g_gfx[handle].uva = g_gfx[handle].bytes = 0; g_gfx[handle].owner = 0;
    return 0;
}

static uint64_t page_up(uint64_t value) { return (value + 4095) & ~4095ULL; }

static int linux_vm_range_owned(uint64_t addr, uint64_t len) {
    if (!len || addr + len < addr) return 0;
    uint64_t end = addr + len;
    uint64_t brk_top = page_up(g_linux_vm.brk);
    if (addr >= LINUX_BRK_BASE && end <= brk_top) return 1;
    return addr >= LINUX_MMAP_BASE && end <= g_linux_vm.mmap_next;
}

static uint64_t linux_brk(uint64_t requested) {
    if (!g_linux_vm.pml4) return (uint64_t)-12;
    if (!requested) return g_linux_vm.brk;
    if (requested < LINUX_BRK_BASE || requested > LINUX_BRK_LIMIT) return g_linux_vm.brk;
    uint64_t old_top = page_up(g_linux_vm.brk);
    uint64_t new_top = page_up(requested);
    if (new_top > old_top) {
        uint64_t va = old_top;
        for (; va < new_top; va += 4096) {
            if (linux_map_page(va, 3) == 0) continue;
            for (uint64_t rollback = old_top; rollback < va; rollback += 4096) {
                uint64_t *pte = linux_vm_pte(rollback, 0);
                if (pte) *pte = 0;
            }
            flush_tlb();
            return g_linux_vm.brk;
        }
    } else {
        for (uint64_t va = new_top; va < old_top; va += 4096) {
            uint64_t *pte = linux_vm_pte(va, 0);
            if (pte) *pte = 0;
        }
        flush_tlb();
    }
    g_linux_vm.brk = requested;
    return requested;
}

static uint64_t linux_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                           uint64_t flags, uint64_t fd, uint64_t offset) {
    if (!g_linux_vm.pml4 || addr || !len || len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE ||
        (flags & 0x22) != 0x22 || fd != (uint64_t)-1 || offset)
        return (uint64_t)-22;
    len = page_up(len);
    uint64_t base = page_up(g_linux_vm.mmap_next);
    if (base > LINUX_MMAP_LIMIT || len > LINUX_MMAP_LIMIT - base) return (uint64_t)-12;
    uint64_t va = base;
    for (; va < base + len; va += 4096) {
        if (linux_map_page(va, prot) == 0) continue;
        for (uint64_t rollback = base; rollback < va; rollback += 4096) {
            uint64_t *pte = linux_vm_pte(rollback, 0);
            if (pte) *pte = 0;
        }
        flush_tlb();
        return (uint64_t)-12;
    }
    g_linux_vm.mmap_next = base + len;
    flush_tlb();
    return base;
}

static int linux_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    if (!g_linux_vm.pml4 || (addr & 4095) || !len ||
        len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE) return -22;
    len = page_up(len);
    if (!linux_vm_range_owned(addr, len)) return -12;
    for (uint64_t va = addr; va < addr + len; va += 4096) {
        uint64_t *pte = linux_vm_pte(va, 0);
        if (!pte || !(*pte & ~0xFFFULL)) return -12;
        *pte = (*pte & ~(PTE_P | PTE_RW)) | (prot ? PTE_P : 0) |
               ((prot & 2) ? PTE_RW : 0) | PTE_US;
    }
    flush_tlb();
    return 0;
}

static int linux_munmap(uint64_t addr, uint64_t len) {
    if (!g_linux_vm.pml4 || (addr & 4095) || !len ||
        len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE) return -22;
    len = page_up(len);
    if (!linux_vm_range_owned(addr, len)) return -12;
    for (uint64_t va = addr; va < addr + len; va += 4096) {
        uint64_t *pte = linux_vm_pte(va, 0);
        if (pte) *pte = 0;
    }
    flush_tlb();
    return 0;
}

static uint64_t *linux_address_space(const uint64_t code_pte[LINUX_LOAD_PAGES],
                                     uint8_t *code, uint64_t code_pages,
                                     uint8_t *stk) {
    uint64_t *kpml4 = (uint64_t *)g_kernel_cr3;
    uint64_t *pml4 = copy_page_table(kpml4);
    if (!pml4) return 0;

    int l4 = (int)((LINUX_LOAD_VA >> 39) & 0x1FF);
    int l3 = (int)((LINUX_LOAD_VA >> 30) & 0x1FF);
    int l2 = (int)((LINUX_LOAD_VA >> 21) & 0x1FF);
    uint64_t *old_pdpt = (uint64_t *)(kpml4[l4] & ~0xFFFULL);
    if (!(kpml4[l4] & PTE_P) || (kpml4[l4] & PTE_PS)) return 0;
    uint64_t *pdpt = copy_page_table(old_pdpt);
    if (!pdpt || !(old_pdpt[l3] & PTE_P) || (old_pdpt[l3] & PTE_PS)) return 0;
    uint64_t *old_pd = (uint64_t *)(old_pdpt[l3] & ~0xFFFULL);
    uint64_t *pd = copy_page_table(old_pd);
    uint64_t *code_pt = copy_page_table(0);
    if (!pd || !code_pt) return 0;
    pml4[l4] = make_table_entry(pdpt);
    pdpt[l3] = make_table_entry(pd);
    pd[l2] = make_table_entry(code_pt);
    int code_index = (int)((LINUX_LOAD_VA >> 12) & 0x1FF);
    for (uint64_t i = 0; i < code_pages; i++) {
        if (code_pte[i])
            code_pt[code_index + i] = (uint64_t)(code + i * 4096) | code_pte[i];
    }

    int s4 = (int)((LINUX_STK_TOP >> 39) & 0x1FF);
    int s3 = (int)((LINUX_STK_TOP >> 30) & 0x1FF);
    int s2 = (int)((LINUX_STK_TOP >> 21) & 0x1FF);
    if (kpml4[s4] & PTE_P) return 0;
    uint64_t *stack_pdpt = copy_page_table(0);
    uint64_t *stack_pd = copy_page_table(0);
    uint64_t *stack_pt = copy_page_table(0);
    if (!stack_pdpt || !stack_pd || !stack_pt) return 0;
    pml4[s4] = make_table_entry(stack_pdpt);
    stack_pdpt[s3] = make_table_entry(stack_pd);
    stack_pd[s2] = make_table_entry(stack_pt);
    int stack_index = (int)((LINUX_STK_TOP >> 12) & 0x1FF);
    for (int i = 0; i < USER_STK_PAGES; i++)
        stack_pt[stack_index - 1 - i] = (uint64_t)(stk + (uint64_t)i * 4096) | PTE_P | PTE_RW | PTE_US;

    pml4[(USER_CODE_VA >> 39) & 0x1FF] = 0;
    pml4[(LINUX_BRK_BASE >> 39) & 0x1FF] = 0;
    pml4[(LINUX_MMAP_BASE >> 39) & 0x1FF] = 0;
    return pml4;
}

struct linux_image { uint64_t *pml4; uint64_t entry, rsp; uint8_t *code; };

/* Validate an ELF64, map its PT_LOAD segments into a fresh address space, and
 * build the SysV initial stack (argv/envp/auxv) in `stk`.  Shared by program
 * launch (run_linux_elf_frames) and execve (do_execve_inplace). */
static int linux_load_image(const uint8_t *img, uint64_t size, uint8_t *stk,
                            const struct linux_exec_args *args, struct linux_image *out) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;
    if (!stk || size < sizeof(*eh)) return -1;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->type != 2 || eh->machine != 62 || eh->version != 1 ||
        eh->phentsize != sizeof(struct elf64_phdr) || !eh->phnum ||
        eh->phoff > size || (uint64_t)eh->phnum > (size - eh->phoff) / eh->phentsize)
        return -2;

    uint64_t max_end = LINUX_LOAD_VA;
    const struct elf64_phdr *ph = (const struct elf64_phdr *)(img + eh->phoff);
    int loads = 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1) continue;
        if (ph[i].filesz > ph[i].memsz || ph[i].offset > size ||
            ph[i].filesz > size - ph[i].offset || ph[i].vaddr < LINUX_LOAD_VA ||
            ph[i].memsz > LINUX_LOAD_LIMIT - LINUX_LOAD_VA ||
            ph[i].vaddr - LINUX_LOAD_VA > LINUX_LOAD_LIMIT - LINUX_LOAD_VA - ph[i].memsz)
            return -3;
        if (ph[i].vaddr + ph[i].memsz > max_end) max_end = ph[i].vaddr + ph[i].memsz;
        loads++;
    }
    uint64_t code_pages = page_up(max_end - LINUX_LOAD_VA) / 4096;
    uint8_t *code = (uint8_t *)page_alloc(code_pages);
    if (!loads || !code) return -5;
    uint64_t code_pte[LINUX_LOAD_PAGES];
    for (uint64_t i = 0; i < LINUX_LOAD_PAGES; i++) code_pte[i] = 0;
    uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1) continue;
        uint64_t dst = ph[i].vaddr - LINUX_LOAD_VA;
        for (uint64_t j = 0; j < ph[i].filesz; j++) code[dst + j] = img[ph[i].offset + j];
        uint64_t first_page = dst / 4096;
        uint64_t end_page = page_up(dst + ph[i].memsz) / 4096;
        for (uint64_t page = first_page; page < end_page; page++)
            code_pte[page] |= PTE_P | PTE_US | ((ph[i].flags & 2) ? PTE_RW : 0);
        if (eh->phoff >= ph[i].offset &&
            eh->phoff + (uint64_t)eh->phnum * eh->phentsize <= ph[i].offset + ph[i].filesz)
            phdr_addr = ph[i].vaddr + eh->phoff - ph[i].offset;
    }
    if (!phdr_addr || eh->entry < LINUX_LOAD_VA || eh->entry >= max_end) return -4;

    uint64_t *process_pml4 = linux_address_space(code_pte, code, code_pages, stk);
    if (!process_pml4) return -5;

    static const char default_argv0[] = "linux-abi-selftest";
    uint64_t cursor = LINUX_STK_TOP;
    uint64_t argv_ptr[LINUX_EXEC_MAX], envp_ptr[LINUX_EXEC_MAX];
    int argc = args ? args->argc : 1;
    int envc = args ? args->envc : 0;
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = 1; while (args->envp[i][len - 1]) len++;
        cursor -= len;
        memcpy(stk + 4096 - (LINUX_STK_TOP - cursor), args->envp[i], len);
        envp_ptr[i] = cursor;
    }
    for (int i = argc - 1; i >= 0; i--) {
        const char *src = args ? args->argv[i] : default_argv0;
        uint64_t len = 1; while (src[len - 1]) len++;
        cursor -= len;
        memcpy(stk + 4096 - (LINUX_STK_TOP - cursor), src, len);
        argv_ptr[i] = cursor;
    }
    uint64_t randomp = (cursor & ~15ULL) - 16;
    uint8_t *random_bytes = stk + 4096 - (LINUX_STK_TOP - randomp);
    uint64_t random0 = linux_random_state(), random1 = linux_random_state();
    for (int i = 0; i < 8; i++) {
        random_bytes[i] = (uint8_t)(random0 >> (i * 8));
        random_bytes[8 + i] = (uint8_t)(random1 >> (i * 8));
    }
    uint64_t words = (uint64_t)(1 + argc + 1 + envc + 1 + 26);
    uint64_t rsp = (randomp - words * sizeof(uint64_t)) & ~15ULL;
    uint64_t *sp = (uint64_t *)(stk + 4096 - (LINUX_STK_TOP - rsp));
    uint64_t n = 0;
    sp[n++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) sp[n++] = argv_ptr[i];
    sp[n++] = 0;
    for (int i = 0; i < envc; i++) sp[n++] = envp_ptr[i];
    sp[n++] = 0;
    sp[n++] = 3;  sp[n++] = phdr_addr;         /* AT_PHDR                      */
    sp[n++] = 4;  sp[n++] = eh->phentsize;     /* AT_PHENT                     */
    sp[n++] = 5;  sp[n++] = eh->phnum;         /* AT_PHNUM                     */
    sp[n++] = 6;  sp[n++] = 4096;              /* AT_PAGESZ                    */
    sp[n++] = 9;  sp[n++] = eh->entry;         /* AT_ENTRY                     */
    sp[n++] = 11; sp[n++] = 0;                 /* AT_UID                       */
    sp[n++] = 12; sp[n++] = 0;                 /* AT_EUID                      */
    sp[n++] = 13; sp[n++] = 0;                 /* AT_GID                       */
    sp[n++] = 14; sp[n++] = 0;                 /* AT_EGID                      */
    sp[n++] = 23; sp[n++] = 0;                 /* AT_SECURE                    */
    sp[n++] = 25; sp[n++] = randomp;           /* AT_RANDOM                    */
    sp[n++] = 31; sp[n++] = argv_ptr[0];       /* AT_EXECFN                    */
    sp[n++] = 0;  sp[n++] = 0;                 /* AT_NULL                      */

    out->pml4 = process_pml4;
    out->entry = eh->entry;
    out->rsp = rsp;
    out->code = code;
    return 0;
}

static int run_linux_elf_frames(const uint8_t *img, uint64_t size,
                                uint8_t *stk, uint8_t *kstack_top,
                                const struct linux_exec_args *args, int on_sched) {
    uint8_t *ustk = stk;
    if (on_sched) {                            /* private stack so the reap can free it */
        ustk = (uint8_t *)page_alloc(USER_STK_PAGES);
        if (!ustk) return -5;
    }
    struct linux_image im;
    int lr = linux_load_image(img, size, ustk, args, &im);
    if (lr < 0) return lr;

    struct proc *parent = current;
    struct proc *p = proc_alloc();
    if (!p) return -5;
    p->ppid = parent->pid;
    p->abi = USER_ABI_LINUX;
    p->code = im.code;
    p->stk = ustk;
    p->kstack_top = kstack_top;
    p->vm.pml4 = im.pml4;
    p->vm.brk = LINUX_BRK_BASE;
    p->vm.mmap_next = LINUX_MMAP_BASE;
    proc_set_name(p, args ? args->argv[0] : "linux");
    p->state = PROC_RUNNING;

    if (on_sched) {
        /* Busybox pivot: run the Linux process as a preemptible scheduler task
         * and block (wait4-style) until it becomes a zombie, then reap it — the
         * same mechanism fork/pipe/signals build on. */
        uint8_t *kstk = (uint8_t *)page_alloc(4);
        if (!kstk) { p->vm.pml4 = 0; proc_free(p); return -5; }
        p->on_sched = 1;
        p->kstack_top = kstk + 16384;
        p->pgid = p->pid; p->sid = p->pid;     /* the launched job leads its own session */
        int saved_fg = g_tty.fg_pgrp;
        g_tty.fg_pgrp = p->pid;                /* it is the foreground job (Ctrl-C targets it) */
        sched_init();
        usched_spawn_user(im.entry, im.rsp, kstk, 16384, 0, 0, (uint64_t)im.pml4, p);
        sched_start();
        sched_wait((volatile int *)&p->state, PROC_ZOMBIE, p->pid);
        sched_stop();
        g_tty.fg_pgrp = saved_fg;
        current = parent;
        arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
        arch_set_tss_rsp0((uint64_t)(g_kstack0 + sizeof(g_kstack0)));
        write_fs_base(parent->fs_base);
        int exit_code = p->exit_status;
        if (p->kstack_top) page_free((void *)(p->kstack_top - 16384), 4);
        free_user_pages(p->vm.pml4);       /* reclaim frames (scheduler stopped) */
        p->vm.pml4 = 0;
        proc_free(p);
        return exit_code;
    }

    current = p;                               /* syscalls now act on this proc */
    write_fs_base(0);
    arch_set_syscall_stack((uint64_t)kstack_top);
    uint64_t kernel_cr3 = read_cr3();
    write_cr3((uint64_t)im.pml4);
    enter_user(im.entry, im.rsp, 0);
    write_cr3(kernel_cr3);
    int exit_code = p->exit_status;
    current = parent;                          /* back to the caller's context  */
    write_fs_base(parent->fs_base);
    p->vm.pml4 = 0;
    proc_free(p);
    return exit_code;
}

static int run_linux_elf(const uint8_t *img, uint64_t size) {
    return run_linux_elf_frames(img, size, g_ustk,
                                g_ksys0 + sizeof(g_ksys0), 0, 0);
}

/* Process-table smoke/stress: fill the table, verify pids are distinct + monotonic
 * and slots are reused after free.  `stress` repeats the alloc/free churn. */
int arch_proc_test(int stress) {
    int rounds = stress ? 50 : 1, ok = 1;
    for (int r = 0; r < rounds && ok; r++) {
        struct proc *p[PROC_MAX];
        int n = 0, base = g_next_pid;
        for (struct proc *q; (q = proc_alloc()) != 0; ) {
            if (q->pid != base + n || q->state == PROC_UNUSED) ok = 0;
            p[n++] = q;
            if (n >= PROC_MAX) break;
        }
        if (n < 2) ok = 0;
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++)
                if (p[i] == p[j] || p[i]->pid == p[j]->pid) ok = 0;
        for (int i = 0; i < n; i++) proc_free(p[i]);
    }
    kputs(ok ? "[proc-test] alloc/free/reuse PASS\n" : "[proc-test] alloc/free/reuse FAIL\n");
    return ok;
}

void arch_user_init(void) {
    proc_table_init();
    g_ucode = (uint8_t *)page_alloc(USER_CODE_PAGES); g_ustk = (uint8_t *)page_alloc(USER_STK_PAGES);
    g_ccode = (uint8_t *)page_alloc(USER_CODE_PAGES); g_cstk = (uint8_t *)page_alloc(USER_STK_PAGES);
    if (!g_ucode || !g_ustk || !g_ccode || !g_cstk) { kputs("[user] region alloc failed\n"); g_ucode = 0; return; }

    uint64_t pml4 = read_cr3() & ~0xFFFULL;
    g_kernel_cr3 = pml4;
    uint64_t cr0  = wp_disable();
    for (int i = 0; i < USER_CODE_PAGES; i++)
        map_user_page(pml4, USER_CODE_VA + (uint64_t)i * 4096, (uint64_t)(g_ucode + i * 4096), PTE_RW | PTE_US);
    for (int i = 0; i < USER_STK_PAGES; i++)
        map_user_page(pml4, USER_STK_TOP - (uint64_t)(i + 1) * 4096, (uint64_t)(g_ustk + i * 4096), PTE_RW | PTE_US);
    wp_restore(cr0);
    flush_tlb();
    g_pt = walk_pt(pml4, USER_CODE_VA);        /* cache leaf PT for fast swaps  */

    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    arch_setup_syscall_msrs();
    kputs("[user] ring-3 program region ready\n");

    blk_init();
    if (fat_mount() == 0) kputs("[fs] fat16 ram disk mounted\n");
    else                  kputs("[fs] fat16 mount failed\n");

    void *pf_a = page_alloc(1); page_free(pf_a, 1);
    void *pf_b = page_alloc(1);
    kputs(pf_b == pf_a ? "[pagefree] page free-list reuse OK\n" : "[pagefree] page free-list reuse FAIL\n");
    page_free(pf_b, 1);

    int elf_result = run_linux_elf(linux_hello_elf, sizeof(linux_hello_elf));
    if (elf_result == 0) kputs("[linux-abi] ELF64 loader + SysV stack ok\n");
    else { kputs("[linux-abi] self-test failed: "); kput_u32dec((uint32_t)elf_result); kputc('\n'); }

    int musl_result = arch_user_run("elf-fs-probe", exec_probe_prog,
                                    sizeof(exec_probe_prog), "");
    if (musl_result == 0) kputs("[musl] filesystem libc program exited cleanly\n");
    else { kputs("[musl] filesystem probe failed: "); kput_u32dec((uint32_t)musl_result); kputc('\n'); }

    int sched_result = run_linux_elf_frames(linux_hello_elf, sizeof(linux_hello_elf),
                                            g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (sched_result == 0) kputs("[linux-sched] Linux ELF ran as a scheduled process + reaped OK\n");
    else { kputs("[linux-sched] scheduled run failed: "); kput_u32dec((uint32_t)sched_result); kputc('\n'); }

    int fork_result = run_linux_elf_frames(linux_fork_elf, sizeof(linux_fork_elf),
                                           g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (fork_result == 0) kputs("[linux-fork] fork + wait4 (real Linux ABI) OK\n");
    else { kputs("[linux-fork] fork/wait4 test failed: "); kput_u32dec((uint32_t)fork_result); kputc('\n'); }

    int exec_result = run_linux_elf_frames(linux_forkexec_elf, sizeof(linux_forkexec_elf),
                                           g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (exec_result == 0) kputs("[linux-exec] fork + execve + wait4 (real Linux ABI) OK\n");
    else { kputs("[linux-exec] fork/execve test failed: "); kput_u32dec((uint32_t)exec_result); kputc('\n'); }

    int storm_result = run_linux_elf_frames(linux_forkstorm_elf, sizeof(linux_forkstorm_elf),
                                            g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (storm_result == 0) kputs("[linux-forkstorm] 20x fork/wait4 with task-slot reuse OK\n");
    else { kputs("[linux-forkstorm] fork storm failed: "); kput_u32dec((uint32_t)storm_result); kputc('\n'); }

    int pipe_result = run_linux_elf_frames(linux_pipe_elf, sizeof(linux_pipe_elf),
                                           g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (pipe_result == 0) kputs("[linux-pipe] pipe + dup2 + fork I/O OK\n");
    else { kputs("[linux-pipe] pipe test failed: "); kput_u32dec((uint32_t)pipe_result); kputc('\n'); }

    int sig_result = run_linux_elf_frames(linux_sig_elf, sizeof(linux_sig_elf),
                                          g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (sig_result == 128 + 15) kputs("[linux-sig] default signal terminate (SIGTERM) OK\n");
    else { kputs("[linux-sig] signal test failed: "); kput_u32dec((uint32_t)sig_result); kputc('\n'); }

    int sigh_result = run_linux_elf_frames(linux_sigh_elf, sizeof(linux_sigh_elf),
                                           g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (sigh_result == 0) kputs("[linux-sigh] custom SIGINT handler + rt_sigreturn OK\n");
    else { kputs("[linux-sigh] handler test failed: "); kput_u32dec((uint32_t)sigh_result); kputc('\n'); }

    int tty_result = run_linux_elf_frames(linux_tty_elf, sizeof(linux_tty_elf),
                                          g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (tty_result == 0) kputs("[linux-tty] termios ioctl + process groups OK\n");
    else { kputs("[linux-tty] tty test failed: "); kput_u32dec((uint32_t)tty_result); kputc('\n'); }

    int proc_result = run_linux_elf_frames(linux_proc_elf, sizeof(linux_proc_elf),
                                           g_ustk, g_ksys0 + sizeof(g_ksys0), 0, 1);
    if (proc_result == 0) kputs("[linux-proc] syscall tail (uname/access/nanosleep) + procfs OK\n");
    else { kputs("[linux-proc] syscall/procfs test failed: "); kput_u32dec((uint32_t)proc_result); kputc('\n'); }

    int mbb_result = arch_run_linux("/MBB.BIN", "");   /* musl-static mini-busybox: echo|grep pipeline */
    if (mbb_result == 0) kputs("[mbb] mini-busybox pipeline (echo | grep) OK\n");
    else { kputs("[mbb] mini-busybox failed: "); kput_u32dec((uint32_t)mbb_result); kputc('\n'); }

    int shell_result = arch_run_linux("/MUSL.BIN", "");
    if (shell_result == 0) kputs("[shell-run] scheduled launch of /MUSL.BIN via shell path OK\n");
    else { kputs("[shell-run] shell launch failed: "); kput_u32dec((uint32_t)shell_result); kputc('\n'); }

    kputs("[test] scheduler + process self-tests:\n");   /* fast smoke; full via `stest` */
    sched_test_run(0);
    arch_proc_test(0);
    arch_procsched_test();
}

/* ---- Phase B: preemptive user-process scheduling (self-test) ----------------
 * Two ring-3 processes each run in their OWN address space (private code /
 * counter / stack pages at identical user VAs) and bump a private counter in a
 * tight loop with interrupts ON.  The LAPIC timer preempts and round-robins them
 * — swapping CR3 on every switch — plus the kernel driver task via sched64.c;
 * proven when both counters advance.  Results return via out-params: this routine
 * writes nothing to the fb console, so it is safe to run under the window
 * manager (kputs would paint over the desktop). */
void     sched_init(void);
void     sched_start(void);
void     sched_stop(void);
int      usched_spawn_user(uint64_t entry, uint64_t user_rsp, uint8_t *kstack,
                          uint64_t ksize, uint64_t rbx, uint64_t r15v, uint64_t cr3, void *proc);
uint64_t sched_switches(void);
uint64_t usched_loop_addr(void);
uint64_t usched_loop_len(void);
uint64_t usched_block_loop_addr(void);
uint64_t usched_block_loop_len(void);
uint64_t usched_exit_prog_addr(void);
uint64_t usched_exit_prog_len(void);
uint64_t usched_fork_prog_addr(void);
uint64_t usched_fork_prog_len(void);

static struct proc g_demo_proc;             /* identity for the demo procs' syscalls */
static struct proc *g_fork_child;           /* child created by do_demo_fork          */
static uint8_t     *g_fork_child_data;       /* the child's copied counter page        */

#define UDEMO_CODE_VA 0x0000710000000000ULL
#define UDEMO_DATA_VA 0x0000710000001000ULL
#define UDEMO_STK_VA  0x0000710000002000ULL

int arch_usched_selftest(uint32_t *a_out, uint32_t *b_out, uint32_t *sw_out) {
    static uint64_t cr3_a, cr3_b;
    static uint8_t *data_a, *data_b, *kstk_a, *kstk_b;
    static int ready;
    if (!ready) {
        uint8_t *code_a = (uint8_t *)page_alloc(1);
        uint8_t *code_b = (uint8_t *)page_alloc(1);
        uint8_t *stk_a  = (uint8_t *)page_alloc(1);
        uint8_t *stk_b  = (uint8_t *)page_alloc(1);
        data_a = (uint8_t *)page_alloc(1);
        data_b = (uint8_t *)page_alloc(1);
        kstk_a = (uint8_t *)page_alloc(4);          /* 16 KiB kernel stacks */
        kstk_b = (uint8_t *)page_alloc(4);
        uint64_t *as_a = copy_page_table((const uint64_t *)g_kernel_cr3);
        uint64_t *as_b = copy_page_table((const uint64_t *)g_kernel_cr3);
        if (!code_a || !code_b || !stk_a || !stk_b || !data_a || !data_b ||
            !kstk_a || !kstk_b || !as_a || !as_b) return -1;
        uint64_t la = usched_block_loop_len();       /* proc A blocks on read() each pass */
        const uint8_t *sa = (const uint8_t *)usched_block_loop_addr();
        for (uint64_t i = 0; i < la; i++) code_a[i] = sa[i];
        uint64_t lb = usched_loop_len();             /* proc B free-runs                  */
        const uint8_t *sb = (const uint8_t *)usched_loop_addr();
        for (uint64_t i = 0; i < lb; i++) code_b[i] = sb[i];
        /* two private address spaces: identical user VAs -> distinct physical pages */
        uint64_t cr0 = wp_disable();
        map_user_page((uint64_t)as_a, UDEMO_CODE_VA, (uint64_t)code_a, PTE_US);
        map_user_page((uint64_t)as_a, UDEMO_DATA_VA, (uint64_t)data_a, PTE_US | PTE_RW);
        map_user_page((uint64_t)as_a, UDEMO_STK_VA,  (uint64_t)stk_a,  PTE_US | PTE_RW);
        map_user_page((uint64_t)as_b, UDEMO_CODE_VA, (uint64_t)code_b, PTE_US);
        map_user_page((uint64_t)as_b, UDEMO_DATA_VA, (uint64_t)data_b, PTE_US | PTE_RW);
        map_user_page((uint64_t)as_b, UDEMO_STK_VA,  (uint64_t)stk_b,  PTE_US | PTE_RW);
        wp_restore(cr0);
        cr3_a = (uint64_t)as_a;
        cr3_b = (uint64_t)as_b;
        g_demo_proc.abi = USER_ABI_DEMO;
        ready = 1;
    }
    volatile uint64_t *ca = (volatile uint64_t *)data_a;
    volatile uint64_t *cb = (volatile uint64_t *)data_b;
    ca[0] = 0; cb[0] = 0;

    struct proc *saved = current;                    /* the demo repoints `current` */
    sched_init();
    usched_spawn_user(UDEMO_CODE_VA, UDEMO_STK_VA + 4096, kstk_a, 16384, 0, UDEMO_DATA_VA, cr3_a, &g_demo_proc);
    usched_spawn_user(UDEMO_CODE_VA, UDEMO_STK_VA + 4096, kstk_b, 16384, 0, UDEMO_DATA_VA, cr3_b, &g_demo_proc);
    sched_start();
    uint64_t t0 = millis(), last = t0;
    while (millis() - t0 < 250) {
        uint64_t t = millis();
        if (t - last >= 30) { sched_wake(0); last = t; }   /* unblock proc A's read() */
        __asm__ volatile("pause");
    }
    sched_stop();
    current = saved;

    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));   /* restore boot stacks */
    arch_set_tss_rsp0((uint64_t)(g_kstack0 + sizeof(g_kstack0)));

    uint64_t a = ca[0], b = cb[0];
    if (a_out)  *a_out  = (uint32_t)a;
    if (b_out)  *b_out  = (uint32_t)b;
    if (sw_out) *sw_out = (uint32_t)sched_switches();
    /* A advances only when woken (blocked meanwhile); B free-runs -> B >> A */
    return (a > 0 && b > a * 4) ? 0 : -1;
}

/* ---- Phase C step 1: run a real struct proc through its full lifecycle -------
 * Allocate a process from the table, give it its own kernel stack + address
 * space, schedule it to run a tiny ring-3 program that does one unit of work and
 * exit(42).  The kernel (task 0) polls until the child is a ZOMBIE, then reaps
 * it.  This proves a table process — not a demo task — is scheduled, runs in its
 * own CR3, exits via syscall, and is reaped: the foundation for wait4. */
int arch_procsched_test(void) {
    struct proc *p = proc_alloc();
    if (!p) return -1;
    p->abi = USER_ABI_DEMO;
    p->state = PROC_RUNNING;
    uint8_t *code = (uint8_t *)page_alloc(1);
    uint8_t *data = (uint8_t *)page_alloc(1);
    uint8_t *stk  = (uint8_t *)page_alloc(1);
    uint8_t *kstk = (uint8_t *)page_alloc(4);
    uint64_t *as  = copy_page_table((const uint64_t *)g_kernel_cr3);
    if (!code || !data || !stk || !kstk || !as) { proc_free(p); return -1; }
    uint64_t len = usched_exit_prog_len();
    const uint8_t *src = (const uint8_t *)usched_exit_prog_addr();
    for (uint64_t i = 0; i < len; i++) code[i] = src[i];
    uint64_t cr0 = wp_disable();
    map_user_page((uint64_t)as, UDEMO_CODE_VA, (uint64_t)code, PTE_US);
    map_user_page((uint64_t)as, UDEMO_DATA_VA, (uint64_t)data, PTE_US | PTE_RW);
    map_user_page((uint64_t)as, UDEMO_STK_VA,  (uint64_t)stk,  PTE_US | PTE_RW);
    wp_restore(cr0);
    p->vm.pml4 = as;
    volatile uint64_t *ctr = (volatile uint64_t *)data; ctr[0] = 0;

    struct proc *saved = current;
    sched_init();
    usched_spawn_user(UDEMO_CODE_VA, UDEMO_STK_VA + 4096, kstk, 16384, 0, UDEMO_DATA_VA, (uint64_t)as, p);
    sched_start();
    uint64_t t0 = millis();
    while (p->state != PROC_ZOMBIE && millis() - t0 < 500) __asm__ volatile("pause");
    sched_stop();
    current = saved;
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    arch_set_tss_rsp0((uint64_t)(g_kstack0 + sizeof(g_kstack0)));

    int ok = (p->state == PROC_ZOMBIE) && (p->exit_status == 42) && (ctr[0] == 1);
    int pid = p->pid, st = p->exit_status; uint32_t work = (uint32_t)ctr[0];
    proc_free(p);                            /* reap the zombie */
    kputs("[procsched] pid="); kput_u32dec((uint32_t)pid);
    kputs(" work=");           kput_u32dec(work);
    kputs(" status=");         kput_u32dec((uint32_t)st);
    kputs(ok ? " scheduled+exited+reaped OK\n" : " FAIL\n");
    return ok ? 0 : -1;
}

/* ---- Phase C step 2: race-free blocking wait4 ------------------------------
 * Same as arch_procsched_test but the parent BLOCKS on the child via sched_wait
 * (atomic check-and-sleep) instead of polling, and the child's exit wakes it.
 * Proves the parent sleeps until the child exits with no lost-wakeup race. */
int arch_waitproc_test(void) {
    struct proc *p = proc_alloc();
    if (!p) return -1;
    p->abi = USER_ABI_DEMO;
    p->state = PROC_RUNNING;
    uint8_t *code = (uint8_t *)page_alloc(1);
    uint8_t *data = (uint8_t *)page_alloc(1);
    uint8_t *stk  = (uint8_t *)page_alloc(1);
    uint8_t *kstk = (uint8_t *)page_alloc(4);
    uint64_t *as  = copy_page_table((const uint64_t *)g_kernel_cr3);
    if (!code || !data || !stk || !kstk || !as) { proc_free(p); return -1; }
    uint64_t len = usched_exit_prog_len();
    const uint8_t *src = (const uint8_t *)usched_exit_prog_addr();
    for (uint64_t i = 0; i < len; i++) code[i] = src[i];
    uint64_t cr0 = wp_disable();
    map_user_page((uint64_t)as, UDEMO_CODE_VA, (uint64_t)code, PTE_US);
    map_user_page((uint64_t)as, UDEMO_DATA_VA, (uint64_t)data, PTE_US | PTE_RW);
    map_user_page((uint64_t)as, UDEMO_STK_VA,  (uint64_t)stk,  PTE_US | PTE_RW);
    wp_restore(cr0);
    p->vm.pml4 = as;

    struct proc *saved = current;
    sched_init();
    usched_spawn_user(UDEMO_CODE_VA, UDEMO_STK_VA + 4096, kstk, 16384, 0, UDEMO_DATA_VA, (uint64_t)as, p);
    sched_start();
    sched_wait((volatile int *)&p->state, PROC_ZOMBIE, p->pid);   /* block until child exits */
    sched_stop();
    current = saved;
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    arch_set_tss_rsp0((uint64_t)(g_kstack0 + sizeof(g_kstack0)));

    int ok = (p->state == PROC_ZOMBIE) && (p->exit_status == 42);
    int pid = p->pid, st = p->exit_status;
    proc_free(p);
    kputs("[waitproc] pid="); kput_u32dec((uint32_t)pid);
    kputs(" status=");        kput_u32dec((uint32_t)st);
    kputs(ok ? " parent blocked in wait, child exited+reaped OK\n" : " FAIL\n");
    return ok ? 0 : -1;
}

/* Deep-clone one paging level for fork(): private copies of every user (US) leaf
 * page, shared kernel pages/tables.  Large pages (PS) and kernel (non-US) leaves
 * are shared as-is; only user paths allocate.  level: 3=PDPT, 2=PD, 1=PT. */
static uint64_t clone_pt(uint64_t src_phys, int level) {
    uint64_t *src = (uint64_t *)src_phys;
    uint64_t *dst = (uint64_t *)page_alloc(1);
    if (!dst) return 0;
    for (int i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_P)) { dst[i] = 0; continue; }
        if (level == 1) {
            if (e & PTE_US) {
                uint8_t *f = (uint8_t *)page_alloc(1);
                if (!f) { dst[i] = 0; continue; }
                const uint8_t *s = (const uint8_t *)(e & ~0xFFFULL);
                for (int b = 0; b < 4096; b++) f[b] = s[b];
                dst[i] = (uint64_t)f | (e & 0xFFFULL);
            } else dst[i] = e;                          /* share kernel page      */
        } else if (e & PTE_PS) {
            dst[i] = e;                                 /* share large kernel page */
        } else {
            uint64_t sub = clone_pt(e & ~0xFFFULL, level - 1);
            if (!sub) { dst[i] = 0; continue; }
            dst[i] = sub | (e & 0xFFFULL);
        }
    }
    return (uint64_t)dst;
}

/* Build a child address space for fork(): share the kernel mappings, but deep-copy
 * the four user PML4 regions (code, stack, brk, mmap) so the child gets private
 * pages holding the parent's contents.  Eager copy (COW is a later optimization). */
static uint64_t *fork_clone_as(uint64_t parent_pml4) {
    uint64_t *dst = copy_page_table((const uint64_t *)parent_pml4);   /* share kernel slots */
    if (!dst) return 0;
    const uint64_t user_va[4] = { LINUX_LOAD_VA, LINUX_BRK_BASE, LINUX_MMAP_BASE, LINUX_STK_TOP };
    for (int k = 0; k < 4; k++) {
        int slot = (int)((user_va[k] >> 39) & 0x1FF);
        uint64_t e = ((uint64_t *)parent_pml4)[slot];
        if (!(e & PTE_P)) continue;
        uint64_t sub = clone_pt(e & ~0xFFFULL, 3);       /* clone the PDPT subtree */
        if (!sub) return 0;
        dst[slot] = sub | (e & 0xFFFULL);
    }
    return dst;
}

/* Free a fully-private paging subtree (STK/BRK/MMAP regions never overlap the
 * shared kernel identity map): return every user leaf page AND intermediate
 * table to the allocator. */
static void free_pt(uint64_t phys, int level) {
    uint64_t *t = (uint64_t *)phys;
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) { if (e & PTE_US) page_free((void *)(e & ~0xFFFULL), 1); }
        else if (!(e & PTE_PS)) free_pt(e & ~0xFFFULL, level - 1);
    }
    page_free((void *)phys, 1);
}

/* Free only the user leaf pages of a subtree, leaving tables in place — for the
 * LOAD region, whose tables interleave with the shared kernel identity map. */
static void free_pt_pages(uint64_t phys, int level) {
    uint64_t *t = (uint64_t *)phys;
    for (int i = 0; i < 512; i++) {
        uint64_t e = t[i];
        if (!(e & PTE_P)) continue;
        if (level == 1) { if (e & PTE_US) page_free((void *)(e & ~0xFFFULL), 1); }
        else if (!(e & PTE_PS)) free_pt_pages(e & ~0xFFFULL, level - 1);
    }
}

/* Reclaim a dead process's private frames: the STK/BRK/MMAP subtrees (pages +
 * tables), the LOAD region's code pages, and the PML4.  Shared kernel pages are
 * left untouched.  The AS must NOT be the active CR3. */
static void free_user_pages(uint64_t *pml4) {
    if (!pml4) return;
    const uint64_t priv_va[3] = { LINUX_STK_TOP, LINUX_BRK_BASE, LINUX_MMAP_BASE };
    for (int k = 0; k < 3; k++) {
        int slot = (int)((priv_va[k] >> 39) & 0x1FF);
        if (pml4[slot] & PTE_P) { free_pt(pml4[slot] & ~0xFFFULL, 3); pml4[slot] = 0; }
    }
    int lslot = (int)((LINUX_LOAD_VA >> 39) & 0x1FF);
    if (pml4[lslot] & PTE_P) free_pt_pages(pml4[lslot] & ~0xFFFULL, 3);
    page_free(pml4, 1);
}

/* Real fork(57) for a scheduled Linux process: eager-copy the parent's address
 * space and fd table into a child that resumes at the fork syscall's return with
 * rax=0 and the parent's callee-saved registers; the parent gets the child pid.
 * Runs inside the syscall (interrupts off), so building the child is atomic vs
 * the scheduler. */
static uint64_t do_linux_fork(void) {
    struct proc *par = current;
    struct proc *ch = proc_alloc();
    if (!ch) return (uint64_t)-11;                       /* -EAGAIN */
    uint64_t *cas  = fork_clone_as((uint64_t)par->vm.pml4);
    uint8_t  *ckstk = (uint8_t *)page_alloc(4);
    if (!cas || !ckstk) { proc_free(ch); return (uint64_t)-12; }
    ch->ppid = par->pid;
    ch->abi  = USER_ABI_LINUX;
    ch->on_sched = 1;
    ch->fs_base = par->fs_base;                           /* inherit TLS base */
    ch->vm.pml4 = cas;
    ch->vm.brk  = par->vm.brk;
    ch->vm.mmap_next = par->vm.mmap_next;
    for (int i = 0; i < PROC_FD_MAX; i++) {                           /* clone fd table */
        ch->fds[i] = par->fds[i];
        if (par->fds[i].used) {
            if (par->fds[i].kind == LINUX_FD_PIPE_R) g_pipes[par->fds[i].pipe].readers++;
            else if (par->fds[i].kind == LINUX_FD_PIPE_W) g_pipes[par->fds[i].pipe].writers++;
        }
    }
    ch->sigmask = par->sigmask;                                      /* inherit signal disposition */
    ch->sigrestorer = par->sigrestorer;
    for (int i = 0; i <= 64; i++) ch->sighandler[i] = par->sighandler[i];
    ch->pgid = par->pgid; ch->sid = par->sid;                        /* inherit process group/session */
    proc_set_name(ch, par->name);
    ch->kstack_top = ckstk + 16384;
    ch->state = PROC_RUNNING;
    const uint64_t *callee = (const uint64_t *)syscall_user_callee();
    usched_spawn_forked(syscall_user_rip(), syscall_user_rsp(), ckstk, 16384,
                        (uint64_t)cas, ch, callee);      /* child frame: rax=0 */
    return (uint64_t)ch->pid;                            /* parent's fork() returns child pid */
}

/* wait4(61): reap a zombie child, or block until one exits.  Race-free: the sleep
 * condition (any zombie child) is scanned with interrupts off inside the syscall,
 * and a child's exit wakes chan = parent pid, so a wake between scan and sleep is
 * not lost.  Returns the reaped pid and a W_EXITCODE-format status. */
static int linux_wait4(int wpid, int *wstatus, int options) {
    for (;;) {
        __asm__ volatile("cli");
        struct proc *zombie = 0; int have = 0;
        for (int i = 1; i < PROC_MAX; i++) {
            struct proc *c = &g_proc[i];
            if (c->state == PROC_UNUSED || c->ppid != current->pid) continue;
            if (wpid > 0 && c->pid != wpid) continue;
            have = 1;
            if (c->state == PROC_ZOMBIE) { zombie = c; break; }
        }
        if (!have) { __asm__ volatile("sti"); return -10; }          /* -ECHILD */
        if (zombie) {
            int pid = zombie->pid, st = (zombie->exit_status & 0xFF) << 8;
            if (zombie->kstack_top) page_free((void *)(zombie->kstack_top - 16384), 4);
            free_user_pages(zombie->vm.pml4); /* reclaim frames (interrupts still off) */
            zombie->vm.pml4 = 0;
            proc_free(zombie);                 /* reap the zombie slot */
            __asm__ volatile("sti");
            if (wstatus) *wstatus = st;
            return pid;
        }
        if (options & 1) { __asm__ volatile("sti"); return 0; }      /* WNOHANG */
        sched_block(current->pid);             /* marked BLOCKED before sti;hlt (no lost wake) */
    }
}

/* fork() for a demo proc: duplicate the caller's address space (its three UDEMO
 * pages) into a child that resumes at the parent's fork-return point with rax=0,
 * and return the child pid to the parent.  Runs inside the syscall (interrupts
 * off), so allocating + registering the child is atomic vs the scheduler. */
static uint64_t do_demo_fork(void) {
    struct proc *par = current;
    struct proc *ch = proc_alloc();
    if (!ch) return (uint64_t)-1;
    ch->abi = USER_ABI_DEMO;
    ch->state = PROC_RUNNING;
    uint64_t *cas   = copy_page_table((const uint64_t *)g_kernel_cr3);
    uint8_t  *ccode = (uint8_t *)page_alloc(1);
    uint8_t  *cdata = (uint8_t *)page_alloc(1);
    uint8_t  *cstk  = (uint8_t *)page_alloc(1);
    uint8_t  *ckstk = (uint8_t *)page_alloc(4);
    if (!cas || !ccode || !cdata || !cstk || !ckstk) { proc_free(ch); return (uint64_t)-1; }
    uint64_t pml4 = (uint64_t)par->vm.pml4;
    uint64_t *pt;
    pt = walk_pt(pml4, UDEMO_CODE_VA);
    const uint8_t *pcode = (const uint8_t *)(pt[(UDEMO_CODE_VA >> 12) & 0x1FF] & ~0xFFFULL);
    pt = walk_pt(pml4, UDEMO_DATA_VA);
    const uint8_t *pdata = (const uint8_t *)(pt[(UDEMO_DATA_VA >> 12) & 0x1FF] & ~0xFFFULL);
    pt = walk_pt(pml4, UDEMO_STK_VA);
    const uint8_t *pstk = (const uint8_t *)(pt[(UDEMO_STK_VA >> 12) & 0x1FF] & ~0xFFFULL);
    for (int i = 0; i < 4096; i++) { ccode[i] = pcode[i]; cdata[i] = pdata[i]; cstk[i] = pstk[i]; }
    uint64_t cr0 = wp_disable();
    map_user_page((uint64_t)cas, UDEMO_CODE_VA, (uint64_t)ccode, PTE_US);
    map_user_page((uint64_t)cas, UDEMO_DATA_VA, (uint64_t)cdata, PTE_US | PTE_RW);
    map_user_page((uint64_t)cas, UDEMO_STK_VA,  (uint64_t)cstk,  PTE_US | PTE_RW);
    wp_restore(cr0);
    ch->vm.pml4 = cas;
    g_fork_child = ch;
    g_fork_child_data = cdata;
    usched_spawn_user(syscall_user_rip(), syscall_user_rsp(), ckstk, 16384,
                      0, UDEMO_DATA_VA, (uint64_t)cas, ch);   /* child frame: rax=0 */
    return (uint64_t)ch->pid;                /* parent's fork() returns the child pid */
}

/* ---- Phase C step 3: fork() ------------------------------------------------
 * Schedule a parent proc that fork()s; the parent bumps counter[1] + exit(8),
 * the child (a duplicate address space, rax=0) bumps counter[0] + exit(7).  The
 * kernel waits for both and checks each branch ran in its own address space. */
int arch_fork_test(void) {
    g_fork_child = 0; g_fork_child_data = 0;
    struct proc *p = proc_alloc();
    if (!p) return -1;
    p->abi = USER_ABI_DEMO;
    p->state = PROC_RUNNING;
    uint8_t *code = (uint8_t *)page_alloc(1);
    uint8_t *data = (uint8_t *)page_alloc(1);
    uint8_t *stk  = (uint8_t *)page_alloc(1);
    uint8_t *kstk = (uint8_t *)page_alloc(4);
    uint64_t *as  = copy_page_table((const uint64_t *)g_kernel_cr3);
    if (!code || !data || !stk || !kstk || !as) { proc_free(p); return -1; }
    uint64_t len = usched_fork_prog_len();
    const uint8_t *src = (const uint8_t *)usched_fork_prog_addr();
    for (uint64_t i = 0; i < len; i++) code[i] = src[i];
    uint64_t cr0 = wp_disable();
    map_user_page((uint64_t)as, UDEMO_CODE_VA, (uint64_t)code, PTE_US);
    map_user_page((uint64_t)as, UDEMO_DATA_VA, (uint64_t)data, PTE_US | PTE_RW);
    map_user_page((uint64_t)as, UDEMO_STK_VA,  (uint64_t)stk,  PTE_US | PTE_RW);
    wp_restore(cr0);
    p->vm.pml4 = as;
    volatile uint64_t *pd = (volatile uint64_t *)data; pd[0] = 0; pd[1] = 0;

    struct proc *saved = current;
    sched_init();
    usched_spawn_user(UDEMO_CODE_VA, UDEMO_STK_VA + 4096, kstk, 16384, 0, UDEMO_DATA_VA, (uint64_t)as, p);
    sched_start();
    sched_wait((volatile int *)&p->state, PROC_ZOMBIE, p->pid);               /* wait parent */
    if (g_fork_child)
        sched_wait((volatile int *)&g_fork_child->state, PROC_ZOMBIE, g_fork_child->pid); /* wait child */
    sched_stop();
    current = saved;
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    arch_set_tss_rsp0((uint64_t)(g_kstack0 + sizeof(g_kstack0)));

    volatile uint64_t *cd = (volatile uint64_t *)g_fork_child_data;
    int ppid = p->pid, pstat = p->exit_status;
    int cpid = g_fork_child ? g_fork_child->pid : -1;
    int cstat = g_fork_child ? g_fork_child->exit_status : -1;
    int parent_ran = (pd[1] == 1);
    int child_ran  = (g_fork_child_data && cd[0] == 1);
    int ok = parent_ran && child_ran && pstat == 8 && cstat == 7;
    proc_free(p);
    if (g_fork_child) proc_free(g_fork_child);
    kputs("[fork] parent pid="); kput_u32dec((uint32_t)ppid);
    kputs(" exit=");             kput_u32dec((uint32_t)pstat);
    kputs("  child pid=");       kput_u32dec((uint32_t)cpid);
    kputs(" exit=");             kput_u32dec((uint32_t)cstat);
    kputs(ok ? "  fork: both branches ran in their own AS OK\n" : "  FAIL\n");
    return ok ? 0 : -1;
}

/* ---- `ps` view: walk the live process table (SYS_TASKINFO).  proc[0] is the
 * kernel; the rest are the active ring-3 procs (current + its ancestors). ---- */
static int proc_info(int i, int *pid, int *ring, int *state, char *name) {
    int seen = 0;
    for (int k = 0; k < PROC_MAX; k++) {
        if (g_proc[k].state == PROC_UNUSED) continue;
        if (seen++ != i) continue;
        struct proc *p = &g_proc[k];
        *pid = p->pid;
        *ring = (p->pid == 0) ? 0 : 3;
        *state = (p == current) ? 0 : 1;        /* running proc vs waiting caller */
        int j = 0; while (p->name[j] && j < 15) { name[j] = p->name[j]; j++; } name[j] = 0;
        return 0;
    }
    return -1;
}

/* load an image into a frame set and run it to completion in ring 3.  argv[0]
 * (the program's single string argument) is staged in the code-region tail while
 * the CALLER is still mapped, then handed to umain via rdi (see trap64.s). */
static int run_prog(const char *name, const uint8_t *img, uint64_t size,
                    uint8_t *code, uint8_t *stk, uint8_t *kstack_top, const char *arg) {
    uint64_t cap = (uint64_t)USER_CODE_PAGES * 4096;
    if (size > cap) return -2;
    for (uint64_t i = 0; i < size; i++) code[i] = img[i];
    for (uint64_t i = size; i < cap; i++) code[i] = 0;      /* zero tail (.bss) */
    int ai = 0;                                             /* stage argv[0]     */
    if (arg) while (arg[ai] && ai < 255) { code[USER_ARG_OFF + ai] = (uint8_t)arg[ai]; ai++; }
    code[USER_ARG_OFF + ai] = 0;
    map_region(code, stk);
    arch_set_syscall_stack((uint64_t)kstack_top);
    struct proc *parent = current;
    struct proc *p = proc_alloc();
    if (!p) return -2;
    p->ppid = parent->pid;
    p->abi = USER_ABI_TINYOS;                   /* shares kernel AS + fixed window */
    proc_set_name(p, name);
    p->state = PROC_RUNNING;
    current = p;
    enter_user(USER_CODE_VA, USER_STK_TOP - 16, USER_CODE_VA + USER_ARG_OFF);
    int ec = p->exit_status;
    current = parent;
    proc_free(p);
    return ec;
}

/* primary program launched from the kernel console (`run`), nesting level 0 */
static int arch_user_run(const char *name, const uint8_t *img, uint64_t size, const char *arg) {
    if (!g_ucode) return -1;
    g_depth = 0;
    int ec = run_prog(name, img, size, g_ucode, g_ustk, g_ksys0 + sizeof(g_ksys0), arg);
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    return ec;
}

struct uprog_ent { const char *name; const unsigned char *img; unsigned size; };
static const struct uprog_ent g_uprogs[] = {
    { "hello", hello_prog, (unsigned)sizeof(hello_prog) },
    { "count", count_prog, (unsigned)sizeof(count_prog) },
    { "greet", greet_prog, (unsigned)sizeof(greet_prog) },
    { "sh",    sh_prog,    (unsigned)sizeof(sh_prog)    },
    { "vi",    vi_prog,    (unsigned)sizeof(vi_prog)    },
};

static int name_eq(const char *a, const char *b) {
    int j = 0; while (a[j] && a[j] == b[j]) j++;
    return a[j] == 0 && b[j] == 0;
}

static int find_prog(const char *name) {
    for (unsigned i = 0; i < sizeof(g_uprogs) / sizeof(g_uprogs[0]); i++)
        if (name_eq(g_uprogs[i].name, name)) return (int)i;
    return -1;
}

static int run_linux_path(const char *path, uint8_t *stk, uint8_t *kstack_top,
                          const struct linux_exec_args *args, int on_sched) {
    char full[80];
    while (path[0] == '.' && path[1] == '/') path += 2;
    fs_resolve(path, full);

    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
    if (fat_resolve(full, &attr, &cluster, &size) < 0 || (attr & 0x10) || !size)
        return -1;

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) return -5;
    int read = fat_read(cluster, size, image, size);
    int result = read == (int)size
        ? run_linux_elf_frames(image, size, stk, kstack_top, args, on_sched) : -1;
    kfree(image);
    return result;
}

/* Shell entry point: launch a Linux program from the filesystem as a scheduled
 * (preemptible) process, split `arg` into argv[1..], wait for it to exit, and
 * return its exit code (or a negative errno).  The stk/kstack args are ignored
 * by the scheduled path, which allocates the process its own frames. */
int arch_run_linux(const char *path, const char *arg) {
    static struct linux_exec_args args;        /* large; the shell blocks until exit */
    args.argc = 0; args.envc = 0;
    int j = 0; while (path[j] && j < LINUX_EXEC_STR - 1) { args.argv[0][j] = path[j]; j++; }
    args.argv[0][j] = 0; args.argc = 1;
    for (const char *p = arg; p && *p && args.argc < LINUX_EXEC_MAX; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        int m = 0;
        while (*p && *p != ' ' && m < LINUX_EXEC_STR - 1) args.argv[args.argc][m++] = *p++;
        args.argv[args.argc][m] = 0; args.argc++;
        while (*p && *p != ' ') p++;
    }
    return run_linux_path(path, g_ustk, g_ksys0 + sizeof(g_ksys0), &args, 1);
}

/* execve(59) in place for a scheduled process: load the new image, replace the
 * running process's address space + vm/name state, retarget its scheduler task
 * CR3, and iretq into the new entry.  Never returns on success (the image is
 * replaced); returns a negative errno on failure so the caller can report it. */
static int do_execve_inplace(const char *path, const struct linux_exec_args *args) {
    char full[80];
    while (path[0] == '.' && path[1] == '/') path += 2;
    fs_resolve(path, full);
    uint8_t attr; uint16_t cluster; uint32_t size;
    if (fat_resolve(full, &attr, &cluster, &size) < 0 || (attr & 0x10) || !size) return -2;
    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) return -12;
    int ok = fat_read(cluster, size, image, size) == (int)size;
    uint8_t *stk = ok ? (uint8_t *)page_alloc(USER_STK_PAGES) : 0;
    struct linux_image im;
    int r = (ok && stk) ? linux_load_image(image, size, stk, args, &im) : -12;
    kfree(image);
    if (r < 0) return r;
    uint64_t *old_pml4 = current->vm.pml4;
    current->vm.pml4 = im.pml4;
    current->vm.brk = LINUX_BRK_BASE;
    current->vm.mmap_next = LINUX_MMAP_BASE;
    current->code = im.code;
    current->stk = stk;
    current->fs_base = 0;
    for (int i = 0; i <= 64; i++) current->sighandler[i] = 0;   /* execve resets caught signals */
    current->sigpending = 0;
    write_fs_base(0);
    proc_set_name(current, args->argv[0]);
    sched_set_current_cr3((uint64_t)im.pml4);              /* keep the task's CR3 in sync */
    write_cr3((uint64_t)im.pml4);                          /* activate new AS before freeing old */
    free_user_pages(old_pml4);                             /* old image no longer mapped */
    enter_user_exec(im.entry, im.rsp, (uint64_t)im.pml4);  /* never returns */
    return 0;                                              /* unreachable */
}

static int linux_execve(const char *path, const char *const *argv,
                        const char *const *envp) {
    if (!path) return -14;
    struct linux_exec_args args;
    args.argc = args.envc = 0;
    if (argv) {
        while (args.argc < LINUX_EXEC_MAX && argv[args.argc]) {
            int j = 0;
            while (argv[args.argc][j] && j < LINUX_EXEC_STR - 1) {
                args.argv[args.argc][j] = argv[args.argc][j]; j++;
            }
            if (argv[args.argc][j]) return -7;
            args.argv[args.argc][j] = 0; args.argc++;
        }
        if (args.argc == LINUX_EXEC_MAX && argv[args.argc]) return -7;
    }
    if (!args.argc) {
        int j = 0;
        while (path[j] && j < LINUX_EXEC_STR - 1) { args.argv[0][j] = path[j]; j++; }
        if (path[j]) return -7;
        args.argv[0][j] = 0; args.argc = 1;
    }
    if (envp) {
        while (args.envc < LINUX_EXEC_MAX && envp[args.envc]) {
            int j = 0;
            while (envp[args.envc][j] && j < LINUX_EXEC_STR - 1) {
                args.envp[args.envc][j] = envp[args.envc][j]; j++;
            }
            if (envp[args.envc][j]) return -7;
            args.envp[args.envc][j] = 0; args.envc++;
        }
        if (args.envc == LINUX_EXEC_MAX && envp[args.envc]) return -7;
    }
    if (current->on_sched) return do_execve_inplace(path, &args);   /* returns only on failure */
    if (!g_linux_code || !g_linux_stk || !g_linux_kstack_top) return -14;
    int result = run_linux_path(path, g_linux_stk,
                                g_ksys2 + sizeof(g_ksys2), &args, 0);
    if (result < 0) return result;
    g_user_exit_code = result;
    sys_exit_resume();
    return 0;
}

int arch_user_run_named(const char *name, const char *arg) {
    if (name_eq(name, "musl")) {
        (void)arg;
        return run_linux_elf(musl_probe_elf, sizeof(musl_probe_elf));
    }
    int idx = find_prog(name);
    if (idx < 0)
        return run_linux_path(name, g_ustk,
                              g_ksys0 + sizeof(g_ksys0), 0, 0);
    return arch_user_run(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size, arg);
}

const char *arch_user_prog_name(int i) {
    int count = (int)(sizeof(g_uprogs) / sizeof(g_uprogs[0]));
    if (i == count) return "musl";
    return (i >= 0 && i < count) ? g_uprogs[i].name : 0;
}

/* SYS_EXEC: run a named program as a nested (level-1) child of the caller.  Only
 * one level of nesting is supported (a shell launching a program). */
static int arch_exec_named(const char *name, const char *arg) {
    if (g_depth != 0 || !g_ucode) return -1;
    int idx = find_prog(name);
    g_depth = 1;
    int ec;
    if (idx >= 0)
        ec = run_prog(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size,
                      g_ccode, g_cstk, g_ksys1 + sizeof(g_ksys1), arg);
    else {
        /* Linux ELF: split the shell arg string into argv[1..] so a real command
         * line like `./tcc.bin -static a.c -o a.bin` reaches the program. */
        static struct linux_exec_args ea;   /* non-reentrant: g_depth guards it */
        ea.argc = 0; ea.envc = 0;
        int k = 0;
        while (name[k] && k < LINUX_EXEC_STR - 1) { ea.argv[0][k] = name[k]; k++; }
        ea.argv[0][k] = 0; ea.argc = 1;
        for (const char *p = arg; p && *p && ea.argc < LINUX_EXEC_MAX; ) {
            while (*p == ' ') p++;
            if (!*p) break;
            int m = 0;
            while (*p && *p != ' ' && m < LINUX_EXEC_STR - 1) ea.argv[ea.argc][m++] = *p++;
            ea.argv[ea.argc][m] = 0; ea.argc++;
            while (*p && *p != ' ') p++;                 /* drop an overlong token's tail */
        }
        ec = run_linux_path(name, g_cstk, g_ksys1 + sizeof(g_ksys1), &ea, 0);
    }
    map_region(g_ucode, g_ustk);               /* remap the window back to the parent */
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    g_depth = 0;
    return ec;
}
