# Cycle 6 Plan — Tier 1 "mini-Unix" that runs busybox-static

> Goal: make TinyOS run a real **`busybox` static binary** — its shell (`ash`)
> and the common applets (`ls`, `cat`, `grep`, `sed`, `ps`, `sleep`, …),
> including **pipelines** and **Ctrl-C**. This is the "is it a usable Unix yet?"
> milestone. It deliberately stops short of dynamic linking, real threads, and
> networking (that is Tier 2+ — see [`ARCHITECTURE.md`](ARCHITECTURE.md) §17 and
> the tiered roadmap).

## Why busybox-static as the target

- **It is one static binary** → no dynamic linker / shared libraries needed, so
  it sidesteps the entire Tier 2 dynamic-linking problem while still being a
  real, unmodified Linux program.
- **It is the canonical minimal userland** → the shell plus ~100 applets at
  once. Getting it to run proves the *process model*, not just a lucky program.
- **It exercises exactly the missing substrate**: `fork`/`execve`/`wait4` (the
  shell runs commands), `pipe`+`dup2` (pipelines), signals (Ctrl-C, `SIGCHLD`),
  and a tty with `termios` (line editing / job control). If busybox's shell
  works, most static CLI tooling works.
- **It is measurable**: each phase below gates on a concrete busybox command.

## Current state (the gap)

TinyOS today runs **one** Linux ELF **synchronously to completion** in a fixed
nesting slot, with a **bump allocator** (never frees), **APs parked**, and
**~30 syscalls**. busybox needs the opposite: **many concurrent processes**,
**reclaimable memory**, and the process/signal/tty syscalls. The syscalls are
the visible tip; the real work is turning "run a program" into "run processes".

Legend: new syscalls listed as `name (nr)` using x86-64 Linux numbers.

---

## Phase A — Process substrate

**Why:** `fork`, concurrent commands, pipelines, signals, and `ps` all require a
real process object instead of the current one-at-a-time model. Nothing else in
this cycle is possible without it.

**Tasks:**
- Define `struct proc`: `pid`, `ppid`, `pgid`/`sid`, `state`
  (`RUNNING`/`RUNNABLE`/`SLEEPING`/`ZOMBIE`), `cr3` (own PML4), saved user
  register context, kernel stack, `fd_table*`, brk/mmap arena, `cwd`, ABI mode,
  `exit_status`, signal handlers + pending/blocked masks, wait channel.
- Process table (fixed array, e.g. 64) + PID allocator; `current` pointer.
- Replace the fixed `level0/level1` frame sets and `g_depth` guard with
  per-process kernel stacks and per-process address spaces.
- Route `syscall_c` through `current` (its fd table, cwd, arena, ABI).

**Acceptance:** the existing boot ELF runs as `proc[1]` with its own struct; all
current `[linux-abi]` self-test markers still pass, no `[EXC]`.

---

## Phase B — Preemptive multi-process scheduling

**Why:** a pipeline runs several children **at the same time**, and a child that
blocks on `read` must not freeze the machine. Requires switching user processes,
not just kernel threads.

**Tasks:**
- Extend `sched64` (LAPIC timer) to schedule **user** processes: on tick, save
  `current`'s user context, pick the next `RUNNABLE`, switch `CR3`, restore, and
  `iretq` to user. Make it always-on (not just the opt-in demo).
- Sleep/wake primitives: `sleep_on(chan)` / `wake(chan)` for blocking syscalls.
- Harden the interrupts-on path already used by the demo (SynIC masked, stray
  vectors EOI-and-return — both already exist).

**Acceptance:** two user processes advance counters concurrently (a user-mode
version of `sched_demo`); a `read` that blocks in one process still lets the
other run.

---

## Phase C — fork / execve / wait4 / exit

**Why:** THE shell primitive. busybox `ash` forks a child, `execve`s the
command, and `wait4`s for it.

**Tasks:**
- `fork (57)`: duplicate `current` — **eager-copy** the user pages first
  (COW is a later optimization), clone the fd table and signal handlers; child
  returns 0, parent returns child pid; child added `RUNNABLE`.
