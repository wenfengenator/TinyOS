# tinyos — Design Document

> **Looking for a gentler introduction?** [`GUIDE.md`](GUIDE.md) is a detailed,
> concept-first walkthrough for entry-level OS developers. This document is the
> terser architecture reference.
>
> **This document describes the original 32-bit (i386) edition.** For the
> 64-bit UEFI edition — long mode, SMP, the framebuffer desktop, persistent
> FAT16 on Hyper-V/QEMU, the Linux-compatible ABI, musl, and the on-device
> self-hosting compiler — see [`ARCHITECTURE.md`](ARCHITECTURE.md).

## 1. Overview

tinyos is a tiny teaching operating system for the 32-bit x86 (i386) platform.
It boots from a 512-byte boot sector, switches the CPU into protected mode, and
runs a small **kernel in C (ring 0)** that hosts **user programs in C++ (ring 3)**.
The user programs reach the kernel only through `int 0x80` system calls.

It demonstrates the classic OS building blocks:

| Feature            | Where                                                     |
|--------------------|-----------------------------------------------------------|
| Bootloader         | `boot.asm` (16-bit → 32-bit protected mode)               |
| Kernel / ring 0    | `kernel/*.c`, `kernel/entry.s`                            |
| User layer / ring 3| in-kernel tasks `user/*.cpp`; programs `programs/*.cpp`    |
| System calls       | `int 0x80` ABI (`kernel/syscall.c`, `user/ulib.h`)        |
| File handling      | in-RAM filesystem (`kernel/fs.c`)                         |
| User login         | `user/login.cpp` authenticating against `/etc/passwd`     |
| Multitasking       | task table + scheduler (`kernel/sched.c`)                 |
| Preemption         | PIT/PIC timer (`kernel/timer.c`) + emulator time-slicing  |
| Paging / isolation | `kernel/mm.c` (per-task address spaces)                  |
| Keyboard input     | `kernel/kbd.c` (IRQ1) + stdin ring buffer                |
| Block FS + exec    | embedded ramdisk (`kernel/rd.c`) + `exec` (`kernel/proc.c`) |
| Processes          | `fork` / `wait` / `exec` (`kernel/proc.c`)               |
| Shell              | `programs/shell.cpp` (ls, ps, cat, run, fork, race)      |

Because QEMU is not available in the development sandbox, tinyos also ships a
**Unicorn-based emulator** (`tools/emulate.py`) that runs the exact same image.

## 2. Design goals & principles

* **Assembly only where required.** Real-mode boot, the protected-mode switch,
  the entry/interrupt stubs, and privileged instructions are assembly; every-
  thing else is C (kernel) or C++ (user).
* **Hard privilege separation.** User code runs in ring 3 and cannot touch the
  GDT/IDT/TSS, I/O ports, or `hlt`/`cli`. Its only channel is `int 0x80`.
* **Runnable without QEMU.** The emulator is a first-class run target so the OS
  can be exercised anywhere Python + Unicorn are available.

## 3. Repository layout

```
boot.asm            16-bit boot sector: load kernel, enable A20, enter PM
kernel/
  entry.s           _start, int 0x80 stub, IRQ0/IRQ1 stubs, switch_context
  kernel.c          console, GDT/IDT/TSS, boot, freestanding libc
  mm.c              physical frame allocator + paging + address spaces
  syscall.c         int 0x80 dispatch (hardware path)
  fs.c              in-RAM filesystem (open/read/write/close/stat/lseek)
  rd.c              embedded block-backed ramdisk (program images)
  proc.c            process management: exec / fork / wait
  sched.c           task table + round-robin scheduler
  timer.c           8259 PIC remap + 8253/4 PIT (preemption source)
  kbd.c             PS/2 keyboard (IRQ1) + stdin ring buffer
  kernel.h  mm.h    shared kernel definitions / ABI constants
  linker.ld         flat kernel image linked at 0x10000
user/               in-kernel ring-3 tasks (linked into the kernel image)
  ulib.h            C++ syscall wrappers (the user-space runtime)
  login.cpp         login task; on success exec()s the shell
  worker.cpp        demo worker task
programs/           standalone programs packed into the ramdisk (exec'd)
  crt0.s user.ld    startup + linker script (linked at USER_BASE)
  shell.cpp         interactive shell
  hello.cpp count.cpp   example programs
tools/
  emulate.py        Unicorn PC emulator (runs the OS without QEMU)
  mkramdisk.py      packs programs into the embedded ramdisk archive
  xcc, xcxx         portable C / C++ compiler wrappers
docs/               this document, project plan, test plan, test results
Makefile            build + run targets
```

