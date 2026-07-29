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

Status: filesystem syscall substrate complete; new partition awaits Hyper-V validation.

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
- Validate StorVSC mounting and persistence on the new partition in Hyper-V.

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

Port a minimal musl configuration. Add `arch_prctl(ARCH_SET_FS)`, TLS, `set_tid_address`, `rt_sigprocmask`, `clock_gettime`, `writev`, terminal `ioctl`, and required auxv entries including `AT_RANDOM` and `AT_PHDR`.

Acceptance: cross-built static C programs use `printf`, `malloc`, and file I/O unchanged.

## Phase 3: TinyCC

Cross-build TCC as a static ELF, run it on TinyOS, compile a C source file stored on the persistent filesystem, and execute the produced ELF. Then rebuild TCC with TCC on TinyOS.

## Later

Expand POSIX process support and libc, add a standalone assembler/linker only if needed, then bring up a C++ runtime and Clang/LLVM when memory, storage, and build tooling can support them.
