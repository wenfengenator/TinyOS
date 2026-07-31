    .code64
    .section .text
    .global _start
/* Phase E.1: default signal action.  Send ourselves SIGTERM (no handler) — the
 * kernel must deliver it at the kill() syscall return and terminate the process
 * with status 128+SIGTERM (=143).  If delivery fails we fall through to exit(7),
 * so the launcher's return value distinguishes the two. */
_start:
    movl    $39, %eax                     /* getpid */
    syscall
    movl    %eax, %edi
    movl    $15, %esi                     /* SIGTERM */
    movl    $62, %eax                     /* kill(self, SIGTERM) */
    syscall
    movl    $7, %edi                      /* only reached if the signal was NOT delivered */
    movl    $231, %eax
    syscall