## 4. Toolchain

| Language | Tool               | Notes                                            |
|----------|--------------------|--------------------------------------------------|
| ASM      | GNU `as` (--32)    | AT&T syntax                                       |
| C        | `gcc -m32`         | `-ffreestanding -fno-pic -nostdlib`, via `tools/xcc` |
| C++      | `g++ -m32`         | `+ -fno-exceptions -fno-rtti`, via `tools/xcxx`   |
| Link     | GNU `ld` + `objcopy` | flat binaries                                   |

`tools/xcc`/`xcxx` prefer a system `gcc`/`g++`; inside the Copilot sandbox they
fall back to the WSL host toolchain under `/var/lib/snapd/hostfs`.

## 5. Physical memory map

```
0x00000 – 0x004FF   real-mode IVT / BIOS data area
0x00600             console cursor (shared dword: current VGA cell index)
0x07C00             boot sector (512 bytes)
0x10000             kernel image (code + data + user tasks + ramdisk), _start
0x11xxx …           kernel .bss (GDT/IDT/TSS, page tables, g_files, g_tasks)
0x64000 – 0x80000   ring-3 stacks of in-kernel tasks (16 KB apart)
0x88000 – 0x90000   per-task ring-0 (kernel) stacks (4 KB apart)
0xB8000             VGA text buffer (80×25)
0x400000 – 0x1000000  frame pool (12 MB) for page tables + user pages
```

Once paging is on, every address space **identity-maps the low 16 MB** and adds
per-task user pages at high virtual addresses:

```
0x40000000          USER_BASE       — exec'd program image is mapped here
0xB0000000          USER_STACK_TOP  — top of the program's ring-3 stack
```

## 6. Boot flow

```
BIOS ──► boot.asm (0x7C00, 16-bit real mode)
          • set segments/stack, save boot drive
          • int 0x13: load 64 sectors (kernel) to 0x10000
          • enable A20 (port 0x92)
          • lgdt (flat GDT), set CR0.PE
          • ljmp 0x08:pm_entry  ── far jump into 32-bit code
       ──► pm_entry (32-bit): load data segs, esp; jmp 0x10000
       ──► _start (entry.s): set segments/stack, call kmain()
       ──► kmain(): console, GDT/IDT/TSS, PIC/PIT, filesystem, tasks
                     └─ iret ─► ring 3 (login task)
```

## 7. Privilege model (GDT / IDT / TSS)

The kernel installs a 6-entry GDT:

| Sel   | Ring | Type       |
|-------|------|------------|
| 0x08  | 0    | code       |
| 0x10  | 0    | data       |
| 0x18  | 3    | code       |
| 0x20  | 3    | data       |
| 0x28  | -    | TSS        |

The IDT holds two live gates:

* `0x20` — IRQ0 (PIT), DPL 0, → `irq0_stub`
* `0x80` — system call, **DPL 3** (callable from ring 3), → `syscall_stub`

The **TSS** supplies `ss0`/`esp0` so a ring-3 → ring-0 transition (a syscall or
an IRQ) automatically switches to the kernel stack.

The kernel drops to ring 3 with an `iret` onto a synthetic frame
`(SS=0x23, ESP=task stack, EFLAGS=0x202, CS=0x1B, EIP=task entry)`.

## 8. System-call ABI (`int 0x80`)

Number in `eax`; arguments in `ebx`, `ecx`, `edx`; return value in `eax`.

| eax | name       | args                      | returns          |
|-----|------------|---------------------------|------------------|
| 1   | exit       | code                      | –                |
| 3   | read       | fd, buf, len              | bytes read       |
| 4   | write      | fd, buf, len              | bytes written    |
| 5   | open       | path, flags (1=O_APPEND)  | fd or -1         |
| 6   | close      | fd                        | 0/-1             |
| 7   | yield      | –                         | 0                |
| 8   | getpid     | –                         | pid              |
| 9   | exec       | path                      | new pid or -1    |
| 10  | spawn      | task_index                | 0                |
| 11  | stat       | path, *size               | 0/-1             |
| 12  | lseek      | fd, off, whence           | new offset       |
| 13  | fork       | –                         | child pid / 0    |
| 14  | wait       | pid                       | child exit code  |
| 15  | readdir    | index, name buf           | 0/-1             |
| 16  | taskinfo   | index, buf                | 0/-1             |

