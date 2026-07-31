# tinyos — Test Results & Build Log

This file logs, in order, every significant step taken to build and verify
tinyos, with the observed result. Re-run everything with `make run-emu`.

Legend: ✅ pass · ⚠️ expected-negative (informs a design choice) · ℹ️ info

---

## Phase 0 — Environment & toolchain discovery

| # | Step | Result |
|---|------|--------|
| 0.1 | Probe for `nasm`, `qemu`, compilers | ℹ️ no `nasm`, no `qemu`, no `gcc` on the snap `PATH`; found GNU `as`/`ld`/`objcopy`, `python3` |
| 0.2 | Install `unicorn` via `pip --target` (unset `PIP_PREFIX`) | ✅ `unicorn 2.1.4` importable |
| 0.3 | Locate a C/C++ compiler | ℹ️ WSL host `gcc-13`/`g++-13` at `/var/lib/snapd/hostfs/usr/bin` (needs `LD_LIBRARY_PATH`) |
| 0.4 | Compile freestanding i386 C & C++ (`-m32 -ffreestanding -c`) | ✅ ELF32 / Intel 80386 objects |

## Phase 0 — Unicorn capability de-risk (probe programs)

| # | Probe | Result |
|---|-------|--------|
| P1 | PM + GDT + `iret`→ring 3 (CPL=3) + ring-3 write + `int 0x80` hook | ✅ PASS (`cpl=3`, hook saw `int 0x80`) |
| P2 | Redirect EIP inside INTR hook to run a "real" handler | ⚠️ `UC_ERR_EXCEPTION` → do **not** redirect |
| P3 | Dispatch-and-continue: service `int 0x80`, resume ring 3 | ✅ PASS |
| P4 | `lgdt` / `lidt` / `ltr` under Unicorn | ✅ PASS (no fault) |
| P5 | `int 0x80` vectored through a real IDT (no hook) | ⚠️ `UC_ERR_EXCEPTION` → emulator must service syscalls |
| P6 | Port I/O `in`/`out` under Unicorn | ✅ PASS (no fault) → safe to add PIC/PIT |

**Conclusion:** the emulator can run real ring-0/ring-3 code and privileged
setup, but must service `int 0x80` itself and drive preemption itself.

## Phase 1 — Bootloader

| # | Step | Result |
|---|------|--------|
| 1.1 | Write `boot.asm` (16-bit → PM, load 64 sectors, A20, GDT, CR0.PE, far jmp) | ✅ assembles |
| 1.2 | `boot.bin` size / signature | ✅ 512 bytes, `0x55 0xAA` |
| 1.3 | Disassembly check | ✅ int 0x13 read, port 0x92 A20, `lgdt`, CR0.PE, `ljmp 0x08:pm_entry`, then `ljmp 0x08:0x10000` |

## Phase 2 — C kernel + C++ user foundation

| # | Step | Result |
|---|------|--------|
| 2.1 | `kernel/entry.s` + `kernel.c` (console, GDT/IDT/TSS) + `user/user.cpp` | ✅ build |
| 2.2 | Two-stage emulator run | ✅ kernel prints (ring 0), C++ app prints via `int 0x80` at `cpl 3`, exit |

## Phases 3–7 — Features

| # | Step | Result |
|---|------|--------|
| 3.1 | Expand syscall ABI (fd-based write/read, open, close, getpid, yield, spawn) | ✅ build |
| 4.1 | `kernel/fs.c` in-RAM filesystem; seed `/etc/passwd`, `/README`, `/tmp/log` | ✅ symbols exported, files seeded |
| 4.2 | Fix emulator FS lookup (`bytes == str` bug) | ✅ `open(/etc/passwd) → fd 3` |
| 5.1 | `kernel/sched.c` task table + round-robin; create login + 2 workers | ✅ build |
| 6.1 | `kernel/timer.c` PIC remap + PIT 100 Hz + `irq0_stub` | ✅ build (harmless under emulator) |
| 6.2 | Emulator time-slice scheduler (quantum = 40000 insns) | ✅ workers interleave |
| 7.1 | `user/login.cpp` — prompt, `read`, authenticate vs `/etc/passwd` | ✅ `welcome, root!` |
| 8.1 | Console scrolling (kernel + emulator) so >25 lines don't wrap-garble | ✅ clean screen |

