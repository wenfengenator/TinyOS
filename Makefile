# tinyos build
#
#   make               build os.img (boot + kernel + user)   [ARCH=i386, default]
#   make run           run on QEMU (real hardware path; needs qemu-system-i386)
#   make run-emu       run on the bundled Unicorn emulator (works without QEMU)
#   make ARCH=x64        build the x64 UEFI app (build/BOOTX64.EFI)
#   make ARCH=x64 run    boot the x64 build in a QEMU window (OVMF); needs a display
#   make run-x64-headless boot the x64 build headless (serial only; for CI/sandbox)
#   make clean
#
# Toolchain: assembly via GNU as/ld/objcopy; C via tools/xcc (gcc), C++ via
# tools/xcxx (g++).  See tools/xcc for how the compiler is located.

AS       := as
LD       := ld
OBJCOPY  := objcopy
CC       := tools/xcc
CXX      := tools/xcxx

BUILD    := build

CWARN    := -Wall -Wextra
CFLAGS   := -m32 -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
            -fno-asynchronous-unwind-tables -fno-builtin -mno-sse -mno-mmx \
            -O2 $(CWARN) -Wno-array-bounds -Ikernel
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics
ASFLAGS  := --32
LDFLAGS  := -m elf_i386 -T kernel/linker.ld -nostdlib

