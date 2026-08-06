/* SPDX-License-Identifier: GPL-2.0-only */

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

/* NMI (vector 2): ignore on real HW (watchdog/thermal/spurious) rather than
 * halting or (on an AP with no IDT) triple-faulting; fatal HW errors are #MC. */
    .global isr_nmi
isr_nmi:
    iretq
    .global arch_nmi_addr
arch_nmi_addr:
    leaq    isr_nmi(%rip), %rax
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
    movq    %rbx, g_user_callee+0(%rip)    /* stash callee-saved regs for fork()   */
    movq    %rbp, g_user_callee+8(%rip)
    movq    %r12, g_user_callee+16(%rip)
    movq    %r13, g_user_callee+24(%rip)
    movq    %r14, g_user_callee+32(%rip)
    movq    %r15, g_user_callee+40(%rip)
    movq    %rcx, g_user_rip(%rip)         /* stash user rip (fork child resume)   */
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
    subq    $16, %rsp                      /* save retval (16-byte aligned)        */
    movq    %rax, (%rsp)
    movq    %rax, %rdi                     /* signal_check(retval) -> handler or 0  */
    call    signal_check
    cli                                    /* close the SYSRET/IRETQ window: a timer IRQ
                                            * between `popq %rsp` and sysretq would push a
                                            * CPL0 frame onto the USER stack.  IF is restored
                                            * from r11 (sysret) / the frame (iretq) below. */
    testq   %rax, %rax
    jz      .Lsyscall_ret
    movq    %rax, %rdi                     /* deliver a custom handler (never returns) */
    jmp     enter_user_signal
.Lsyscall_ret:
    movq    (%rsp), %rax                   /* restore syscall return value         */
    addq    $16, %rsp
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

/* void enter_user_exec(uint64_t rip, uint64_t rsp, uint64_t cr3) — execve tail:
 * switch to the new address space and iretq into ring 3 at the new entry with a
 * zeroed register file.  Never returns (the process image is replaced). */
    .global enter_user_exec
enter_user_exec:
    pushq   $0x1B                          /* SS  = udata | 3           */
    pushq   %rsi                           /* user RSP                  */
    pushq   $0x202                         /* RFLAGS: IF=1              */
    pushq   $0x23                          /* CS  = ucode | 3           */
    pushq   %rdi                           /* user RIP = new entry      */
    movq    %rdx, %cr3                     /* switch to the new address space */
    xorl    %eax, %eax
    xorl    %ebx, %ebx
    xorl    %ecx, %ecx
    xorl    %edx, %edx
    xorl    %esi, %esi
    xorl    %edi, %edi
    xorl    %ebp, %ebp
    xorq    %r8,  %r8
    xorq    %r9,  %r9
    xorq    %r10, %r10
    xorq    %r11, %r11
    xorq    %r12, %r12
    xorq    %r13, %r13
    xorq    %r14, %r14
    xorq    %r15, %r15
    iretq

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

/* ---- Phase B user-preemption demo: a position-independent ring-3 loop that
 * bumps counter[rbx] at base r15.  Register-only + relative jmp => it can be
 * copied to any page and preempted at any instruction (it never touches its
 * stack), so no stack frame or absolute relocation is involved. */
    .global usched_loop
usched_loop:
1:  incq    (%r15,%rbx,8)
    pause
    jmp     1b
usched_loop_end:

    .global usched_loop_addr
usched_loop_addr:
    leaq    usched_loop(%rip), %rax
    ret
    .global usched_loop_len
usched_loop_len:
    leaq    usched_loop_end(%rip), %rax
    leaq    usched_loop(%rip), %rdx
    subq    %rdx, %rax
    ret

/* Same as usched_loop but blocks in a syscall each pass (demo "blocking read"):
 * bump counter, invoke syscall 0x2000 (kernel calls sched_block), repeat when
 * woken.  Register-only + rel jmp => position-independent like usched_loop. */
    .global usched_block_loop
usched_block_loop:
1:  incq    (%r15,%rbx,8)
    movq    $0x2000, %rax
    xorq    %rdi, %rdi
    syscall
    jmp     1b
usched_block_loop_end:

    .global usched_block_loop_addr
usched_block_loop_addr:
    leaq    usched_block_loop(%rip), %rax
    ret
    .global usched_block_loop_len
usched_block_loop_len:
    leaq    usched_block_loop_end(%rip), %rax
    leaq    usched_block_loop(%rip), %rdx
    subq    %rdx, %rax
    ret

/* Ring-3 exit demo (Phase C): do one unit of work, then exit(42) via demo
 * syscall 0x2001.  The kernel marks the proc a zombie and retires the task. */
    .global usched_exit_prog
usched_exit_prog:
    incq    (%r15,%rbx,8)
    movq    $0x2001, %rax
    movq    $42, %rdi
    syscall