## Component checks (current build)

| ID | Check | Value / Result |
|----|-------|----------------|
| C1 | `boot.bin` | ✅ 512 bytes, sig `0x55 0xAA` |
| C3 | `kernel.bin` | ✅ 5490 bytes (< 32 KB) |
| C4 | exported symbols | ✅ `g_current`,`g_ntask`,`g_fds`,`g_files`,`g_tasks` present |
| C5 | warnings | ✅ none (only benign `ld` RWX-segment note) |

## Integration test — `make run-emu` (latest run)

Emulated VGA console:

```
[kernel] tinyos online: 32-bit protected mode, ring 0 (C)
[kernel] GDT + IDT + TSS ready; syscall gate int 0x80 installed
[kernel] PIC remapped; PIT armed at 100 Hz (preemption timer)
[kernel] RAM filesystem mounted (/etc/passwd, /README, /tmp/log)
[kernel] scheduler started; launching login task in ring 3...
[login] tinyos secure login
[login] username: root
[login] password: ****
[login] authenticated. welcome, root!
[login] spawning worker tasks A and B...
[worker 1] starting
[worker 1] tick 0
[worker 2] starting
[worker 2] tick 0
[worker 1] tick 1
[worker 2] tick 1
[worker 1] tick 2
[worker 2] tick 2
[worker 1] exiting
[worker 2] exiting
[kernel] contents of /tmp/log (32 bytes):
[worker 1] done
[worker 2] done
[kernel] all tasks exited; scheduler idle, halting.
```

Emulator trace (abridged):

```
('int13.read', 'lba', 1, 'count', 64)
('boot', 'CR0.PE set', '0x7c46', '-> stage B')
('sys_read.stdin', '->', b'root')
('sys_read.stdin', '->', b'toor')
('sys_open', '/etc/passwd', 'flags', 0, '->', 3)
('sys_read.file', 'fd', 3, '->', 22)
('sys_spawn', 'task', 1)
('sys_spawn', 'task', 2)
('sys_exit', 'task', 0, 'code', 0)
('sys_open', '/tmp/log', 'flags', 1, '->', 3)   # workerA append
('sys_exit', 'task', 1, 'code', 0)
('sys_open', '/tmp/log', 'flags', 1, '->', 3)   # workerB append
('sys_exit', 'task', 2, 'code', 0)
('preempt' x96  <- preemptive context switches between tasks)
```

### Assertions

| ID | Assertion | Result |
|----|-----------|--------|
| T1 | boot output + `int13.read` + `CR0.PE` | ✅ |
| T2 | 5 kernel init lines | ✅ |
| T3 | syscalls at `cpl 3` (ring 3) | ✅ |
| T4 | stdin read `root` / `toor` (echoed `****`) | ✅ |
| T5 | `/etc/passwd` open→fd 3, read→22 bytes | ✅ |
| T6 | `authenticated. welcome, root!` | ✅ |
| T7 | two workers run | ✅ |
| T8 | preemptive interleaving (96 context switches) | ✅ |
| T9 | `/tmp/log` holds **both** worker records (append) | ✅ |
| T10| clean self-termination, no `UcError` | ✅ |

**Result: all assertions pass; the integration demo runs end-to-end and halts.**

## Notes / known limitations

* ℹ️ Hardware preemption (`irq0_stub` context switch) and keyboard IRQ1 are not
  exercised in the sandbox — Unicorn delivers no IRQs; the emulator performs the
  equivalent time-slice switch and feeds scripted stdin. See `docs/DESIGN.md §12`.
* ℹ️ The boot sector's protected-mode far jump is not followed by the emulator
  (fixed-mode Unicorn); its real-mode portion is executed in stage A and the
  handoff to the kernel is verified.

---

## Phase 11 — Advanced features (mini-Unix layer)