# object order matters: entry first (.text.boot -> 0x10000), user last (ring 3)
# x64-only kernel sources (kernel/*64.c) belong to the ARCH=x64 path, not here.
KSRC  := $(filter-out $(wildcard kernel/*64.c),$(wildcard kernel/*.c))
USRC  := $(wildcard user/*.cpp)
KOBJS := $(BUILD)/entry.o $(patsubst kernel/%.c,$(BUILD)/k_%.o,$(KSRC))
UOBJS := $(patsubst user/%.cpp,$(BUILD)/u_%.o,$(USRC))

# standalone ramdisk programs (linked at USER_BASE, packed + embedded)
PROG_SRC := $(wildcard programs/*.cpp)
PROGS    := $(patsubst programs/%.cpp,%,$(PROG_SRC))
PROG_BIN := $(patsubst %,$(BUILD)/%.prog,$(PROGS))
RD_ARGS  := $(foreach p,$(PROGS),$(p) $(BUILD)/$(p).prog)

# emulator (Unicorn).  `make deps` installs unicorn into a repo-local .pylibs/
# on first use; if unicorn is already installed system-wide it is used as-is.
PYLIBS ?= .pylibs
PYTHON ?= python3
EMU    := PYTHONPATH=$(PYLIBS) $(PYTHON) tools/emulate.py

# ---- architecture switch: ARCH=i386 (default) or ARCH=x64 (UEFI long mode) ----
ARCH      ?= i386
QEMU64    ?= qemu-system-x86_64
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd
ESP       := $(BUILD)/esp

# x64 UEFI app: PE32+ built with pure binutils (no mingw/gnu-efi/clang).
#   -fPIC + --strip-all -> position-independent, no fragile HIGHLOW base relocs,
#                          and no stray .comment section => OVMF loads it cleanly.
#   ld -mi386pep --subsystem=10 -> EFI application; low image base so 32-bit
#                          address references still resolve at link time.
EFI_CFLAGS  := -m64 -ffreestanding -fshort-wchar -mno-red-zone -fno-stack-protector \
               -fPIC -fvisibility=hidden -fno-ident -fno-builtin -fno-asynchronous-unwind-tables \
               -mno-sse -mno-mmx -O2 $(CWARN) -Ikernel -I$(BUILD)
EFI_LDFLAGS := -mi386pep -e efi_main --subsystem=10 --image-base 0x100000 \
               -nostdlib --strip-all

# x64 kernel objects linked into the single PE32+ EFI application.
EFI_HDRS := kernel/boot64.h kernel/font8x8.h kernel/wm64.h kernel/arch64.h kernel/fat64.h
EFI_OBJS := $(BUILD)/efi_main.o $(BUILD)/entry64.o \
            $(BUILD)/kmain64.o $(BUILD)/fb64.o $(BUILD)/mm64.o \
            $(BUILD)/time64.o $(BUILD)/kbd64.o $(BUILD)/demo64.o $(BUILD)/pong64.o \
            $(BUILD)/smp64.o $(BUILD)/vmbus64.o \
            $(BUILD)/wm64.o $(BUILD)/term64.o $(BUILD)/login64.o \
            $(BUILD)/arch64.o $(BUILD)/trap64.o $(BUILD)/fat64.o

# x64 ring-3 user programs: compiled from programs/*.cpp, embedded as blobs.
# -mcmodel=large + link at USER_CODE_VA => absolute 64-bit addressing that is
# correct when the image is loaded at that same virtual address (RIP-relative
# -fPIC mis-placed .rodata by a byte under -O2).
UPROG_CXXFLAGS := -m64 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                  -fno-stack-protector -fno-asynchronous-unwind-tables -fno-builtin \
                  -mno-red-zone -fno-pic -fno-pie -mcmodel=large -mno-sse -mno-mmx -O2 -Iuser
X64PROGS     := hello count greet sh vi
X64PROG_HDRS := $(patsubst %,$(BUILD)/u%.h,$(X64PROGS))

ifeq ($(ARCH),x64)
ALL_TGT := $(BUILD)/BOOTX64.EFI
RUN_TGT := run-x64
else
ALL_TGT := os.img fs.img
RUN_TGT := run-i386
endif

.PHONY: all run run-i386 run-x64 run-x64-headless run-x64-disk vhdx bootx64 esp run-emu run-emu-kernel deps clean
all: $(ALL_TGT)

$(BUILD):
	mkdir -p $(BUILD)

# ---- assembly ----
$(BUILD)/entry.o: kernel/entry.s | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

# ---- C kernel ----
$(BUILD)/k_%.o: kernel/%.c $(wildcard kernel/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- C++ user layer ----
$(BUILD)/u_%.o: user/%.cpp $(wildcard user/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# ---- standalone ramdisk programs (linked at USER_BASE) ----
$(BUILD)/%.prog: programs/%.cpp programs/crt0.s programs/user.ld user/ulib.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Iuser -c -o $(BUILD)/p_$*.o $<
	$(AS) $(ASFLAGS) -o $(BUILD)/crt0.o programs/crt0.s
	$(LD) -m elf_i386 -T programs/user.ld -o $(BUILD)/$*.pelf $(BUILD)/crt0.o $(BUILD)/p_$*.o
	$(OBJCOPY) -O binary --set-section-flags .bss=alloc,load,contents $(BUILD)/$*.pelf $@

# ---- ramdisk archive embedded into the kernel ----
$(BUILD)/ramdisk.img: $(PROG_BIN) tools/mkramdisk.py | $(BUILD)
	$(PYTHON) tools/mkramdisk.py $@ $(RD_ARGS)

$(BUILD)/ramdisk.o: $(BUILD)/ramdisk.img | $(BUILD)
	printf '.section .note.GNU-stack,"",@progbits\n.section .rodata\n.balign 16\n.global __ramdisk_start\n__ramdisk_start:\n.incbin "%s"\n.global __ramdisk_end\n__ramdisk_end:\n' "$(BUILD)/ramdisk.img" > $(BUILD)/ramdisk.s
	$(AS) $(ASFLAGS) -o $@ $(BUILD)/ramdisk.s

# ---- boot sector (flat 512 bytes) ----
boot.bin: $(BUILD)/boot.o
	$(LD) -m elf_i386 -Ttext 0x7c00 -e _start -o $(BUILD)/boot.elf $<
	$(OBJCOPY) -O binary $(BUILD)/boot.elf $@

# ---- kernel image (flat, loaded at 0x10000) ----
kernel.bin: $(KOBJS) $(UOBJS) $(BUILD)/ramdisk.o kernel/linker.ld
	$(LD) $(LDFLAGS) -o $(BUILD)/kernel.elf $(KOBJS) $(UOBJS) $(BUILD)/ramdisk.o
	$(OBJCOPY) -O binary $(BUILD)/kernel.elf $@
	nm $(BUILD)/kernel.elf > $(BUILD)/kernel.sym
	@sz=$$(wc -c < $@); echo "kernel.bin = $$sz bytes"; \
	 if [ $$sz -gt 65536 ]; then echo "WARNING: kernel > 64KB (boot loads 128 sectors)"; fi

# ---- disk image: boot sector + kernel, padded to 1.44 MB ----
os.img: boot.bin kernel.bin
	cat boot.bin kernel.bin > $@
	truncate -s 1474560 $@
	@echo "os.img ready"

QEMU ?= qemu-system-i386

# build the FAT16 disk image from disk/ (or use your own mkfs.vfat image)
fs.img: tools/mkfatfs.py $(wildcard disk/* disk/*/*)
	$(PYTHON) tools/mkfatfs.py $@ disk

# graphical QEMU (VGA + PS/2 keyboard) — the normal way to use tinyos.
# Attaches fs.img as a second disk so `mount`/`cd /mnt` work.
# `make run` dispatches to the arch-specific target (run-i386 / run-x64).
run: $(RUN_TGT)

run-i386: os.img fs.img
	$(QEMU) -drive format=raw,file=os.img \
	        -drive format=raw,file=fs.img,index=1,media=disk

# headless QEMU: console mirrored to the terminal over the serial port
run-serial: os.img fs.img
	$(QEMU) -display none -serial stdio -net none \
	        -drive format=raw,file=os.img \
	        -drive format=raw,file=fs.img,index=1,media=disk

# ---- x64 / UEFI build (ARCH=x64) ----------------------------------------
# The UEFI loader (boot/efi_main.c) and the 64-bit kernel (kernel/*64.*) are
# compiled freestanding and linked straight to one PE32+ EFI application.
$(BUILD)/efi_main.o: boot/efi_main.c $(EFI_HDRS) | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c -o $@ $<
$(BUILD)/%64.o: kernel/%64.c $(EFI_HDRS) | $(BUILD)
	$(CC) $(EFI_CFLAGS) -c -o $@ $<
$(BUILD)/entry64.o: kernel/entry64.s | $(BUILD)
	$(AS) --64 -o $@ $<

# AP startup trampoline: real->protected->long mode, linked flat at 0x8000 and
# embedded as a byte blob the BSP copies to physical 0x8000 before SIPIs.
$(BUILD)/ap_trampoline.bin: kernel/ap_trampoline.s | $(BUILD)
	$(AS) --64 -o $(BUILD)/ap_tramp_pre.o $<
	$(LD) -m elf_x86_64 -Ttext=0x8000 -e _tramp_start -nostdlib -o $(BUILD)/ap_tramp.elf $(BUILD)/ap_tramp_pre.o
	$(OBJCOPY) -O binary $(BUILD)/ap_tramp.elf $@
# emit the trampoline as a same-TU C array (reliable under ld -mi386pep)
$(BUILD)/ap_tramp_array.h: $(BUILD)/ap_trampoline.bin | $(BUILD)
	$(PYTHON) -c "d=open('$(BUILD)/ap_trampoline.bin','rb').read(); open('$(BUILD)/ap_tramp_array.h','w').write('static const unsigned char ap_tramp[%d]={%s};\n'%(len(d),','.join(str(b) for b in d)))"
$(BUILD)/smp64.o: $(BUILD)/ap_tramp_array.h

# ring-0/ring-3 plumbing (trap64.s)
$(BUILD)/trap64.o: kernel/trap64.s | $(BUILD)
	$(AS) --64 -o $@ $<

# x64 ring-3 user programs: compile a .cpp to a flat PIC image, embed as a C blob
$(BUILD)/u%.bin: programs/%.cpp programs/crt0_64.s programs/user64.ld user/ulib.h | $(BUILD)
	$(CXX) $(UPROG_CXXFLAGS) -c -o $(BUILD)/u$*.o $<
	$(AS) --64 -o $(BUILD)/ucrt0.o programs/crt0_64.s
	$(LD) -m elf_x86_64 -T programs/user64.ld -o $(BUILD)/u$*.elf $(BUILD)/ucrt0.o $(BUILD)/u$*.o
	$(OBJCOPY) -O binary --set-section-flags .bss=alloc,load,contents $(BUILD)/u$*.elf $@
$(BUILD)/u%.h: $(BUILD)/u%.bin | $(BUILD)
	$(PYTHON) -c "d=open('$<','rb').read(); open('$@','w').write('static const unsigned char $*_prog[%d]={%s};\n'%(len(d),','.join(str(b) for b in d)))"
$(BUILD)/arch64.o: $(X64PROG_HDRS)

# x64 filesystem: a small FAT16 image (built from disk/ by tools/mkfatfs.py) is
# embedded as a C blob and mounted as a writable RAM disk by kernel/fat64.c; the
# ring-3 readdir/readfile/writefile syscalls route there.  FATFS_KB sizes it.
FATFS_KB ?= 256
$(BUILD)/fatfs.img: tools/mkfatfs.py $(wildcard disk/* disk/*/*) | $(BUILD)
	$(PYTHON) tools/mkfatfs.py $@ disk $(FATFS_KB)
$(BUILD)/fatfs.h: $(BUILD)/fatfs.img | $(BUILD)
	$(PYTHON) -c "d=open('$(BUILD)/fatfs.img','rb').read(); open('$(BUILD)/fatfs.h','w').write('static const unsigned char fatfs_img[%d]={%s};\n'%(len(d),','.join(str(b) for b in d)))"
$(BUILD)/fat64.o: $(BUILD)/fatfs.h

$(BUILD)/BOOTX64.EFI: $(EFI_OBJS)
	$(LD) $(EFI_LDFLAGS) -o $@ $(EFI_OBJS)
	@file $@

bootx64: $(BUILD)/BOOTX64.EFI

# stage an EFI System Partition tree (removable-media path /EFI/BOOT/BOOTX64.EFI)
esp: $(BUILD)/BOOTX64.EFI
	mkdir -p $(ESP)/EFI/BOOT
	cp $(BUILD)/BOOTX64.EFI $(ESP)/EFI/BOOT/BOOTX64.EFI
	$(PYTHON) -c "open('$(ESP)/TINYWAD.DAT','wb').write(bytes(range(256))*16384)"

# boot the x64 build under qemu-system-x86_64 + OVMF in a graphical window so the
# framebuffer demo is visible; the serial console is mirrored to the terminal.
# Prefers KVM (much faster) and falls back to TCG.  Give the demo keyboard focus
# by clicking the QEMU window: arrows move the box, Q/ESC quits.
run-x64: esp
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU64) -machine q35,accel=kvm:tcg -m 512 \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ESP) \
	  -vga std -serial stdio -no-reboot

