# tinyos — Architecture & Design (64-bit UEFI edition)

> This document is the in-depth architecture and design reference for the
> **64-bit (`ARCH=x64`) UEFI edition** of tinyos: a long-mode kernel that boots
> from UEFI, brings up all CPU cores, drives a framebuffer desktop, persists a
> FAT16 filesystem on Hyper-V and QEMU, and hosts a **Linux-compatible ABI** rich
> enough to run **musl** libc programs and an **on-device, self-hosting C
> compiler** (TinyCC).
>
> For the original 32-bit BIOS teaching kernel see [`DESIGN.md`](DESIGN.md) and
> the beginner walkthrough [`GUIDE.md`](GUIDE.md). For the project history and
> milestones see [`PROJECT_PLAN.md`](PROJECT_PLAN.md).

---

## Table of contents

1. [Two editions, one repository](#1-two-editions-one-repository)
2. [The big picture](#2-the-big-picture)
3. [Boot flow: UEFI to long-mode kernel](#3-boot-flow-uefi-to-long-mode-kernel)
4. [Memory model](#4-memory-model)
5. [Privilege model: GDT, TSS, IDT, ring 0/3](#5-privilege-model-gdt-tss-idt-ring-03)
6. [Symmetric multiprocessing (SMP)](#6-symmetric-multiprocessing-smp)
7. [Interrupts and the preemptive scheduler](#7-interrupts-and-the-preemptive-scheduler)
8. [Display: framebuffer, double buffer, window manager](#8-display-framebuffer-double-buffer-window-manager)
9. [Input: PS/2 and Hyper-V synthetic keyboard](#9-input-ps2-and-hyper-v-synthetic-keyboard)
10. [Storage stack: block layer, virtio-blk, StorVSC](#10-storage-stack-block-layer-virtio-blk-storvsc)
11. [Filesystem: FAT16 and the VFS](#11-filesystem-fat16-and-the-vfs)
12. [The Linux-compatible ABI](#12-the-linux-compatible-abi)
13. [Userland: musl and the on-device toolchain](#13-userland-musl-and-the-on-device-toolchain)
14. [Design principles and rationale](#14-design-principles-and-rationale)
15. [Multi-pane terminal, power, and real hardware](#15-multi-pane-terminal-power-and-real-hardware)
16. [Building and running](#16-building-and-running)
17. [Testing and verification](#17-testing-and-verification)
18. [Known limitations and future work](#18-known-limitations-and-future-work)

---

## 1. Two editions, one repository

tinyos grew in cycles, and the repository keeps **both** the original 32-bit
kernel and the 64-bit one so the progression is visible:

| Edition | Boot | CPU mode | Display | Targets | Source |
|---------|------|----------|---------|---------|--------|
| **i386** (original) | 512-byte MBR boot sector (`boot.asm`) | 32-bit protected mode | VGA text (0xB8000) | Unicorn emulator, `qemu-system-i386` | `kernel/*.c` (excluding `*64.c`), `user/`, `programs/*.cpp` |
| **x64** (this doc) | UEFI PE32+ application (`boot/efi_main.c`) | 64-bit long mode | GOP linear framebuffer | `qemu-system-x86_64` + OVMF, **Hyper-V Gen 2** | `kernel/*64.c`, `boot/efi_main.c`, `programs/*.cpp` |

The Makefile selects between them with `ARCH=i386` (default) and `ARCH=x64`.
The 32-bit kernel sources (`kernel/*.c`) and 64-bit kernel sources
(`kernel/*64.c`) are disjoint; only a few user programs (`programs/*.cpp`) and
tools are shared.

The rest of this document is about the **x64 edition**.

---

## 2. The big picture

```
                         ┌───────────────────────────────────────────────┐
                         │                UEFI firmware                   │
                         │   (OVMF on QEMU / Hyper-V Gen 2 firmware)      │
                         └───────────────┬───────────────────────────────┘
                                         │ loads /EFI/BOOT/BOOTX64.EFI
                                         ▼
   ┌──────────────────────── BOOTX64.EFI (one PE32+ image) ──────────────────────┐
   │  boot/efi_main.c  ── GOP framebuffer, ACPI RSDP, memory map, ExitBootServices │
   │        │  hands a boot_info_t to the kernel via kernel/entry64.s              │
   │        ▼                                                                      │
   │  kmain64()  ── serial+fb console, time, heap, SMP, keyboard, ring 0/3 setup   │
   │        │                                                                      │
   │        ├── smp64      bring every AP to long mode (INIT-SIPI-SIPI)            │
   │        ├── arch64     GDT/TSS/IDT, SYSCALL/SYSRET, ring-3 loader, Linux ABI   │
   │        ├── fat64      FAT16 over a block layer (RAM / virtio-blk / StorVSC)   │
   │        ├── disp64     one shared double buffer + scanline-diff present        │
   │        ├── wm64/term64 keyboard-driven desktop: terminal + system monitor     │
   │        └── login64    sign-in panel, then the desktop                         │
   └──────────────────────────────────────────────────────────────────────────────┘
```

Everything is one freestanding binary — there is no dynamic loader, no modules,
and no host OS underneath. The kernel is **C**; ring-3 user programs are **C/C++**
(the desktop apps) or **static Linux ELF64** binaries (musl programs and TCC).

---

## 3. Boot flow: UEFI to long-mode kernel

The firmware is already in 64-bit long mode with paging and an identity map when
it launches our PE32+ application, so the boot job is mostly *harvesting state*
before we lose it, then never looking back.

**`boot/efi_main.c`** (runs under the UEFI ABI, before `ExitBootServices`):

1. **Graphics Output Protocol (GOP).** Query the available modes and `SetMode`
   to the largest 32-bpp mode up to 1920×1080. Record the framebuffer base,
   width, height, pixel pitch, and pixel format.
2. **Assets.** Load `TINYWAD.DAT` from the EFI System Partition (demo asset).
3. **ACPI.** Find the ACPI RSDP from the UEFI configuration table (used later to
   discover CPUs via the MADT).
4. **Memory map.** Fetch the UEFI memory map (kept for the heap allocator).
5. **`ExitBootServices`.** Take ownership of the machine. After this, firmware
   services are gone; only hardware the firmware left in place remains (the
   16550 UART and the linear framebuffer).
6. Fill a `boot_info_t` (framebuffer geometry, memory map, RSDP, asset) and jump
   to the kernel via the tiny assembly trampoline in `kernel/entry64.s`.

**`kmain64()`** (`kernel/kmain64.c`) then brings the system up in order:

```
serial_init → fb_init → time_init(rsdp) → heap_init → kbd64_init
            → smp_init → hvkbd_init → arch_init
            → arch_user_init → disp_init → login_run → wm_run
```

Text is mirrored to **both** COM1 and the framebuffer, so a headless boot can be
verified over serial while a VM console shows the same output. This dual console
is why the whole bring-up is greppable in the boot logs (`[tinyos64] …`,
`[smp] …`, `[fs] …`, `[linux-abi] …`).

**Build note.** The EFI app is produced with plain binutils — no mingw,
gnu-efi, or clang. `gcc -m64 -fPIC` → `ld -mi386pep --subsystem=10` →
a PE32+ that OVMF and Hyper-V load cleanly. `-fPIC` avoids fragile base
relocations; a low image base keeps 32-bit address references resolvable.

---

## 4. Memory model

- **Identity map.** The kernel runs on the firmware's identity map: virtual
  address == physical address for all kernel memory. This is what lets drivers
  hand a buffer's virtual address straight to a DMA engine as a physical
  address (see the storage stack).
- **Heap.** `mm64.c` provides `heap_init`, `kmalloc`, and `page_alloc`
  (4 KiB-aligned) carved out of the largest conventional-memory region from the
  UEFI map. `kmalloc` is a bump/free-list allocator; `page_alloc`/`page_free`
  manage a 4 KiB frame free-list, so **dead processes return their frames**
  (see [§12](#12-the-linux-compatible-abi)). `page_free` rejects out-of-arena,
  misaligned, and double/overlapping frees as defense-in-depth.
- **User address spaces.** Linux ELF programs get their **own** 4-level page
  tables (a private PML4) cloned from the kernel's, with only the user regions
  replaced. Because the kernel runs on the *firmware's* identity map — and real
  UEFI firmware sets `US` and even an available software bit on those entries —
  the teardown/fork walks cannot use the hardware `US` bit to tell "a page we
  allocated" from "a shared firmware page". Instead every table entry **we**
  create carries a software-owned bit (`PTE_OWN`, bit 9) that is stripped from
  all copied entries, and the free/clone walks key off it. See
  [§12](#12-the-linux-compatible-abi).

---

## 5. Privilege model: GDT, TSS, IDT, ring 0/3

`arch64.c` + `trap64.s` install the CPU's protection machinery from scratch
(the firmware's is discarded):

- **GDT** with null, ring-0 code/data, ring-3 code/data, and a **TSS**
  descriptor. The TSS carries `RSP0` — the stack the CPU switches to when a
  ring-3 → ring-0 transition happens.
- **IDT** with all 256 gates installed. CPU exceptions 0–31 get vector-specific
  stubs that dump registers and halt; hardware vectors 32–255 share a stub that
  simply EOIs the Local APIC and returns, so a stray host interrupt is
  non-fatal.
- **`SYSCALL`/`SYSRET`.** The fast system-call path is wired through the `STAR`,
  `LSTAR`, and `SFMASK` MSRs. `trap64.s`'s `syscall_entry` saves the caller
  state on a per-nesting-level kernel stack and forwards all six System V
  argument registers (`rdi, rsi, rdx, r10, r8, r9`) to `syscall_c`.

**Two ABIs share one entry.** A per-process flag (`USER_ABI_TINYOS` /
`USER_ABI_LINUX`) selects which dispatch table `syscall_c` uses:

- **TinyOS ABI** — the small custom syscall set the desktop shell, `vi`, and
  games use (`readfile`, `writefile`, `readdir`, `mkdir`, `getchar`, `gotoxy`,
  `winsize`, …).
- **Linux ABI** — the x86-64 syscall subset that lets stock musl binaries and
  TCC run unmodified. See [§12](#12-the-linux-compatible-abi).

Two critical UEFI-era gotchas are baked into the design: our GDT invalidates the
firmware's IDT (its gates reference the firmware's code segment), so we **must**
install our own IDT or the first fault triple-resets silently; and the firmware
write-protects the page tables, so live edits clear `CR0.WP` around the change.

---

## 6. Symmetric multiprocessing (SMP)

`smp64.c` brings up every application processor (AP):

1. Read the ACPI **MADT** for the CPU list and the Local APIC base.
2. Copy a hand-written real-mode → protected → long-mode **trampoline**
   (`kernel/ap_trampoline.s`) to low physical memory.
3. Send **INIT-SIPI-SIPI** to each AP; each one climbs to long mode, adopts a
   per-CPU stack, and parks.

On a 10-vCPU Hyper-V VM the boot log shows all ten cores online. APs currently
park idle after bring-up; the preemptive scheduler ([§7](#7-interrupts-and-the-preemptive-scheduler))
runs kernel threads on the BSP. SMP-aware scheduling and TLB shootdown are future
work — the bring-up itself is the demonstrated capability.

---

## 7. Interrupts and the preemptive scheduler

`sched64.c` + `trap64.s` implement a round-robin **preemptive scheduler for
kernel threads**, driven by the **Local APIC timer**:

- `sched_init` masks the legacy PIC, software-enables the LAPIC, installs the
  timer (vector 0x20) and spurious (0xFF) IDT gates, and registers the caller as
  task 0.
- `sched_start` calibrates the LAPIC timer against the millisecond clock
  (~100 Hz) and enables interrupts; `sched_spawn(fn, arg)` creates a thread with
  its own stack and a fake interrupt frame.
- `timer_isr` saves all registers, calls `sched_tick` to pick the next thread,
  and `iretq`s into it.

**Long-mode gotcha, learned the hard way:** in long mode the CPU pushes the full
5-word interrupt frame (`SS, RSP, RFLAGS, CS, RIP`) on *every* interrupt,
including same-privilege ones — unlike 32-bit. New-thread fake frames must
include `SS` and `RSP` or `iretq` loads garbage and triple-faults.

The system runs **interrupts-off (polled)** everywhere except the opt-in
scheduler demo. A subtle consequence surfaced on Hyper-V: an unmasked SynIC
message interrupt could latch pending and fire the instant the demo did `sti`.
The robust fix was to make the shared hardware-vector stub EOI-and-return rather
than halt, so stray host interrupts can never take the system down.

---

## 8. Display: framebuffer, double buffer, window manager

The display stack is layered:

```
wm64 / login64 / games ── draw into ──▶ disp64 back buffer ──▶ fb64 ──▶ GOP framebuffer
```

- **`fb64.c`** owns the raw GOP framebuffer: pixel packing for the firmware's
  format (RGBX/BGRX), rectangle fills, an integer-scaled 8×8 font
  (`font8x8.h`), and the blit primitives `fb_present` (full frame) and
  `fb_present_diff` (only changed scanlines).
- **`disp64.c`** is the **display driver**: a single kernel-owned double buffer
  plus a *shadow* of what is currently on screen. `disp_present()` does a
  **scanline diff** — it copies only the rows that changed since the last
  present. This matters because the framebuffer is slow MMIO (and on Hyper-V a
  synthetic-video channel): an idle desktop writes essentially nothing. GOP
  offers a single linear framebuffer with no page-flip or vsync, so this
  software double buffer is the practical maximum; true tear-free flipping would
  need a real GPU driver (virtio-gpu / Hyper-V synthvid).
- **`wm64.c`** is a keyboard-driven compositor: a desktop background, a task
  bar, and windows (a **terminal** and a live **system monitor** with CPU %,
  battery, and lid state), with the focused window drawn last. Keys: **F1**
  cycle focus, **F2** move mode + arrows, **F3** sleep. It renders **on change**
  (and at the ~2 FPS cursor-blink/clock cadence when idle) rather than a fixed
  30 FPS, so an idle desktop costs almost nothing. When a full-screen graphics
  app is presenting, `wm_pump` **stands down** so the compositor does not
  overdraw it — see [§15](#15-multi-pane-terminal-power-and-real-hardware).
- **`term64.c`** is the terminal widget and command console. It hosts the
  ring-3 shell and can split into a **multi-pane** grid of concurrent shells
  (F5/F4); program stdout is routed to the pane owned by the writing process.
- Full-screen apps — **`login64.c`** (sign-in panel), **`pong64.c`**,
  **`demo64.c`**, the **`sched`** demo, and the **TinyGL** graphics apps — all
  render into the same shared `disp64` buffer and present through the diff path.

UI metrics adapt to resolution: an 8-px font on a small (1024×768 Hyper-V)
screen, 16-px on large screens, so the desktop is neither cramped nor blurry.

---

## 9. Input: PS/2 and Hyper-V synthetic keyboard

Two keyboard back-ends feed the same key codes:

- **`kbd64.c`** — the i8042 **PS/2** controller (QEMU), reading scancode set 1
  from port 0x60 and translating via unshifted/shifted tables, with arrows
  mapped to `KEY_*` codes.
- **`vmbus64.c`** `hvkbd_getkey` — the **Hyper-V synthetic keyboard** over
  VMBus, using the identical scancode → ASCII tables so behavior matches.

Both are **polled** (the kernel runs interrupts-off outside the scheduler
demo). Higher layers just call `kbd64_getkey()` / `hvkbd_getkey()`. TAB, Enter,
Backspace, Shift, and Caps Lock are handled in the tables; consumers decide what
control keys mean (for example, `vi` expands TAB to spaces).

---

## 10. Storage stack: block layer, virtio-blk, StorVSC

Persistence has to reach **Hyper-V** (the daily-driver target) and **QEMU** (the
dev box), which expose completely different disk interfaces. A thin block layer
in `fat64.c` hides that behind `blk_read`/`blk_write`:

```
fat64  ──▶  blk_read / blk_write / blk_write_many
                     │
       ┌─────────────┼───────────────────────────┐
       ▼             ▼                           ▼
   virtio-blk     StorVSC (VMBus)            RAM disk
   vblk64.c       storvsc64.c + vmbus64.c    (embedded image)
   (QEMU dev)     (Hyper-V daily)            (fallback)
```

- **`vblk64.c`** — a legacy virtio-blk PCI driver: PCI scan, one split
  virtqueue, 3-descriptor request chains. Used on the QEMU dev box.
- **`storvsc64.c`** — the Hyper-V synthetic SCSI (StorVSC) driver over VMBus
  (`vmbus64.c`): open a channel, run the vstor handshake, then issue SCSI
  `READ(10)`/`WRITE(10)` via a **GPA-direct** data buffer (guest-physical pages
  handed to the host). Because kernel memory is identity-mapped, the buffer's
  virtual address *is* its physical address.
- Both use a **page-aligned bounce buffer** (`g_dma`) so DMA always targets
  identity-mapped memory. **This is a load-bearing rule:** a ring-3 user
  buffer's virtual address is *not* its physical address, so passing it straight
  to a DMA engine corrupts data. All multi-sector block I/O stages through a
  kernel bounce buffer, then CPU-copies to/from the caller.

**Read-ahead cache.** `fat64.c` keeps a small direct-mapped cache of 4 KiB
windows for the device disks. TCC reads a 2.5 MB `libc.a` in ~1 KiB chunks; each
`blk_read` miss fills a whole 4 KiB window with one device transfer and serves
the following small sequential reads from RAM. This — plus a cluster-chain
position cache so seeks do not re-walk the FAT from the start — took a compile
that once ran over a minute down to ~3 seconds on Hyper-V.

On Hyper-V the FAT16 volume lives in a dedicated GPT **data partition**
(`tools/mkgptesp.py` lays out a protective MBR, GPT headers, a FAT32 EFI System
Partition, and the FAT16 data partition), auto-seeded on first boot and
surviving firmware GPT repair.

---

## 11. Filesystem: FAT16 and the VFS

`fat64.c` is a full FAT16 reader/writer:

- Boot sector/BPB parsing, FAT chain traversal, 8.3 directory entries,
  and **nested subdirectories** (path resolution walks arbitrary depth).
- Whole-file APIs (`fat_read`/`fat_write`) *and* offset-capable APIs
  (`fat_pread`/`fat_pwrite`/`fat_truncate`) with sector read-modify-write,
  cluster-chain growth/shrink, and zero-filled sparse gaps.
- A single-sector **FAT cache** with dirty tracking, plus the block-level
  read-ahead cache described above.

Above it sits a small **VFS** and a **kernel-side current working directory**
(`fs_resolve`/`fs_chdir`), so relative paths work and child programs inherit the
cwd. The disk image is produced by `tools/mkfatfs.py`, which now lays out
directory trees **recursively** — that is what lets the full musl `/include`
header tree (with `bits/`, `sys/`, …) live on the disk.

---

## 12. The Linux-compatible ABI

This is the substrate that turns tinyos from "runs its own programs" into "runs
stock Linux static binaries". The design choices (ratified early) were:

- **CPU/ABI:** x86-64 System V AMD64.
- **Syscalls:** the **Linux x86-64** convention (`rax` = number; args in
  `rdi, rsi, rdx, r10, r8, r9`; `syscall` instruction).
- **Executables:** static, non-PIE **ELF64 `ET_EXEC`** — the loader maps
  `PT_LOAD` segments at their fixed `p_vaddr` (standard `0x400000`), with no
  dynamic linker, PLT, or GOT.

**ELF loader + initial stack.** `arch64.c` validates the ELF and program
headers, maps each `PT_LOAD` (honoring `PF_W` writability), zeroes BSS, and
builds a canonical System V initial stack: `argc`, `argv[]`, `envp[]`, and an
`auxv` including `AT_PAGESZ`, `AT_PHDR`, `AT_ENTRY`, `AT_RANDOM`, and
`AT_EXECFN`.

**Per-process virtual memory.** Each Linux program runs under its own PML4,
cloned from the saved kernel CR3 with only the user regions replaced, plus an
isolated high user stack. Reserved arenas back the memory syscalls:

- `brk` (heap) and anonymous `mmap`/`munmap`/`mprotect` over private arenas.
- VM operations walk/create the 4-level tables, allocate zeroed pages, enforce
  process-owned ranges, and roll back on partial failure. Every entry we map
  carries the `PTE_OWN` software bit, so **process exit reclaims all private
  frames and page-table pages** (`free_user_pages`) without ever touching the
  shared firmware/kernel identity map ([§4](#4-memory-model)).

**Filesystem syscalls.** A per-process fd table (tty on 0/1/2, regular files
from 3) backs `openat`, `close`, `read`, `write`, `writev`, `lseek`,
`ftruncate`, `fstat`/`newfstatat`, `getdents64`, `mkdirat`, `unlinkat`, and
`getcwd`, all over the FAT16 VFS, plus `/dev/null` and blocking tty reads.

**`execve`.** Syscall 59 replaces the running image from a filesystem ELF: it
stages the caller's `argv`/`envp` in kernel memory, tears down the old image,
builds a fresh private address space and System V stack, and never returns on
success. Getting the nested syscall-stack lifecycle right (a dedicated
replacement stack) was the subtle part.

**The full implemented set (~30 syscalls):** `read, write, open, openat, close,
lseek, fstat, newfstatat, mmap, mprotect, munmap, brk, rt_sigprocmask,
rt_sigaction, rt_sigreturn, ioctl, writev, getpid, gettid, fork, execve, wait4,
pipe, dup2, kill, clone, ftruncate, getcwd, arch_prctl, set_tid_address,
set_robust_list, getdents64, clock_gettime, mkdirat, unlinkat, getrandom, exit,
exit_group`. Anything else returns `-ENOSYS`. Since v2.0.0 the process
primitives — **`fork`, `execve`, `wait4`, `pipe`, and signal delivery** — run
through the preemptive scheduler with per-task syscall stacks, which is what
lets the mbb shell pipe commands and the multi-pane terminal host concurrent
shells.

The shell reaches all of this: an unknown command is tokenized into `argv` and
run through `execve`, so `./musl.bin`, `./tcc.bin foo.c -o foo.bin`, and
`./foo.bin` all work from the prompt.

### MicroPython on the Linux ABI

MicroPython is deliberately not embedded in the kernel. The official v1.26.1
Unix port is built as a static, non-PIE musl ELF64 program and packaged as
`/PYTHON.BIN`. It therefore follows the same ELF loader, System V stack,
private page-table, VFS, tty, memory-syscall, and process-reclamation path as
TinyCC and the other static Linux programs.

The `minimal` variant is extended at build time with two native user C modules:
`tgl` and `taudio`. Their ordinary Python object and drawing logic runs in ring
3. Private syscall numbers outside the Linux range provide only the hardware
boundary: TinyGL buffer creation/presentation/destruction and Intel HDA
readiness/tone/diagnostics/volume.

TinyGL buffer creation maps the buffer object's pixel pages directly into the
MicroPython process. `Surface.fill`, `disc`, `text`, and the other raster
methods write to that mapping without copying frames through the kernel;
`present` asks the compositor to display it. See
[`MICROPYTHON.md`](MICROPYTHON.md) for the end-to-end build and runtime path.

---

## 13. Userland: musl and the on-device toolchain

**musl libc.** Programs are linked against a cross-built static **musl 1.2.4**
`libc.a`. `arch_prctl(ARCH_SET_FS)` + TLS, `set_tid_address`, `clock_gettime`,
`writev`, terminal `ioctl`, and the auxv entries are exactly what musl's startup
needs. A static musl program using `printf`, `malloc`, and file I/O runs
unmodified — the full C standard library is present because `libc.a` *is* the
complete library.

**On-device C compiler (TinyCC).** `make setup-tcc` (`tools/setup_tcc.sh`)
builds **TinyCC** and **musl** from source vendored in `third_party/` (no
network access or distro packages) and stages a self-contained toolchain root:

- `/tcc.bin` — the compiler, a static musl ELF.
- `/lib/{crt1.o,crti.o,crtn.o,libc.a,libtcc1.a}` — CRT objects and libraries at
  the path TCC searches by default (`tcc -vv` shows `/lib`).
- `/include/` — the **full musl header tree** (`stdio.h`, `stdlib.h`,
  `string.h`, `ctype.h`, `math.h`, nested `bits/`, `sys/`, …).

Because those live at TCC's built-in search paths, a bare command works:

```
./tcc.bin kr.c -o kr.bin      # resolves headers from /include, links /lib
./kr.bin
```

TinyCC is patched to **link statically by default** (tinyos has no dynamic
linker, so a default dynamic executable with `PT_INTERP`/PLT would fault). It
compiles ordinary `main()`-based C — K&R-style programs using `<stdio.h>`,
`<stdlib.h>`, `<string.h>`, `<ctype.h>` build and run on-device — and it is
**self-hosting**: TCC can recompile its own preprocessed source into a working
second-generation compiler, verified end to end.

This closes the north-star loop: the OS can build native executables for itself.

---

## 14. Design principles and rationale

- **Assembly only where required.** Just the AP trampoline, the SYSCALL/IDT/
  context-switch stubs, and a boot shim; everything else is C.
- **One binary, no host.** The whole system is a single freestanding PE32+.
  There is no module loader and nothing beneath the kernel.
- **Verifiable headlessly.** Every bring-up step logs to COM1, so a CI/sandbox
  can confirm behavior by grepping the serial log even with no display. Boot
  self-test probes (`[linux-abi] …`) exercise the ELF loader, VM, fd I/O, and
  stat/dir paths before login.
- **Two ABIs, gated per process.** Legacy flat programs keep working alongside
  Linux ELF programs; the boundary is a single per-process flag.
- **Identity-mapped kernel, bounce-buffered DMA.** VA == PA makes drivers simple
  and is the reason DMA must never touch a user VA directly.
- **Correctness before speed, then measure.** The FS caches were added only
  after a real workload (TCC linking `libc.a`) proved the bottleneck was I/O
  round trips, not CPU.
- **Safety nets on untestable paths.** Hyper-V-only code (StorVSC) self-tests
  before claiming the disk and falls back to a RAM disk on any failure, so a
  half-finished driver never destroys data.

---

## 15. Multi-pane terminal, power, and real hardware

v2.0.0 turned the x64 desktop from a demo into something usable for long
sessions on a laptop. Three threads of work: a concurrent terminal, power
efficiency, and hardening on a real machine (a **ThinkPad T440s** — Haswell,
Intel HD 4400, 4 cores, AHCI SSD, xHCI USB).

### Multi-pane terminal

The terminal splits into up to **four concurrent shells** in a 2-column grid
(filled left→right, then top→bottom). **F5** spawns a pane, **F4** cycles focus,
`exit`/Ctrl-D closes one and the survivors re-flow and renumber.

- **One process per pane.** Each pane runs the mbb shell as its own scheduled
  task. A **router** (the pane-0 task) owns the keyboard: it reads a key, applies
  window hotkeys (F1–F5), and otherwise pushes it into the **focused pane's input
  ring**; each shell blocks on `pane_getkey` for its own ring. A process's
  `stdout` (`usys_write`) is routed to the terminal grid of *its* pane via
  `current->pane`.
- **Per-pane line discipline.** Canonical/raw mode, echo, the foreground process
  group, the cooked input line, and command history are all **per pane**. This
  was essential: a single shared line discipline let a raw-mode program (`vi`)
  in one pane silently disable echo in the others.
- **Robust teardown.** Closing a *middle* pane renumbers the higher shells;
  `pane_getkey` re-reads its pane index on every wait and `reap_pane` wakes all
  channels, so a renumbered shell is never orphaned on a stale ring.

### Power management

The original desktop and shell loops spun at 100% CPU. They now **idle on
`HLT`**:

- `idle_wait()` runs `sti; hlt` (gated on the scheduler) in the shell read
  loops; the scheduler-stopped native console arms a scoped one-shot LAPIC tick
  around the halt. Idle cores drop from ~100% to ~0%.
- The compositor renders **on change** (plus the 500 ms cursor-blink/clock
  cadence), so a quiet screen blits nothing.
- The system monitor gained a **CPU %** meter (halted-time accounting), battery,
  voltage, and **LID** rows.
- **Sleep** — framebuffer blanked, deepest idle — triggers on **F3**, **lid
  close** (EC), or the **power button** (ACPI PM1 `PWRBTN_STS`), and wakes on
  lid-open or any key. `power_poll()` samples the lid ~4×/s and acts only on a
  real open⇄closed *transition*, so a wrong static reading can never blank the
  screen.

### Real hardware: storage, USB, and firmware-safe teardown

- **AHCI** mounts the internal SATA disk read/write at **`/mnt`**, persistent
  across reboots. **xHCI USB mass-storage** mounts a stick at **`/usb`** — the
  bridge for moving files in and out. Both sit behind the same FAT engine
  (`datavol64.c`) with a one-sector FAT cache and batched cluster writes (the fix
  for an ~8 s USB-write hang that used to leave copied files empty).
- The **embedded controller** (T440s, GJHT25WW) is read for battery percent /
  voltage and, from EC register `0x46`, AC-present (bit 4) and lid (bit 2).
- **Firmware-safe page-table teardown.** The kernel runs on the firmware's
  identity map, and this firmware sets `US` (and an available software bit) on
  its kernel entries — which alias the whole heap arena. The teardown/fork walks
  therefore cannot trust `US` to mean "user page": doing so double-freed / freed
  **live kernel pages** on process exit, stable in QEMU but crashing the T440s
  with `#PF`/`#GP`. The fix tags only entries **we** create with `PTE_OWN`
  (bit 9), strips it from every copied table, and gates all free/clone walks on
  it — so teardown provably never descends into shared firmware mappings.
- **Graphics apps own the screen.** When a full-screen TinyGL app presents,
  `gfx_present` timestamps the frame and the compositor **stands down**
  (`wm_pump` early-returns) so the terminal freezes cleanly in the background
  instead of overdrawing the graphics; the app reads input through the pane
  router, so it can be driven and quit (`q`) from a pane. On exit the compositor
  resumes.

---

## 16. Building and running

```bash
# 64-bit UEFI build
make ARCH=x64                 # -> build/BOOTX64.EFI (one PE32+ image)
make ARCH=x64 run             # QEMU + OVMF in a window (needs a display)
make run-x64-headless         # QEMU + OVMF, serial only (CI/sandbox)

# Hyper-V Gen 2 artifact (attach as the VM's disk; Secure Boot OFF)
make tinyos.vhdx              # -> tinyos.vhdx (GPT: FAT32 ESP + FAT16 data)

# One-time: build musl + TinyCC from vendored source (third_party/) into /tmp
make setup-tcc

# Build pinned MicroPython with the native TinyGL and audio modules
make setup-micropython
```

Requirements: `gcc`/`g++` + binutils (`ld` supporting `-mi386pep`, `objcopy`
supporting `pei-x86-64`), Python 3, `qemu-system-x86_64` + OVMF for local runs,
and `qemu-img` to package the VHDX. No mingw, gnu-efi, or clang required. The
target toolchain (musl libc + TinyCC) is built from source vendored in
`third_party/`, so **no network access or distro packages are required**.

At the sign-in panel log in with `root` / `root`. From the terminal, `help`
lists commands; `./musl.bin`, `./printf.bin`, and `./tcc.bin file.c -o out.bin`
demonstrate the Linux-ABI path.

`python /hello.py`, `python /audio.py`, `python /tgl.py`, and
`python /reversi.py` exercise the MicroPython language, HDA, zero-copy TinyGL,
and graphical game paths respectively.

---

## 17. Testing and verification

- **Headless boot markers.** `make run-x64-headless` prints an ordered set of
  markers; a healthy boot reaches `[linux-abi] … ok`, `[fs] fat16 … mounted`,
  the `[tcc] compile + execute cycle complete` sequence, and `[login] awaiting
  credentials`, with no `[EXC]`.
- **Boot self-tests.** An embedded probe exercises the ELF loader, `brk`/`mmap`/
  `mprotect`/`munmap`, fd offset I/O, `stat`+directory VFS, `ftruncate`+sparse
  holes, and the musl + on-device-TCC compile/run cycle before the desktop
  starts.
- **Image validation.** VHDX generation is checked with `qemu-img` (both GPT
  header/entry CRCs, partition LBAs, ESP contents).
- **Hardware.** The daily-driver Hyper-V VM is the final gate; the COM1 serial
  log is captured over a named pipe to confirm the same markers on real
  hardware (10/10 cores, StorVSC persistence, Linux-ABI probes, TCC).
- **MicroPython.** The pinned interpreter is checked as a static ELF, its
  built-in modules are imported on the host, scripts are compiled with
  `mpy-cross`, and graphics/audio are exercised under QEMU + OVMF with emulated
  Intel HDA. Reversi rendering is verified from a framebuffer capture.

See [`TEST_RESULTS.md`](TEST_RESULTS.md) for the recorded outcomes.

---

## 18. Known limitations and future work

- **A ring-3 fault terminates the faulting program**, not the kernel: a bad user
  program prints `[EXC] usr pid … killed` and its pane's shell continues. (A
  kernel-mode fault still panics and halts.)
- **APs park idle.** SMP bring-up works, but scheduling is BSP-only; SMP-aware
  scheduling and TLB shootdown are future work.
- **No true page-flip.** The display is a software double buffer over a single
  GOP framebuffer; tear-free animation needs a real GPU driver.
- **No USB-HID.** Real PCs need a PS/2 or legacy-USB keyboard; there is no
  USB-HID input stack yet.
- **Storage is AHCI/SATA + USB only.** NVMe-only machines have no persistence
  path, and the on-disk layout is FAT (512-byte clusters).
- **The firmware-`US` teardown fix is real-hardware-specific.** It is validated
  by construction and on-device; OVMF/QEMU does not exercise the failing
  condition.
