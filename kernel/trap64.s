/* tinyos x64 privilege-level plumbing (Cycle 4, Phase B).
 *
 * All the assembly the ring-0/ring-3 split needs that cannot be expressed in C:
 *   - arch_load_gdt / arch_ltr   : load our GDT and reload every segment + TR
 *   - isr0..isr31 / isr_common   : CPU exception stubs -> C dumper (isr_c)
 *   - syscall_entry              : SYSCALL landing pad (IA32_LSTAR)
 *   - enter_user / sys_exit_resume: drop to ring 3 (iretq) and come back on exit
 *
 * The shared kernel/user stack pointers live HERE in .bss (not in a C file) so
 * the hot paths only ever reference same-object symbols — the -mi386pep EFI link
 * step mis-resolves cross-object data symbol addresses taken from -fPIC C, so we
 * keep every cross-object interaction a plain (PC-relative) CALL. */
    .code64
    .text

/* void arch_load_gdt(void *gdtr) — lgdt, reload data segs, far-return into CS */
    .global arch_load_gdt
arch_load_gdt:
    lgdt    (%rdi)
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movw    %ax, %gs
    leaq    .Lreload(%rip), %rax
    pushq   $0x08
    pushq   %rax
    lretq
.Lreload:
    ret

/* void arch_ltr(uint16_t sel) */
    .global arch_ltr
arch_ltr:
    ltr     %di
    ret

/* void arch_set_syscall_stack(uint64_t rsp_top) */
    .global arch_set_syscall_stack
arch_set_syscall_stack:
    movq    %rdi, g_ksyscall_rsp(%rip)
    ret

/* uint64_t arch_isr_addr(unsigned i) — address of the i-th exception stub */
    .global arch_isr_addr
arch_isr_addr:
    cmpq    $32, %rdi
    jae     1f
    leaq    isr_stub_table(%rip), %rax
    movq    (%rax,%rdi,8), %rax
    ret
1:  leaq    isr_default(%rip), %rax
    ret

/* void arch_setup_syscall_msrs(void) — enable SYSCALL and point LSTAR at us.
 *   STAR[63:48]=0x10 (sysret base: +8=udata 0x18, +16=ucode 0x20)
 *   STAR[47:32]=0x08 (syscall CS = kcode; SS = 0x10 = kdata) */
    .global arch_setup_syscall_msrs
arch_setup_syscall_msrs:
    movl    $0xC0000080, %ecx          /* IA32_EFER */
    rdmsr
    orl     $1, %eax                   /* SCE */
    wrmsr
    movl    $0xC0000081, %ecx          /* IA32_STAR */
    movl    $0x00100008, %edx
    xorl    %eax, %eax
    wrmsr
    movl    $0xC0000082, %ecx          /* IA32_LSTAR = syscall_entry */
    leaq    syscall_entry(%rip), %rax
    movq    %rax, %rdx
    shrq    $32, %rdx
    wrmsr
    movl    $0xC0000084, %ecx          /* IA32_FMASK: clear IF+DF on entry */
    movl    $0x600, %eax
    xorl    %edx, %edx
    wrmsr
    ret

/* SYSCALL entry.  CPU: rcx=user rip, r11=user rflags, CPL now 0, still on the
 * user stack.  Linux ABI: rax=num, args=rdi/rsi/rdx/r10/r8/r9. */
    .global syscall_entry
syscall_entry:
    movq    %rsp, g_user_rsp(%rip)         /* transient: bridge the stack switch  */
    movq    g_ksyscall_rsp(%rip), %rsp     /* switch to this level's kernel stack  */
    pushq   g_user_rsp(%rip)               /* save user rsp ON the kernel stack    */
    pushq   %rcx                           /* user rip                             */
    pushq   %r11                           /* user rflags                          */
    pushq   %rdi                           /* preserve caller-saved regs the user  */
    pushq   %rsi                           /* expects intact across a syscall      */
    pushq   %rdx
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r9                           /* C arg6 = Linux syscall arg5        */
    movq    %r8, %r9                      /* C arg5 = Linux syscall arg4        */
    movq    %r10, %r8                     /* C arg4 = Linux syscall arg3        */
    movq    %rdx, %rcx                     /* shuffle args -> SysV C ABI           */
    movq    %rsi, %rdx
    movq    %rdi, %rsi
    movq    %rax, %rdi
    call    syscall_c                      /* rax = return value                   */
    addq    $8, %rsp                       /* discard stacked C arg6               */
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %r11
    popq    %rcx
    popq    %rsp                           /* restore user rsp (re-entrant)        */
    sysretq

/* void enter_user(uint64_t rip, uint64_t rsp) — save kernel context, iretq to
 * ring 3.  Returns (to its C caller) when the user calls SYS_EXIT. */
    .global enter_user