# same boot, but headless (no window) — for CI / sandboxes without a display.
run-x64-headless: esp
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU64) -machine q35 -accel tcg -m 512 \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=fat:rw:$(ESP) \
	  -serial stdio -display none -no-reboot

# ---- Hyper-V Gen 2 artifact: a GPT disk with a FAT32 ESP, packaged as VHDX ----
# tools/mkgptesp.py writes the GPT + FAT32 volume by hand (no mkfs.vfat needed).
$(BUILD)/tinyos.img: esp tools/mkgptesp.py
	$(PYTHON) tools/mkgptesp.py $@ $(ESP)

tinyos.vhdx: $(BUILD)/tinyos.img
	qemu-img convert -f raw -O vhdx $< $@
	@echo "tinyos.vhdx ready — attach to a Hyper-V Gen 2 VM (Secure Boot OFF)"

vhdx: tinyos.vhdx

# boot the GPT disk image itself (proxy for Hyper-V) in a QEMU window + OVMF
run-x64-disk: $(BUILD)/tinyos.img
	cp $(OVMF_VARS) $(BUILD)/OVMF_VARS.fd
	$(QEMU64) -machine q35,accel=kvm:tcg -m 512 \
	  -drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE) \
	  -drive if=pflash,format=raw,unit=1,file=$(BUILD)/OVMF_VARS.fd \
	  -drive format=raw,file=$(BUILD)/tinyos.img \
	  -vga std -serial stdio -no-reboot

# ensure the Unicorn CPU emulator is importable; install into .pylibs if not.
deps:
	@PYTHONPATH=$(PYLIBS) $(PYTHON) -c "import unicorn" 2>/dev/null && \
	  echo "unicorn: present" || { \
	  echo "installing unicorn into $(PYLIBS)/ ..."; \
	  env -u PIP_PREFIX $(PYTHON) -m pip install --quiet --target $(PYLIBS) unicorn; }

run-emu: os.img fs.img deps
	$(EMU) os.img

run-emu-kernel: kernel.bin deps
	$(EMU) --kernel kernel.bin

clean:
	rm -rf $(BUILD) boot.bin kernel.bin os.img fs.img tinyos.vhdx
