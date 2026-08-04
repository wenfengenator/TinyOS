/* tinyos x64 process model (Cycle 6, Tier 1 Phase A).
 *
 * `struct proc` is the per-process object that replaces the old set of
 * per-program globals in kernel/arch64.c.  A single `current` pointer names the
 * running process; the syscall layer, fd table, and VM arena all act on it.
 *
 * Phase A keeps the existing run-to-completion execution model: a program runs
 * as an allocated proc (current = child) until it exits, then control returns to
 * the caller (current = parent).  Phases B+ add preemptive scheduling, fork, and
 * the rest of the Unix substrate on top of this object. */
#ifndef TINYOS_PROC64_H
#define TINYOS_PROC64_H

#include "boot64.h"                 /* uint*_t */

#define PROC_MAX     64             /* process table size            */
#define PROC_FD_MAX  16             /* per-process file descriptors   */

enum proc_state {
    PROC_UNUSED = 0,                /* table slot is free             */
    PROC_EMBRYO,                    /* allocated, not yet runnable    */
    PROC_RUNNABLE,                  /* ready to run (Phase B)         */
    PROC_RUNNING,                   /* currently executing            */
    PROC_SLEEPING,                  /* blocked on a wait channel      */
    PROC_ZOMBIE,                    /* exited, awaiting reap (Phase C)*/
};

enum user_abi { USER_ABI_TINYOS = 0, USER_ABI_LINUX, USER_ABI_DEMO };

enum linux_fd_kind { LINUX_FD_FILE = 1, LINUX_FD_DIR, LINUX_FD_NULL, LINUX_FD_PIPE_R, LINUX_FD_PIPE_W, LINUX_FD_PROC, LINUX_FD_SOCKET, LINUX_FD_TTY };

/* One open file / directory in a process fd table. */
struct linux_fd {
    int      used;
    int      kind;                  /* enum linux_fd_kind             */
    int      flags;                 /* open() flags                   */
    uint32_t offset;                /* file position / dir index / bound port */
    uint16_t dir_cluster;           /* FAT start cluster for dirs     */
    int      pipe;                  /* pipe pool index (PIPE_R/PIPE_W) */
    int      sock;                  /* TCP connection index (SOCKET)  */
    char     path[256];
};

/* Per-process address-space arena (Linux ABI). */
struct proc_vm {
    uint64_t *pml4;                 /* process PML4; 0 => shares kernel AS + fixed window */
    uint64_t  brk;                  /* current program break          */
    uint64_t  mmap_next;            /* bump cursor for anonymous mmap */
};

struct proc {
    int pid, ppid, pgid, sid;
    int state;                      /* enum proc_state                */
    int abi;                        /* enum user_abi                  */
    int exit_status;
    int on_sched;                   /* runs under the preemptive scheduler (Phase C) */
    uint64_t fs_base;               /* %fs.base (TLS)                 */
    uint64_t sigmask;               /* blocked-signal mask            */
    uint64_t sigpending;            /* pending-signal mask            */
    uint64_t sighandler[65];        /* per-signal handler VA (0=DFL, 1=IGN) */
    uint64_t sigrestorer;           /* SA_RESTORER trampoline (rt_sigreturn) */
    uint64_t clear_tid;             /* set_tid_address / CLONE tidptr */
    struct proc_vm  vm;
    struct linux_fd fds[PROC_FD_MAX];
    uint8_t *code;                  /* loaded image frames (Linux ABI)*/
    uint8_t *stk;                   /* user stack frames              */
    uint8_t *kstack_top;            /* SYSCALL kernel stack top       */
    int      pane;                  /* terminal split-pane index (0..3) this shell owns */
    char name[16];                  /* for `ps`                       */
};

#endif /* TINYOS_PROC64_H */
