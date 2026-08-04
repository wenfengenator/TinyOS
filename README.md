# tinyos

**Release: v2.0.0**

A teaching operating system built from scratch, in two editions that show its
evolution:

* a **32-bit x86 (i386)** kernel that boots from a 512-byte boot sector into
  protected mode and hosts C++ ring-3 programs over an `int 0x80` ABI; and
* a **64-bit (UEFI, long mode)** kernel that boots as a PE32+ EFI application,
  brings up every CPU core, drives a framebuffer desktop, persists a FAT16
  filesystem on Hyper-V and QEMU, and hosts a **Linux-compatible ABI** rich
  enough to run **musl** programs and an **on-device, self-hosting C compiler**.

The kernel is C; user programs are C/C++ or static Linux ELF64 binaries.
Assembly is used only where it must be (boot shim, AP trampoline, syscall/
interrupt/context-switch stubs).

> **New here?** Read [`docs/GUIDE.md`](docs/GUIDE.md) for a beginner walkthrough
> of the 32-bit kernel, then [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for
> the full 64-bit system design.

## The 32-bit edition

It demonstrates the classic building blocks of an OS:

* **System calls** — `read`/`write`/`open`/`close`/`exit`/`getpid`/`yield`/
  `exec`/`fork`/`wait`/`stat`/`lseek`/`readdir`/`taskinfo`, plus `readfile`/
  `writefile`/`getchar`/`clear`/`gotoxy` for the editor, over an `int 0x80` ABI
* **File handling** — an in-RAM filesystem (`/etc/passwd`, `/README`, `/tmp/log`)
* **User login** — a ring-3 program that authenticates against `/etc/passwd`
* **Multitasking** — a task table + round-robin scheduler
* **Preemption** — an 8253 PIT / 8259 PIC timer (hardware) and time-slice
  switching (emulator)
* **Paging** — per-task address spaces with real inter-task isolation (`mm.c`)
* **Keyboard** — PS/2 IRQ1 driver feeding a stdin ring buffer
* **Block FS + exec** — programs packed in an embedded ramdisk, `exec`'d into
  their own isolated address spaces
* **Mounted disk** — an **ATA PIO driver** reads *and writes* a real **FAT16**
  image mounted at `/mnt` (`ata.c`, `fat.c`); edits made with `vi` persist to the
  disk image across reboots.  Unified with the ramfs by a small **VFS** (`vfs.c`)
* **Directories + a shell** — `cd`/`pwd`, `ls`, `ps`, `cat`, `run <prog>`, `fork`,
  `race`, and `vi <file>` (a modal full-screen editor); the shell and `vi` support
  **arrow keys, Delete, Home/End** for in-line editing, `exit`

Assembly is used only where it must be: the real-mode boot sector, the
protected-mode switch, and the entry / interrupt / context-switch stubs.

## Build (32-bit)

```bash
make            # -> boot.bin, kernel.bin, os.img, fs.img (FAT16 disk)
```

Toolchain: GNU `as`/`ld`/`objcopy`, plus `gcc`/`g++ -m32` (via `tools/xcc` /
`tools/xcxx`, which fall back to the WSL host toolchain in the Copilot sandbox).
`fs.img` is built from the `disk/` tree by `tools/mkfatfs.py` (no `mkfs.fat`
needed).

## Run

QEMU is the real-hardware target; the FAT disk is attached automatically as a
second drive so `/mnt` is populated:

```bash
make run          # graphical: VGA + PS/2 keyboard
make run-serial   # headless: console mirrored over COM1 to your terminal
```

Log in as `root` / `toor` (or `guest` / `guest`).

Where QEMU is unavailable, a bundled Unicorn emulator runs the exact same image.
The first `make run-emu` auto-installs the `unicorn` Python package into a
repo-local `.pylibs/` (or run `make deps` yourself); later runs reuse it:

```bash
make run-emu      # boots os.img on tools/emulate.py and prints the VGA screen
```

Example output:

```
tinyos$ cd /mnt              # a real FAT16 volume mounted from fs.img
/mnt $ ls
  docs/  hello.txt  readme.txt
/mnt $ cat readme.txt
Hello from a real FAT16 filesystem on a mounted disk image!
/mnt $ cd /
/ $ vi /note.txt             # modal editor: i insert, :wq save+quit
/ $ run hello
[hello] I am a separate program from the ramdisk, pid=11, isolated @0x40000000
/ $ fork
  [parent] forked child pid 12
  [child]  hello from the forked child!
/ $ race
  [count pid 13] 0
  [count pid 14] 0
  [count pid 13] 1
  [count pid 14] 1
  ...
```

(The `race` tasks never yield — the interleaving is preemptive, and each `run`
program executes in its own paged, isolated address space. Files under `/mnt`
come from the FAT16 disk image via the ATA driver.)

## The 64-bit UEFI edition

Built with `ARCH=x64`, this is a long-mode kernel packaged as a single PE32+ EFI
application (`BOOTX64.EFI`). It boots on `qemu-system-x86_64` + OVMF and on
**Hyper-V Gen 2**. Highlights:

* **UEFI boot + GOP framebuffer**, SMP bring-up of every CPU core
  (INIT-SIPI-SIPI), ACPI MADT discovery, a millisecond clock, and a bump heap
  from the UEFI memory map.
* **Ring 0/3 with SYSCALL/SYSRET**, a preemptive LAPIC-timer scheduler for
  kernel threads, and a keyboard-driven **desktop**: a compositor (`wm64`) with
  a terminal and live system monitor, a sign-in panel, and `pong`/graphics
  demos — all over a shared **double-buffered display driver** (`disp64`) that
  blits only changed scanlines.
* **Persistent FAT16** behind a block layer: **virtio-blk** on QEMU and
  **StorVSC over VMBus** on Hyper-V, with a read-ahead cache and a GPT data
  partition.
* **A Linux-compatible ABI** (~30 x86-64 syscalls, a static ELF64 loader,
  per-process page tables, `brk`/`mmap`, a descriptor-based VFS, and `execve`)
  that runs stock **musl** binaries and an **on-device TinyCC** — building
  ordinary/K&R C against the full musl standard-library headers, and
  recompiling itself.
* **Concurrent processes** — real `fork`/`execve`/`wait4`, `pipe`, and signal
  delivery, with a preemptive scheduler and **per-process frame reclamation**
  (dead processes return their pages to the allocator).
* **A tmux-style multi-pane terminal** — up to four **concurrent shells** in a
  grid; **F5** splits a new pane, **F4** cycles focus. Each pane has its own
  line discipline, history, and cursor, so you can edit in one pane while a
  build runs in another.
* **Power management for battery life** — an **HLT-based idle** (idle cores draw
  ~0% instead of spinning), render-on-change compositing, a live **CPU %** meter,
  and **sleep** (screen off + deep idle) on **F3**, **lid close**, or the
  **power button**; a wake on lid-open or keypress.
* **Runs on real hardware** — bring-up and stability hardening on a **ThinkPad
  T440s** (Haswell): **AHCI** persistence at `/mnt`, **xHCI USB mass-storage**
  read/write at `/usb` (the file bridge to the outside world), EC battery/AC
  reporting, and a firmware-safe page-table teardown (see the release notes).

```bash
make setup-tcc                # one-time: build musl + TinyCC from vendored source (third_party/)
make ARCH=x64                 # build/BOOTX64.EFI (one PE32+ image)
make run-x64-headless         # QEMU + OVMF, serial only (CI/sandbox)
make tinyos.vhdx              # Hyper-V Gen 2 disk (attach; Secure Boot OFF)
make usb                      # tinyos-usb.img — GPT/UEFI image to dd onto a USB stick
make run-x64-disk             # boot the GPT image itself in QEMU (real-hardware path)
```

`make setup-tcc` is a **one-time prerequisite** that builds the musl libc and
TinyCC entirely from source vendored under [`third_party/`](third_party/) — no
network access or distro packages, just a standard host toolchain (`gcc`,
`make`, binutils, Python). It stages the result under `/tmp`, so re-run it if
`/tmp` is cleared (e.g. after a reboot).

### Boot on a real PC (USB)

`tinyos-usb.img` (built by `make usb`) is a raw **GPT disk** whose FAT32 **EFI
System Partition** holds `/EFI/BOOT/BOOTX64.EFI` — exactly what UEFI firmware
boots from a USB stick. Write it and boot with **Secure Boot disabled**:

```bash
sudo dd if=tinyos-usb.img of=/dev/sdX bs=4M conv=fsync status=progress   # ERASES /dev/sdX
```

A physical PC has no virtio-blk/StorVSC, so TinyOS runs **live from a RAM disk**
embedded in the loader (the mbb shell + coreutils, local `git`, Tetris/2048, and
the TinyGL demos). On machines with a **SATA (AHCI)** controller — e.g. the
ThinkPad T440s — the internal disk is mounted read/write at **`/mnt`** and
persists across reboots, and a plugged-in **USB stick** is mounted at **`/usb`**
(xHCI mass-storage) as the bridge for moving files in and out. A **PS/2 or
legacy-USB keyboard** is required (no USB-HID stack yet); NVMe-only laptops have
no persistence path yet.

Sign in as `root` / `root`. From the terminal, `./musl.bin`,
`./tcc.bin file.c -o out.bin`, and `./out.bin` exercise the Linux-ABI path;
`cp kr.c /usb/kr.c` copies a file out to the USB stick. Press **F5** to split
the terminal into concurrent panes (**F4** to cycle focus), and **F3** (or close
the lid) to sleep.
See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design.

## Layout

```
boot.asm          boot sector (assembly)
kernel/           C kernel: mm/paging, fs, ramdisk, procs, sched, timer, kbd,
                  ata (disk), fat (FAT16), vfs (unified tree), entry.s
user/             in-kernel ring-3 tasks (login, worker) + ulib.h
programs/         standalone ramdisk programs: shell, hello, count, vi (editor)
disk/             source tree baked into the FAT16 image (fs.img)
tools/            emulate.py (Unicorn PC), mkramdisk.py, mkfatfs.py, xcc/xcxx
docs/             GUIDE (beginner walkthrough), DESIGN, PROJECT_PLAN, TEST_PLAN, TEST_RESULTS
Makefile
```

**New to OS development?** Start with [`docs/GUIDE.md`](docs/GUIDE.md) — a detailed,
concept-first walkthrough of how tinyos works, from power-on to a running app,
written for entry-level OS developers.

Documentation map:

* [`CHANGELOG.md`](CHANGELOG.md) — release notes per version (v2.0.0, v1.0.0).
* [`docs/GUIDE.md`](docs/GUIDE.md) — beginner, concept-first walkthrough (32-bit).
* [`docs/DESIGN.md`](docs/DESIGN.md) — architecture reference for the 32-bit kernel
  (memory map, boot flow, privilege model, `int 0x80` ABI, scheduler, emulator).
* [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — the full **64-bit UEFI** system
  design (boot flow, SMP, display, storage, filesystem, Linux ABI, toolchain).
* [`docs/PROJECT_PLAN.md`](docs/PROJECT_PLAN.md) — objectives, phases, milestones
  across every cycle.
* [`docs/TEST_PLAN.md`](docs/TEST_PLAN.md) / [`docs/TEST_RESULTS.md`](docs/TEST_RESULTS.md)
  — how it is verified and what has passed.
* [`docs/CYCLE2_PLAN.md`](docs/CYCLE2_PLAN.md), [`docs/CYCLE5_PLAN.md`](docs/CYCLE5_PLAN.md),
  [`docs/CYCLE6_PLAN.md`](docs/CYCLE6_PLAN.md), [`docs/CYCLE7_PLAN.md`](docs/CYCLE7_PLAN.md),
  [`docs/CYCLE8_PLAN.md`](docs/CYCLE8_PLAN.md) — per-cycle plans (x64/graphics;
  compiler substrate; Tier 1 mini-Unix / busybox-static; **TinyGL** graphics
  stack; connectivity / TCP-IP / USB).
