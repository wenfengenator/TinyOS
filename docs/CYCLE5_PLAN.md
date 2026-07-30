# Cycle 5: Self-hosting compiler substrate

## Decisions

- Kernel remains freestanding C; userland is C/C++.
- CPU/ABI: x86-64 System V AMD64.
- Tool syscall ABI: Linux x86-64 (`rax` number; `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` arguments).
- Existing TinyOS desktop syscalls remain available to legacy flat programs through a per-process ABI mode.
- Executables: ELF64 `EM_X86_64`, static `ET_EXEC`, non-PIE first.
- First on-device compiler: TinyCC (TCC). Clang follows only after libc/C++ and larger VM support exist.

## Phase 1: Runtime substrate

### 1a. Linux ABI and ELF loader

Status: first boot checkpoint complete.

- Six-argument Linux syscall entry bridge.
- ABI-gated Linux `write`, `exit`, and `exit_group` without changing existing flat programs.
- Bounded ELF64 loader for `PT_LOAD` segments in the current high user window.
- Initial System V stack with `argc`, `argv`, `envp`, and minimal `auxv`.
- Embedded static ELF boot probe validates the stack, writes to fd 1, and exits through `exit_group`.

Acceptance marker:

```
[linux-abi] hello from static ELF64
[linux-abi] ELF64 loader + SysV stack ok
```

### 1b. Per-process virtual memory

Status: first executable checkpoint complete.

- The Linux ELF runs under a private PML4 while retaining required kernel mappings.
- Static `ET_EXEC` images load at the standard `0x400000` virtual address.
- The process has an isolated high user stack with a System V initial stack.
- Linux-compatible anonymous `brk`, `mmap`, `mprotect`, and `munmap` cover reserved user arenas.
- ELF `PF_W` controls page writability; VM operations cannot modify inherited kernel mappings.
- The boot probe grows `brk` by 128 KiB, maps and touches another 128 KiB through six-argument `mmap`, changes protection, unmaps it, and exits cleanly.

Remaining production work:

- Generalize address spaces from the boot probe into process-owned objects and lifecycle APIs.
- Replace the bump-only physical allocator with frame reclamation for unmap and process exit.
- Add NX enforcement, a growable/guarded stack, overlap-aware VM areas, and fixed-address mappings.
- Move legacy flat programs away from the shared fixed window after their loader transition.

Acceptance: run a static ELF at `0x400000` that allocates beyond the old fixed window and returns cleanly.

### 1c. Descriptor-based VFS

Status: filesystem syscall substrate complete and verified on QEMU and Hyper-V.

- Per-process fd table reserves tty fds 0, 1, and 2 and allocates regular files from fd 3.
- Linux `openat`, `close`, `read`, `write`, and `lseek` operate on persistent FAT16 files.
- `openat` supports create, truncate, append, and read/write access modes.
- FAT16 supports offset reads, sector read-modify-write, cluster-chain growth, and directory size updates.
- The boot probe creates `/abiio.bin`, writes 1024 patterned bytes, seeks to byte 600, reads 32 bytes, and verifies the boundary values.

Acceptance marker:

```
[linux-abi] fd offset I/O ok
```

Remaining VFS work:

- Tty reads and `/dev/null`: complete.
- Directory descriptors and Linux `getdents64`: complete.
- `fstat`, `newfstatat`, `unlinkat`, `mkdirat`, and `getcwd`: complete.
- Linux `ftruncate`, FAT chain shrink/growth, and zero-filled sparse gaps: complete.
- The Hyper-V image now contains a dedicated 16 MiB FAT16 GPT data partition at LBA 133120; GPT/ESP boot and both GPT CRC copies are verified in QEMU.
- StorVSC mounting and cross-reboot persistence on the new partition are verified in Hyper-V.

Metadata/directory acceptance marker:

```
[linux-abi] stat + directory VFS ok
```

The probe validates the x86-64 Linux `stat` layout, enumerates the FAT root with
`getdents64`, creates a directory, creates and unlinks a temporary file, checks
`getcwd`, and verifies `/dev/null` read/write behavior.

File-size acceptance marker:

```
[linux-abi] ftruncate + sparse I/O ok
```

The probe shrinks and extends a file, verifies zero-filled extension bytes,
writes beyond EOF, and verifies the sparse gap remains zero.

