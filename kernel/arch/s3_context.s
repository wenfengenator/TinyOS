/* SPDX-License-Identifier: GPL-2.0-only */

/* Save the C ABI context that firmware does not preserve across S3.  The wake
 * trampoline restores CR3/RSP and jumps to s3_context_resume, which unwinds
 * this frame and returns to acpi_s3_enter as if this call returned zero. */
    .code64
    .section .text
    .global arch_s3_save_context
    .hidden arch_s3_save_context
    .extern acpi_s3_commit
arch_s3_save_context:
    pushfq
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %cr3, %rax
    movq    %rax, 0(%rdi)
    movq    %rsp, 8(%rdi)
    leaq    s3_context_resume(%rip), %rax
    movq    %rax, 16(%rdi)
    call    acpi_s3_commit
    jmp     s3_context_unwind

s3_context_resume:
    xorl    %eax, %eax
s3_context_unwind:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    popfq
    ret

    .section .note.GNU-stack,"",@progbits
