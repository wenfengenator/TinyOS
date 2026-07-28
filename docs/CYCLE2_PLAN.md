# tinyos — Cycle 2 Plan (x64 / UEFI / graphics)

Minimal requirements for the next development cycle. Keep the existing 32-bit
build working exactly as-is; add a **second architecture** that boots as a UEFI
application on **Hyper-V Gen 2**, renders graphics through the GOP framebuffer,
and lands the **enabling infrastructure for Doom** (graphics + heap + input +
timing) plus a graphics demo. The actual Doom port is explicitly deferred.

## 1. Objective

> Boot an x86-64 long-mode tinyos as a UEFI app on Hyper-V Gen 2, draw to a
> linear framebuffer, bring up all CPU cores (SMP), and prove the plumbing Doom
> needs — without regressing the 32-bit BIOS build.

## 2. Scope decisions (locked)

| Question            | Decision                                                          |
|---------------------|-------------------------------------------------------------------|
| Architecture model  | **Dual-arch.** 32-bit BIOS stays default; add `ARCH=x64`.          |
| Hyper-V target      | **Gen 2 (UEFI only).** New PE32+ EFI loader; `boot.asm` untouched. |
| "Play Doom" this cycle | **Enabling infra + graphics demo.** Real Doom port = next cycle. |
| SMP this cycle      | **Bring-up + per-CPU state + locking + concurrency demo.** SMP-aware scheduling = next cycle. |
| Verification        | **qemu-system-x86_64 + OVMF** locally; Hyper-V Gen 2 by the user.  |

## 3. Environment facts (probed, not assumed)

* `qemu-system-x86_64` 8.2.2 **and** OVMF firmware are already installed
  (`/usr/share/OVMF/OVMF_CODE_4M.fd`, `OVMF_VARS_4M.fd`) — local UEFI boot works.
  *(The old "no QEMU" constraint in PROJECT_PLAN.md is out of date.)*
* **No** `mingw`, `clang`, or `gnu-efi`, and **no passwordless sudo** → cannot
  install packages. So the UEFI build must use only what is present:
  `gcc-13 -m64 -ffreestanding` → `ld -mi386pep --subsystem=10` (EFI application)
  → `objcopy -O pei-x86-64`. Both `i386pep` (ld) and `pei-x86-64` (objcopy) are
  confirmed available.
* `qemu-img` can produce a **VHDX** for Hyper-V.

## 4. Minimal requirements

| # | Requirement | Acceptance criteria |
|---|-------------|---------------------|
| **R1** | **Dual-arch build.** `make ARCH=x64` builds the UEFI image; default `make` is unchanged. | Both build from clean; 32-bit `make run-emu` stays green; **no new package installs**. |
| **R2** | **UEFI loader** (`boot/efi_main.c` → `BOOTX64.EFI`): query memory map + GOP, exit boot services, hand off to the kernel. | Loads under qemu+OVMF and reaches kernel entry (serial proof). |
| **R3** | **x64 long-mode kernel entry** (`kernel/entry64.s`, `.code64`): `syscall`/`sysret` ABI, IDT for CPU faults + timer + keyboard, 64-bit context switch. | Reaches C `kmain` in long mode; prints to serial + framebuffer. |
| **R4** | **GOP framebuffer console**: pixel plot, bitmap-font text, clear/scroll (replaces the `0xB8000` path on x64). | Renders a test pattern + text on qemu+OVMF and on Hyper-V Gen 2. |
| **R5** | **Doom enabling infra**: heap/`malloc` (multi-MB), millisecond timer, keyboard input, and large-file read (WAD-sized) from ramdisk/ESP — driven by a **graphics demo**. | Demo animates the framebuffer, responds to keys, keeps time; the doomgeneric I/O shim surface is documented for next cycle. |
| **R6** | **Hyper-V Gen 2 artifact**: FAT32 ESP with `/EFI/BOOT/BOOTX64.EFI`, packaged as **VHDX**. | User boots the VHDX on Hyper-V Gen 2 (Secure Boot off) to the demo. |
| **R7** | **Local run target**: `make ARCH=x64 run` launches qemu+OVMF on the ESP. | One command boots the x64 image graphically in the sandbox. |
| **R8** | **Multi-core SMP**: enumerate CPUs and start every AP into long mode (UEFI **MP Services** before `ExitBootServices`, else ACPI MADT + INIT-SIPI-SIPI + trampoline). Per-CPU GDT/TSS/stack + LAPIC id; **spinlocks** around shared state (`g_tasks`/`g_current`, fd table, framebuffer console, heap). Concurrency demo. | On `qemu -smp N` and Hyper-V (N vCPUs): every core reaches long mode and prints its APIC id; the demo shows all cores doing lock-coordinated work with no corruption. |

## 5. Non-goals (kept out to stay minimal)

* Full Doom gameplay / shipping a real WAD as playable (next cycle).
* Secure Boot signing.
* x64 support in the Unicorn emulator — it stays **32-bit text-only**.
* **SMP-aware scheduling**: dispatching the task table across cores, load
  balancing / per-CPU run queues, and TLB shootdown (APs park idle this cycle).
* Hyper-V Gen 1 / legacy-BIOS x64; networking; sound.

## 6. Verification matrix

| Path            | How verified                                                    |
|-----------------|-----------------------------------------------------------------|
| 32-bit (i386)   | Existing Unicorn emulator suite — unchanged (regression gate).  |
| x64 build       | `make ARCH=x64` links a valid PE32+ (`objcopy`/`file` checks).  |
| x64 boot+gfx    | `make ARCH=x64 run` → qemu-system-x86_64 + OVMF (framebuffer).  |
| SMP bring-up    | `make ARCH=x64 run` with `-smp N`; each core prints its APIC id. |
| Hyper-V Gen 2   | User boots the VHDX; reports framebuffer + input working.       |

## 7. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| No mingw/gnu-efi and no sudo | Use the confirmed `ld -mi386pep` + `objcopy pei-x86-64` route; zero installs. |
| Emulator can't exercise x64/graphics/IRQ | qemu+OVMF is the x64 system-under-test; Hyper-V is the final gate. |
| GOP mode differs (qemu vs Hyper-V) | Query available modes at runtime; never hardcode resolution/stride. |
| WAD (~4 MB) vs memory | Size the heap and file loader for ≥ shareware `DOOM1.WAD` up front. |
| Scope creep from "Doom" | R5 ships infra + demo only; the port is a separate, later cycle. |
| Data races once APs run kernel code | Add spinlocks around every shared `g_*` structure, the heap, and the framebuffer console **before** enabling APs; keep AP work minimal this cycle. |
| AP bring-up complexity (real-mode trampoline) | Prefer UEFI **MP Services** to start APs pre-`ExitBootServices`; fall back to ACPI MADT + INIT-SIPI-SIPI only if needed. |

## 8. Suggested build order

1. R1 dual-arch Makefile skeleton (`ARCH` switch, x64 flags, PE32+ link rule).
2. R2 minimal `BOOTX64.EFI` that prints via UEFI + exits boot services.
3. R3 long-mode entry → C `kmain` (serial first).
4. R4 GOP framebuffer console.
5. R5 heap + timer + keyboard + large-file read + graphics demo.
6. R7 `make ARCH=x64 run` (qemu+OVMF), then R6 VHDX for Hyper-V.
7. R8 SMP: add spinlocks to shared state first, then AP bring-up + concurrency
   demo (verify with `-smp N`). APs park idle — no cross-core scheduling yet.
