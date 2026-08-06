# tinyos — Test & Integration Plan

## 1. Strategy

tinyos is validated at three levels:

1. **Primitive de-risking** — before writing the OS, verify (with tiny Unicorn
   programs) that the emulator can perform every low-level operation the kernel
   relies on. This turns unknown emulator behaviour into known facts.
2. **Component checks** — after each build, verify the produced binaries
   structurally (size, boot signature, disassembly, exported symbols).
3. **Integration test** — run the whole image on the emulator and assert the
   end-to-end behaviour (boot → login → filesystem → multitasking → preemption
   → clean halt).

Because no QEMU is available, the **Unicorn emulator is the system-under-test**
for execution; the boot sector's real-mode path is exercised in emulator
stage A, and the hardware-only paths (`syscall_stub`, `irq0_stub`) are verified
by construction + disassembly.

> **This plan covers the original 32-bit (i386) kernel.** The 64-bit UEFI
> edition is validated differently — headless boot markers over COM1 on
> `qemu-system-x86_64` + OVMF, embedded boot self-test probes, `qemu-img` VHDX
> validation, and confirmation on a real Hyper-V Gen 2 VM. See
> [`TEST_RESULTS.md`](TEST_RESULTS.md) (Phase 16+) and
> [`ARCHITECTURE.md`](ARCHITECTURE.md) §16.

## 2. Environment

* Build: `make` (uses `tools/xcc`/`xcxx`, GNU `as`/`ld`/`objcopy`).
* Run:   `make run-emu` (Unicorn; auto-installs `unicorn` into `.pylibs/` on first use, or `make deps`).
* Real hardware/QEMU (not available here): `make run`.

## 3. Primitive de-risk tests (Phase 0)

| ID  | What it proves                                                    | Result |
|-----|-------------------------------------------------------------------|--------|
| P1  | PM + GDT + `iret` to ring 3 (CPL=3) + ring-3 memory write + `int 0x80` hook | PASS |
| P2  | Faithful IDT vectoring by redirecting EIP in the INTR hook        | FAIL (expected) → dispatch-and-continue chosen |
| P3  | Dispatch-and-continue: handle `int 0x80` and resume ring 3        | PASS |
| P4  | `lgdt` / `lidt` / `ltr` execute without faulting                  | PASS |
| P5  | Software `int` vectored through a real IDT (no hook)              | FAIL (expected) → confirms shim is required |
| P6  | Port I/O (`in`/`out`) executes without faulting                   | PASS |

## 4. Component checks

| ID  | Target        | Assertion                                                    |
|-----|---------------|-------------------------------------------------------------|
| C1  | `boot.bin`    | exactly 512 bytes; bytes 510–511 = `0x55 0xAA`               |
| C2  | `boot.bin`    | disassembly shows int 0x13 load, A20, `lgdt`, CR0.PE, far jmp |
| C3  | `kernel.bin`  | links; `_start` at 0x10000; ≤ 32 KB (fits 64-sector load)   |
| C4  | symbols       | `g_tasks`, `g_current`, `g_ntask`, `g_files`, `g_fds` exported |
| C5  | build         | C + C++ + ASM compile with no warnings (except benign RWX)  |

## 5. Feature test cases (verified via the emulator run)

| ID  | Feature        | Steps                                                   | Expected                                             |
|-----|----------------|---------------------------------------------------------|------------------------------------------------------|
| T1  | Boot           | `make run-emu`                                          | BIOS line printed; `int13.read` + `CR0.PE` in trace  |
| T2  | Kernel/ring 0  | –                                                       | 5 `[kernel]` init lines (PM, GDT/IDT/TSS, PIC/PIT, fs, sched) |
| T3  | Ring 3 / syscall| login runs unprivileged                                | trace shows syscalls at `cpl 3`                      |
| T4  | stdin `read`   | login prompts                                           | `username: root`, `password: ****`                   |
| T5  | File `open/read`| login reads `/etc/passwd`                              | `sys_open …/etc/passwd → 3`; `sys_read.file → 22`     |
| T6  | Login/auth     | correct creds                                           | `authenticated. welcome, root!`                      |
| T7  | Multitasking   | login `spawn`s workers 1 & 2                            | both workers produce `starting`/`tick`/`exiting`     |
| T8  | Preemption     | workers never call `yield`                              | output interleaves `[worker 1]`/`[worker 2]`; trace shows many `preempt` |
| T9  | File `write`/append | workers append to `/tmp/log`                       | final `/tmp/log` holds **both** `worker 1 done` + `worker 2 done` |
| T10 | Exit/halt      | all tasks call `exit`                                   | `all tasks exited; scheduler idle, halting.`         |

## 6. Advanced-feature test cases (mini-Unix layer)