enter_user:
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    pushq   g_kresume_rsp(%rip)            /* save previous resume point (nesting) */
    movq    %rsp, g_kresume_rsp(%rip)
    pushq   $0x1B                          /* SS  = udata | 3           */
    pushq   %rsi                           /* user RSP                  */
    pushq   $0x002                         /* RFLAGS (IF=0)             */
    pushq   $0x23                          /* CS  = ucode | 3           */
    pushq   %rdi                           /* user RIP                  */
    movq    %rdx, %rdi                     /* arg0 in rdi for umain(arg) */
    iretq

/* void sys_exit_resume(void) — restore kernel context saved by enter_user and
 * return as though enter_user() had returned normally. */
    .global sys_exit_resume
sys_exit_resume:
    movq    g_kresume_rsp(%rip), %rsp
    popq    g_kresume_rsp(%rip)            /* restore previous resume point        */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret

/* ---- preemptive scheduler: LAPIC-timer ISR + kernel-thread context switch ----
 * In long mode the CPU pushes a full [SS][RSP][RFLAGS][CS][RIP] frame on every
 * interrupt (even same-privilege).  Save every GP register, hand the saved rsp
 * to sched_tick (which returns the next thread's rsp), switch stacks, restore
 * that thread's registers, iretq. */
    .global timer_isr
timer_isr:
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rsp, %rdi                 /* arg0 = interrupted thread's rsp */
    call    sched_tick                 /* rax = next thread's rsp         */
    movq    %rax, %rsp                 /* switch kernel stacks            */
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    iretq

/* First entry of a new kernel thread: iretq lands here with rdi=fn, rsi=arg. */
    .global thread_trampoline
thread_trampoline:
    movq    %rdi, %rax                 /* rax = fn         */
    movq    %rsi, %rdi                 /* rdi = arg (arg0) */
    call    *%rax                      /* fn(arg)          */
    call    sched_exit                 /* retire if it returns */
1:  jmp     1b

/* Spurious LAPIC interrupt: no EOI required, just return. */
    .global spurious_isr
spurious_isr:
    iretq

/* PC-relative address accessors (C taking asm-symbol addresses is unsafe under
 * the -mi386pep EFI link; hand them back from asm instead). */
    .global timer_isr_addr
timer_isr_addr:
    leaq    timer_isr(%rip), %rax
    ret
    .global spurious_isr_addr
spurious_isr_addr:
    leaq    spurious_isr(%rip), %rax
    ret
    .global thread_trampoline_addr
thread_trampoline_addr:
    leaq    thread_trampoline(%rip), %rax
    ret

/* ---- exception stubs ---- */
    .macro ISR n
    .global isr\n
isr\n:
    .if (\n==8)||(\n==10)||(\n==11)||(\n==12)||(\n==13)||(\n==14)||(\n==17)
    .else
    pushq   $0                             /* dummy error code          */
    .endif
    pushq   $\n
    jmp     isr_common
    .endm

    ISR 0
    ISR 1
    ISR 2
    ISR 3
    ISR 4
    ISR 5
    ISR 6
    ISR 7
    ISR 8
    ISR 9
    ISR 10
    ISR 11
    ISR 12
    ISR 13
    ISR 14
    ISR 15
    ISR 16
    ISR 17
    ISR 18
    ISR 19
    ISR 20
    ISR 21
    ISR 22
    ISR 23
    ISR 24
    ISR 25
    ISR 26
    ISR 27
    ISR 28
    ISR 29
    ISR 30
    ISR 31

/* Unhandled hardware interrupt (vectors 32..255).  The kernel is polled
 * everywhere except the scheduler, which turns interrupts on for the LAPIC timer;
 * a stray host interrupt (e.g. a VMBus/SynIC vector left pending from boot) must
 * not be fatal, so acknowledge it at the LAPIC and return.  CPU exceptions
 * (0..31) still dump and halt via their own isr0..isr31 stubs. */
isr_default:
    pushq   %rax
    movq    $0xFEE000B0, %rax              /* LAPIC EOI register (xAPIC MMIO) */
    movl    $0, (%rax)
    popq    %rax
    iretq

/* stack at entry: [vector][errcode][rip][cs][rflags][rsp][ss] */
isr_common:
    movq    0(%rsp), %rdi                  /* vector    */
    movq    8(%rsp), %rsi                  /* err code  */
    movq    16(%rsp), %rdx                 /* fault rip */
    call    isr_c
1:  cli
    hlt
    jmp     1b

    .section .rodata
    .align 8
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

    .bss
    .align 8
g_user_rsp:      .quad 0
g_ksyscall_rsp:  .quad 0
g_kresume_rsp:   .quad 0

    .section .note.GNU-stack,"",@progbits