`fd 0` = stdin (keyboard), `fd 1/2` = console; `fd ≥ 3` = open files.
The C++ wrappers live in `user/ulib.h`.

## 9. Filesystem

A fixed table `g_files[8]` of `{name, data[256], size, used}` records with a
global descriptor table `g_fds[16]`. Seeded at boot with `/etc/passwd`,
`/README`, and an empty writable `/tmp/log`. `open` supports append mode so
several writers accumulate records (used by the two workers).

## 10. Tasks & scheduling

`g_tasks[4]` holds each task's saved context (`eax…eflags`), `state`
(`UNUSED/READY/RUNNING/BLOCKED/EXITED`), `pid`, stack, and name. The demo
creates:

* task 0 — `login`  (READY)
* task 1 — `workerA`(BLOCKED until login succeeds)
* task 2 — `workerB`(BLOCKED)

The scheduler is round-robin over READY tasks. `login` unblocks the workers via
`spawn` after authentication, then exits.

## 11. Preemption & context switching

**Hardware path (QEMU / real machine) — implemented and verified.** `timer.c`
remaps the 8259 PIC to vectors 0x20–0x2F and programs the PIT (channel 0) to
100 Hz. Tasks run with `IF=1`, so IRQ0 fires ~every 10 ms into `irq0_stub`,
which builds a `struct regs` frame and calls `timer_tick()`. If the interrupted
task was in ring 3, `timer_tick` calls `ctx_switch()` (`kernel/sched.c`), which
saves the interrupted registers into the current TCB and copies the next task's
context into the frame — so the stub's `iret` resumes the next task. The same
`ctx_switch` is used by `sys_exit` / `sys_yield` / `sys_wait`. On QEMU two
`count` tasks visibly interleave.

**Emulator path.** Unicorn cannot deliver IRQs, so `tools/emulate.py` runs each
task for a fixed instruction **quantum** and performs the equivalent save/restore
itself. Same observable behaviour, different driver.

### Interactive console

The console writes to the VGA text buffer **and** mirrors to the serial port
(COM1), moving the VGA hardware cursor via the CRTC registers (0x3D4/0x3D5).
`sys_read(stdin)` blocks (`sti; hlt`) draining the PS/2 keyboard buffer (IRQ1)
or serial RX, echoing characters as typed — `SYS_ECHO` selects plain / masked
(`*`, for passwords) / silent echo. `make run` uses graphical QEMU; `make
run-serial` runs headless with the console on stdio.

## 12. The emulator (`tools/emulate.py`)

Two stages mirror a real PC:

* **Stage A — 16-bit real mode.** Runs the actual boot sector; services
  `int 0x10` (teletype) and `int 0x13` (disk read, CHS→LBA against the image);
  stops the moment `CR0.PE` is set (a fixed-mode Unicorn instance cannot follow
  the far jump into protected mode).
* **Stage B — 32-bit protected mode.** Inherits Stage A's RAM and GDTR, then
  runs the kernel for real. It services `int 0x80` against the kernel's *own*
  memory (`g_files`/`g_fds`, scripted stdin) and drives the preemptive
  scheduler over `g_tasks`. Finally it renders the 0xB8000 text buffer.

### Why a shim is needed — Unicorn limitations (measured)

| Capability                                   | Unicorn behaviour        |
|----------------------------------------------|--------------------------|
| Protected mode, GDT, `iret` to ring 3        | works                    |
| `lgdt`/`lidt`/`ltr`, port `in`/`out`         | works (no fault)         |
| Software `int` vectored through the IDT      | **not supported** (`UC_ERR_EXCEPTION`) |
| Redirect EIP inside an interrupt hook        | **not supported**        |
| Hardware IRQ delivery (PIT/PIC)              | **not supported**        |

Consequently the emulator services `int 0x80` in a `UC_HOOK_INTR` hook
(dispatch-and-continue) and drives preemption itself. The kernel's C handlers
(`syscall_dispatch`, `irq0_stub`) are the equivalent **hardware** path.

