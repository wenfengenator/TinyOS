# tinyos — Project Plan

## 1. Objective

Grow the original single-file "hello from the boot sector" into a small but
genuine operating system that demonstrates: a kernel, a user layer, user
programs, system calls (including file handling), user login, multitasking, and
preemption — with the kernel in C, user apps in C++, and assembly used only
where unavoidable.

## 2. Constraints

* Development sandbox has **no QEMU**, **no `nasm`**, and a snap-confined `PATH`.
* Toolchain available: GNU `as`/`ld`/`objcopy`, Python 3, and (via the WSL host
  mount) `gcc-13`/`g++-13`; `unicorn` and `ziglang` installable via `pip`.
* Therefore a **Unicorn-based emulator** is the primary run/verify target, and a
  portable compiler wrapper (`tools/xcc`/`xcxx`) hides the toolchain location.

## 3. Work breakdown & status

| # | Phase                       | Deliverable                                             | Status |
|---|-----------------------------|---------------------------------------------------------|--------|
| 0 | De-risk emulator            | Confirm Unicorn can do PM, ring 3, `int 0x80`, `lgdt/ltr`, port I/O | ✅ done |
| 1 | Toolchain                   | `tools/xcc`, `tools/xcxx`, Makefile, boot→C→C++ build   | ✅ done |
| 2 | Foundation                  | C kernel (console, GDT/IDT/TSS) + C++ ring-3 app        | ✅ done |
| 3 | System calls                | `int 0x80` dispatch: write/read/exit/open/close/getpid/yield/spawn | ✅ done |
| 4 | Filesystem                  | in-RAM files + fd table; `/etc/passwd`, `/README`, `/tmp/log` | ✅ done |
| 5 | Multitasking                | task table + round-robin scheduler; ≥3 tasks            | ✅ done |
| 6 | Preemption                  | PIC/PIT timer (hw) + emulator time-slice switching      | ✅ done |
| 7 | User login                  | C++ login: prompt, `read` creds, auth vs `/etc/passwd`  | ✅ done |
| 8 | Emulator                    | two-stage boot + syscall/fs/scheduler shim + VGA render | ✅ done |
| 9 | Documentation               | design, project plan, test plan, test results           | ✅ done |
|10 | Integration demo            | end-to-end login→spawn→preempt→file→halt                | ✅ done |

## 4. Milestones (in order achieved)

1. **M0** — Unicorn proven capable of ring-0/ring-3 + syscalls (4 probe tests).
2. **M1** — `boot.asm` enters protected mode; assembled + disassembled OK.
3. **M2** — C kernel + C++ user "hello via int 0x80" runs on the emulator.
4. **M3** — Filesystem + expanded syscalls.
5. **M4** — Preemptive multitasking of two workers (interleaved output).
6. **M5** — Login authenticates against `/etc/passwd` and spawns the workers.
7. **M6** — Full integration demo + documentation.

## 5. Key design decisions

| Decision | Rationale |
|----------|-----------|
| GNU `as` instead of `nasm` | `nasm` unavailable; `as` present. |
| `gcc/g++ -m32 -ffreestanding` | User asked for C kernel / C++ apps; no libc needed. |
| Unicorn emulator as run target | No QEMU; Unicorn runs the real image. |
| `int 0x80` serviced in the emulator | Unicorn can't vector `int` through the IDT. |
| Emulator drives preemption | Unicorn delivers no IRQs; time-slice switching instead. |
| Shared in-memory structures (`g_*`) | One source of truth for kernel C code and the emulator. |
| Fixed cursor at 0x600 | Lets kernel C and emulator share the console cursor. |

## 6. Risks & mitigations

| Risk | Mitigation / outcome |
|------|----------------------|
| Unicorn can't model rings/interrupts | Measured up front (Phase 0); shim designed around it. |
| Boot sector untestable without QEMU | Real-mode part executed in emulator stage A; verified via disassembly. |
| Hardware preemption path untestable here | Implemented to spec, isolated in `timer.c`; emulator demonstrates the switch. |
| Kernel outgrowing the 64-sector load | Size check in Makefile; raise `KERNEL_SECT` if needed. |

## 7. Advanced features (mini-Unix layer) — all implemented

| # | Feature                              | Deliverable                                              | Status |
|---|--------------------------------------|----------------------------------------------------------|--------|
| A | Per-task kernel stacks + ctx switch  | `kstack` per task, TSS.esp0, `switch_context` (entry.s); emulator drives the switch | ✅ done |
| B | Paging + per-task isolation          | `kernel/mm.c`: frame allocator, page tables, per-task address spaces; Unicorn walks CR3 | ✅ done |
| C | Block-backed FS + exec of images     | embedded ramdisk (`rd.c`, `mkramdisk.py`) + `do_exec` into an isolated space (`proc.c`) | ✅ done |
| D | Keyboard IRQ1 + stdin                | `kernel/kbd.c` ring buffer; emulator injects keystrokes into the same buffer | ✅ done |
| E | Shell + fork/wait/stat/lseek         | `programs/shell.cpp`; `fork`/`wait`/`stat`/`lseek`/`readdir`/`taskinfo` syscalls | ✅ done |
| F | Real hardware bring-up (QEMU)        | blocking `sys_read`, real `ctx_switch`/IRQ0 preemption, VGA cursor + echo, serial console | ✅ done |
| G | ATA disk + FAT16 mount + VFS         | `kernel/ata.c`, `kernel/fat.c` (read **and** write-back), `kernel/vfs.c`; `/mnt` served from `fs.img`; `tools/mkfatfs.py` | ✅ done |
| H | Directories + `vi` editor            | `cd`/`pwd`/dir-aware `ls`/`cat` (CWD); modal editor `programs/vi.cpp` with exec-arg + console syscalls | ✅ done |

De-risked first: verified Unicorn honours **CR3 paging** (page-table walks) and
port I/O, and re-confirmed it cannot vector `int`/IRQ through the IDT — so the
emulator page-walks user pointers and drives scheduling / exec / fork itself,
while the kernel keeps the real hardware implementations.  Features F–H were then
verified on **real `qemu-system-i386`** (VGA + PS/2 keyboard and headless serial),
not just the emulator.

## 8. Future work (not in scope)

* Copy-on-write `fork`, demand paging, and directory compaction / free-space
  reporting for the FAT volume (single-file write-back is implemented).
* `exec` that replaces the caller's image (currently spawns a new task).
* More of a libc, a pipe/IPC layer, and multiple simultaneous shells.
