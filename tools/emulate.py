#!/usr/bin/env python3
"""
tinyos emulator (Unicorn-based) — runs the OS when QEMU is unavailable.

Because Unicorn cannot vector software interrupts through the IDT or deliver
hardware IRQs, this emulator plays the role of the low-level kernel:

  Stage A (16-bit real mode): runs the real boot sector, services BIOS int 0x10
      (teletype) and int 0x13 (disk read), and stops when CR0.PE is set.
  Stage B (32-bit protected mode): runs the C kernel for real (console,
      GDT/IDT/TSS, iret to ring 3), then:
        * services `int 0x80` syscalls against the kernel's OWN memory
          (g_files/g_fds for the filesystem, scripted stdin for SYS_READ);
        * drives the preemptive round-robin scheduler by save/restoring the
          g_tasks[] contexts every time quantum (and on SYS_YIELD / SYS_EXIT).
      The struct layouts below must match kernel/kernel.h.

Usage:
    emulate.py <disk image>          # full two-stage boot
    emulate.py --kernel <kernel.bin> # load kernel at 0x10000, skip bootloader
"""
import sys, struct, os

try:
    from unicorn import *
    from unicorn.x86_const import *
except ImportError:
    sys.exit("unicorn not importable; set PYTHONPATH to the unicorn install dir")

# ---- machine ----
RAM, GDT_BASE, KERNEL = 0x1000000, 0x1000, 0x10000
VGA, CURSOR, KSTACK   = 0xB8000, 0x600, 0x90000
FLOPPY_SPT, FLOPPY_HEADS = 18, 2
QUANTUM = 200000                     # instructions per scheduling slice

# ---- syscalls / states (match kernel.h) ----
SYS_EXIT, SYS_READ, SYS_WRITE, SYS_OPEN, SYS_CLOSE, SYS_YIELD, SYS_GETPID = 1,3,4,5,6,7,8
SYS_EXEC, SYS_SPAWN, SYS_STAT, SYS_LSEEK, SYS_FORK, SYS_WAIT, SYS_READDIR, SYS_TASKINFO, SYS_ECHO = 9,10,11,12,13,14,15,16,17
SYS_READFILE, SYS_GETCHAR, SYS_CLEAR, SYS_GOTOXY, SYS_WRITEFILE = 18,19,20,21,22
T_UNUSED, T_READY, T_RUNNING, T_BLOCKED, T_EXITED = 0,1,2,3,4

# ---- paging / memory layout (match mm.h) ----
PAGE_SIZE, FRAME_BASE, PHYS_TOP = 0x1000, 0x400000, 0x1000000
USER_BASE, USER_STACK_TOP, ARG_ADDR = 0x40000000, 0xB0000000, 0x50000000
PAGE_USER_RW = 0x6              # PAGE_USER | PAGE_RW

# ---- struct offsets (match kernel.h) ----
TASK_STRIDE = 88
ST_STATE, ST_PID, ST_PPID, ST_USTACK, ST_CR3, ST_KSTACK, ST_EXIT, ST_WAITON, ST_NAME = \
    40, 44, 48, 52, 56, 60, 64, 68, 72
FILE_STRIDE, F_NAME, F_DATA, F_SIZE, F_USED = 288, 0, 24, 280, 284
FD_STRIDE, FD_FILE, FD_OFF, FD_USED = 12, 0, 4, 8
NFILE, NFD, NTASK = 8, 16, 8
FSIZE = 256

REGMAP = [(UC_X86_REG_EAX,0),(UC_X86_REG_EBX,4),(UC_X86_REG_ECX,8),(UC_X86_REG_EDX,12),
          (UC_X86_REG_ESI,16),(UC_X86_REG_EDI,20),(UC_X86_REG_EBP,24),(UC_X86_REG_ESP,28),
          (UC_X86_REG_EIP,32),(UC_X86_REG_EFLAGS,36)]

ATTR_BY_PID = {0: 0x0E, 1: 0x0A, 2: 0x0B}   # login=yellow, workerA=green, workerB=cyan
KATTR = 0x07
STDIN_SCRIPT = b"root\ntoor\nls\ncd /mnt\nls\ncat readme.txt\ncd docs\ncat note.txt\ncd /\nrun hello\nrace\nexit\n"


def gdt_entry(base, limit, access, flags):
    d  = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | ((access & 0xFF) << 40)
    d |= (((limit >> 16) & 0xF) << 48) | ((flags & 0xF) << 52) | (((base >> 24) & 0xFF) << 56)
    return d.to_bytes(8, "little")


def load_syms(path):
    syms = {}
    if path and os.path.exists(path):
        for line in open(path):
            parts = line.split()
            if len(parts) == 3:
                try: syms[parts[2]] = int(parts[0], 16)
                except ValueError: pass
    return syms


