/* tinyos x64 arch layer (Cycle 4, Phase B): ring-0/ring-3 privilege split. */
#ifndef TINYOS_ARCH64_H
#define TINYOS_ARCH64_H

#include "boot64.h"

void arch_init(void);                       /* install our GDT + TSS + IDT       */
void arch_user_init(void);                  /* set up the reusable ring-3 region */
int  arch_user_run_named(const char *name, const char *arg); /* run a built-in program; exit code, or -1 if unknown */
const char *arch_user_prog_name(int i);     /* iterate built-in program names (i=0..), or 0 */

/* ring-3 stdout sink + terminal control, provided by the tty layer (kernel/term64.c) */
void usys_write(const char *p, unsigned long n);
void term_clear_active(void);               /* clear the terminal running the program */
void term_gotoxy_active(int row, int col);  /* move its cursor (SYS_GOTOXY)            */

#endif /* TINYOS_ARCH64_H */