| ID  | Feature          | Steps                                   | Expected                                                    |
|-----|------------------|-----------------------------------------|-------------------------------------------------------------|
| T11 | Paging           | kernel enables CR0.PG                    | "paging enabled; low 16 MB identity-mapped"; demo keeps running |
| T12 | Keyboard/stdin   | shell reads command lines               | scripted keystrokes appear as typed commands                |
| T13 | Block FS / ls    | `ls`                                    | lists fs files **and** ramdisk programs (hello, shell, ...)  |
| T14 | stat + lseek     | `cat /README`                           | prints the file contents                                    |
| T15 | taskinfo / ps    | `ps`                                    | lists pids/states/names of live tasks                       |
| T16 | exec + isolation | `run hello`                             | hello runs at cr3 ≠ shell's cr3 (`0x408000` vs `0x400000`)   |
| T17 | wait             | `run hello`                             | shell blocks, resumes after hello exits                     |
| T18 | fork             | `fork`                                  | both `[parent] forked child N` and `[child] hello ...` print |
| T19 | preemption       | `race` (two `count` tasks)              | output interleaves `[count pid A]`/`[count pid B]`           |
| T20 | shell exit       | `exit`                                  | `[shell] bye`; scheduler halts cleanly                      |
| T21 | FAT16 mount      | `ls /mnt`                               | lists `readme.txt`, `hello.txt`, `docs/` from `fs.img`       |
| T22 | FAT file read    | `cat /mnt/readme.txt`                    | prints "Hello from a real FAT16 filesystem on a mounted disk image!" |
| T23 | cd + pwd + subdir| `cd /mnt/docs` ; `pwd` ; `cat note.txt` | prompt/`pwd` follow the CWD; prints the subdir note          |
| T24 | vi create/save   | `vi /note.txt`, insert text, `:wq`      | file persists; `cat /note.txt` shows the typed text          |
| T25 | FAT write-back   | `vi /mnt/log.txt`, type, `:wq` (FatImage) | `fs.img` on disk is modified; re-reading returns the text  |

## 6b. Real-hardware (QEMU) test cases

Run on `qemu-system-i386` with the FAT disk attached
(`-drive file=fs.img,index=1,media=disk`). Verified interactively (VGA + PS/2) and
headless (`-serial stdio`).

| ID  | Feature            | Steps                          | Expected                                             |
|-----|--------------------|--------------------------------|------------------------------------------------------|
| Q1  | login + echo       | type `root` / `toor`           | username echoes, password masked with `*`; shell starts |
| Q2  | VGA cursor         | at any prompt                  | hardware cursor sits at the prompt; backspace erases |
| Q3  | preemption on HW   | `race`                         | IRQ0 preempts; two `count` tasks interleave          |
| Q4  | FAT mount on HW    | `cd /mnt` ; `ls` ; `cat readme.txt` | reads the real FAT16 image via the ATA driver   |
| Q5  | vi on HW           | `vi /note.txt`, insert, `:wq`  | redraws via console syscalls; file saved             |
| Q6  | FAT persistence    | `vi /mnt/log.txt` + `:wq`, then **reboot** QEMU | `cat /mnt/log.txt` still shows the text after restart |
| Q7  | Arrow keys + Delete| in `vi`: type `axb`, ESC, `←` `←`, `Delete` | cursor moves; `Delete` removes `x` → file is `ab`   |
| Q8  | Shell line editor  | type `lss`, `←`, `Delete`, Enter | edits to `ls` and runs it (arrows/Delete drive in-line editing) |

## 6c. MicroPython integration test cases

Run the host checks after `make setup-micropython`; run hardware-facing cases
inside the x64 image under QEMU + OVMF with `ich9-intel-hda` and `hda-output`.

| ID | Feature | Steps | Expected |
|---|---|---|---|
| MP1 | Reproducible source | run `make setup-micropython` | v1.26.1 commit matches the pinned hash |
| MP2 | Static ELF | run `file /tmp/tinyos-micropython/micropython` | x86-64, statically linked, no interpreter |
| MP3 | Module registration | import `tgl, taudio` on the host | imports succeed; `tgl.Surface` exists |
| MP4 | Script compatibility | run `mpy-cross programs/reversi.py` | compilation succeeds |
| MP5 | Image packaging | extract `/PYTHON.BIN` and `/reversi.py` from the FAT image | bytes match the build outputs |
| MP6 | Language runtime | run `python /hello.py` in TinyOS | greeting and square list print; process exits cleanly |
| MP7 | Native graphics | run `python /tgl.py` and `python /reversi.py` | surface renders, keys update it, `q` restores desktop |
| MP8 | Native audio | run `python /audio.py`, then make a Reversi move | HDA diagnostics print; tone syscalls return; game redraws |

For MP7, capture the QEMU framebuffer and check that the surface is nonblank,
centered, and has no clipped or overlapping text. Host Linux can validate
imports, but it cannot execute TinyOS's private graphics or audio syscalls.

## 7. Integration scenario (current demo)

```
boot -> kernel (GDT/IDT/TSS, PIC/PIT, paging, fs) -> login (ring 3, keyboard)
   root/toor -> exec("shell")
shell (isolated ring-3 program):
   help | run hello (exec -> isolated space -> wait)
        | fork (parent+child) | race (2 preempted count tasks) | exit
-> scheduler idle -> halt
```

Pass criteria: T1–T24 hold in a single `make run-emu` run and it self-terminates
(no hang, no `UcError`); Q1–Q5 hold on real `qemu-system-i386`.

## 8. Negative / robustness checks

| ID  | Check                                | Expected                          |
|-----|--------------------------------------|-----------------------------------|
| N1  | wrong password (edit `STDIN_SCRIPT`) | `authentication FAILED`, exit 1   |
| N2  | open a missing file                  | `open → -1`                       |
| N3  | console output > 25 lines            | scrolls (no wrap-around garble)   |

## 8. How to reproduce

```bash
make            # build boot.bin, kernel.bin, os.img, and fs.img (FAT16 disk)
make run-emu    # execute on the Unicorn emulator and print the VGA screen

# real hardware / QEMU (FAT disk auto-attached as -hdb):
make run        # graphical: VGA + PS/2 keyboard
make run-serial # headless: console mirrored over COM1 to your terminal
```

The FAT image is rebuilt from `disk/` by `tools/mkfatfs.py` whenever `disk/`
changes; edit files under `disk/` (or point the target at your own
`mkfs.vfat` image) to change what appears under `/mnt`.

Results of the latest run are recorded in `docs/TEST_RESULTS.md`.