## 13. Hardware vs emulator fidelity

| Component        | On QEMU / hardware              | Under `tools/emulate.py`          |
|------------------|---------------------------------|-----------------------------------|
| Boot + PM switch | real                            | real (stage A → stage B handoff)  |
| Kernel init      | real                            | real (runs the C code)            |
| Console / VGA    | real (0xB8000)                  | real (shared 0xB8000 + cursor)    |
| `int 0x80`       | IDT → `syscall_stub` (C)        | serviced in Python (same effects) |
| Filesystem state | `g_files`/`g_fds`               | same memory, same layout          |
| Preemption       | PIT IRQ0 context switch         | time-slice context switch (Python)|
| Mounted disk     | ATA PIO ⇄ FAT16 (`-hdb fs.img`) | `FatImage` reads/writes `fs.img` in py |

## 14. Paging & per-task address spaces (`mm.c`)

After the descriptor tables are up, `mm_init()` builds a physical **frame
allocator** (bitmap over 4–16 MB) and two sets of page tables that identity-map
the low 16 MB: a *user-accessible* set (`kpd_u`, for in-kernel ring-3 tasks that
live in low memory) and a *supervisor-only* set (`kpd_s`).  `paging_enable()`
loads CR3 and sets CR0.PG.

`exec`'d programs get an **isolated address space** from `vm_create_user()`:
the kernel identity map is supervisor-only, so their ring-3 code can reach only
their own USER pages (program image at `USER_BASE`, stack at `USER_STACK_TOP`).
Two programs mapping the same virtual address therefore see different physical
frames — real inter-task isolation.  `vm_clone()` copies an address space for
`fork`.

Unicorn honours CR3 page-table walks natively, so paging runs for real under the
emulator; `tools/emulate.py` walks the current CR3 to translate user pointers
when servicing syscalls.

## 15. Keyboard & stdin (`kbd.c`)

The PS/2 keyboard raises **IRQ1**; `irq1_stub` calls `on_keyboard()`, which reads
the scancode from port 0x60, translates it, and pushes an ASCII byte into the
`g_kbd` ring buffer.  `sys_read(STDIN)` drains that buffer.  Under the emulator
(no IRQ1) `tools/emulate.py` pushes the user's scripted keystrokes into the same
ring buffer, so the stdin path is identical to hardware.

`on_keyboard()` tracks the **Shift** (make `0x2A`/`0x36`, break `0xAA`/`0xB6`) and
**Caps Lock** (`0x3A`) modifiers and selects between an unshifted and a shifted
scan-code→ASCII table, so shifted characters such as `:` (Shift+`;`), `?`, `_` and
capital letters are produced correctly — this is what makes `vi`'s `:w`/`:q`
commands and uppercase text entry work on the real PS/2 keyboard.  (Serial input
and the emulator inject raw ASCII, so they bypass the scan-code layer entirely.)

The navigation keys (arrows, Delete, Home, End, Page Up/Down) arrive as **extended
scan codes** prefixed with `0xE0`.  `on_keyboard()` latches the prefix and maps the
follow-up code to a small set of non-ASCII key bytes (`KEY_UP`…`KEY_PGDN`, `0x80`+,
shared in `kernel.h`/`ulib.h`).  `vi` uses them to move the cursor and forward-delete
(`KEY_DEL`), and `sys_read`'s shell line editor uses them for in-line editing.

## 16. Block-backed ramdisk & exec (`rd.c`, `proc.c`)

Standalone programs in `programs/` are compiled as flat binaries linked at
`USER_BASE`, packed by `tools/mkramdisk.py` into a small `'TRD'` archive, and
embedded into the kernel image (`.incbin`).  `rd.c` exposes it as a read-only,
**block-addressable** device (`block_read`) with a name→image lookup.

`do_exec(path)` looks the program up, creates an isolated address space, maps and
copies the image at `USER_BASE`, maps a user stack, and creates a `READY` task.

## 17. Processes: fork / wait (`proc.c`)

* `do_fork()` allocates a task, copies the caller's context and address space
  (`vm_clone`), and returns the child pid to the parent / 0 to the child.
