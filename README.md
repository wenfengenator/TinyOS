# tinyos

A tiny teaching OS for 32-bit x86: a **boot sector** brings the CPU into
protected mode and loads a **kernel written in C** (ring 0) that hosts
**user programs written in C++** (ring 3). User programs talk to the kernel only
through `int 0x80` system calls.

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

## Build

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

See [`docs/DESIGN.md`](docs/DESIGN.md) for the architecture (memory map, boot
flow, privilege model, syscall ABI, scheduler, and how the emulator stands in
for hardware interrupts).