# ===================== Stage A: real-mode boot =====================
def run_stage_a(img, trace):
    uc = Uc(UC_ARCH_X86, UC_MODE_16)
    uc.mem_map(0, RAM)
    uc.mem_write(0x7c00, img[:512])
    uc.reg_write(UC_X86_REG_CS, 0); uc.reg_write(UC_X86_REG_IP, 0x7c00)
    for r in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS): uc.reg_write(r, 0)
    uc.reg_write(UC_X86_REG_SP, 0x7c00); uc.reg_write(UC_X86_REG_DX, 0)
    out = bytearray()

    def on_intr(uc, intno, _):
        ah = uc.reg_read(UC_X86_REG_AH)
        if intno == 0x10 and ah == 0x0E:
            out.append(uc.reg_read(UC_X86_REG_AL) & 0xFF)
        elif intno == 0x13 and ah == 0x02:
            cnt = uc.reg_read(UC_X86_REG_AL); cyl = uc.reg_read(UC_X86_REG_CH)
            sec = uc.reg_read(UC_X86_REG_CL); hd  = uc.reg_read(UC_X86_REG_DH)
            es  = uc.reg_read(UC_X86_REG_ES); bx  = uc.reg_read(UC_X86_REG_BX)
            lba = (cyl * FLOPPY_HEADS + hd) * FLOPPY_SPT + (sec - 1)
            src = img[lba*512:(lba+cnt)*512]
            if len(src) < cnt*512: src += b"\x00"*(cnt*512-len(src))
            uc.mem_write((es << 4) + bx, src)
            uc.reg_write(UC_X86_REG_AH, 0)
            uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)
            trace.append(("int13.read", "lba", lba, "count", cnt))

    def on_code(uc, addr, size, _):
        if uc.reg_read(UC_X86_REG_CR0) & 1:
            trace.append(("boot", "CR0.PE set", hex(addr), "-> stage B"))
            uc.emu_stop()

    uc.hook_add(UC_HOOK_INTR, on_intr)
    uc.hook_add(UC_HOOK_CODE, on_code)
    try: uc.emu_start(0x7c00, 0, count=200000)
    except UcError as e: trace.append(("stageA.err", str(e)))
    return bytearray(uc.mem_read(0, RAM)), uc.reg_read(UC_X86_REG_GDTR), bytes(out)