- `execve (59)` (have): adapt to replace the *current process's* address space in
  place, reset signal handlers to default, honor close-on-exec fds.
- `wait4 (61)` / `waitid (247)`: parent sleeps until a child is `ZOMBIE`; reap it,
  return status; deliver `SIGCHLD`.
- `exit`/`exit_group` (have): mark `ZOMBIE`, wake the parent, release resources.
- **Frame reclamation (required here):** add a user-page free list so `exit`,
  `munmap`, and fork-copy churn don't exhaust the bump heap — busybox forks a lot.

**Acceptance:** a static `fork()`+`execve()`+`wait4()` C program prints parent
and child correctly; then `busybox sh -c 'echo hi'` runs and returns.

---

## Phase D — Pipes and fd plumbing

**Why:** pipelines (`ls | grep x`) and shell redirection.

**Tasks:**
- `pipe (22)` / `pipe2 (293)`: a kernel ring-buffer pipe object with read/write
  ends; blocking read (reader sleeps when empty) and write (writer sleeps when
  full) via Phase B's sleep/wake; `SIGPIPE` on write to a closed read end.
- `dup (32)` / `dup2 (33)` / `dup3 (292)`: fd duplication.
- `fcntl (72)`: `F_DUPFD`, `F_GETFD`/`F_SETFD` (close-on-exec), `F_GETFL`/`F_SETFL`
  (`O_NONBLOCK`).
- fd table refactor: reference-counted open-file objects (dup shares offset),
  per-fd close-on-exec flag honored by `execve`.

**Acceptance:** `busybox sh -c 'echo hello | grep hell'` prints `hello`.

---

## Phase E — Signals

**Why:** Ctrl-C (`SIGINT`), child reaping (`SIGCHLD`), `SIGPIPE`, and programs
that install handlers. The shell cannot work without them.

**Tasks:**
- `rt_sigaction (13)`, `rt_sigprocmask (14)` (have, extend), `rt_sigreturn (15)`,
  `kill (62)`, `tgkill (234)`, `sigaltstack (131)`.
- **Delivery:** on return-to-user (from a syscall or the timer), if a pending
  unblocked signal exists, push a signal frame on the user stack, run the
  handler, and `rt_sigreturn` restores the interrupted context. Implement default
  actions (terminate / ignore / stop).
- Wire `SIGCHLD` on child exit and `SIGPIPE` on broken pipe.

**Acceptance:** a program with a `SIGINT` handler catches a delivered signal;
the shell reaps children via `SIGCHLD` without zombies piling up.

---

## Phase F — TTY, termios, and process groups

**Why:** the shell needs line discipline (canonical vs raw), `Ctrl-C`/`Ctrl-Z`,
window size, and a foreground process group; many applets call `isatty`.

**Tasks:**
- A tty device backing fds 0/1/2 with `termios` state via `ioctl (16)`:
  `TCGETS`/`TCSETS` (canonical/raw, echo), `TIOCGWINSZ` (size),
  `TIOCGPGRP`/`TIOCSPGRP` (foreground group).
- Process groups / sessions: `setpgid (109)`, `getpgid (121)`, `setsid (112)`,
  `getsid (124)`.
- Route the existing keyboard into the tty input queue; the line discipline
  turns `Ctrl-C` into `SIGINT` for the foreground process group.

**Acceptance:** interactive `busybox sh` shows a prompt, runs commands, and
`Ctrl-C` interrupts a running command back to the prompt.

---

## Phase G — Syscall long tail + minimal procfs

**Why:** busybox applets touch a broad set of syscalls, and a few read `/proc`.

**Tasks:**
- Fill in: `poll (7)` / `select (23)` / `ppoll (271)`, `nanosleep (35)` /
  `clock_nanosleep (230)`, `getppid (110)`, `getuid/geteuid/getgid/getegid`
  (102/107/104/108, return root), `umask (95)`, `uname (63)`, `sysinfo (99)`,
  `getrusage (98)`, `access (21)` / `faccessat (269)`,
  `readlink (89)` / `readlinkat (267)`, `statx (332)`, `prlimit64 (302)`,
  `sched_getaffinity (204)` (stub), `getrlimit (97)`.
