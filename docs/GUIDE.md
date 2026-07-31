# tinyos Internals — A Guide for Entry-Level OS Developers

This document explains **how tinyos is built and how it works**, from the first
instruction the CPU runs after power-on all the way up to a C++ application
running in user mode. It assumes you can read C and a little assembly, but it does
**not** assume you already know x86 protected mode, paging, or how system calls
work — those concepts are introduced as we meet them.

If you just want to build and run it, see [`../README.md`](../README.md). For a
terse architecture reference, see [`DESIGN.md`](DESIGN.md). *This* file is the long,
teaching-oriented walkthrough.

> This walkthrough covers the original **32-bit (i386)** kernel. For the
> **64-bit UEFI** edition — long mode, SMP, the framebuffer desktop, persistent
> FAT16 on Hyper-V/QEMU, the Linux-compatible ABI, musl, and the on-device
> self-hosting compiler — see [`ARCHITECTURE.md`](ARCHITECTURE.md).

---

## Table of contents

0. [Background: the concepts you need](#0-background-the-concepts-you-need)
1. [The big picture](#1-the-big-picture)
2. [The memory map](#2-the-memory-map)
3. [Stage 1 — the boot sector (`boot.asm`)](#3-stage-1--the-boot-sector-bootasm)
4. [Stage 2 — kernel entry and `kmain`](#4-stage-2--kernel-entry-and-kmain)
5. [Segmentation: the GDT and the TSS](#5-segmentation-the-gdt-and-the-tss)
6. [Interrupts: IDT, PIC, PIT, and the stubs](#6-interrupts-idt-pic-pit-and-the-stubs)
7. [Memory management: frames and paging (`mm.c`)](#7-memory-management-frames-and-paging-mmc)
8. [Processes and the scheduler (`sched.c`, `proc.c`)](#8-processes-and-the-scheduler-schedc-procc)
9. [System calls (`syscall.c`, `ulib.h`, `crt0.s`)](#9-system-calls-syscallc-ulibh-crt0s)
10. [Console and keyboard](#10-console-and-keyboard)
11. [Filesystems: ramfs, ramdisk, ATA, FAT16, VFS](#11-filesystems-ramfs-ramdisk-ata-fat16-vfs)
12. [The user programs](#12-the-user-programs)
13. [The build system and toolchain](#13-the-build-system-and-toolchain)
14. [Two ways to run it: QEMU vs the Unicorn emulator](#14-two-ways-to-run-it-qemu-vs-the-unicorn-emulator)
15. [End-to-end trace: what happens when you type `cat /mnt/readme.txt`](#15-end-to-end-trace-what-happens-when-you-type-cat-mntreadmetxt)
16. [Reference tables](#16-reference-tables)
17. [Glossary](#17-glossary)
18. [Where to go next](#18-where-to-go-next)

---

## 0. Background: the concepts you need

An operating system's job is to sit between the raw hardware and the programs a
user wants to run. To do that on a 32-bit x86 PC you have to understand a handful
of hardware mechanisms. Here is the minimum mental model; each is expanded later.

### 0.1 What happens when a PC turns on

1. The CPU starts in **real mode** — a 16-bit mode from 1978 where memory is
   addressed as `segment:offset` and there is no memory protection at all. Any
   program can touch any memory and any I/O port.
2. The firmware (**BIOS**) runs, initializes basic hardware, and looks for a
   bootable disk. It reads the **first 512-byte sector** (the *boot sector*) into
   memory at physical address `0x7C00` and jumps there.
3. That 512 bytes is *your* code. Its job is to load the rest of the OS and switch
   the CPU into a modern mode. This is `boot.asm`.

### 0.2 Real mode vs protected mode

**Protected mode** is the 32-bit mode that real operating systems use. It gives you:

* a flat 4 GB address space (offsets are 32 bits),
* **privilege levels** ("rings") so the kernel can be protected from applications,
* **paging** (virtual memory), and
* a structured interrupt mechanism.

Switching from real to protected mode requires setting up a *descriptor table*
(the GDT, below) and flipping one bit in a control register. tinyos does this in
the boot sector.

### 0.3 Privilege rings

x86 has four privilege rings, 0 (most privileged) to 3 (least). Real systems use
only two:

* **Ring 0 — the kernel.** Can execute any instruction, touch any memory, do I/O.
* **Ring 3 — user programs.** Restricted: no direct I/O, can't touch kernel memory,
  can't run privileged instructions. If it tries, the CPU faults into the kernel.

The only *legitimate* way for ring-3 code to ask the kernel to do something is a
**system call** (§9). This ring separation is what stops a buggy app from crashing
the whole machine.

### 0.4 Segmentation and the GDT

Before paging existed, x86 divided memory using **segments**. Even though tinyos
uses paging for the real work, x86 *still* requires a **Global Descriptor Table
(GDT)**: a table of "segment descriptors" that define code/data regions and, more
importantly for us, their privilege level. Loading a segment *selector* (a small
number like `0x08`) into a segment register points the CPU at one of these
descriptors. tinyos uses "flat" segments (base 0, limit 4 GB) so segmentation
effectively disappears, leaving paging to do the real memory work. See §5.

### 0.5 Interrupts and the IDT

An **interrupt** makes the CPU stop what it is doing, save a little state, and jump
to a handler. There are three flavours you'll meet:

* **Hardware interrupts (IRQs)** — a device (timer, keyboard) signals the CPU.
* **Exceptions** — the CPU itself detects a problem (divide-by-zero, page fault).
* **Software interrupts** — a program deliberately executes `int N` to call into
  the kernel. tinyos uses `int 0x80` for system calls, just like old Linux.

The **Interrupt Descriptor Table (IDT)** maps each interrupt number (0–255) to a
handler function and the privilege level allowed to trigger it. See §6.

### 0.6 Paging and virtual memory

**Paging** lets each process see its own private 4 GB **virtual** address space.
The CPU translates virtual addresses to physical ones using **page tables** that
the kernel builds; the `CR3` register points at the current process's top-level
table. Two benefits:

* **Isolation** — process A cannot even *name* process B's memory.
* **Flexibility** — the same virtual address (e.g. `0x40000000`) can map to
  different physical RAM in different processes.

tinyos enables paging early and gives each `exec`'d program its own address space.
See §7.

### 0.7 System calls

A system call is a controlled doorway from ring 3 to ring 0. The user puts a
*number* (which service) and *arguments* in registers, executes `int 0x80`, and
the CPU switches to ring 0 and runs the kernel's handler. tinyos's ABI: syscall
number in `EAX`, arguments in `EBX`/`ECX`/`EDX`, result back in `EAX`. See §9.

With those seven ideas you can understand the entire system. Let's build it up.

---

## 1. The big picture

tinyos is a tiny but complete Unix-flavoured OS. The layers, from lowest to highest:

```
┌─────────────────────────────────────────────────────────────────────────┐
│  USER SPACE (ring 3)   login · shell · vi · hello · count · workers     │
│      talk to the kernel ONLY through  int 0x80  system calls            │
├─────────────────────────────────────────────────────────────────────────┤
│  SYSTEM-CALL INTERFACE   int 0x80  →  syscall_stub  →  syscall_dispatch │
├─────────────────────────────────────────────────────────────────────────┤
│  KERNEL (ring 0, C)                                                     │
│    scheduler · paging/MM · VFS (ramfs + FAT16) · ramdisk · drivers      │
│    (PS/2 keyboard, PIT timer, ATA disk, VGA/serial console)             │
├─────────────────────────────────────────────────────────────────────────┤
│  BOOT (16-bit asm)   BIOS → boot.asm → protected mode → kernel          │
├─────────────────────────────────────────────────────────────────────────┤
│  HARDWARE / EMULATOR   qemu-system-i386   or  tools/emulate.py (Unicorn)│
└─────────────────────────────────────────────────────────────────────────┘
```

**The language rule of the project:** assembly is used *only* where it is
unavoidable — the boot sector, the mode switch, and the tiny interrupt/entry stubs.
Everything else in the kernel is C; user applications are C++.

**The life of the system**, end to end:

```
power on
  → BIOS loads boot.asm at 0x7C00
    → boot.asm loads the kernel to 0x10000, enters 32-bit protected mode, jumps in
      → entry.s:_start sets up the stack, calls kmain()
        → kmain(): GDT, IDT, TSS, PIC, PIT, paging, filesystems, tasks
          → enter_user(): iret into the login task in ring 3
            → login authenticates, exec()s the shell
              → shell reads commands and runs programs (ls, cat, vi, run, …)
```

The rest of this guide walks each arrow.

---

## 2. The memory map

Everything below refers to these physical addresses (from `kernel/kernel.h` and
`kernel/mm.h`). Keeping this table handy makes the code much easier to read.

| Region                         | Address range               | Purpose                                              |
|--------------------------------|-----------------------------|------------------------------------------------------|
| Boot sector                    | `0x07C00`                   | Where BIOS loads `boot.asm`                          |
| Shared console cursor          | `0x00600`                   | One `uint32` cell index (kernel & emulator share it) |
| Kernel image                   | `0x10000` →                 | `kernel.bin` (code+data+ramdisk) loaded here         |
| Per-task ring-0 (kernel) stacks| `0x90000` down, 4 KB apart  | `KSTACK_TOP - i*0x1000`                               |
| Per-task ring-3 (user) stacks  | `0x80000` down, 16 KB apart | `USTACK_TOP(i) = 0x80000 - i*0x4000` (system tasks)  |
| VGA text buffer                | `0xB8000`                   | 80×25 grid of `(char, attribute)` cells              |
| Frame pool (physical)          | `0x400000` – `0x1000000`    | 12 MB pool the frame allocator hands out             |
| `USER_BASE`                    | `0x40000000`                | Where `exec`'d program images are mapped (virtual)   |
| `ARG_ADDR`                     | `0x50000000`                | The `exec` argument string page (virtual)            |
| `USER_STACK_TOP`               | `0xB0000000`                | Top of an `exec`'d program's user stack (virtual)    |

Two things to notice:

* Addresses **below 16 MB** are **identity-mapped** — virtual address == physical
  address. The kernel lives here, and so do the "system" tasks (login, workers).
* Addresses like `0x40000000` are **only virtual** — they exist inside an `exec`'d
  program's private page tables and map to frames from the pool. Two programs both
  using `0x40000000` see *different* physical RAM. That is memory isolation.

---

## 3. Stage 1 — the boot sector (`boot.asm`)

The boot sector is 512 bytes of 16-bit code that BIOS loads at `0x7C00`. Its job is
to get us into 32-bit protected mode with the kernel loaded. Written in GNU
assembler (AT&T syntax), `--32` … actually `.code16` for the real-mode part.

**Step 1 — set up a stack and save the boot drive.**

```asm
_start:
    cli                     # no interrupts while we set up
    xorw  %ax, %ax
    movw  %ax, %ds ; %es ; %ss
    movw  $0x7c00, %sp      # stack just below our code
    movb  %dl, boot_drive   # BIOS passed the boot drive number in DL
    sti
```

**Step 2 — load the kernel from disk.** BIOS interrupt `int 0x13` reads sectors
using ancient *cylinder/head/sector* (CHS) addressing. We read 128 sectors
(64 KB) starting at sector 2 (sector 1 *is* the boot sector) into `0x1000:0000`,
which is physical `0x10000`:

```asm
    movw  $0x1000, %ax ; movw %ax, %es   # ES:BX = 0x1000:0000 = 0x10000
    xorw  %bx, %bx
    movb  $0x02, %ah                     # int 0x13 function 2 = read sectors
    movb  $128,  %al                     # 128 sectors = 64 KB
    movb  $0x00, %ch ; movb $0x02, %cl   # cylinder 0, sector 2
    movb  $0x00, %dh                     # head 0
    movb  boot_drive, %dl
    int   $0x13
    jc    disk_error                     # carry flag set = read failed
```

> **Beginner note.** The kernel *must* fit in those 64 KB. The Makefile prints
> `kernel.bin = NNNNN bytes` and warns if it exceeds 65536. If you add a lot of
> code you must raise `KERNEL_SECT` in `boot.asm`.

**Step 3 — enable the A20 line.** For historical reasons the 21st address line
(A20) is disabled at boot, which would make memory wrap around at 1 MB. We enable
it via the "fast A20" port `0x92`:

```asm
    inb   $0x92, %al ; orb $0x02, %al ; outb %al, $0x92
```

**Step 4 — install a temporary GDT and enter protected mode.** We need a GDT even
to switch modes (§5). The boot sector's GDT has three entries: a mandatory null
descriptor, a flat 32-bit **code** segment (selector `0x08`), and a flat 32-bit
**data** segment (selector `0x10`):

```asm
    cli
    lgdt  gdtr              # load the GDT register
    movl  %cr0, %eax
    orl   $1, %eax          # set the PE (Protection Enable) bit
    movl  %eax, %cr0        # we are now in 32-bit protected mode…
    ljmp  $0x08, $pm_entry  # …but CS still holds an old value; a FAR JUMP fixes it
```

That far jump is essential: setting `CR0.PE` turns on protected mode, but the code
segment register still caches the old real-mode value. A far jump reloads `CS` with
the new 32-bit code selector `0x08` and continues at `pm_entry` in 32-bit mode:

```asm
    .code32
pm_entry:
    movw  $0x10, %ax        # load the data selector into all data segments
    movw  %ax, %ds ; %es ; %fs ; %gs ; %ss
    movl  $0x90000, %esp    # a stack for the kernel
    ljmp  $0x08, $0x10000   # jump to the kernel entry point
```

Finally, the last two bytes of the sector are the **boot signature** `0x55 0xAA`,
which BIOS checks to decide the disk is bootable:

```asm
    .org  510
    .byte 0x55, 0xaa
```

At this point the CPU is in 32-bit protected mode, the kernel is at `0x10000`, and
we jump into it.

---

## 4. Stage 2 — kernel entry and `kmain`

The very first bytes of `kernel.bin` are **not** C — they are the assembly stub
`_start` in `kernel/entry.s`. The linker script (`kernel/linker.ld`) guarantees
this by placing the special section `.text.boot` first at `0x10000`:

```
ENTRY(_start)
SECTIONS { . = 0x10000; .text : { *(.text.boot) *(.text) *(.text.*) } … }
```

So when `boot.asm` jumps to `0x10000`, it lands on:

```asm
    .section .text.boot
_start:
    cli
    movw  $0x10, %ax        # reload data segments (belt and braces)
    movw  %ax, %ds ; %es ; %fs ; %gs ; %ss
    movl  $0x90000, %esp    # KSTACK_TOP: the kernel's stack
    call  kmain             # into C!
1:  hlt ; jmp 1b            # kmain never returns
```

`kmain` (in `kernel/kernel.c`) is the C entry point and reads like a checklist of
"things an OS must set up," in order:

```c
void kmain(void) {
    serial_init();          // COM1 serial port (so we can run headless)
    console_clear();        // wipe the VGA screen
    setup_gdt();            // our real 6-entry GDT (ring 0 + ring 3 + TSS)
    setup_idt();            // interrupt table: int 0x80, IRQ0, IRQ1
    setup_tss();            // task-state segment (holds the ring-0 stack pointer)
    pic_remap();            // move hardware IRQs off the CPU's exception vectors
    pit_init(100);          // program the timer to fire 100×/second
    mm_init(); paging_enable();  // build page tables, turn on paging
    fs_init();              // seed the in-RAM filesystem
    fat_mount();            // mount the FAT16 disk at /mnt (if present)

    task_create(0, &login_main,  0, "login",   T_READY);    // task 0
    task_create(1, &worker_main, 1, "workerA", T_BLOCKED);  // task 1
    task_create(2, &worker_main, 2, "workerB", T_BLOCKED);  // task 2
    g_ntask = 3; g_current = 0;

    enter_user(g_tasks[0].eip, g_tasks[0].ustack);  // drop to ring 3, run login
}
```

Each of these steps is a section below. The final call, `enter_user`, is where the
kernel deliberately *demotes itself* to ring 3 to run the first user program — we
cover it in §9.

---

## 5. Segmentation: the GDT and the TSS

### 5.1 The GDT

`setup_gdt` in `kernel.c` installs the real GDT with **six** descriptors:

| Index | Selector | Purpose         | Access byte | Ring |
|------:|:--------:|-----------------|:-----------:|:----:|
| 0     | `0x00`   | null (required) | `0x00`      | —    |
| 1     | `0x08`   | kernel code     | `0x9A`      | 0    |
| 2     | `0x10`   | kernel data     | `0x92`      | 0    |
| 3     | `0x18`   | user code       | `0xFA`      | 3    |
| 4     | `0x20`   | user data       | `0xF2`      | 3    |
| 5     | `0x28`   | TSS             | `0x89`      | —    |

All the code/data segments are **flat**: base `0x00000000`, limit `0xFFFFF` with
4 KB granularity = the full 4 GB. So a "segment" imposes no address translation; it
exists only to carry the **privilege level** and the code/data distinction.

A **selector** is the value you load into a segment register. It is the table index
shifted left by 3, OR-ed with the requested privilege level (RPL) in the low 2 bits.
That's why the user selectors are `0x1B` and `0x23`, not `0x18` and `0x20`:

```
0x18 (user code index) | 3 (RPL) = 0x1B  →  UCODE_SEL
0x20 (user data index) | 3 (RPL) = 0x23  →  UDATA_SEL
```

When the CPU is running with `CS = 0x1B`, its **Current Privilege Level (CPL)** is
3 — it's user code. When `CS = 0x08`, CPL is 0 — kernel code.

### 5.2 The TSS

The **Task State Segment** is an x86 structure the kernel must provide for one
specific reason in tinyos: **when an interrupt happens while the CPU is in ring 3,
the CPU needs a ring-0 stack to switch to.** It finds that stack pointer in the
TSS's `esp0`/`ss0` fields.

```c
static void setup_tss(void) {
    tss.ss0  = KDATA_SEL;    // kernel stack segment
    tss.esp0 = KSTACK_TOP;   // = 0x90000, the kernel stack to use on ring3→ring0
    tss.iomap_base = sizeof(tss);
    __asm__ volatile("ltr %0" :: "r"((uint16_t)TSS_SEL));  // load task register
}
```

So the moment a user program executes `int 0x80` (or the timer fires while user
code runs), the CPU automatically switches to the kernel stack at `0x90000` before
running the handler. Every task shares this one kernel stack in tinyos — a
simplification that works because the kernel never blocks mid-syscall on this path.

---

## 6. Interrupts: IDT, PIC, PIT, and the stubs

### 6.1 The IDT

`setup_idt` fills in just three of the 256 interrupt vectors — that's all tinyos
needs:

```c
idt_set(0x20, irq0_stub,    KCODE_SEL, 0x8E);  // IRQ0  = PIT timer
idt_set(0x21, irq1_stub,    KCODE_SEL, 0x8E);  // IRQ1  = PS/2 keyboard
idt_set(0x80, syscall_stub, KCODE_SEL, 0xEE);  // int 0x80 = system call
```

The **type byte** matters:

* `0x8E` = 32-bit interrupt gate, **DPL 0** — only ring 0 (or hardware) may invoke.
* `0xEE` = 32-bit interrupt gate, **DPL 3** — ring-3 code is *allowed* to execute
  `int 0x80`. If `int 0x80` were `0x8E`, a user program calling it would fault.

### 6.2 The PIC

Hardware IRQs arrive through two chained **8259 Programmable Interrupt
Controllers**. By default they deliver IRQ0–15 on CPU vectors 0–15 — which collide
with CPU *exceptions* (e.g. vector 8 is both "double fault" and the RTC IRQ). So the
first thing any kernel does is **remap** them. `pic_remap` moves IRQ0–15 to vectors
`0x20`–`0x2F`:

```c
outb(0x21, 0x20); outb(0xA1, 0x28);   // master IRQs → 0x20+, slave → 0x28+
…
outb(0x21, 0xFC); outb(0xA1, 0xFF);   // mask everything except IRQ0 and IRQ1
```

`0xFC` = binary `11111100`, i.e. bits 0 and 1 clear = IRQ0 (timer) and IRQ1
(keyboard) **enabled**, everything else ignored.

### 6.3 The PIT

The **8253/8254 Programmable Interval Timer** is our heartbeat. `pit_init(100)`
programs channel 0 to fire IRQ0 100 times per second:

```c
uint32_t div = 1193182u / hz;      // the PIT's fixed input clock / desired rate
outb(0x43, 0x36);                  // channel 0, lo/hi byte, mode 3 (square wave)
outb(0x40, div & 0xFF); outb(0x40, div >> 8);
```

Those 100 ticks/second are what make multitasking **preemptive** (§8): every
10 ms the timer interrupts whatever is running and gives the scheduler a chance to
switch tasks.

### 6.4 The stubs and the `struct regs` frame

The IDT points at *assembly* stubs (`entry.s`), not C functions, because on entry
we must save every register by hand. The `int 0x80` stub is the template:

```asm
syscall_stub:
    pusha                 # push EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI (8 regs)
    pushl %ds ; %es ; %fs ; %gs
    movw  $0x10, %ax      # switch to kernel data segments
    movw  %ax, %ds ; %es
    pushl %esp            # pass a pointer to everything we just pushed…
    call  syscall_dispatch  # …as a `struct regs *`
    addl  $4, %esp
    popl  %gs ; %fs ; %es ; %ds
    popa
    iret                  # return to whoever we interrupted
```

The registers pushed by the stub, **plus** the `EIP/CS/EFLAGS/ESP/SS` the CPU
itself pushes when crossing from ring 3 to ring 0, form the `struct regs` in
`kernel.h`:

```c
struct regs {
    uint32_t gs, fs, es, ds;                             // pushed by the stub
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax; // pusha
    uint32_t eip, cs, eflags, useresp, ss;               // pushed by the CPU
};
```

This layout is the key trick behind context switching: because the handler receives
a *pointer to the caller's saved registers*, it can **rewrite them** and the `iret`
at the end will resume with the modified values — possibly belonging to a *different
task* (§8.4).

The `irq0_stub` (timer) and `irq1_stub` (keyboard) are similar, but they also send
an **End-Of-Interrupt** to the PIC (`outb 0x20, 0x20`) so the next IRQ can be
delivered. `int 0x80` is a software trap, not a hardware IRQ, so it sends no EOI.

> **Historical note.** `kernel/timer.c` also defines `on_timer()` and `entry.s`
> defines `switch_context()`. These illustrate the "textbook" design (an IRQ0 that
> calls a stack-swapping routine). The path tinyos *actually* uses is the
> frame-rewrite in `timer_tick`→`ctx_switch` (§8.4); the other two are left as
> readable references and are not on the hot path.

---

## 7. Memory management: frames and paging (`mm.c`)

### 7.1 The physical frame allocator

Physical RAM from 4 MB to 16 MB is the **frame pool**. `mm.c` tracks which 4 KB
**frames** are free using a bitmap:

```c
uint32_t frame_alloc(void) {      // find a free frame, zero it, return its address
    for (i = 0; i < NFRAMES; i++)
        if (!bit_get(i)) { bit_set(i); memset(pa,0,PAGE_SIZE); return pa; }
    return 0;                     // out of memory
}
```

Because the low 16 MB is identity-mapped, the returned physical address is *also* a
usable pointer — the kernel can `memset` it directly. That is why frame handling is
so short here.

### 7.2 Page tables and the two "kernel" mappings

An x86 32-bit page translation is two levels: a **page directory** (1024 entries,
one per 4 MB) points at **page tables** (1024 entries × 4 KB pages). `CR3` holds the
physical address of the current page directory.

`mm_init` builds two complete identity maps of the low 16 MB (four page tables
each), differing only in one flag:

```c
kpt_u[t][i] = pa | PRESENT | RW | USER;   // "_u": user-accessible
kpt_s[t][i] = pa | PRESENT | RW;          // "_s": supervisor-only (no USER bit)
```

* `kpd_u` (**u**ser-accessible) — ring-3 code *can* read/write these pages.
  `g_kernel_pd = kpd_u`.
* `kpd_s` (**s**upervisor-only) — ring-3 code touching these pages faults.

Why two? Because tinyos has **two kinds of ring-3 tasks**:

1. **System tasks** (login, workers) created by `task_create`. They run in ring 3
   but *share* `g_kernel_pd = kpd_u`, so they live in identity-mapped low memory and
   are **not** isolated from the kernel. They are a convenience for early boot.
2. **`exec`'d programs** (shell, vi, hello, count). They get a fresh address space
   from `vm_create_user()` built on `kpd_s`, so the kernel identity map is
   *supervisor-only* and their code can only reach their own user pages — **real
   isolation**.

### 7.3 Mapping, isolation, and `fork`

`vm_map(pd, va, pa, flags)` installs one page: it allocates a page table for that
4 MB region if needed, then writes the page-table entry. `do_exec` uses it to map a
program's image at `USER_BASE`, a stack below `USER_STACK_TOP`, and the argument
page at `ARG_ADDR`.

Because each `exec`'d program has its own page directory, `0x40000000` maps to
different physical frames in each — process A literally cannot form a pointer to
process B's memory.

`fork` needs a *copy* of an address space. `vm_clone` walks every user mapping
(virtual address ≥ `USER_BASE`), allocates a fresh frame for each, copies the
contents, and maps it into the new directory:

```c
uint32_t vm_clone(uint32_t src) {
    uint32_t dst = vm_create_user();
    for each present user page in src:
        np = frame_alloc(); memcpy(np, src_page, PAGE_SIZE);
        vm_map(dst, va, np, USER | RW);
    return dst;
}
```

### 7.4 Turning it on

```c
void paging_enable(void) {
    vm_switch(g_kernel_pd);              // load CR3
    cr0 |= 0x80000000; write CR0;        // set the PG (paging) bit
}
```

After this instruction, **every** memory access goes through the page tables.

---

## 8. Processes and the scheduler (`sched.c`, `proc.c`)

### 8.1 The task control block

Everything the kernel knows about a task is one `task_t` (the "TCB"):

```c
typedef struct {
    uint32_t eax..eflags;    // saved CPU context (10 registers)  offsets 0..36
    uint32_t state;          // T_UNUSED/READY/RUNNING/BLOCKED/EXITED
    uint32_t pid, ppid;      // process id, parent id
    uint32_t ustack, cr3, kstack;  // user stack top, address space, kernel stack
    uint32_t exit_code; int32_t wait_on;  // for wait()/exit()
    char name[16];
} task_t;                    // 88 bytes; there are NTASK = 8 slots
```

> **Important:** this exact layout (field offsets, 88-byte stride) is a *contract*
> with the emulator (`tools/emulate.py`), which reads and writes the same table.
> Don't reorder fields without updating both.

### 8.2 States

```
 T_UNUSED ──create──▶ T_READY ◀──preempt/yield──▶ T_RUNNING ──exit──▶ T_EXITED
                          ▲                            │
                          └──────── wake ◀── wait ─────┘ (T_BLOCKED)
```

### 8.3 Round-robin selection

The scheduler just scans the table for the next runnable task after the current one:

```c
static int ready_after(int cur) {
    for (k = 1; k <= NTASK; k++) { i = (cur + k) % NTASK; if (READY) return i; }
    return -1;
}
```

### 8.4 The context switch — the heart of multitasking

This is the cleverest part of tinyos, so go slowly. Recall from §6.4 that when an
interrupt occurs the stub builds a `struct regs` on the kernel stack and passes a
*pointer* to it. `ctx_switch(struct regs *r, int next)` does the swap **by editing
that frame in place**:

```c
void ctx_switch(struct regs *r, int next) {
    task_t *c = &g_tasks[g_current];
    if (c->state == T_RUNNING) c->state = T_READY;   // outgoing task is runnable again
    // 1. SAVE the interrupted task's live registers OUT of the frame into its TCB
    c->eax=r->eax; …; c->esp=r->useresp; c->eip=r->eip; c->eflags=r->eflags;

    task_t *t = &g_tasks[next];
    // 2. LOAD the next task's registers INTO the frame
    r->eax=t->eax; …; r->useresp=t->esp; r->eip=t->eip; r->eflags=t->eflags;
    r->cs = UCODE_SEL; r->ss = UDATA_SEL; r->ds=r->es=r->fs=r->gs = UDATA_SEL;
    load_cr3(t->cr3);                    // 3. switch to the next task's address space
    if (t->state == T_READY) t->state = T_RUNNING;
    g_current = next;
}
```

Now when the stub finishes with `iret`, the CPU pops `EIP/CS/EFLAGS/ESP/SS` — but
those are now the **next task's** values, so execution resumes in a *different*
program, in *its* address space (`CR3` was reloaded). That is a context switch.

Three entry points call it:

* `timer_tick(r)` — from IRQ0, 100×/s. It preempts **only if the interrupted code
  was ring 3** (`(r->cs & 3) == 3`); it never preempts the kernel itself:
  ```c
  void timer_tick(struct regs *r) {
      g_ticks++;
      if ((r->cs & 3) != 3) return;         // don't preempt ring 0
      int nx = ready_after(g_current);
      if (nx >= 0 && nx != g_current) ctx_switch(r, nx);
  }
  ```
* `sched_yield(r)` — from `SYS_YIELD`, a *cooperative* switch.
* `sched_exit`/`sched_wait` — switch away when a task exits or blocks.

This is why `race` (two `count` programs) shows interleaved output on real
hardware: neither program yields, but the timer forcibly switches between them.

### 8.5 `exec`: loading a program

`do_exec(path, arg)` (in `proc.c`) turns a program image into a running task:

```c
1. rd_get(path, &img, &size);       // find the image in the ramdisk
2. idx = task_alloc();              // grab a free TCB slot
3. pd = vm_create_user();           // fresh, ISOLATED address space
4. map the image at USER_BASE (copying it into fresh frames)
5. map a 16 KB user stack just below USER_STACK_TOP
6. copy `arg` into a frame mapped at ARG_ADDR
7. set eip=USER_BASE, esp=USER_STACK_TOP-16, cr3=pd, state=READY
8. return the new pid
```

A subtlety worth knowing: real Unix `exec` *replaces* the calling process. tinyos's
`exec` instead **spawns a new task** and returns its pid, so callers pair it with
`wait` (the shell does `exec("count"); wait(pid)`). This keeps the model simple.

### 8.6 `fork` and `wait`

`do_fork(r)` copies the caller: a new TCB with the parent's registers, `EAX` forced
to `0` in the child (so the child's `fork()` returns 0 while the parent gets the
child's pid), and a cloned address space (`vm_clone`).

`sched_wait(r, pid)` returns immediately if the target already exited; otherwise it
marks the caller `T_BLOCKED` with `wait_on = pid` and switches away. When a task
calls `exit`, `sched_exit` records the code and `wake_waiters` flips any blocked
parent back to `T_READY`, delivering the exit code via the parent's saved `EAX`.

---

## 9. System calls (`syscall.c`, `ulib.h`, `crt0.s`)

### 9.1 The ABI

The contract between user and kernel:

| Register | Meaning                         |
|----------|---------------------------------|
| `EAX`    | syscall number (in), result (out) |
| `EBX`    | argument 1                      |
| `ECX`    | argument 2                      |
| `EDX`    | argument 3                      |

A user program triggers it with `int 0x80`. See §16 for the full number table.

### 9.2 The user side: `ulib.h`

User programs never write `int 0x80` by hand; they call thin inline wrappers in
`user/ulib.h`:

```cpp
static inline int syscall(int n, int a=0, int b=0, int c=0) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return ret;
}
static inline int write(const char *s){ return syscall(WRITE, STDOUT,(int)s,strlen(s)); }
static inline int read(int fd,char*b,int n){ return syscall(READ, fd,(int)b,n); }
// … open, close, exec, fork, wait, stat, readdir, readfile, writefile, getchar, …
```

### 9.3 The kernel side: `syscall_dispatch`

The `int 0x80` stub (§6.4) calls `syscall_dispatch(struct regs *r)`, which is one big
`switch` on `r->eax`. It writes results back into `r->eax` (which becomes the user's
return value after `iret`):

```c
void syscall_dispatch(struct regs *r) {
    switch (r->eax) {
    case SYS_WRITE:  kprint_n((char*)r->ecx, r->edx, UATTR); r->eax = r->edx; break;
    case SYS_READ:   /* stdin: blocking line editor; else fs_read */ …
    case SYS_EXEC:   r->eax = do_exec((char*)r->ebx, (char*)r->ecx); break;
    case SYS_FORK:   r->eax = do_fork(r); break;
    case SYS_WAIT:   sched_wait(r, (int)r->ebx); break;
    case SYS_READFILE:  r->eax = vfs_readfile(...); break;
    case SYS_WRITEFILE: r->eax = vfs_writefile(...); break;
    …
    }
}
```

Notice how naturally `fork`/`wait`/`yield` fit: because they receive `r` (the saved
context), they can perform a context switch simply by rewriting it.

### 9.4 Entering ring 3 the first time, and `_start`/`umain`

How does the kernel, running in ring 0, get *into* ring 3 to run the first program?
It fakes an interrupt return. `enter_user` pushes a ring-3 `SS/ESP/EFLAGS/CS/EIP`
frame and executes `iret`:

```c
static void enter_user(uint32_t entry, uint32_t ustack) {
    asm("… ; pushl $0x23  /*SS*/ ; pushl %0 /*ESP*/ ; pushl $0x202 /*EFLAGS*/ ;"
        "  pushl $0x1B /*CS=user*/ ; pushl %1 /*EIP*/ ; iret" :: "r"(ustack),"r"(entry));
}
```

`iret` doesn't care whether a *real* interrupt happened — it just loads that frame,
and because `CS = 0x1B` has RPL 3, the CPU drops to ring 3 and starts running the
task's code. This is the standard trick for the first descent into user mode.

For a task created by `task_create` (login, workers), `entry` is a C++ function like
`login_main`. For an `exec`'d program, `entry` is `USER_BASE` — and the first thing
there is **not** `main`, it's `_start` in `programs/crt0.s`:

```asm
_start:
    pushl $0x50000000       # ARG_ADDR: the argument string exec mapped
    call  umain             # call the program's entry
    addl  $4, %esp
    movl  %eax, %ebx        # exit code = umain()'s return value
    movl  $1, %eax          # SYS_EXIT
    int   $0x80
```

**Why `umain` and not `main`?** These programs are compiled *freestanding*
(`-ffreestanding -nostdlib`) — there is no C library, so there is no
`__libc_start_main` to call `main`, hand it `argc/argv`, and call `exit()` with its
return. tinyos supplies that glue itself in this 7-line `crt0`. Using a distinct
name `umain` (declared `extern "C"` so it isn't C++-mangled) makes the custom
contract — `int umain(const char *arg)`, return value becomes the exit code —
explicit and avoids any special compiler treatment of the symbol `main`. It mirrors
exactly how a normal Linux binary works: the kernel jumps to `_start`, not `main`.

---

## 10. Console and keyboard

### 10.1 Output: the VGA text buffer

The screen is memory-mapped at `0xB8000`: an 80×25 grid where each cell is two
bytes — an ASCII character and an **attribute** (foreground/background colour). To
print a character you write a cell and advance a cursor:

```c
VGA[idx] = ((uint16_t)attr << 8) | ch;   // attribute in the high byte
```

The current cursor position is stored as a cell index at the shared address `0x600`
(shared so the emulator and kernel agree on it). `move_cursor` also updates the
hardware cursor via the VGA CRT controller ports `0x3D4`/`0x3D5`, so the blinking
underscore tracks your text. `console_scroll` shifts all rows up when you reach the
bottom.

Colours encode roles: kernel messages are light grey (`KATTR`), user output is
green (`UATTR`), login prompts are yellow (`LATTR`).

### 10.2 The serial mirror

Every character printed is *also* sent to the **COM1 serial port** (`0x3F8`). This
is what makes `make run-serial` work: with no graphics, the whole console appears in
your terminal, and typed characters come back in over the same port. It's invaluable
for automated testing.

### 10.3 Input: the PS/2 keyboard driver

A key press raises **IRQ1**; `irq1_stub` calls `on_keyboard()`, which reads a
**scancode** from port `0x60`. Scancodes are not ASCII — they're key *positions* in
"set 1." The driver translates them via lookup tables:

```c
static const char sc_ascii[128] = { 0,27,'1','2',…,'q','w',… };  // unshifted
static const char sc_shift[128] = { 0,27,'!','@',…,'Q','W',… };  // shifted
```

It maintains modifier state and produces the right byte:

* **Shift** — make codes `0x2A`/`0x36`, break codes `0xAA`/`0xB6`; selects
  `sc_shift`. This is what lets you type `:` (Shift+`;`) for vi's `:wq`.
* **Caps Lock** — `0x3A` toggles; inverts letter case.
* **Extended keys** — arrows, Delete, Home/End, PgUp/PgDn arrive as a `0xE0`
  prefix followed by the code. The driver latches the prefix and maps them to the
  non-ASCII bytes `KEY_UP … KEY_PGDN` (`0x80`+), which `vi` and the shell interpret.
* Everything else with the high bit set is a **key release** and is ignored.

Translated bytes go into the `g_kbd` ring buffer; `kbd_pop()` drains it.

### 10.4 `stdin` and the line editor

`SYS_READ` on fd 0 blocks until input arrives (`sti; hlt; cli` idles the CPU until
an IRQ), draining the keyboard buffer first, then the serial port. It supports three
**echo modes** (`SYS_ECHO`): plain (shell), masked `*` (password), and silent.

In plain mode it is a proper **in-line editor**: ←/→ move the cursor within the
line, Home/End jump, Backspace deletes to the left, Delete removes the character at
the cursor. After each edit it redraws the line and repositions the hardware cursor
using `console_getpos`/`console_setpos`. Login's masked/silent input stays simple
(append-only) and ignores the navigation keys.

---

## 11. Filesystems: ramfs, ramdisk, ATA, FAT16, VFS

tinyos has several storage layers, unified by a small VFS so the shell doesn't care
where a file lives.

```
                 ┌───────────── VFS (vfs.c) ─────────────┐
   path "/mnt/…" │  route by prefix                       │ path "/etc/…", "/tmp/…"
        ┌────────▼─────────┐                     ┌────────▼─────────┐
        │  FAT16 (fat.c)   │                     │  ram fs (fs.c)   │
        │  on a real disk  │                     │  8 files in RAM  │
        └────────┬─────────┘                     └──────────────────┘
        ┌────────▼─────────┐
        │  ATA PIO (ata.c) │  reads/writes 512-byte sectors of -hdb fs.img
        └──────────────────┘

  (separate) ramdisk (rd.c): a read-only 'TRD' archive of PROGRAM images, used by exec
```

### 11.1 The in-RAM filesystem (`fs.c`)

The simplest store: a fixed array of 8 files, each a name plus a 256-byte buffer,
seeded at boot with `/etc/passwd` (`root:toor` / `guest:guest`), `/README`, and an
empty `/tmp/log`. It also owns the global file-descriptor table `g_fds` (fds 3+;
0/1 are the special stdin/stdout handled by the console).

### 11.2 The ramdisk (`rd.c`)

This is *not* a general filesystem — it's a read-only archive of **program images**
that `exec` loads from. `tools/mkramdisk.py` packs the compiled `shell`, `hello`,
`count`, `vi` binaries into a `'TRD'` archive:

```
'TRD\0' | count | count×{ name[16], offset, size } | program bytes…
```

The Makefile embeds that archive directly into `kernel.bin` with `.incbin`, exposing
it as the symbol `__ramdisk_start`. `rd_get("shell", …)` returns a pointer to the
image. (On real hardware the same bytes *could* come from a disk via `block_read` —
the API is deliberately block-shaped.)

### 11.3 The ATA disk driver (`ata.c`)

To mount a *real* disk image we need a disk driver. `ata.c` is a minimal **ATA PIO**
driver that reads and writes 512-byte sectors on the primary bus, slave device
(that's QEMU's `-hdb fs.img`). It talks to the drive by writing the sector number
(LBA) and a command to the I/O ports `0x1F0`–`0x1F7` and polling the status port:

```c
outb(ATA_LBA0..2, lba); outb(ATA_CMD, 0x20 /*READ*/);
ata_poll();                       // wait for BSY clear, DRQ set
for (i=0;i<256;i++) *buf++ = inw(ATA_DATA);   // 256 words = 512 bytes
```

### 11.4 The FAT16 reader/writer (`fat.c`)

FAT16 is the classic DOS filesystem — simple enough to implement in ~250 lines.
`fat_mount` reads sector 0 (the **BPB**, BIOS Parameter Block) to learn the layout:
sectors per cluster, number of FATs, root-directory size, where data begins. Then:

* **Reading** — `fat_lookup`/`fat_list` walk 32-byte directory entries; `fat_read`
  follows the **cluster chain** in the FAT (a linked list of 16-bit cluster numbers)
  to gather a file's data.
* **Writing** (`fat_write`) — to save a file it walks to the parent directory, frees
  the old cluster chain, allocates fresh clusters (`fat_alloc` scans the FAT for a
  free `0x0000` entry), writes the data with `ata_write`, links the new chain,
  updates **every** copy of the FAT, and rewrites the 8.3 directory entry (first
  cluster + size). Because `ata_write` goes straight to the backing image,
  **edits persist across reboots**.

### 11.5 The VFS (`vfs.c`)

The unifier. Any path syscall (`stat`, `readdir`, `readfile`, `writefile`) checks
the prefix: `/mnt` and below go to FAT; everything else goes to ramfs. So the shell's
`cat` and `vi`'s save work identically whether the file is in RAM or on the mounted
disk — they never know the difference.

`tools/mkfatfs.py` builds a valid FAT16 image from the `disk/` directory (the host
has no `mkfs.fat`), and `make fs.img` runs it.

---

## 12. The user programs

| Program            | Where         | Ring | Address space          | Role                                   |
|--------------------|---------------|:----:|------------------------|----------------------------------------|
| `login`            | `user/`       | 3    | shared kernel map (`kpd_u`) | first task; authenticates, execs shell |
| `worker` A/B       | `user/`       | 3    | shared kernel map      | demo tasks that interleave + log        |
| `shell`            | `programs/`   | 3    | **isolated** (`exec`)  | the command line                       |
| `vi`               | `programs/`   | 3    | isolated               | modal full-screen editor               |
| `hello`, `count`   | `programs/`   | 3    | isolated               | tiny demo programs (`run`, `race`)     |

`user/*.cpp` programs are compiled *into the kernel image* and launched by
`task_create`; they share the kernel's (user-accessible) address space. `programs/*.cpp`
are **standalone** — linked at `USER_BASE`, packed into the ramdisk, and loaded by
`exec` into their own isolated address spaces. This split is exactly why `run hello`
and `race` demonstrate real isolation and preemption while `login` and the workers
demonstrate the simpler shared-space model.

The shell (`programs/shell.cpp`) is a good read: it keeps a current working
directory, `resolve()`s relative paths (handling `.` and `..`), and dispatches
`help`/`ls`/`cd`/`pwd`/`cat`/`run`/`vi`/`ps`/`fork`/`race`/`exit` onto the syscalls.

---

## 13. The build system and toolchain

### 13.1 The tools

* **`as` / `ld` / `objcopy`** (GNU binutils) assemble and link, and convert ELF to
  flat binaries.
* **`tools/xcc` / `tools/xcxx`** are wrapper scripts that find a working
  `gcc`/`g++` (they fall back to the WSL host's `gcc-13`/`g++-13` in the sandbox).
* All kernel code is compiled `-m32 -ffreestanding -fno-pic -fno-stack-protector
  -fno-builtin -mno-sse` — i.e. "no operating system, no runtime, plain 32-bit
  code." C++ adds `-fno-exceptions -fno-rtti` (no runtime support for those).

### 13.2 Linker scripts place code at fixed addresses

Flat binaries have no loader to relocate them, so the link address must be right:

* `kernel/linker.ld` links the kernel at `0x10000`, `.text.boot` first (so `_start`
  is exactly at `0x10000`).
* `programs/user.ld` links standalone programs at `USER_BASE` (`0x40000000`),
  `.text._start` first (so `crt0`'s `_start` is first).

### 13.3 The `.bss` gotcha

A flat binary (`objcopy -O binary`) normally *drops* the `.bss` section (zero-init
globals) because it contains no bytes on disk — but then `exec` wouldn't allocate
room for it and a program with big global arrays (like `vi`) would fault. The fix is
in the Makefile: `objcopy --set-section-flags .bss=alloc,load,contents` forces the
zero bytes into the image so `exec` maps them.

### 13.4 Assembling the disk images

```
programs/*.cpp ──xcxx──▶ *.prog ──mkramdisk.py──▶ ramdisk.img ──.incbin──▶ kernel.bin
boot.asm ──as/ld──▶ boot.bin                                                    │
                                    cat boot.bin kernel.bin ▶ os.img ◀──────────┘  (padded to 1.44 MB)
disk/ ──mkfatfs.py──▶ fs.img   (the FAT16 disk mounted at /mnt)
```

---

## 14. Two ways to run it: QEMU vs the Unicorn emulator

tinyos runs on **real `qemu-system-i386`** (`make run` graphical, `make run-serial`
headless) *and* on a bundled **Unicorn-based CPU emulator** (`make run-emu`). Why two?
The emulator exists so you can run and test the OS in environments without QEMU.

The crucial thing to understand for learning: **the emulator runs the real kernel
machine code**, but the Unicorn CPU library has limits — it does *not* vector
`int`/IRQ through the IDT and does *not* deliver hardware interrupts. So
`tools/emulate.py` fills those gaps in Python:

| Mechanism        | On QEMU / real hardware              | Under the Unicorn emulator                 |
|------------------|--------------------------------------|--------------------------------------------|
| Boot + mode switch | real                               | real (runs `boot.asm`)                     |
| Kernel init      | real                                 | real (runs the C `kmain`)                  |
| Paging (CR3)     | real                                 | real — Unicorn walks the page tables       |
| `int 0x80`       | IDT → `syscall_stub` (C)             | intercepted; serviced in Python (same numbers) |
| Preemption       | PIT IRQ0 → `ctx_switch`              | Python time-slices over the same `g_tasks` |
| `/mnt` (FAT)     | ATA driver reads/writes `fs.img`     | a Python `FatImage` reads/writes `fs.img`  |
| Keyboard         | PS/2 IRQ1 → `g_kbd`                   | Python pushes scripted keys into `g_kbd`   |

The **shared contract** that makes this work is the in-memory layout: the `g_tasks`
table (§8.1), the syscall numbers (§16), and the filesystem structures. Python and
the C kernel manipulate the *same bytes*, so behaviour matches. An image edited by
one FAT implementation is valid for the other.

---

## 15. End-to-end trace: what happens when you type `cat /mnt/readme.txt`

Let's connect every layer with one command. Assume you're at the shell prompt.

1. **Keystrokes.** Each key you press raises **IRQ1**. `irq1_stub` → `on_keyboard`
   reads the scancode from port `0x60`, translates it (applying Shift/Caps), and
   pushes an ASCII byte into `g_kbd`. The `t` `t` … of "cat …" accumulate there.

2. **The shell reads a line.** The shell called `sys::read(STDIN, line, 127)`, which
   is `SYS_READ`. The kernel's stdin handler blocks with `sti; hlt`, waking on each
   IRQ1 to drain `g_kbd`, echoing characters (and honouring ←/→/Backspace/Delete via
   the line editor) until you press Enter. It returns the line `cat /mnt/readme.txt`.

3. **The shell parses.** `split()` gives `cmd="cat"`, `arg="/mnt/readme.txt"`.
   `resolve()` turns it into the absolute path `/mnt/readme.txt`.

4. **`readfile` syscall.** The shell calls `sys::readfile(path, buf, 511)` =
   `int 0x80` with `EAX=SYS_READFILE`. The CPU switches to ring 0 (loading the kernel
   stack from the TSS's `esp0`), `syscall_stub` builds a `struct regs`, and
   `syscall_dispatch` runs `vfs_readfile("/mnt/readme.txt", buf, 511)`.

5. **VFS routes to FAT.** The path starts with `/mnt`, so the VFS strips it and calls
   into `fat.c`. `fat_lookup` walks the FAT16 root directory for `README  TXT`,
   finding its first cluster and size.

6. **ATA reads the disk.** `fat_read` follows the cluster chain; for each cluster it
   calls `ata_read(lba, 1, buf)`, which pokes ports `0x1F0`–`0x1F7`, waits for the
   drive, and reads 256 words. On the emulator, `FatImage` returns the same bytes
   from `fs.img` in Python instead.

7. **Back to user space.** The bytes are copied into the shell's `buf`;
   `syscall_dispatch` sets `r->eax` to the byte count; `iret` returns to ring 3 with
   that count in `EAX`.

8. **The shell prints.** It calls `sys::write(buf)` = `SYS_WRITE`, and
   `kprint_n` writes each character into the VGA buffer at `0xB8000` (green
   attribute) and mirrors it to COM1. You see:
   `Hello from a real FAT16 filesystem on a mounted disk image!`

9. **Meanwhile**, every 10 ms the PIT fired IRQ0 → `timer_tick`, but since the only
   ready task was the shell, no switch happened. Had you run `race`, the timer would
   have preempted between the two `count` tasks here.

That single command exercised: the keyboard driver and IRQ1, the stdin line editor,
the syscall mechanism (twice), the VFS, the FAT16 filesystem, the ATA driver, the
console, and the timer — essentially the whole OS.

---

## 16. Reference tables

### 16.1 System calls (`int 0x80`, number in `EAX`)

| #  | Name         | Arguments (EBX, ECX, EDX)        | Returns                    |
|----|--------------|----------------------------------|----------------------------|
| 1  | `exit`       | code                             | (does not return)          |
| 3  | `read`       | fd, buf, len                     | bytes read (fd 0 = stdin)  |
| 4  | `write`      | fd, buf, len                     | bytes written (fd 1 = console) |
| 5  | `open`       | path, flags                      | fd or -1                   |
| 6  | `close`      | fd                               | 0/-1                       |
| 7  | `yield`      | —                                | reschedules                |
| 8  | `getpid`     | —                                | pid                        |
| 9  | `exec`       | path, arg                        | new pid or -1              |
| 10 | `spawn`      | task index                       | unblocks a preset task     |
| 11 | `stat`       | path, {size,isdir}               | 0/-1                       |
| 12 | `lseek`      | fd, off, whence                  | new offset                 |
| 13 | `fork`       | —                                | child pid / 0 / -1         |
| 14 | `wait`       | pid                              | child's exit code          |
| 15 | `readdir`    | path, index, namebuf             | 0/-1                       |
| 16 | `taskinfo`   | index, buf                       | 0/-1 (for `ps`)            |
| 17 | `echo`       | mode (0 plain / 1 mask / 2 silent) | 0                        |
| 18 | `readfile`   | path, buf, maxlen                | bytes read                 |
| 19 | `getchar`    | —                                | one raw key (blocking)     |
| 20 | `clear`      | —                                | clears screen              |
| 21 | `gotoxy`     | row, col                         | positions cursor           |
| 22 | `writefile`  | path, buf, len                   | bytes written (ram or FAT) |

### 16.2 Segment selectors

| Selector | Meaning     | CPL |
|----------|-------------|-----|
| `0x08`   | kernel code | 0   |
| `0x10`   | kernel data | 0   |
| `0x1B`   | user code   | 3   |
| `0x23`   | user data   | 3   |
| `0x28`   | TSS         | —   |

### 16.3 Interrupt vectors used

| Vector | Source        | Handler        | DPL |
|--------|---------------|----------------|-----|
| `0x20` | PIT (IRQ0)    | `irq0_stub`    | 0   |
| `0x21` | keyboard (IRQ1) | `irq1_stub`  | 0   |
| `0x80` | system call   | `syscall_stub` | 3   |

### 16.4 Source-file map

| File                 | Responsibility                                             |
|----------------------|------------------------------------------------------------|
| `boot.asm`           | 512-byte boot sector: real mode → protected mode           |
| `kernel/entry.s`     | `_start`, the int/IRQ stubs, `switch_context`              |
| `kernel/kernel.c`    | freestanding libc, console, GDT/IDT/TSS, `kmain`           |
| `kernel/mm.c`        | frame allocator + paging                                   |
| `kernel/sched.c`     | tasks, round-robin, `ctx_switch`, exit/yield/wait          |
| `kernel/proc.c`      | `exec`, `fork`                                             |
| `kernel/syscall.c`   | `int 0x80` dispatch + stdin line editor                    |
| `kernel/timer.c`     | PIC remap + PIT                                            |
| `kernel/kbd.c`       | PS/2 keyboard driver (scancodes, modifiers, extended keys) |
| `kernel/fs.c`        | in-RAM filesystem + fd table                               |
| `kernel/rd.c`        | embedded ramdisk of program images                         |
| `kernel/ata.c`       | ATA PIO disk driver                                        |
| `kernel/fat.c`       | FAT16 read + write                                         |
| `kernel/vfs.c`       | unifies ramfs + `/mnt` FAT                                 |
| `user/ulib.h`        | user-space syscall wrappers                                |
| `user/login.cpp`     | login/authentication task                                  |
| `user/worker.cpp`    | demo worker tasks                                          |
| `programs/crt0.s`    | user program startup (`_start` → `umain`)                  |
| `programs/shell.cpp` | the shell                                                  |
| `programs/vi.cpp`    | the editor                                                 |
| `tools/emulate.py`   | Unicorn CPU emulator + Python service layer                |
| `tools/mkramdisk.py` | packs programs into the ramdisk archive                    |
| `tools/mkfatfs.py`   | builds the FAT16 `fs.img`                                  |

---

## 17. Glossary

* **A20 line** — the 21st address line, disabled at boot for legacy reasons; must be
  enabled to address memory above 1 MB.
* **BPB (BIOS Parameter Block)** — the header in a FAT filesystem's first sector that
  describes its geometry.
* **CPL / DPL / RPL** — Current / Descriptor / Requested Privilege Level; the 0–3
  ring numbers that gate access.
* **CR0, CR3** — control registers; `CR0.PE` enables protected mode, `CR0.PG`
  enables paging, `CR3` holds the current page directory's physical address.
* **EOI (End Of Interrupt)** — the signal a handler sends the PIC so it will deliver
  the next IRQ.
* **Frame** — a 4 KB unit of *physical* memory. A **page** is a 4 KB unit of
  *virtual* memory. Page tables map pages to frames.
* **GDT / IDT / TSS** — Global Descriptor Table (segments), Interrupt Descriptor
  Table (handlers), Task State Segment (holds the ring-0 stack pointer).
* **IRQ** — a hardware Interrupt ReQuest line (e.g. IRQ0 = timer, IRQ1 = keyboard).
* **LBA** — Logical Block Addressing, a flat sector number (vs. old CHS).
* **PIC / PIT** — Programmable Interrupt Controller (routes IRQs) / Programmable
  Interval Timer (periodic tick).
* **PIO** — Programmed I/O; the CPU moves disk data itself via `in`/`out` (vs. DMA).
* **Ring** — an x86 privilege level; tinyos uses ring 0 (kernel) and ring 3 (user).
* **Scancode** — the raw key-position code a keyboard sends, distinct from ASCII.
* **`struct regs`** — the saved CPU state a stub builds on entry; editing it and
  `iret`-ing is how tinyos switches tasks.

---

## 18. Where to go next

Good next features to study or build, roughly in order of difficulty (see
`PROJECT_PLAN.md` §8 for the current list):

* **Signals + Ctrl-C** — detect Ctrl in the keyboard driver, add a `kill`/signal
  path, and terminate the foreground task. Ties keyboard → scheduler → process life.
* **Pipes + I/O redirection** (`ls | cat`, `cmd > file`) — needs per-process file
  descriptor tables, a `pipe()`/`dup2()`, and shell parsing of `|`/`>`/`<`. The most
  complete "it's really Unix" demonstration.
* **A real ELF loader** — load standard ELF binaries instead of flat images.
* **Copy-on-write `fork`** and demand paging — make `vm_clone` lazy via page faults.

To find your way around, start at `kmain` in `kernel/kernel.c` and follow the calls;
keep the [memory map](#2-the-memory-map) and the [end-to-end trace](#15-end-to-end-trace-what-happens-when-you-type-cat-mntreadmetxt)
next to you. Everything the kernel does is reachable from that one function.
