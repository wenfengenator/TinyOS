# Changelog

All notable changes to tinyos are recorded here. The format loosely follows
[Keep a Changelog](https://keepachangelog.com/); versions use semantic
versioning of the **64-bit UEFI edition** (`TINYOS_VERSION`).

## [Unreleased]

### Added
- **MicroPython v1.26.1.** The official minimal Unix port now builds as a
  static, non-PIE musl ELF64 program and is packaged as `/PYTHON.BIN` in the
  live RAM filesystem, persistent data image, VHDX, and USB image.
- **Native Python graphics and audio.** Built-in `tgl` and `taudio` C modules
  expose zero-copy TinyGL surfaces, drawing primitives, shared 8x8 text,
  keyboard polling, Intel HDA tones, master volume, and diagnostics.
- **Python demos and game.** `/hello.py`, `/tgl.py`, `/audio.py`, and a
  graphical, audio-enabled `/reversi.py` are included in every image. Reversi
  supports depth-4 AI, two-player mode, readable TinyGL text, and sound cues.
- **MicroPython integration guide.** `docs/MICROPYTHON.md` documents the build,
  packaging, ELF runtime path, native module boundary, APIs, extension steps,
  limitations, and validation workflow.

### Changed
- The reproducible MicroPython builder compiles repository user C modules via
  `USER_C_MODULES` while leaving the pinned upstream source unmodified.

### Validated
- Static interpreter bootstrap, native-module imports, `mpy-cross` script
  compilation, full GPT/VHDX/USB packaging, and graphical/audio Reversi under
  QEMU + OVMF with emulated Intel HDA.

## [2.0.0] - 2026-08-03

Second major release of the 64-bit UEFI edition. The theme of this cycle was
**fundamentals on real hardware**: running for long stretches on battery
without crashing, a genuinely useful terminal for coding, and reliable file
exchange with the outside world. Bring-up and hardening were done on a
**ThinkPad T440s** (Haswell, Intel HD 4400, 4 cores, 2 batteries), validated in
QEMU + OVMF and Hyper-V Gen 2 along the way.

### Added
- **Multi-pane terminal.** A tmux-style split view hosting up to **four
  concurrent shells** in a 2-column grid (filled left→right, then top→bottom).
  **F5** spawns a new pane, **F4** cycles focus; `exit`/Ctrl-D closes a pane and
  the survivors re-flow. Each pane is an independent scheduled process with its
  **own line discipline** (canonical/raw mode, echo), command history, cooked
  input buffer, and cursor — so editing in one pane never disturbs another. The
  focused pane is marked by a bright border + blinking cursor; unfocused panes
  stay legible (slightly dimmer text).
- **Power management.** An **HLT-based idle** path (`idle_wait`, plus a scoped
  LAPIC idle-tick for the scheduler-stopped native console) drops idle cores
  from a busy 100% to ~0%. The compositor now renders **on change** instead of
  at a fixed 30 FPS. A **CPU %** meter and **LID** state were added to the system
  monitor. **Sleep** (framebuffer blanked, deepest idle) triggers on **F3**,
  **lid close**, or the **power button**, and wakes on lid-open or any keypress.
- **USB mass-storage (xHCI) read/write** mounted at **`/usb`** — the bridge for
  moving files in and out of TinyOS. `cp`, `cat`, and `vi` work against USB
  files and persist.
- **Battery / AC reporting** on the T440s embedded controller (percent, voltage,
  AC-present, charging) surfaced in the system monitor.
- **Graphics-app screen takeover.** A full-screen TinyGL app in the foreground
  now owns the display: the WM compositor **stands down** while it presents, so
  the terminal/shell panes freeze cleanly in the background instead of the
  desktop chrome overdrawing the graphics. Graphics apps read keyboard input
  through the pane router, so they can be driven and quit (`q`) from a pane.
- **`CHANGELOG.md`** (this file).

### Changed
- **Self-contained offline build.** The target toolchain (musl libc + TinyCC)
  is now built from source vendored in `third_party/`, so `make setup-tcc` needs
  only a standard host toolchain (gcc, make, binutils, Python) — no network
  access or distro packages. All three images (QEMU, Hyper-V VHDX, T440s USB)
  then build from a single `make` on a clean machine.
- **Per-process frame reclamation.** A dead process now returns its private page
  frames and page-table pages to the allocator (`free_user_pages`), instead of
  leaking them — required for long-running, fork-heavy sessions.
- **Concurrent-process Linux ABI.** Real `fork(2)`, `execve(2)`, `wait4(2)`,
  `pipe(2)`, and signal delivery are wired through the preemptive scheduler
  (previously programs ran synchronously). Per-task syscall stacks make
  mid-syscall preemption (and idle HLT) safe.
- **VFS-routed program execution.** `exec`, `access`, and tab-completion resolve
  through the VFS, so binaries and completion work uniformly in `/`, `/tmp`,
  `/mnt`, and `/usb` (previously bypassed to the RAM-FAT only).
- Unfocused pane text brightened for readability while keeping a clear focused
  vs. unfocused distinction.

### Fixed
- **Real-hardware page-table teardown (#GP / #PF crashes).** UEFI firmware sets
  the `US` (user) *and* an available software bit on its kernel identity map,
  which aliases the whole heap arena. The teardown walk keyed off those bits and
  **double-freed / freed live kernel pages** on process exit — stable in QEMU,
  fatal on the T440s. Fixed by tagging only our own page-table entries with a
  software-owned bit (`PTE_OWN`, cleared on every table copy) and gating all
  free/fork walks on it, so teardown never touches shared firmware mappings.
- **USB write hang** (a ~8 s stall that left copied files empty). Root cause: the
  FAT allocator re-read the same on-disk FAT sector hundreds of times with no
  cache, blowing the I/O watchdog. Fixed with a one-sector FAT cache plus
  batched, no-zero-fill cluster writes; USB writes now persist reliably.
- **Battery CHG/AC regression** — corrected the T440s EC register/bit (AC and lid
  both live in EC `0x46`).
- **Pane input races** — a shell renumbered by closing a *middle* pane could be
  orphaned on its old input channel; `pane_getkey` now re-reads its pane index
  each wait and `reap_pane` wakes all channels.
- **Pane input lockup after cycling panes (esp. with `vi`).** A shell's
  "check the ring, else block" was not atomic (a `pane_push` landing in the
  window was lost), and per-pane wait channels meant a shell renumbered by a
  reap could block on a channel `pane_push(focus)` never wakes — leaving a pane
  (often a `vi` pane) permanently unable to receive input or be closed. Fixed by
  blocking all readers on one shared channel and making the check-and-block
  atomic (`cli`), so any key wakes the right reader.
- **Use-after-free closing all panes then relaunching.** `reap_pane` freed the
  pane's terminal unconditionally, but pane 0's terminal is owned by the WM
  window — closing every pane freed it, and the next shell relaunch's ELF image
  `kmalloc` reused that memory (surfacing as `?ELF` binary garbage in the
  terminal, then a panic). The WM's pane-0 terminal is now never freed by the
  pane teardown.
- **Hyper-V function keys F4/F5.** The synthetic-keyboard driver (`vmbus64.c`)
  mapped only F1–F3, so pane split/cycle (F4/F5) worked under QEMU and on real
  hardware but not in a Hyper-V VM. It now maps F1–F5 to match the PS/2 driver.
- Native-console (scheduler-stopped) idle no longer pegs a core.

### Known limitations
- The `#GP`/`#PF` teardown fix is firmware-specific and cannot be reproduced
  under OVMF/QEMU; it is validated by construction and on-device testing.
- No USB-HID stack yet — a PS/2 or legacy-USB keyboard is required on real PCs.
- NVMe-only machines still have no persistence path (AHCI/SATA only).
- No true GPU page-flip; the display remains a software double buffer.

## [1.0.0] - Prior release

First tagged release of the 64-bit UEFI edition: UEFI boot + GOP framebuffer,
SMP bring-up, a keyboard-driven desktop (compositor, terminal, system monitor,
sign-in), persistent FAT16 over virtio-blk (QEMU) and StorVSC/VMBus (Hyper-V)
with a GPT data partition, and a Linux-compatible ABI (~30 syscalls, static
ELF64 loader, per-process page tables, VFS, `execve`) running stock **musl**
binaries and a self-hosting on-device **TinyCC**. The original 32-bit i386
teaching kernel remains in the same repository.