1:  jmp     1b
usched_exit_prog_end:

    .global usched_exit_prog_addr
usched_exit_prog_addr:
    leaq    usched_exit_prog(%rip), %rax
    ret
    .global usched_exit_prog_len
usched_exit_prog_len:
    leaq    usched_exit_prog_end(%rip), %rax
    leaq    usched_exit_prog(%rip), %rdx
    subq    %rdx, %rax
    ret

/* Ring-3 fork demo (Phase C): fork(); the parent bumps counter[1] and exit(8),
 * the child (which resumes here with rax=0) bumps counter[0] and exit(7). */
    .global usched_fork_prog
usched_fork_prog:
    movq    $0x2002, %rax
    syscall
    testq   %rax, %rax
    jz      2f
    incq    8(%r15)
    movq    $0x2001, %rax
    movq    $8, %rdi
    syscall
1:  jmp     1b
2:  incq    (%r15)
    movq    $0x2001, %rax
    movq    $7, %rdi
    syscall
3:  jmp     3b
usched_fork_prog_end:

    .global usched_fork_prog_addr
usched_fork_prog_addr:
    leaq    usched_fork_prog(%rip), %rax
    ret
    .global usched_fork_prog_len
usched_fork_prog_len:
    leaq    usched_fork_prog_end(%rip), %rax
    leaq    usched_fork_prog(%rip), %rdx
    subq    %rdx, %rax
    ret

/* fork support: the user rip/rsp captured at the most recent SYSCALL entry. */
    .global syscall_user_rip
syscall_user_rip:
    movq    g_user_rip(%rip), %rax
    ret
    .global syscall_user_rsp
syscall_user_rsp:
    movq    g_user_rsp(%rip), %rax
    ret
    .global syscall_user_callee
syscall_user_callee:
    leaq    g_user_callee(%rip), %rax      /* &{rbx,rbp,r12,r13,r14,r15} */
    ret

/* Signal delivery: enter a ring-3 handler.  g_sig_rsp = user stack with the
 * restorer return address on top; g_sig_signo = handler arg0; rdi = handler. */
    .global enter_user_signal
enter_user_signal:
    movq    g_sig_rsp(%rip), %rsi
    movq    g_sig_signo(%rip), %rdx
    pushq   $0x1B                          /* SS */
    pushq   %rsi                           /* user RSP = signal frame */
    pushq   $0x202                         /* RFLAGS: IF=1 */
    pushq   $0x23                          /* CS */
    pushq   %rdi                           /* RIP = handler */
    movq    %rdx, %rdi                     /* handler arg0 = signo */
    xorl    %esi, %esi
    xorl    %edx, %edx
    xorl    %ecx, %ecx
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    xorl    %r10d, %r10d
    xorl    %r11d, %r11d
    xorl    %ebx, %ebx
    xorl    %ebp, %ebp
    xorl    %r12d, %r12d
    xorl    %r13d, %r13d
    xorl    %r14d, %r14d
    xorl    %r15d, %r15d
    iretq

/* rt_sigreturn tail: restore the saved context (rdi = &sigframe_ctx) and iretq
 * back to the interrupted instruction. */
    .global resume_context
resume_context:
    pushq   $0x1B                          /* SS */
    pushq   72(%rdi)                       /* saved rsp */
    pushq   64(%rdi)                       /* saved rflags */
    pushq   $0x23                          /* CS */
    pushq   56(%rdi)                       /* saved rip */
    movq    0(%rdi),  %r15
    movq    8(%rdi),  %r14
    movq    16(%rdi), %r13
    movq    24(%rdi), %r12
    movq    32(%rdi), %rbp
    movq    40(%rdi), %rbx
    movq    48(%rdi), %rax
    xorl    %ecx, %ecx
    xorl    %edx, %edx
    xorl    %esi, %esi
    xorl    %r8d, %r8d
    xorl    %r9d, %r9d
    xorl    %r10d, %r10d
    xorl    %r11d, %r11d
    xorl    %edi, %edi
    iretq

/* signal_check helper: stash the delivery frame rsp + signo for enter_user_signal. */
    .global sig_set_frame
sig_set_frame:
    movq    %rdi, g_sig_rsp(%rip)
    movq    %rsi, g_sig_signo(%rip)
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
    movq    24(%rsp), %rcx                 /* fault cs (CPL: ring-3 faults recover) */
    movq    40(%rsp), %r8                  /* interrupted rsp (for the panic backtrace) */
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
g_user_rip:      .quad 0
g_user_callee:   .zero 48
g_sig_rsp:       .quad 0
g_sig_signo:     .quad 0
g_ksyscall_rsp:  .quad 0
g_kresume_rsp:   .quad 0

    .section .note.GNU-stack,"",@progbits
