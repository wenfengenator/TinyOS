/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos ring-3 self-test payload (Cycle 4, Phase B).
 *
 * A minimal position-independent user program: it issues SYS_WRITE to print a
 * line, then SYS_EXIT.  Assembled + linked flat and objcopy'd to a raw blob that
 * the kernel embeds and maps into a user address space (see kernel/arch64.c).
 *
 * Syscall ABI (mirrors the i386 numbers): rax=number, rdi/rsi/rdx = args,
 * `syscall`, result in rax.  SYS_WRITE=4 (fd, buf, len), SYS_EXIT=1 (code). */
    .code64
    .section .text
    .global _ustart
_ustart:
    leaq    msg(%rip), %rsi            /* buf  */
    movl    $1, %edi                   /* fd = stdout */
    movl    $(msg_end - msg), %edx     /* len  */
    movl    $4, %eax                   /* SYS_WRITE */
    syscall
    xorl    %edi, %edi                 /* exit code 0 */
    movl    $1, %eax                   /* SYS_EXIT */
    syscall
1:  jmp     1b                         /* unreachable */

msg:
    .ascii  "hello from ring 3 (user mode)\n"
msg_end:

    .section .note.GNU-stack,"",@progbits
