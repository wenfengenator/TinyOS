    .code64
    .section .text
    .global _start
/* Phase G: syscall long-tail + procfs.  Exercise uname, access, open+read of
 * /proc/meminfo, nanosleep, getppid.  exit_group(0) only on full success. */
_start:
    subq    $704, %rsp
    movq    %rsp, %rdi                    /* uname(buf) */
    movl    $63, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    cmpb    $84, (%rsp)                   /* sysname starts with 'T' (TinyOS) */
    jne     .Lfail
    leaq    pmem(%rip), %rdi              /* access("/proc/meminfo", 0) */
    xorl    %esi, %esi
    movl    $21, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    leaq    pmem(%rip), %rdi              /* open("/proc/meminfo", O_RDONLY) */
    xorl    %esi, %esi
    xorl    %edx, %edx
    movl    $2, %eax
    syscall
    testq   %rax, %rax
    js      .Lfail
    movl    %eax, %r12d                   /* fd */
    movl    %r12d, %edi                   /* read(fd, buf, 200) */
    leaq    400(%rsp), %rsi
    movl    $200, %edx
    xorl    %eax, %eax
    syscall
    testq   %rax, %rax
    jle     .Lfail
    cmpb    $77, 400(%rsp)                /* content starts with 'M' (MemTotal) */
    jne     .Lfail
    movl    %r12d, %edi                   /* close(fd) */
    movl    $3, %eax
    syscall
    movq    $0, 608(%rsp)                 /* timespec {0, 1000000} = 1ms */
    movq    $1000000, 616(%rsp)
    leaq    608(%rsp), %rdi               /* nanosleep(&ts, NULL) */
    xorl    %esi, %esi
    movl    $35, %eax
    syscall
    testq   %rax, %rax
    jnz     .Lfail
    movl    $110, %eax                    /* getppid (exercise) */
    syscall
    xorl    %edi, %edi                    /* exit_group(0) */
    movl    $231, %eax
    syscall
.Lfail:
    movl    $1, %edi
    movl    $231, %eax
    syscall

    .section .rodata
pmem:   .asciz "/proc/meminfo"