Acceptance: a static ELF performs partial file I/O and seek across multiple clusters, then verifies persistence after reboot.

## Phase 2: libc

Status: static musl checkpoint complete on QEMU and Hyper-V.

- Added `arch_prctl(ARCH_SET_FS)`, TLS, `set_tid_address`, `rt_sigprocmask`, `clock_gettime`, `writev`, terminal `ioctl`, and required auxv entries including `AT_RANDOM` and `AT_PHDR`.
- A real static musl program validates `printf`, `malloc`, and persistent file I/O.
- Static ELF files load directly from FAT16; the ring-3 shell runs them by pathname, for example `./musl.bin`.
- Linux ELF `PT_LOAD` backing is allocated from the image's actual load span rather than the legacy 64 KiB program window. The current isolated low-address page-table window supports static images up to 2 MiB.
- Linux `execve(59)` replaces the active image from a filesystem ELF. It transfers bounded caller `argv` and `envp` vectors to a canonical System V initial stack, resets process ABI state, and never returns on success.
- A replacement ELF validates exact `argc`, `argv`, `envp`, and null terminators before exiting; the original TinyOS parent then resumes cleanly.
- The replacement probe has a 70,487-byte loaded span with validated strings beyond 64 KiB, proving demand-sized copying and mapping through the former boundary.

Acceptance: cross-built static C programs use `printf`, `malloc`, and file I/O unchanged.

## Phase 3: TinyCC

Status: TCC self-host checkpoint complete on QEMU; Hyper-V validation pending.

- Cross-built TinyCC 0.9.28rc as a 322,712-byte static musl `ET_EXEC` with no interpreter. Its `0x6dd40` load span fits the demand-sized 2 MiB Linux ELF window.
- TinyOS launches TCC from FAT16 through a static musl launcher with a real `argv`/`envp` vector.
- TCC compiles a freestanding `/hello.c` into `/made.bin` on-device using `-nostdlib -static` and a fixed `0x400000` link address.
- TinyOS immediately loads and executes `/made.bin`, then returns to its TinyOS parent and reaches login without exceptions.
- A second boot recompiles over the existing output successfully. The generated file is an 839-byte static x86-64 `ET_EXEC`, and the reconstructed FAT bytes execute successfully on Linux too.
- TCC1 rebuilds TinyCC from a 914,052-byte preprocessed translation unit and a response file containing the staged musl CRT objects and static archives. The preprocessed source avoids FAT16's current shallow-directory and 8.3-name limits.
- The rebuilt 509,833-byte `/tcc2.bin` is a static x86-64 ELF. TinyOS launches it to compile `/hello.c` into `/made2.bin`, then executes that second-generation output.
- FAT16 caches the active FAT sector and batches mirrored FAT updates until directory metadata commits. This avoids synchronous block reads for cluster traversal and removes several StorVSC transactions per output cluster during the TCC self-rebuild.
- Both generated files were reconstructed from the post-boot FAT image. TCC2 reports `tcc version 0.9.28rc (x86_64 Linux)`, and the 839-byte MADE2 ELF runs successfully on Linux.
- `make setup-tcc` now recreates the external musl/TinyCC inputs under `/tmp` from pinned TinyCC revision `380597704ee9784442ef3c7ef06e105258a11c5d` and musl 1.2.4 packages. A clean-room run reproduced the self-host payload, rebuilt static TCC2, and compiled and ran the freestanding probe.
- The Hyper-V image includes a manual normal-C probe: `/printf.c` uses `main()` and `#include <stdio.h>`, `/printrun.bin` invokes TCC with `/printf.rsp`, and the response file links musl CRT objects, `libc.a`, and `libtcc1.a` into static `/printf.bin`. This probe is not run during boot.

Acceptance markers:

```
[tcc] compiling /hello.c -> /made.bin
[tcc] on-device compiled ELF executed
[tcc] compile + execute cycle complete
[tcc-self] rebuilding TCC -> /tcc2.bin
[tcc-self] rebuilt compiler returned cleanly
[tcc-self] rebuilt TCC compiling /hello.c
[tcc-self] self-host compile + execute complete
```

Next: verify the self-host payload on Hyper-V, then move from a preprocessed translation unit to a source tree as FAT/path support grows.

## Later

Expand POSIX process support and libc, add a standalone assembler/linker only if needed, then bring up a C++ runtime and Clang/LLVM when memory, storage, and build tooling can support them.
