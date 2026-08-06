/* SPDX-License-Identifier: GPL-2.0-only */

# tinyos low-level entry & stubs (the little bit that must be assembly)
    .code32

# ---- kernel entry: placed first in the image (0x10000) ----
    .section .text.boot
    .global _start
    .extern kmain
_start:
    cli
    movw    $0x10, %ax              # KDATA_SEL
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %fs
    movw    %ax, %gs
    movw    %ax, %ss
    movl    $0x90000, %esp          # KSTACK_TOP
    call    kmain
1:  hlt
    jmp     1b

    .section .text

# ---- int 0x80 syscall entry (hardware path) ----
# Builds a `struct regs` on the stack, calls C syscall_dispatch(&regs), returns.
    .global syscall_stub
    .extern syscall_dispatch
syscall_stub:
    pusha                           # EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
    pushl   %ds
    pushl   %es
    pushl   %fs
    pushl   %gs
    movw    $0x10, %ax              # kernel data segments
    movw    %ax, %ds
    movw    %ax, %es
    pushl   %esp                    # -> struct regs *
    call    syscall_dispatch
    addl    $4, %esp
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    popa
    iret

# ---- IRQ0 (PIT timer) handler: preempts the running ring-3 task ----
    .global irq0_stub
    .extern timer_tick
irq0_stub:
    pusha
    pushl   %ds
    pushl   %es
    pushl   %fs
    pushl   %gs
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    pushl   %esp                # -> struct regs *
    call    timer_tick
    addl    $4, %esp
    movb    $0x20, %al          # end-of-interrupt to master PIC
    outb    %al, $0x20
    popl    %gs
    popl    %fs
    popl    %es
    popl    %ds
    popa
    iret

# ---- IRQ1 (PS/2 keyboard) handler ----
    .global irq1_stub
    .extern on_keyboard
irq1_stub:
    pusha
    pushl   %ds
    pushl   %es
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    call    on_keyboard
    movb    $0x20, %al          # end-of-interrupt to master PIC
    outb    %al, $0x20
    popl    %es
    popl    %ds
    popa
    iret

# ---- switch_context(uint32_t *save_esp, uint32_t load_esp) ----
# Per-task-kernel-stack context switch used by the scheduler on real hardware:
# push callee-saved regs, stash ESP into the outgoing task, load the incoming
# task's ESP, pop its regs, return onto its stack.  (Under the Unicorn emulator
# the equivalent save/restore is performed by tools/emulate.py.)
    .global switch_context
switch_context:
    movl    4(%esp), %eax       # save_esp (outgoing)
    movl    8(%esp), %edx       # load_esp (incoming)
    pushl   %ebx
    pushl   %esi
    pushl   %edi
    pushl   %ebp
    movl    %esp, (%eax)        # *save_esp = current kernel esp
    movl    %edx, %esp          # switch to incoming kernel stack
    popl    %ebp
    popl    %edi
    popl    %esi
    popl    %ebx
    ret

    .section .note.GNU-stack,"",@progbits