* `do_wait(pid)` blocks the caller until the child exits, then returns its exit
  code.  Under the emulator, `exit` wakes any task blocked in `wait` on that pid
  and delivers the code as the `wait` return value.

## 18. The shell (`programs/shell.cpp`)

A ring-3 program (itself exec'd by `login`) that reads command lines from stdin
and provides: `help`, `ls` (`readdir`), `ps` (`taskinfo`), `cat <file>`
(`stat`+`open`+`read`), `run <prog>` (`exec`+`wait`), `fork`, `race` (two
concurrent `count` tasks that interleave preemptively), and `exit`.  Command
lines are edited by the kernel's `sys_read`, which implements an in-line editor
with a movable cursor: **←/→** move, **Home/End** jump, **Backspace** deletes to the
left and **Delete** at the cursor, redrawing the line via the VGA cursor.

## 19. Virtual filesystem, ATA disk & FAT16 mount (`vfs.c`, `ata.c`, `fat.c`)

tinyos presents a single unified tree through a small **VFS** (`kernel/vfs.c`):

* `/` and everything except `/mnt` is the in-RAM filesystem (`fs.c`, `g_files`).
* `/mnt` is a **real FAT16 volume** read from a mounted disk image.

The disk is a second drive (`-hdb fs.img`, primary slave). `kernel/ata.c` is a
minimal **ATA PIO** driver (`ata_read`/`ata_write` of 512-byte LBA sectors, polling
the status port 0x1F7). `kernel/fat.c` mounts that drive at boot: it parses the
BPB, then implements directory walks (`fat_lookup`), directory listing
(`fat_readdir`) and cluster-chain reads (`fat_read`) by following the FAT.

Writes go the other way: `fat_write(path,data,len)` walks to the parent directory,
frees any old cluster chain, allocates a fresh chain (`fat_alloc` scans for a free
FAT entry), writes the data sectors with `ata_write`, links the chain and updates
**every** FAT copy (`fat_set`), then rewrites the 8.3 directory entry (first
cluster + size).  Because `ata_write` goes straight to the backing image, edits
**persist across reboots** — `vi /mnt/log.txt` + `:wq` survives a power-cycle.

The VFS routes every path syscall — `stat`, `readdir`, `readfile`, `writefile`,
`open` — to either ramfs or FAT by prefix, so the shell and `vi` are oblivious to
where a file physically lives. On the emulator the ATA port returns *error* (so the
in-kernel `fat_mount` fails fast), and `tools/emulate.py` instead reads/writes the
same `fs.img` in Python (`FatImage`, which mirrors `fat_write` and flushes back to
the file) to serve `/mnt` — identical results, and images written by one are read
by the other.

The disk image is built by `tools/mkfatfs.py fs.img disk/` (the host has no
`mkfs.fat`), which lays down the boot sector/BPB, two FATs and the root directory
from a source tree. `make fs.img` runs it automatically.

## 20. Directories, CWD & the `vi` editor

The shell (`programs/shell.cpp`) keeps a current working directory and resolves
relative paths, adding `cd`, `pwd` and directory-aware `ls`/`cat`. Paths crossing
into `/mnt` transparently hit the FAT volume.

`vi` (`programs/vi.cpp`) is a small modal, full-screen editor exec'd like any other
program (`vi <file>` — the path is passed via the exec **argument page** at
`ARG_ADDR`). It uses the console syscalls `clear`/`gotoxy`/`getchar` to redraw and
`readfile`/`writefile` to load and save. Commands: `h j k l` **or the arrow keys**
move, `i`/`a`/`o` insert, `x` or **Delete** removes a char, `Home`/`End` jump to
line start/end, `:w` write, `:q` quit, `:wq` write-and-quit. Because a flat
user binary omits `.bss`, the editor's large line buffers are forced into the image
via `objcopy --set-section-flags .bss=alloc,load,contents` (see the `Makefile`).

## 21. Extending tinyos

* Add a syscall: pick a number in `kernel/kernel.h`, implement it in
  `kernel/syscall.c` (hardware) and `tools/emulate.py` (emulator), wrap it in
  `user/ulib.h`.
* Add a user program: drop a `user/*.cpp` with an `extern "C"` entry, create a
  task for it in `kmain`.
* Grow the kernel past 32 KB: raise `KERNEL_SECT` in `boot.asm`.
