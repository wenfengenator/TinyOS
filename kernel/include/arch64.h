/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 arch layer (Cycle 4, Phase B): ring-0/ring-3 privilege split. */
#ifndef TINYOS_ARCH64_H
#define TINYOS_ARCH64_H

#include "boot64.h"

void arch_init(void);                       /* install our GDT + TSS + IDT       */
void arch_fb_mem_diag(uint64_t base, uint64_t size); /* report framebuffer memory type (perf) */
void arch_fb_set_wc(uint64_t base, uint64_t size);   /* make the framebuffer write-combining */
void arch_set_irq_gate(int vec, uint64_t handler); /* live IDT gate (scheduler timer) */
void arch_user_init(void);                  /* set up the reusable ring-3 region */
int  arch_user_run_named(const char *name, const char *arg); /* run a built-in program; exit code, or -1 if unknown */
int  arch_run_linux(const char *path, const char *arg);      /* launch a Linux ELF from the FS as a scheduled process */
const char *arch_user_prog_name(int i);     /* iterate built-in program names (i=0..), or 0 */

/* G6: map a buffer object's frames into the Linux address space (zero copy) */
uint64_t arch_bo_map_user(void *kva, uint32_t bytes);        /* returns user VA, or 0 */
int      arch_bo_zerocopy_test(void);       /* 0 user VA shares BO frames, -1 fail */

/* ring-3 stdout sink + terminal control, provided by the tty layer (kernel/term64.c) */
void usys_write(const char *p, unsigned long n);
void term_clear_active(void);               /* clear the terminal running the program */
void term_gotoxy_active(int row, int col);  /* move its cursor (SYS_GOTOXY)            */
void term_active_size(int *cols, int *rows);/* active terminal grid size (SYS_WINSIZE) */

/* scheduler support + Phase B user-preemption self-test */
void arch_set_tss_rsp0(uint64_t rsp0);      /* set TSS.RSP0 for the running proc  */
void arch_switch_current(void *proc);       /* scheduler: set `current` on a switch */
int  arch_usched_selftest(uint32_t *a, uint32_t *b, uint32_t *switches); /* two ring-3 procs, own address spaces; results via out-params */
int  arch_proc_test(int stress);            /* process-table alloc/free/reuse test  */
int  arch_procsched_test(void);             /* Phase C: schedule a real proc, run to exit, reap */
int  arch_waitproc_test(void);              /* Phase C: parent blocks in wait4 until child exits */
int  arch_fork_test(void);                  /* Phase C: fork() into a child address space */

#endif /* TINYOS_ARCH64_H */