| # | Step | Result |
|---|------|--------|
| 11.0 | De-risk: Unicorn honours CR3 paging (VA 0x40000 -> PA 0x100000 via page table) | ✅ PASS |
| 11.1 | `kernel/mm.c`: frame allocator + paging; enable CR0.PG, identity-map 16 MB | ✅ demo runs with paging on |
| 11.2 | Per-task `cr3`/`kstack`; emulator switches CR3 on context switch | ✅ |
| 11.3 | `kernel/kbd.c` IRQ1 + `g_kbd` ring buffer; login reads keyboard stdin | ✅ `username: root` etc. |
| 11.4 | Ramdisk build (`programs/`, `mkramdisk.py`, `.incbin`) embedded in kernel | ✅ `hello.prog` packed |
| 11.5 | `exec` into isolated address space (`proc.c` + emulator mm/translate) | ✅ hello at cr3 0x408000 |
| 11.6 | `fork` / `wait`; `stat` / `lseek` / `readdir` / `taskinfo` | ✅ |
| 11.7 | `programs/shell.cpp` interactive shell (exec'd by login) | ✅ |
| 11.8 | `switch_context` primitive (entry.s) for the hardware ctx switch | ✅ builds |

Current build: `kernel.bin ≈ 12.7 KB` (incl. embedded ramdisk); no warnings.

### Integration test — shell session (`make run-emu`, latest)

```
[shell] tinyos shell ready. type 'help'.
tinyos$   help ls ps cat<f> run<p> fork race exit
tinyos$ [hello] I am a separate program from the ramdisk, pid=11, isolated @0x40000000
tinyos$   [parent] forked child pid 12
  [child]  hello from the forked child!
tinyos$   starting two isolated 'count' tasks (watch them interleave)
  [count pid 13] 0
  [count pid 14] 0
  [count pid 13] 1
  [count pid 14] 1
  [count pid 13] 2
  [count pid 14] 2
tinyos$ [shell] bye
[kernel] no runnable tasks; scheduler idle, halting.
```

Trace highlights: `exec shell -> pid 10 cr3 0x400000`, `exec hello -> pid 11
cr3 0x408000` (distinct address space ⇒ isolation), `fork parent 10 -> child 12`,
preemptive `count` interleaving.

### Assertions

| ID | Assertion | Result |
|----|-----------|--------|
| T11 | paging enabled, demo continues | ✅ |
| T12 | keyboard-driven shell commands | ✅ |
| T13 | `ls` shows fs files + ramdisk programs | ✅ |
| T14 | `cat /README` prints contents (stat+lseek) | ✅ |
| T15 | `ps` lists tasks | ✅ |
| T16 | `run hello` isolated (cr3 0x408000 ≠ 0x400000) | ✅ |
| T17 | `wait` blocks shell until hello exits | ✅ |
| T18 | `fork` runs parent + child | ✅ |
| T19 | `race` interleaves two `count` tasks (preemption) | ✅ |
| T20 | `exit` halts cleanly, no `UcError` | ✅ |

**Result: all advanced-feature assertions pass; the mini-Unix demo runs end to
end and self-terminates.**

---

## Phase 12 — Real-hardware (QEMU) bring-up

Tested on host `qemu-system-i386` 8.2.2 (`make run` graphical; `make run-serial`
headless). Initial QEMU run exposed gaps the emulator had masked; all fixed:

| # | Issue on QEMU | Fix | Result |
|---|---------------|-----|--------|
| 12.1 | `sys_read(stdin)` non-blocking → login read empty → auth FAILED | block with `sti;hlt`, draining PS/2 (IRQ1) + serial RX | ✅ reads input |
| 12.2 | No task scheduling on hardware (exit/exec/fork/timer didn't switch) | real `ctx_switch` + `sched_exit/yield/wait` + `timer_tick` preemption (`sched.c`) | ✅ login→shell→run/fork/race |
| 12.3 | Preempted task never rescheduled | `ctx_switch` marks outgoing RUNNING→READY | ✅ `count` tasks interleave |
| 12.4 | Emulator hung on `serial_putc` (Unicorn `in` returns 0) | bounded LSR wait + emulator answers COM1 status port | ✅ emulator OK |
| 12.5 | VGA cursor not at prompt | update CRTC cursor (0x3D4/0x3D5) in `putc` | ✅ cursor tracks text |
| 12.6 | Typed characters not echoed | echo in `sys_read`; `SYS_ECHO` mode (plain / `*` mask / silent) | ✅ username shows, password masked |

### QEMU verification (serial capture, PS/2 keys via monitor `sendkey`)

```
[login] username: root
[login] password: ****
[login] authenticated. welcome, root!
tinyos$ help
  help ls ps cat<f> run<p> fork race exit
```

Preemption on QEMU (`race`): `[count pid 11] 0 / [count pid 12] 0 / 11] 1 /
12] 1 / 11] 2 / 12] 2` — two isolated exec'd tasks interleaving under the PIT
timer. **tinyos now runs on real hardware (QEMU) and on the emulator.**

## Phase 13 — Directories, `vi`, and a mounted FAT16 filesystem

Added a real block device and on-disk filesystem, directory navigation, and a
full-screen editor. Verified on **both** the emulator (`make run-emu`) and real
`qemu-system-i386` (`make run` / `make run-serial`, FAT disk as `-hdb fs.img`).

| # | Feature | Implementation | Result |
|---|---------|----------------|--------|
| 13.1 | ATA PIO disk driver | `kernel/ata.c` (LBA sector read/write, polls port 0x1F7) | ✅ reads `fs.img` on QEMU |
| 13.2 | FAT16 reader | `kernel/fat.c` (BPB parse, dir walk, cluster-chain read) | ✅ lists + reads files/subdirs |
| 13.3 | Unified VFS | `kernel/vfs.c` routes `/mnt`→FAT, rest→ramfs | ✅ transparent to shell/vi |
| 13.4 | Image builder | `tools/mkfatfs.py` writes BPB+FATs+root (no host mkfs.fat) | ✅ `make fs.img` from `disk/` |
| 13.5 | Directories + CWD | `cd`/`pwd`, path resolve, dir-aware `ls`/`cat` (`shell.cpp`) | ✅ prompt tracks CWD |
| 13.6 | `vi` editor | `programs/vi.cpp` + exec-arg page + `clear`/`gotoxy`/`getchar`/`readfile`/`writefile` | ✅ create/edit/save/quit |
| 13.7 | exec `.bss` fix | flat binary omitted `.bss` → page fault; `objcopy --set-section-flags .bss=alloc,load,contents` | ✅ vi's buffers load |
| 13.8 | Emulator parity | `FatImage` parses the same `fs.img` in Python (in-kernel `fat_mount` fails fast when ATA errors) | ✅ identical `/mnt` results |
| 13.9 | PS/2 Shift/Caps | `on_keyboard` tracks Shift (0x2A/0x36) + Caps (0x3A), adds a shifted scancode table | ✅ `:` (Shift+;) and capitals work; `vi` `:wq` saves on the graphical PS/2 keyboard |

### QEMU verification (headless serial capture)

```
/ $ ls
  etc/
  README
  tmp/
  mnt/
/ $ cat /mnt/readme.txt
Hello from a real FAT16 filesystem on a mounted disk image!
/ $ cd /mnt/docs
/mnt/docs $ cat note.txt
a note inside the docs subdirectory
/mnt/docs $ exit
[shell] bye
```

`vi` on QEMU: `vi /note.txt` → `i` insert "hello from vi" → `:wq`; then
`cat /note.txt` prints the saved text.

### Emulator verification (`make run-emu`, ~1s, self-halts)

```
/mnt $ ls
  docs/  hello.txt  readme.txt
/mnt $ cat readme.txt
Hello from a real FAT16 filesystem on a mounted disk image!
/mnt $ cd docs
/mnt/docs $ cat note.txt
a note inside the docs subdirectory
  [count pid 12] 0 / [count pid 13] 0 / 12] 1 / 13] 1 / 12] 2 / 13] 2
[shell] bye
```

**T21–T24 and Q1–Q5 all pass.** `cd`, `vi`, and a mountable real filesystem work
on the emulator and on real hardware (QEMU).

### PS/2 keyboard Shift fix (graphical QEMU, `sendkey`)

Injecting keys through the QEMU monitor (`sendkey shift-a`, `sendkey shift-semicolon`)
confirmed the scan-code path: in `vi`, `i` `Shift-a` `b` `ESC` `:wq` produced the
file `/k.txt` containing `Ab` (verified with `cat`).  Before the fix, Shift+`;`
yielded `;` (not `:`), so `vi` command mode was unreachable on the PS/2 keyboard.

## Phase 14 — FAT16 write-back (edits persist to the mounted image)

`SYS_WRITEFILE` used to always land in the RAM fs, so `vi /mnt/...` saves were lost
on reboot.  Added real FAT16 write-back so `/mnt` edits persist to `fs.img`.

| # | Implementation | Result |
|---|----------------|--------|
| 14.1 | `kernel/fat.c` `fat_write`: parent walk, free old chain, `fat_alloc` free clusters, `ata_write` data, update all FATs (`fat_set`) + 8.3 dirent | ✅ writes to the real disk |
| 14.2 | `kernel/vfs.c` `vfs_writefile`: route `/mnt`→`fat_write`, else ram fs; `SYS_WRITEFILE` now calls it | ✅ transparent to `vi` |
| 14.3 | `tools/emulate.py` `FatImage.write` mirrors `fat_write` and flushes `fs.img` | ✅ emulator persists too |

### QEMU persistence across a reboot (two boots, same `fs.img`)

```
run 1:  vi /mnt/log.txt  ->  i PERSIST123  ESC  :wq
        / $ cat /mnt/log.txt
        PERSIST123
run 2 (fresh QEMU, same fs.img):
        / $ cat /mnt/log.txt
        PERSIST123            <- survived the reboot
```
Result: **PERSISTED_OK**.

### Cross-reader/writer parity (kernel ⇄ emulator)

* Emulator `FatImage` reads the kernel-written `/log.txt` → `PERSIST123`.
* Emulator writes `/log.txt`=`EMU_WROTE_THIS` and creates `/emu_new.txt`; on QEMU
  the kernel `cat`s both back correctly (`KERNEL_READS_EMU_WRITES`).

Both the C kernel (`ata.c`+`fat.c`) and the Python `FatImage` read **and** write the
same on-disk FAT16 layout, so an image edited under one is valid for the other.
**T25 and Q6 pass — files edited under `/mnt` now survive a restart.**

## Phase 15 — Arrow keys, Delete & shell in-line editing

Navigation keys send **extended (`0xE0`-prefixed) scan codes** that the driver
previously dropped, so arrows/Delete did nothing on the PS/2 keyboard.

| # | Implementation | Result |
|---|----------------|--------|
| 15.1 | `kernel/kbd.c` latches the `0xE0` prefix and maps arrows/Delete/Home/End/PgUp/PgDn to `KEY_*` bytes (`kernel.h`/`ulib.h`) | ✅ nav keys reach apps |
| 15.2 | `programs/vi.cpp` handles `KEY_*` for cursor movement + forward `Delete` in both modes | ✅ editing with arrows |
| 15.3 | `kernel/syscall.c` `sys_read` gained an in-line editor (←/→, Home/End, Backspace, Delete) via `console_getpos`/`console_setpos` | ✅ shell line editing |

### QEMU verification (graphical PS/2, monitor `sendkey`)

```
vi:   i  a x b  ESC  left left  delete  :wq
      / $ cat /a.txt
      ab                       <- 'x' deleted after moving left twice
shell: l s s  left  delete  ENTER
      / $ (runs "ls")
        etc/  README  tmp/  a.txt  mnt/
```
Result: **VI_ARROWS_DELETE: OK**, **SHELL_LINE_EDIT: OK**.  Q7 and Q8 pass — arrow
keys and Delete now work in `vi` and drive the shell's in-line editor.  (Serial and
the emulator inject raw ASCII and don't emit `KEY_*` codes; `h j k l` still work
there.)

---

## Phase 16+ — the 64-bit UEFI edition (`ARCH=x64`)

The x64 kernel is verified by (a) headless boot markers over COM1 on
`qemu-system-x86_64` + OVMF, (b) embedded boot self-test probes, (c) `qemu-img`
validation of the VHDX, and (d) confirmation on a real 10-core **Hyper-V Gen 2**
VM via a COM1 named pipe. `✅` = confirmed on QEMU and (where noted) Hyper-V.

### Boot & platform
| Step | Result |
|---|---|
| UEFI PE32+ loads; GOP framebuffer acquired; ExitBootServices; long-mode entry | ✅ |
| Serial + framebuffer console online; TSC MHz + heap MB reported | ✅ |
| SMP: all CPUs online (10/10 on Hyper-V) via MADT + INIT-SIPI-SIPI | ✅ |
| GDT/TSS/IDT installed; ring 0/3 ready; `[tinyos64] … BOOT OK` | ✅ |

### Storage & filesystem
| Step | Result |
|---|---|
| virtio-blk persistence on QEMU (write → reboot → read) | ✅ QEMU |
| StorVSC handshake + SCSI READ/WRITE(10) via GPA-direct; STOR STG 12 | ✅ Hyper-V |
| GPT: FAT32 ESP + FAT16 data partition; both GPT CRCs valid (`qemu-img`) | ✅ |
| Cross-reboot persistence on the dedicated data partition | ✅ Hyper-V |
| FAT16 offset I/O, sector RMW, chain grow/shrink, sparse holes | ✅ |

### Linux-compatible ABI (boot self-test probes)
| Marker | Result |
|---|---|
| `[linux-abi] hello from static ELF64` / `ELF64 loader + SysV stack ok` | ✅ |
| `[linux-abi] fd offset I/O ok` | ✅ |
| `[linux-abi] stat + directory VFS ok` | ✅ |
| `[linux-abi] ftruncate + sparse I/O ok` | ✅ |
| `[linux-abi] auxv + TLS + libc stubs ok` | ✅ |
| `[elf-large] >64 KiB load + execve stack verified` | ✅ |

### Userland & toolchain
| Step | Result |
|---|---|
| Static **musl** program: `printf` + `malloc` + file I/O verified | ✅ |
| `execve` replacement with argv/envp transfer | ✅ |
| On-device **TinyCC** compiles `/hello.c` → runs it (`[tcc] compile + execute cycle complete`) | ✅ |
| TinyCC **self-host**: recompiles its own source into a working TCC2 | ✅ |
| Bare `./tcc.bin printf.c -o printf.bin` (static by default) + run | ✅ Hyper-V |
| K&R program vs full musl `/include` headers: compile + run on-device | ✅ |
| `malloc`/`free`/`realloc`/`calloc` stress (host + on-device) | ✅ |

### Regressions found & fixed during bring-up (notable)
| Symptom | Root cause → fix |
|---|---|
| Multi-sector writes came back all zeros (QEMU) | DMA used a ring-3 user VA (VA≠PA) → bounce all block DMA through an identity-mapped kernel buffer |
| On-device compile took > 1 min | Small synchronous StorVSC reads of `libc.a` → 4 KiB read-ahead block cache + FAT chain-position cache → ~3 s |
| Bare `tcc a.c -o a.bin` output faulted (`[EXC] 14`) | TCC default output was dynamic (PT_INTERP/PLT) → patch TinyCC to link static by default |
| TAB did nothing in `vi` | insert mode dropped control chars → expand TAB to spaces at the next tab stop |
| `sched` demo `[EXC] 255` on Hyper-V only | a pending SynIC interrupt fired at `sti` → shared HW-vector stub EOIs and returns instead of halting |

A healthy headless boot ends at `[login] awaiting credentials (default root/root)`
with **zero** `[EXC]` lines.
