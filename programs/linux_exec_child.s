/* SPDX-License-Identifier: GPL-2.0-only */

    .section .text
    .global _start
_start:
    cmpq $2, (%rsp)
    jne stack_fail
    mov 8(%rsp), %rdi
    lea expected_argv0(%rip), %rsi
    call string_equal
    test %eax, %eax
    jz stack_fail
    mov 16(%rsp), %rdi
    lea expected_argv1(%rip), %rsi
    call string_equal
    test %eax, %eax
    jz stack_fail
    cmpq $0, 24(%rsp)
    jne stack_fail
    mov 32(%rsp), %rdi
    lea expected_env0(%rip), %rsi
    call string_equal
    test %eax, %eax
    jz stack_fail
    cmpq $0, 40(%rsp)
    jne stack_fail

    mov $1, %rax
    mov $1, %rdi
    lea message(%rip), %rsi
    mov $message_end-message, %rdx
    syscall

    mov $231, %rax
    xor %rdi, %rdi
    syscall

stack_fail:
    mov $1, %rax
    mov $2, %rdi
    lea failure(%rip), %rsi
    mov $failure_end-failure, %rdx
    syscall
    mov $231, %rax
    mov $2, %rdi
    syscall

string_equal:
    movzbq (%rdi), %rax
    cmpb (%rsi), %al
    jne 1f
    test %al, %al
    je 2f
    inc %rdi
    inc %rsi
    jmp string_equal
1:  xor %eax, %eax
    ret
2:  mov $1, %eax
    ret

    .section .rodata
    .space 70000, 0xA5
expected_argv0:
    .asciz "/exec.bin"
expected_argv1:
    .asciz "alpha"
expected_env0:
    .asciz "TINYOS=1"
message:
    .ascii "[elf-large] >64 KiB load + execve stack verified\n"
message_end:
failure:
    .ascii "[execve] argv/envp stack mismatch\n"
failure_end:

    .section .note.GNU-stack,"",@progbits
