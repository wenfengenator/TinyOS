# tinyos build
#
#   make            build os.img (boot + kernel + user)
#   make run        run on QEMU (real hardware path; needs qemu-system-i386)
#   make run-emu    run on the bundled Unicorn emulator (works without QEMU)
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
KSRC  := $(wildcard kernel/*.c)
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

.PHONY: all run run-emu run-emu-kernel deps clean
all: os.img fs.img

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
run: os.img fs.img
	$(QEMU) -drive format=raw,file=os.img \
	        -drive format=raw,file=fs.img,index=1,media=disk

# headless QEMU: console mirrored to the terminal over the serial port
run-serial: os.img fs.img
	$(QEMU) -display none -serial stdio -net none \
	        -drive format=raw,file=os.img \
	        -drive format=raw,file=fs.img,index=1,media=disk

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
	rm -rf $(BUILD) boot.bin kernel.bin os.img fs.img