# ===================== FAT16 image reader (for /mnt on the emulator) =====
class FatImage:
    def __init__(self, d, path=None):
        self.path = path
        self.d = bytearray(d)
        self.spc = d[13]
        self.reserved = d[14] | (d[15] << 8); self.nfats = d[16]
        self.root_entries = d[17] | (d[18] << 8); self.fatsz = d[22] | (d[23] << 8)
        self.total = d[19] | (d[20] << 8)
        self.fat_start = self.reserved
        self.root_start = self.reserved + self.nfats * self.fatsz
        self.root_sectors = (self.root_entries * 32 + 511) // 512
        self.data_start = self.root_start + self.root_sectors
    def sec(self, n): return self.d[n*512:(n+1)*512]
    def fat_next(self, c):
        off = c*2; s = self.sec(self.fat_start + off//512); return s[off%512] | (s[off%512+1] << 8)
    def _parse(self, e):
        base = e[0:8].rstrip(b" ").decode("latin-1").lower()
        ext = e[8:11].rstrip(b" ").decode("latin-1").lower()
        return ((base + "." + ext if ext else base), e[11],
                e[26]|(e[27]<<8), e[28]|(e[29]<<8)|(e[30]<<16)|(e[31]<<24))
    def entries(self, cluster):
        out = []
        if cluster == 0:
            for i in range(self.root_entries):
                e = self.sec(self.root_start + i//16)[(i%16)*32:(i%16)*32+32]
                if e[0] == 0: break
                if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08): continue
                out.append(self._parse(e))
        else:
            c = cluster
            while 2 <= c < 0xFFF8:
                for s in range(self.spc):
                    sec = self.sec(self.data_start + (c-2)*self.spc + s)
                    for k in range(16):
                        e = sec[k*32:k*32+32]
                        if e[0] == 0: return out
                        if e[0] == 0xE5 or e[11] == 0x0F or (e[11] & 0x08): continue
                        out.append(self._parse(e))
                c = self.fat_next(c)
        return out
    def walk(self, sub):
        comps = [c for c in sub.split("/") if c]
        cur = 0
        for i, comp in enumerate(comps):
            hit = None
            for (nm, at, cl, s) in self.entries(cur):
                if nm.lower() == comp.lower(): hit = (at, cl, s); break
            if not hit: return None
            at, cl, s = hit
            if i < len(comps) - 1:
                if not (at & 0x10): return None
                cur = cl
            else:
                return (cl, s, 1 if (at & 0x10) else 0)
        return (0, 0, 1)
    def readdir(self, sub, index):
        w = self.walk(sub)
        if w is None or not w[2]: return None
        ents = self.entries(w[0])
        if not (0 <= index < len(ents)): return None
        nm, at, cl, s = ents[index]
        return nm + ("/" if (at & 0x10) else "")
    def readfile(self, sub, maxlen):
        w = self.walk(sub)
        if w is None or w[2]: return None
        cl, size = w[0], w[1]; out = b""
        while 2 <= cl < 0xFFF8 and len(out) < size and len(out) < maxlen:
            for s in range(self.spc): out += self.sec(self.data_start + (cl-2)*self.spc + s)
            cl = self.fat_next(cl)
        return out[:min(size, maxlen)]

    # ---- write-back (mirrors kernel/fat.c fat_write); persists to the image ----
    def _flush(self):
        if self.path:
            try:
                with open(self.path, "r+b") as f: f.seek(0); f.write(self.d)
            except Exception: pass
    def _cluster_count(self):
        if self.total < self.data_start: return 2
        return (self.total - self.data_start)//self.spc + 2
    def _fat_set(self, c, val):
        off = c*2
        for fi in range(self.nfats):
            base = (self.fat_start + fi*self.fatsz)*512 + off
            self.d[base] = val & 0xFF; self.d[base+1] = (val >> 8) & 0xFF
    def _fat_alloc(self):
        for c in range(2, self._cluster_count()):
            if self.fat_next(c) == 0: self._fat_set(c, 0xFFFF); return c
        return 0
    def _free_chain(self, c):
        while 2 <= c < 0xFFF8:
            nx = self.fat_next(c); self._fat_set(c, 0); c = nx
    def _dir_raw(self, dirc, i):
        eps = 16
        if dirc == 0:
            if i >= self.root_entries: return None
            return self.sec(self.root_start + i//eps)[(i%eps)*32:(i%eps)*32+32]
        epc = self.spc*eps; c = dirc
        for _ in range(i//epc):
            c = self.fat_next(c)
            if c < 2 or c >= 0xFFF8: return None
        within = i % epc
        return self.sec(self.data_start + (c-2)*self.spc + within//eps)[(within%eps)*32:(within%eps)*32+32]
    def _dirent_loc(self, dirc, i):
        eps = 16
        if dirc == 0:
            if i >= self.root_entries: return None
            return (self.root_start + i//eps, (i%eps)*32)
        epc = self.spc*eps; c = dirc
        for _ in range(i//epc):
            c = self.fat_next(c)
            if c < 2 or c >= 0xFFF8: return None
        within = i % epc
        return (self.data_start + (c-2)*self.spc + within//eps, (within%eps)*32)
    @staticmethod
    def _to83(name):
        raw = bytearray(b" "*11); i = k = 0
        while i < len(name) and name[i] != "." and k < 8: raw[k] = ord(name[i].upper()); k += 1; i += 1
        while i < len(name) and name[i] != ".": i += 1
        if i < len(name) and name[i] == ".":
            i += 1; e = 8
            while i < len(name) and e < 11: raw[e] = ord(name[i].upper()); e += 1; i += 1
        return bytes(raw)
    def _parent(self, sub):
        comps = [c for c in sub.split("/") if c]
        if not comps: return None
        cur = 0
        for comp in comps[:-1]:
            hit = None
            for (nm, at, cl, s) in self.entries(cur):
                if nm.lower() == comp.lower(): hit = (at, cl); break
            if not hit or not (hit[0] & 0x10): return None
            cur = hit[1]
        return (cur, comps[-1])
    def write(self, sub, data):
        p = self._parent(sub)
        if p is None: return None
        dirc, fname = p; raw = self._to83(fname)
        slot = freeslot = None; oldcluster = 0; i = 0
        while True:
            e = self._dir_raw(dirc, i)
            if e is None: break
            if e[0] == 0:
                if freeslot is None: freeslot = i
                break
            if e[0] == 0xE5:
                if freeslot is None: freeslot = i
                i += 1; continue
            if e[11] == 0x0F or (e[11] & 0x08): i += 1; continue
            if bytes(e[0:11]) == raw: slot = i; oldcluster = e[26] | (e[27] << 8); break
            i += 1
        if slot is None: slot = freeslot
        if slot is None: return None
        if oldcluster >= 2: self._free_chain(oldcluster)
        bpc = self.spc*512; nclus = (len(data) + bpc - 1)//bpc
        first = prev = 0
        for ci in range(nclus):
            c = self._fat_alloc()
            if not c:
                if first: self._free_chain(first)
                return None
            if prev: self._fat_set(prev, c)
            else: first = c
            for s in range(self.spc):
                buf = bytearray(512); base = ci*bpc + s*512
                if base < len(data): chunk = data[base:base+512]; buf[:len(chunk)] = chunk
                sec = self.data_start + (c-2)*self.spc + s
                self.d[sec*512:(sec+1)*512] = buf
            prev = c
        if prev: self._fat_set(prev, 0xFFFF)
        loc = self._dirent_loc(dirc, slot)
        if loc is None: return None
        lba, off = loc; base = lba*512 + off; L = len(data)
        ent = bytearray(32); ent[0:11] = raw; ent[11] = 0x20
        ent[26] = first & 0xFF; ent[27] = (first >> 8) & 0xFF
        ent[28] = L & 0xFF; ent[29] = (L >> 8) & 0xFF; ent[30] = (L >> 16) & 0xFF; ent[31] = (L >> 24) & 0xFF
        self.d[base:base+32] = ent
        self._flush()
        return L


# ===================== Stage B: kernel + scheduler =====================
class Machine:
    def __init__(self, syms, ram=None, gdtr=None, kernel_bin=None, fat=None):
        self.syms = syms
        self.fat = fat
        self.trace = []
        self.cur = 0
        self.started = False
        self.done = False
        self.stdin_pos = 0
        self.pending = None
        self.next_pid = 10
        self.echo = 0
        uc = Uc(UC_ARCH_X86, UC_MODE_32); uc.mem_map(0, RAM); self.uc = uc
        if ram is not None:
            uc.mem_write(0, bytes(ram)); _, base, limit, _ = gdtr
            uc.reg_write(UC_X86_REG_GDTR, (0, base, limit, 0))
        else:
            gdt = b"".join([gdt_entry(0,0,0,0), gdt_entry(0,0xFFFFF,0x9A,0xC),
                            gdt_entry(0,0xFFFFF,0x92,0xC), gdt_entry(0,0xFFFFF,0xFA,0xC),
                            gdt_entry(0,0xFFFFF,0xF2,0xC)])
            uc.mem_write(GDT_BASE, gdt)
            uc.reg_write(UC_X86_REG_GDTR, (0, GDT_BASE, len(gdt)-1, 0))
            uc.mem_write(KERNEL, open(kernel_bin, "rb").read())
        uc.reg_write(UC_X86_REG_CS, 0x08)
        for r in (UC_X86_REG_DS,UC_X86_REG_ES,UC_X86_REG_FS,UC_X86_REG_GS,UC_X86_REG_SS):
            uc.reg_write(r, 0x10)
        uc.reg_write(UC_X86_REG_ESP, KSTACK); uc.reg_write(UC_X86_REG_EIP, KERNEL)
        uc.hook_add(UC_HOOK_INTR, self._on_intr)
        uc.hook_add(UC_HOOK_INSN, self._on_in, None, 1, 0, UC_X86_INS_IN)

    # answer port reads: COM1 LSR = transmit-ready; ATA status = error (no real
    # disk under the emulator — /mnt is served by the Python FatImage instead).
    def _on_in(self, uc, port, size, user_data=None):
        if port == 0x3FD: return 0x60
        if port == 0x1F7: return 0x01
        return 0

    # ---- memory helpers ----
    def rd32(self, a): return struct.unpack("<I", self.uc.mem_read(a, 4))[0]
    def wr32(self, a, v): self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))
    def rd(self, a, n): return bytes(self.uc.mem_read(a, n)) if n else b""
    def wr(self, a, b): 
        if b: self.uc.mem_write(a, bytes(b))
    def cstr(self, a, maxn=64):
        out = bytearray()
        for i in range(maxn):
            c = self.uc.mem_read(a + i, 1)[0]
            if c == 0: break
            out.append(c)
        return bytes(out)

    def sym(self, name): return self.syms.get(name)

    # ---- console (shared cursor at 0x600) ----
    def _scroll(self):
        data = self.uc.mem_read(VGA + 80*2, 1920*2)
        self.uc.mem_write(VGA, bytes(data))
        self.uc.mem_write(VGA + 1920*2, (bytes([0x20, KATTR]) * 80))
    def _putc(self, ch, attr):
        idx = self.rd32(CURSOR)
        if ch == 0x0A: idx = (idx // 80 + 1) * 80
        else: self.uc.mem_write(VGA + idx*2, bytes([ch, attr])); idx += 1
        if idx >= 2000: self._scroll(); idx = 1920
        self.wr32(CURSOR, idx)
    def _write(self, data, attr):
        for b in data: self._putc(b, attr)

    # ---- filesystem (operates on g_files / g_fds in kernel memory) ----
    def _files(self): return self.sym("g_files")
    def _fds(self):   return self.sym("g_fds")
    def _find_file(self, name):
        base = self._files()
        for i in range(NFILE):
            f = base + i*FILE_STRIDE
            if self.rd32(f+F_USED) and self.cstr(f+F_NAME, 24).decode("latin-1") == name:
                return i
        return -1
    def fs_open(self, name, flags=0):
        fi = self._find_file(name)
        if fi < 0: return -1
        base = self._fds()
        for fd in range(3, NFD):
            d = base + fd*FD_STRIDE
            if not self.rd32(d+FD_USED):
                off = self.rd32(self._files()+fi*FILE_STRIDE+F_SIZE) if (flags & 1) else 0
                self.wr32(d+FD_USED, 1); self.wr32(d+FD_FILE, fi); self.wr32(d+FD_OFF, off)
                return fd
        return -1
    def fs_read(self, fd, buf, ln):
        if fd < 3 or fd >= NFD: return -1
        d = self._fds() + fd*FD_STRIDE
        if not self.rd32(d+FD_USED): return -1
        f = self._files() + self.rd32(d+FD_FILE)*FILE_STRIDE
        off, size = self.rd32(d+FD_OFF), self.rd32(f+F_SIZE)
        if off >= size: return 0
        n = min(size-off, ln)
        self.uva_write(buf, self.rd(f+F_DATA+off, n)); self.wr32(d+FD_OFF, off+n)
        return n
    def fs_write(self, fd, buf, ln):
        if fd < 3 or fd >= NFD: return -1
        d = self._fds() + fd*FD_STRIDE
        if not self.rd32(d+FD_USED): return -1
        f = self._files() + self.rd32(d+FD_FILE)*FILE_STRIDE
        off = self.rd32(d+FD_OFF)
        if off+ln > FSIZE: ln = max(0, FSIZE-off)
        self.wr(f+F_DATA+off, self.uva_read(buf, ln)); off += ln
        self.wr32(d+FD_OFF, off)
        if off > self.rd32(f+F_SIZE): self.wr32(f+F_SIZE, off)
        return ln
    def fs_close(self, fd):
        if fd < 3 or fd >= NFD: return -1
        d = self._fds() + fd*FD_STRIDE
        if not self.rd32(d+FD_USED): return -1
        self.wr32(d+FD_USED, 0); self.wr32(d+FD_FILE, 0xFFFFFFFF)
        return 0

    def stdin_read(self, maxlen):
        # ensure a line is queued in the kernel keyboard buffer, then drain it
        gk, gh, gt = self.sym("g_kbd"), self.sym("g_kbd_head"), self.sym("g_kbd_tail")
        if gk is None:
            return b""
        if self.rd32(gh) == self.rd32(gt):          # buffer empty -> "type" next line
            if self.stdin_pos < len(STDIN_SCRIPT):
                end = STDIN_SCRIPT.find(b"\n", self.stdin_pos)
                end = len(STDIN_SCRIPT) if end < 0 else end + 1
                for ch in STDIN_SCRIPT[self.stdin_pos:end]:
                    self._kbd_push(ch)
                self.stdin_pos = end
        out = bytearray()
        while len(out) < maxlen:
            c = self._kbd_pop()
            if c < 0 or c == 0x0A: break
            out.append(c)
        attr = ATTR_BY_PID.get(self.pid(self.cur), 0x0A)
        if self.echo == 0:   self._write(bytes(out), attr)
        elif self.echo == 1: self._write(b"*" * len(out), attr)
        self._putc(0x0A, attr)                       # echo newline
        return bytes(out)

    def _kbd_push(self, c):
        gk, gh, gt = self.sym("g_kbd"), self.sym("g_kbd_head"), self.sym("g_kbd_tail")
        head, tail = self.rd32(gh), self.rd32(gt)
        n = (head + 1) % 256
        if n != tail:
            self.uc.mem_write(gk + head, bytes([c & 0xFF])); self.wr32(gh, n)
    def _kbd_pop(self):
        gk, gh, gt = self.sym("g_kbd"), self.sym("g_kbd_head"), self.sym("g_kbd_tail")
        head, tail = self.rd32(gh), self.rd32(gt)
        if head == tail: return -1
        c = self.uc.mem_read(gk + tail, 1)[0]; self.wr32(gt, (tail + 1) % 256)
        return c

    # print a file's contents to the console (kernel voice)
    def _dump_file(self, name):
        fi = self._find_file(name)
        if fi < 0: return
        f = self._files() + fi*FILE_STRIDE
        size = self.rd32(f+F_SIZE)
        self._write(("[kernel] contents of %s (%d bytes):\n" % (name, size)).encode(), KATTR)
        self._write(self.rd(f+F_DATA, size), 0x0F)

    # ---- task table helpers ----
    def _tbase(self, i): return self.sym("g_tasks") + i*TASK_STRIDE
    def state(self, i): return self.rd32(self._tbase(i)+ST_STATE)
    def set_state(self, i, s): self.wr32(self._tbase(i)+ST_STATE, s)
    def pid(self, i): return self.rd32(self._tbase(i)+ST_PID)
    def ntask(self): 
        s = self.sym("g_ntask"); return self.rd32(s) if s else 0
    def save_ctx(self, i):
        b = self._tbase(i)
        for reg, off in REGMAP: self.wr32(b+off, self.uc.reg_read(reg))
    def restore_ctx(self, i):
        b = self._tbase(i)
        for reg, off in REGMAP: self.uc.reg_write(reg, self.rd32(b+off))
        cr3 = self.rd32(b+ST_CR3)
        if cr3: self.uc.reg_write(UC_X86_REG_CR3, cr3)     # switch address space
        self.uc.reg_write(UC_X86_REG_CS, 0x1B)
        for r in (UC_X86_REG_DS,UC_X86_REG_ES,UC_X86_REG_FS,UC_X86_REG_GS,UC_X86_REG_SS):
            self.uc.reg_write(r, 0x23)

    # ---- emulator-side MM (mirrors kernel/mm.c on shared memory) ----
    def e_frame_alloc(self):
        fb = self.sym("frame_bitmap")
        nframes = (PHYS_TOP - FRAME_BASE) // PAGE_SIZE
        for i in range(nframes):
            word = self.rd32(fb + (i >> 5) * 4)
            if not (word >> (i & 31)) & 1:
                self.wr32(fb + (i >> 5) * 4, word | (1 << (i & 31)))
                pa = FRAME_BASE + i * PAGE_SIZE
                self.uc.mem_write(pa, b"\x00" * PAGE_SIZE)
                return pa
        raise RuntimeError("out of frames")

    def e_vm_create_user(self):
        pd = self.e_frame_alloc()
        ks = self.sym("kpd_s")
        for i in range(4):
            self.wr32(pd + i * 4, self.rd32(ks + i * 4))
        return pd

    def e_vm_map(self, pd, va, pa, flags):
        pde_a = pd + ((va >> 22) << 2)
        pde = self.rd32(pde_a)
        if not pde & 1:
            pt = self.e_frame_alloc()
            self.wr32(pde_a, pt | flags | 0x3)
            pde = pt | flags | 0x3
        pt = pde & ~0xFFF
        self.wr32(pt + (((va >> 12) & 0x3FF) << 2), (pa & ~0xFFF) | (flags & 0xFFF) | 1)

    def e_vm_clone(self, src):
        dst = self.e_vm_create_user()
        for i in range(USER_BASE >> 22, 1024):
            pde = self.rd32(src + i * 4)
            if not pde & 1: continue
            spt = pde & ~0xFFF
            for j in range(1024):
                pte = self.rd32(spt + j * 4)
                if not pte & 1: continue
                va = (i << 22) | (j << 12)
                fr = self.e_frame_alloc()
                self.uc.mem_write(fr, bytes(self.uc.mem_read(pte & ~0xFFF, PAGE_SIZE)))
                self.e_vm_map(dst, va, fr, PAGE_USER_RW)
        return dst

    def translate(self, va):
        cr3 = self.uc.reg_read(UC_X86_REG_CR3) & ~0xFFF
        pde = self.rd32(cr3 + ((va >> 22) << 2))
        if not pde & 1: return None
        pte = self.rd32((pde & ~0xFFF) + (((va >> 12) & 0x3FF) << 2))
        if not pte & 1: return None
        return (pte & ~0xFFF) | (va & 0xFFF)

    def uva_read(self, va, n):
        out = bytearray()
        while n > 0:
            pa = self.translate(va)
            if pa is None: break
            chunk = min(n, PAGE_SIZE - (va & 0xFFF))
            out += self.uc.mem_read(pa, chunk); va += chunk; n -= chunk
        return bytes(out)

    def uva_write(self, va, data):
        i = 0
        while i < len(data):
            pa = self.translate(va)
            if pa is None: break
            chunk = min(len(data) - i, PAGE_SIZE - (va & 0xFFF))
            self.uc.mem_write(pa, data[i:i+chunk]); va += chunk; i += chunk

    def uva_cstr(self, va, maxn=64):
        out = bytearray()
        for _ in range(maxn):
            b = self.uva_read(va, 1)
            if not b or b[0] == 0: break
            out.append(b[0]); va += 1
        return bytes(out)

    # ---- process management ----
    def e_task_alloc(self):
        for i in range(NTASK):
            if self.state(i) == T_UNUSED: return i
        return -1

    def _rd_lookup(self, name):
        rd = self.sym("__ramdisk_start")
        if rd is None or bytes(self.uc.mem_read(rd, 3)) != b"TRD": return None
        count = self.rd32(rd + 4)
        for i in range(count):
            e = rd + 8 + i * 24
            nm = bytes(self.uc.mem_read(e, 16)).split(b"\0")[0].decode("latin-1")
            if nm == name:
                return rd + self.rd32(e + 16), self.rd32(e + 20)
        return None

    def do_exec(self, path, arg=""):
        prog = self._rd_lookup(path)
        if prog is None: return -1
        img, size = prog
        idx = self.e_task_alloc()
        if idx < 0: return -1
        pd = self.e_vm_create_user()
        off = 0
        while off < size:
            fr = self.e_frame_alloc()
            n = min(PAGE_SIZE, size - off)
            self.uc.mem_write(fr, bytes(self.uc.mem_read(img + off, n)))
            self.e_vm_map(pd, USER_BASE + off, fr, PAGE_USER_RW)
            off += PAGE_SIZE
        for i in range(1, 5):
            self.e_vm_map(pd, USER_STACK_TOP - i * PAGE_SIZE, self.e_frame_alloc(), PAGE_USER_RW)
        af = self.e_frame_alloc()                        # argument page @ ARG_ADDR
        self.uc.mem_write(af, arg.encode("latin-1")[:PAGE_SIZE-1] + b"\x00")
        self.e_vm_map(pd, ARG_ADDR, af, PAGE_USER_RW)
        pidv = self.next_pid; self.next_pid += 1
        self._task_init(idx, USER_BASE, USER_STACK_TOP - 16, pd, pidv, self.pid(self.cur), path)
        self.trace.append(("exec", path, "-> pid", pidv, "task", idx, "cr3", hex(pd)))
        return pidv

    def _task_init(self, idx, eip, esp, cr3, pidv, ppid, name):
        b = self._tbase(idx)
        self.uc.mem_write(b, b"\x00" * TASK_STRIDE)
        self.wr32(b + 32, eip); self.wr32(b + 28, esp); self.wr32(b + 36, 0x202)
        self.wr32(b + ST_STATE, T_READY); self.wr32(b + ST_PID, pidv)
        self.wr32(b + ST_PPID, ppid); self.wr32(b + ST_CR3, cr3)
        self.wr32(b + ST_KSTACK, KSTACK - idx * 0x1000); self.wr32(b + ST_WAITON, 0xFFFFFFFF)
        nm = name.encode()[:15]
        self.uc.mem_write(b + ST_NAME, nm + b"\x00")
        if idx + 1 > self.ntask(): self.wr32(self.sym("g_ntask"), idx + 1)

    def do_fork(self):
        idx = self.e_task_alloc()
        if idx < 0: return -1
        src = self._tbase(self.cur)
        dst = self._tbase(idx)
        self.uc.mem_write(dst, bytes(self.uc.mem_read(src, TASK_STRIDE)))   # copy TCB
        # save the parent's *live* context into the child image first
        for reg, off in REGMAP: self.wr32(dst + off, self.uc.reg_read(reg))
        pidv = self.next_pid; self.next_pid += 1
        self.wr32(dst + ST_PID, pidv); self.wr32(dst + ST_PPID, self.pid(self.cur))
        self.wr32(dst + ST_CR3, self.e_vm_clone(self.rd32(src + ST_CR3)))
        self.wr32(dst + ST_KSTACK, KSTACK - idx * 0x1000)
        self.wr32(dst + ST_STATE, T_READY); self.wr32(dst + ST_WAITON, 0xFFFFFFFF)
        self.wr32(dst + 0, 0)                                        # child: fork() -> 0
        if idx + 1 > self.ntask(): self.wr32(self.sym("g_ntask"), idx + 1)
        self.trace.append(("fork", "parent pid", self.pid(self.cur), "-> child pid", pidv))
        return pidv

    def pick(self, after):
        nt = self.ntask()
        for k in range(1, nt+1):
            i = (after+k) % nt
            if self.state(i) == T_READY: return i
        return None
    def pick_any(self):
        for i in range(self.ntask()):
            if self.state(i) == T_READY: return i
        return None

    def ensure_started(self):
        if not self.started and self.ntask() > 0:
            self.started = True
            self.cur = self.rd32(self.sym("g_current"))
            self.set_state(self.cur, T_RUNNING)

    def switch_to(self, nxt, reason):
        if nxt is None or nxt == self.cur: return
        self.save_ctx(self.cur)
        if self.state(self.cur) == T_RUNNING: self.set_state(self.cur, T_READY)
        self.trace.append((reason, "task", self.cur, "->", nxt))
        self.cur = nxt; self.wr32(self.sym("g_current"), nxt)
        self.restore_ctx(nxt); self.set_state(nxt, T_RUNNING)

    # ---- syscall handler ----
    def _on_intr(self, uc, intno, _):
        if intno != 0x80: return
        self.ensure_started()
        n = uc.reg_read(UC_X86_REG_EAX)
        ebx = uc.reg_read(UC_X86_REG_EBX); ecx = uc.reg_read(UC_X86_REG_ECX); edx = uc.reg_read(UC_X86_REG_EDX)
        setret = lambda v: uc.reg_write(UC_X86_REG_EAX, v & 0xFFFFFFFF)
        if n == SYS_WRITE:
            if ebx in (1, 2):
                attr = ATTR_BY_PID.get(self.pid(self.cur), 0x0A)
                self._write(self.uva_read(ecx, edx), attr); setret(edx)
            else:
                setret(self.fs_write(ebx, ecx, edx))
        elif n == SYS_READ:
            if ebx == 0:
                data = self.stdin_read(edx); self.uva_write(ecx, data); setret(len(data))
                self.trace.append(("read.stdin", "->", data))
            else:
                setret(self.fs_read(ebx, ecx, edx))
        elif n == SYS_OPEN:
            path = self.uva_cstr(ebx).decode("latin-1")
            fd = self.fs_open(path, ecx)
            self.trace.append(("open", path, "->", fd)); setret(fd)
        elif n == SYS_CLOSE:
            setret(self.fs_close(ebx))
        elif n == SYS_GETPID:
            setret(self.pid(self.cur))
        elif n == SYS_STAT:
            path = self.uva_cstr(ebx).decode("latin-1")
            st = self.vfs_stat(path)
            if st is None: setret(0xFFFFFFFF)
            else:
                if ecx: self.uva_write(ecx, struct.pack("<II", st[0], st[1]))
                setret(0)
        elif n == SYS_LSEEK:
            if ebx < 3 or ebx >= NFD or not self.rd32(self._fds()+ebx*FD_STRIDE+FD_USED): setret(0xFFFFFFFF)
            else:
                d = self._fds()+ebx*FD_STRIDE
                fsz = self.rd32(self._files()+self.rd32(d+FD_FILE)*FILE_STRIDE+F_SIZE)
                base = self.rd32(d+FD_OFF) if edx == 1 else (fsz if edx == 2 else 0)
                self.wr32(d+FD_OFF, (base + ecx) & 0xFFFFFFFF); setret(base + ecx)
        elif n == SYS_READFILE:
            data = self.vfs_readfile(self.uva_cstr(ebx).decode("latin-1"), edx)
            if data is None: setret(0xFFFFFFFF)
            else: self.uva_write(ecx, data); setret(len(data))
        elif n == SYS_WRITEFILE:
            setret(self.vfs_writefile(self.uva_cstr(ebx).decode("latin-1"), self.uva_read(ecx, edx)))
        elif n == SYS_READDIR:
            name = self.vfs_readdir(self.uva_cstr(ebx).decode("latin-1"), ecx)
            if name is None: setret(0xFFFFFFFF)
            else: self.uva_write(edx, name.encode()+b"\x00"); setret(0)
        elif n == SYS_GETCHAR:
            setret(self._getchar())
        elif n == SYS_CLEAR:
            self.uc.mem_write(VGA, (bytes([0x20, KATTR]) * 2000)); self.wr32(CURSOR, 0); setret(0)
        elif n == SYS_GOTOXY:
            row = min(max(ebx, 0), 24); col = min(max(ecx, 0), 79)
            self.wr32(CURSOR, row*80+col); setret(0)
        elif n == SYS_TASKINFO:
            if ebx < self.ntask() and self.state(ebx) != T_UNUSED:
                b = self._tbase(ebx)
                blob = struct.pack("<II", self.rd32(b+ST_PID), self.rd32(b+ST_STATE)) + bytes(self.uc.mem_read(b+ST_NAME, 16))
                self.uva_write(ecx, blob); setret(0)
            else: setret(0xFFFFFFFF)
        elif n == SYS_EXEC:
            arg = self.uva_cstr(ecx).decode("latin-1") if ecx else ""
            setret(self.do_exec(self.uva_cstr(ebx).decode("latin-1"), arg))
        elif n == SYS_FORK:
            setret(self.do_fork())
        elif n == SYS_WAIT:
            code = self._exited_code(ebx)
            if code is not None:
                setret(code)                        # child already gone
            else:
                self.wr32(self._tbase(self.cur)+ST_WAITON, ebx & 0xFFFFFFFF)
                self.pending = ("block",); uc.emu_stop()
        elif n == SYS_SPAWN:
            if 0 <= ebx < NTASK and self.state(ebx) == T_BLOCKED: self.set_state(ebx, T_READY)
            self.trace.append(("spawn", "task", ebx)); setret(0)
        elif n == SYS_ECHO:
            self.echo = ebx; setret(0)
        elif n == SYS_YIELD:
            self.pending = ("yield",); uc.emu_stop()
        elif n == SYS_EXIT:
            self.pending = ("exit", ebx); uc.emu_stop()
        else:
            setret(0xFFFFFFFF)

    # ---- unified VFS (ram fs + optional FAT image at /mnt) ----
    def _ram_find(self, path):
        for i in range(NFILE):
            f = self._files() + i*FILE_STRIDE
            if self.rd32(f+F_USED) and self.cstr(f+F_NAME, 24).decode("latin-1") == path:
                return i
        return -1

    def vfs_stat(self, path):
        if path.startswith("/mnt"):
            if not self.fat: return None
            w = self.fat.walk(path[4:])
            return None if w is None else (w[1], w[2])
        if path == "/": return (0, 1)
        i = self._ram_find(path)
        if i >= 0: return (self.rd32(self._files()+i*FILE_STRIDE+F_SIZE), 0)
        for j in range(NFILE):
            f = self._files()+j*FILE_STRIDE
            if self.rd32(f+F_USED) and self.cstr(f+F_NAME,24).decode("latin-1").startswith(path+"/"):
                return (0, 1)
        return None

    def vfs_readdir(self, path, index):
        if path.startswith("/mnt"):
            return self.fat.readdir(path[4:], index) if self.fat else None
        names = []
        root = (path == "/")
        for j in range(NFILE):
            f = self._files()+j*FILE_STRIDE
            if not self.rd32(f+F_USED): continue
            nm = self.cstr(f+F_NAME,24).decode("latin-1")
            if root: rest = nm[1:]
            else:
                if not nm.startswith(path+"/"): continue
                rest = nm[len(path)+1:]
            comp = rest.split("/")[0]; e = comp + ("/" if "/" in rest else "")
            if e not in names: names.append(e)
        if root and self.fat and "mnt/" not in names: names.append("mnt/")
        return names[index] if 0 <= index < len(names) else None

    def vfs_readfile(self, path, maxlen):
        if path.startswith("/mnt"):
            return self.fat.readfile(path[4:], maxlen) if self.fat else None
        i = self._ram_find(path)
        if i < 0: return None
        f = self._files()+i*FILE_STRIDE
        n = min(self.rd32(f+F_SIZE), maxlen)
        return bytes(self.uc.mem_read(f+F_DATA, n))

    def vfs_writefile(self, path, data):
        if path.startswith("/mnt"):
            if not self.fat: return 0xFFFFFFFF
            r = self.fat.write(path[4:], bytes(data))
            return 0xFFFFFFFF if r is None else r
        i = self._ram_find(path)
        if i < 0:
            for j in range(NFILE):
                f = self._files()+j*FILE_STRIDE
                if not self.rd32(f+F_USED):
                    self.uc.mem_write(f, b"\x00"*FILE_STRIDE)
                    self.uc.mem_write(f+F_NAME, path.encode()[:23]+b"\x00")
                    self.wr32(f+F_USED, 1); i = j; break
            if i < 0: return 0xFFFFFFFF
        f = self._files()+i*FILE_STRIDE
        n = min(len(data), FSIZE)
        self.uc.mem_write(f+F_DATA, bytes(data[:n])); self.wr32(f+F_SIZE, n)
        return n

    def _getchar(self):
        # feed the scripted stdin one raw byte at a time
        gk, gh, gt = self.sym("g_kbd"), self.sym("g_kbd_head"), self.sym("g_kbd_tail")
        if gk is not None and self.rd32(gh) == self.rd32(gt) and self.stdin_pos < len(STDIN_SCRIPT):
            self._kbd_push(STDIN_SCRIPT[self.stdin_pos]); self.stdin_pos += 1
        c = self._kbd_pop()
        return c if c >= 0 else 0x0A

    def _exited_code(self, pid):
        for i in range(self.ntask()):
            if self.pid(i) == pid and self.state(i) == T_EXITED:
                return self.rd32(self._tbase(i)+ST_EXIT)
        return None

    def _wake_waiters(self, pid, code):
        for i in range(self.ntask()):
            b = self._tbase(i)
            if self.state(i) == T_BLOCKED and self.rd32(b+ST_WAITON) == pid:
                self.wr32(b+ST_WAITON, 0xFFFFFFFF)
                self.wr32(b+0, code & 0xFFFFFFFF)     # wait() returns exit code
                self.set_state(i, T_READY)

    def run(self, max_bursts=6000):
        uc = self.uc
        for _ in range(max_bursts):
            if self.done: break
            eip = uc.reg_read(UC_X86_REG_EIP)
            self.pending = None
            try:
                uc.emu_start(eip, 0, count=QUANTUM)
            except UcError as e:
                self.trace.append(("UcError", str(e), "eip", hex(uc.reg_read(UC_X86_REG_EIP)))); break
            self.ensure_started()
            p = self.pending
            if p and p[0] == "exit":
                code = p[1]
                b = self._tbase(self.cur)
                self.wr32(b + ST_EXIT, code); self.set_state(self.cur, T_EXITED)
                self.trace.append(("exit", "task", self.cur, "pid", self.pid(self.cur), "code", code))
                self._wake_waiters(self.pid(self.cur), code)
                self._resume_next()
            elif p and p[0] == "block":
                self.save_ctx(self.cur); self.set_state(self.cur, T_BLOCKED)
                self._resume_next()
            elif p and p[0] == "yield":
                self.switch_to(self.pick(self.cur), "yield")
            else:  # quantum expired
                if (uc.reg_read(UC_X86_REG_CS) & 3) == 3 and self.started:
                    self.switch_to(self.pick(self.cur), "preempt")

    def _resume_next(self):
        nxt = self.pick_any()
        if nxt is None:
            self._dump_file("/tmp/log")
            self._write(b"[kernel] no runnable tasks; scheduler idle, halting.\n", KATTR)
            self.done = True
        else:
            self.cur = nxt; self.wr32(self.sym("g_current"), nxt)
            self.restore_ctx(nxt); self.set_state(nxt, T_RUNNING)

    # ---- render VGA text buffer ----
    def render(self):
        ansi = {0x07:"\033[0;37m",0x0A:"\033[1;32m",0x0B:"\033[1;36m",0x0E:"\033[1;33m",
                0x0F:"\033[1;37m",0x0C:"\033[1;31m"}
        buf = bytes(self.uc.mem_read(VGA, 2000*2)); rows = []
        for r in range(25):
            cells = [(buf[(r*80+c)*2], buf[(r*80+c)*2+1]) for c in range(80)]
            text = "".join(chr(ch) if 32 <= ch < 127 else " " for ch,_ in cells)
            if not text.strip(): rows.append(None); continue
            attr = next((a for ch,a in cells if ch not in (0,32)), 0x07)
            rows.append(ansi.get(attr,"\033[0m") + text.rstrip() + "\033[0m")
        while rows and rows[-1] is None: rows.pop()
        bar = "+" + "-"*62 + "+"
        print(bar); print("| tinyos - emulated 80x25 VGA text console" + " "*21 + "|"); print(bar)
        for row in rows: print(row if row is not None else "")
        print(bar)


def main():
    args = sys.argv[1:]
    syms = load_syms(os.environ.get("TINYOS_SYMS", "build/kernel.sym"))
    fat = None
    fs_path = os.environ.get("TINYOS_FS", "fs.img")
    if os.path.exists(fs_path):
        try: fat = FatImage(open(fs_path, "rb").read(), fs_path)
        except Exception: fat = None
    full_trace, boot_out = [], b""
    if len(args) == 2 and args[0] == "--kernel":
        m = Machine(syms, kernel_bin=args[1], fat=fat)
    elif len(args) == 1:
        img = open(args[0], "rb").read()
        ram, gdtr, boot_out = run_stage_a(img, full_trace)
        m = Machine(syms, ram=ram, gdtr=gdtr, fat=fat)
    else:
        sys.exit(__doc__)

    if boot_out:
        print("[BIOS] real-mode boot output:")
        print("    " + boot_out.decode("latin-1").replace("\r\n", "\n    ").rstrip() + "\n")

    m.run()
    m.render()
    print("\n[emulator] trace (scheduling + syscalls):")
    preempts = 0
    for ev in full_trace + m.trace:
        if ev and ev[0] == "preempt":
            preempts += 1
            continue
        print("   ", ev)
    if preempts:
        print("    ('preempt' x%d  <- preemptive context switches between tasks)" % preempts)


if __name__ == "__main__":
    main()