- **futex (202):** a single-threaded static process never hits the futex slow
  path, so a returns-0 / `-ENOSYS`-safe stub is enough for Tier 1 (real futex is
  Tier 2, for threads).
- **Minimal procfs**: `/proc/self/{exe,cmdline,maps,stat}`, `/proc/<pid>/stat`,
  `/proc/mounts`, `/proc/meminfo` — enough for `ps`, `free`, and libc probing.

**Acceptance:** `busybox ps`, `busybox free`, `busybox uname -a`, and
`busybox ls -l /proc` work.

---

## Phase H — Packaging and verification

**Why:** get a genuine busybox-static onto the filesystem and prove applets run.

**Tasks:**
- `make setup-busybox` (mirroring `setup-tcc`): cross-build or fetch a
  **musl-static** `busybox`; stage `/bin/busybox` (+ common applet symlinks) on
  the FAT16 data partition.
- A scripted boot probe (temporary) running a batch of applets + a pipeline for
  headless CI, plus an interactive Hyper-V session for the tty/signal paths.

**Acceptance (cycle done):** interactive `busybox sh` on Hyper-V running `ls`,
`cat`, `grep`, a pipeline, `ps`, `sleep`, and `Ctrl-C` — no `[EXC]`.

---

## Cross-cutting: memory management

Tier 1 forks frequently, so the bump allocator is no longer acceptable:

- Add a **user-page frame allocator with free** (bitmap or free list) used by
  fork-copy, `mmap`, and stacks; freed on `munmap` and process exit.
- Start with **eager page copy** on `fork` (simple, correct); add **copy-on-write**
  (write-protect + `#PF` handler) as an optimization once fork works.
- Keep the kernel heap (`kmalloc`) bump-only for now; only *user* pages need
  reclamation this cycle.

## Order, risk, and dependencies

```
A (proc struct) ─▶ B (preemptive user sched) ─▶ C (fork/exec/wait + frame free)
                                                   │
                              ┌────────────────────┼───────────────┐
                              ▼                    ▼               ▼
                        D (pipes/dup)         E (signals)     F (tty/pgrp)
                              └──────────┬─────────┴───────────────┘
                                         ▼
                              G (syscall tail + procfs) ─▶ H (busybox + verify)
```

- **Highest risk / biggest rearchitecture: Phases A + B.** Moving from
  "run to completion" to "concurrent preemptible processes" touches the syscall
  entry, address-space, and scheduler code at once. Land it behind the existing
  boot self-tests so regressions show immediately over serial.
- **Second risk: signals (E)** — return-to-user signal-frame injection is fiddly
  and interacts with the scheduler and syscall restart semantics.
- Everything else (D, F, G) is comparatively mechanical once A–C + E exist.

## Explicitly out of scope (Tier 2+)

Dynamic linking / glibc, real pthreads + working `futex`, file-backed `mmap` +
demand paging + page cache, networking/sockets, namespaces/cgroups. These are
not needed for busybox-**static** and are deferred.

## Summary table

| Phase | Unlocks | Key new syscalls | Gate |
|---|---|---|---|
| A | process objects | — (refactor) | boot self-tests still pass |
| B | concurrency | — (scheduler) | two user procs run at once |
| C | run commands | fork 57, wait4 61 | `sh -c 'echo hi'` |
| D | pipelines | pipe 22, dup2 33, fcntl 72 | `echo x \| grep x` |
| E | Ctrl-C, reaping | rt_sigaction 13, rt_sigreturn 15, kill 62 | handler catches SIGINT |
| F | interactive shell | ioctl termios, setpgid 109, setsid 112 | interactive `sh` + Ctrl-C |
| G | applet coverage | poll 7, nanosleep 35, uname 63, procfs | `ps`, `free`, `uname -a` |
| H | the payoff | — | interactive busybox on Hyper-V |
