/* tinyos x64 arch layer (Cycle 4, Phase B): ring-0/ring-3 privilege split.
 *
 * arch_init() installs our own GDT (kernel + user code/data), a TSS (so faults
 * from ring 3 land on a kernel stack), and an IDT whose exception gates dump to
 * the serial console.  arch_usermode_selftest() then proves the whole path: it
 * maps a tiny user program (kernel/uprog.s) into a fresh, user-accessible region
 * of the current address space, enables SYSCALL, drops to ring 3 via iretq, and
 * services the program's SYS_WRITE / SYS_EXIT before returning to the caller.
 *
 * This is the foundation for real processes (Phase C): per-process page tables,
 * a scheduler, and exec/fork.  For now it runs one program to completion. */
#include "arch64.h"
#include "fat64.h"             /* RAM-disk FAT16: readdir/readfile/writefile */
#include "uhello.h"            /* static const unsigned char hello_prog[] */
#include "ucount.h"            /* static const unsigned char count_prog[] */
#include "ugreet.h"            /* static const unsigned char greet_prog[] */
#include "ush.h"               /* static const unsigned char sh_prog[]    */
#include "uvi.h"               /* static const unsigned char vi_prog[]    */
#include "uexec_probe.h"       /* TinyOS parent for nested filesystem ELF */
#include "linux_hello.h"       /* static ELF64 Linux-ABI boot probe       */
#include "musl_probe.h"        /* real static musl libc probe             */

/* ---- assembly helpers (kernel/trap64.s) ---- */
void     arch_load_gdt(void *gdtr);
void     arch_ltr(uint16_t sel);
void     arch_set_syscall_stack(uint64_t rsp_top);
void     arch_setup_syscall_msrs(void);
uint64_t arch_isr_addr(unsigned i);
void     enter_user(uint64_t rip, uint64_t rsp, uint64_t arg);
void     sys_exit_resume(void);

/* ---- GDT + TSS ---- */
struct gdtr   { uint16_t limit; uint64_t base; } __attribute__((packed));
struct tss64  {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static uint64_t     g_gdt[7];               /* 0 null,1 kcode,2 kdata,3 udata,4 ucode,5-6 TSS */
static struct tss64 g_tss;
static uint8_t      g_kstack0[16384] __attribute__((aligned(16)));   /* TSS RSP0 (faults)     */
static uint8_t      g_ksys0[16384]   __attribute__((aligned(16)));   /* SYSCALL kstack level 0 */
static uint8_t      g_ksys1[16384]   __attribute__((aligned(16)));   /* SYSCALL kstack level 1 */

/* ---- IDT ---- */
struct idt_entry {
    uint16_t off_lo; uint16_t sel; uint8_t ist; uint8_t type;
    uint16_t off_mid; uint32_t off_hi; uint32_t zero;
} __attribute__((packed));
static struct idt_entry g_idt[256];

static int  g_user_exit_code;
static int  g_cur_pid = 1;
enum user_abi { USER_ABI_TINYOS, USER_ABI_LINUX };
static enum user_abi g_user_abi = USER_ABI_TINYOS;

/* ---- user address space layout (high canonical, clear of physical RAM) ---- */
#define USER_CODE_VA    0x0000700000000000ULL
#define USER_STK_TOP    0x0000700000100000ULL
#define LINUX_LOAD_VA   0x0000000000400000ULL
#define LINUX_STK_TOP   0x00007FFFFFF00000ULL
#define LINUX_BRK_BASE  0x0000500000000000ULL
#define LINUX_BRK_LIMIT 0x0000500010000000ULL
#define LINUX_MMAP_BASE 0x0000600000000000ULL
#define LINUX_MMAP_LIMIT 0x0000600100000000ULL
#define USER_CODE_PAGES 16
#define USER_STK_PAGES  4
#define USER_ARG_OFF    (USER_CODE_PAGES * 4096 - 256)   /* argv[0] staged here */
#define PTE_P   0x1
#define PTE_RW  0x2
#define PTE_US  0x4
#define PTE_PS  0x80

/* C dumper for the exception stubs (kernel/trap64.s). */
void isr_c(uint64_t vec, uint64_t err, uint64_t rip) {
    uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kputs("\n[EXC] vector "); kput_u32dec((uint32_t)vec);
    kputs(" err ");  kput_u64hex(err);
    kputs(" rip ");  kput_u64hex(rip);
    kputs(" cr2 ");  kput_u64hex(cr2);
    kputs("\n[EXC] halted.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

static void set_gate(int i, uint64_t handler) {
    g_idt[i].off_lo  = handler & 0xFFFF;
    g_idt[i].sel     = 0x08;                 /* kernel code               */
    g_idt[i].ist     = 0;
    g_idt[i].type    = 0x8E;                 /* present, DPL0, int gate   */
    g_idt[i].off_mid = (handler >> 16) & 0xFFFF;
    g_idt[i].off_hi  = (handler >> 32) & 0xFFFFFFFF;
    g_idt[i].zero    = 0;
}

static void set_tss_desc(int idx, uint64_t base, uint32_t limit) {
    uint64_t lo = 0;
    lo |= (limit & 0xFFFFULL);
    lo |= (base & 0xFFFFFFULL) << 16;
    lo |= (uint64_t)0x89 << 40;              /* present, 64-bit avail TSS */
    lo |= (uint64_t)((limit >> 16) & 0xF) << 48;
    lo |= ((base >> 24) & 0xFFULL) << 56;
    g_gdt[idx]     = lo;
    g_gdt[idx + 1] = (base >> 32) & 0xFFFFFFFFULL;
}

void arch_init(void) {
    g_gdt[0] = 0;
    g_gdt[1] = 0x00AF9A000000FFFFULL;        /* kernel code (DPL0, L=1)   */
    g_gdt[2] = 0x00CF92000000FFFFULL;        /* kernel data (DPL0)        */
    g_gdt[3] = 0x00CFF2000000FFFFULL;        /* user   data (DPL3)        */
    g_gdt[4] = 0x00AFFA000000FFFFULL;        /* user   code (DPL3, L=1)   */

    memset(&g_tss, 0, sizeof(g_tss));
    g_tss.rsp0       = (uint64_t)(g_kstack0 + sizeof(g_kstack0));
    g_tss.iomap_base = sizeof(g_tss);
    set_tss_desc(5, (uint64_t)&g_tss, sizeof(g_tss) - 1);

    static struct gdtr gp;
    gp.limit = sizeof(g_gdt) - 1;
    gp.base  = (uint64_t)g_gdt;
    kputs("[arch] loading gdt+tss\n");
    arch_load_gdt(&gp);
    arch_ltr(0x28);
    kputs("[arch] gdt+tss ok; loading idt\n");

    for (int i = 0; i < 256; i++) set_gate(i, arch_isr_addr((unsigned)i));
    static struct { uint16_t limit; uint64_t base; } __attribute__((packed)) idtr;
    idtr.limit = sizeof(g_idt) - 1;
    idtr.base  = (uint64_t)g_idt;
    __asm__ volatile("lidt %0" :: "m"(idtr));

    kputs("[arch] gdt + tss + idt installed (ring 0/3 ready)\n");
}

/* Install an interrupt gate at runtime.  The IDT is already loaded (arch_init),
 * so this takes effect immediately; used by the scheduler for the LAPIC timer. */
void arch_set_irq_gate(int vec, uint64_t handler) {
    if (vec >= 0 && vec < 256) set_gate(vec, handler);
}

/* ---- syscall dispatch (called from kernel/trap64.s:syscall_entry) ---- */
static int arch_exec_named(const char *name, const char *arg);  /* nested child */
static int proc_info(int i, int *pid, int *ring, int *state, char *name); /* ps */
static int name_eq(const char *a, const char *b);
static uint64_t linux_brk(uint64_t requested);
static uint64_t linux_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                           uint64_t flags, uint64_t fd, uint64_t offset);
static int linux_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
static int linux_munmap(uint64_t addr, uint64_t len);
static int linux_openat(int dirfd, const char *path, int flags);
static int linux_close(int fd);
static int linux_read(int fd, void *buf, uint32_t len);
static int linux_write(int fd, const void *buf, uint32_t len);
static int64_t linux_lseek(int fd, int64_t offset, int whence);
static int linux_fstat(int fd, void *statbuf);
static int linux_ftruncate(int fd, uint64_t size);
static int linux_newfstatat(int dirfd, const char *path, void *statbuf, int flags);
static int linux_getdents64(int fd, void *buf, uint32_t len);
static int linux_mkdirat(int dirfd, const char *path);
static int linux_unlinkat(int dirfd, const char *path, int flags);
static uint64_t linux_getcwd(char *buf, uint64_t size);
static int linux_arch_prctl(int code, uint64_t addr);
static int linux_clock_gettime(int clock_id, void *timespec);
static int linux_getrandom(void *buf, uint32_t len, uint32_t flags);
static int linux_writev(int fd, const void *iov, int count);
static int linux_ioctl(int fd, uint64_t request, void *arg);
static int arch_user_run(const char *name, const uint8_t *img, uint64_t size,
                         const char *arg);

static uint64_t g_linux_fs_base;
static uint64_t g_linux_sigmask;
static uint64_t g_linux_clear_tid;

static void write_fs_base(uint64_t base) {
    uint32_t lo = (uint32_t)base, hi = (uint32_t)(base >> 32);
    __asm__ volatile("wrmsr" :: "c"(0xC0000100U), "a"(lo), "d"(hi));
    g_linux_fs_base = base;
}

uint64_t syscall_c(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                   uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    if (g_user_abi == USER_ABI_LINUX) {
        switch (num) {
        case 0:                             /* read                       */
            return (uint64_t)(int64_t)linux_read((int)a0, (void *)a1, (uint32_t)a2);
        case 1:                             /* write                      */
            return (uint64_t)(int64_t)linux_write((int)a0, (const void *)a1, (uint32_t)a2);
        case 2:                             /* open                       */
            return (uint64_t)(int64_t)linux_openat(-100, (const char *)a0, (int)a1);
        case 3:                             /* close                      */
            return (uint64_t)(int64_t)linux_close((int)a0);
        case 5:                             /* fstat                      */
            return (uint64_t)(int64_t)linux_fstat((int)a0, (void *)a1);
        case 8:                             /* lseek                      */
            return (uint64_t)linux_lseek((int)a0, (int64_t)a1, (int)a2);
        case 9:                             /* mmap                       */
            return linux_mmap(a0, a1, a2, a3, a4, a5);
        case 10:                            /* mprotect                   */
            return (uint64_t)(int64_t)linux_mprotect(a0, a1, a2);
        case 11:                            /* munmap                     */
            return (uint64_t)(int64_t)linux_munmap(a0, a1);
        case 12:                            /* brk                        */
            return linux_brk(a0);
        case 14: {                          /* rt_sigprocmask             */
            if (a3 != 8) return (uint64_t)-22;
            if (a2) *(uint64_t *)a2 = g_linux_sigmask;
            if (a1) {
                uint64_t mask = *(const uint64_t *)a1;
                if (a0 == 0) g_linux_sigmask |= mask;
                else if (a0 == 1) g_linux_sigmask &= ~mask;
                else if (a0 == 2) g_linux_sigmask = mask;
                else return (uint64_t)-22;
            }
            return 0;
        }
        case 16:                            /* ioctl                      */
            return (uint64_t)(int64_t)linux_ioctl((int)a0, a1, (void *)a2);
        case 20:                            /* writev                     */
            return (uint64_t)(int64_t)linux_writev((int)a0, (const void *)a1, (int)a2);
        case 39:                            /* getpid                     */
        case 186:                           /* gettid                     */
            return (uint64_t)g_cur_pid;
        case 77:                            /* ftruncate                  */
            return (uint64_t)(int64_t)linux_ftruncate((int)a0, a1);
        case 79:                            /* getcwd                     */
            return linux_getcwd((char *)a0, a1);
        case 158:                           /* arch_prctl                 */
            return (uint64_t)(int64_t)linux_arch_prctl((int)a0, a1);
        case 218:                           /* set_tid_address            */
            g_linux_clear_tid = a0;
            return (uint64_t)g_cur_pid;
        case 217:                           /* getdents64                 */
            return (uint64_t)(int64_t)linux_getdents64((int)a0, (void *)a1, (uint32_t)a2);
        case 228:                           /* clock_gettime              */
            return (uint64_t)(int64_t)linux_clock_gettime((int)a0, (void *)a1);
        case 257:                           /* openat                     */
            return (uint64_t)(int64_t)linux_openat((int)a0, (const char *)a1, (int)a2);
        case 258:                           /* mkdirat                    */
            return (uint64_t)(int64_t)linux_mkdirat((int)a0, (const char *)a1);
        case 262:                           /* newfstatat                 */
            return (uint64_t)(int64_t)linux_newfstatat((int)a0, (const char *)a1, (void *)a2, (int)a3);
        case 263:                           /* unlinkat                   */
            return (uint64_t)(int64_t)linux_unlinkat((int)a0, (const char *)a1, (int)a2);
        case 273:                           /* set_robust_list            */
            return 0;
        case 318:                           /* getrandom                  */
            return (uint64_t)(int64_t)linux_getrandom((void *)a0, (uint32_t)a1, (uint32_t)a2);
        case 60:                            /* exit(status)               */
        case 231:                           /* exit_group(status)         */
            g_user_exit_code = (int)a0;
            write_fs_base(0);
            sys_exit_resume();
            return 0;
        default:
            return (uint64_t)-38;           /* -ENOSYS                    */
        }
    }
    switch (num) {
    case 4:                                   /* write(fd, buf, len)       */
        (void)a0;
        usys_write((const char *)a1, (unsigned long)a2);
        return a2;
    case 3: {                                 /* read(fd, buf, max) -> line */
        (void)a0;
        char *buf = (char *)a1; int max = (int)a2, n = 0;
        for (;;) {
            int k = kbd64_getkey();
            if (k < 0) k = hvkbd_getkey();
            if (k >= 0) {
                if (k == '\n' || k == '\r') { char nl = '\n'; usys_write(&nl, 1); break; }
                else if (k == '\b') { if (n > 0) { n--; char b = '\b'; usys_write(&b, 1); } }
                else if (k >= 0x20 && k < 0x7F && n < max) { char c = (char)k; buf[n++] = c; usys_write(&c, 1); }
            }
            wm_pump();                        /* keep the desktop live while blocked */
        }
        return (uint64_t)n;
    }
    case 9:                                   /* exec(name, arg) -> code   */
        return (uint64_t)arch_exec_named((const char *)a0, (const char *)a1);
    case 19: {                                /* getchar() -> one keycode  */
        for (;;) {
            int k = kbd64_getkey();
            if (k < 0) k = hvkbd_getkey();
            if (k >= 0) return (uint64_t)k;
            wm_pump();                        /* keep the desktop live      */
        }
    }
    case 20:                                  /* clear() the terminal grid */
        term_clear_active();
        return 0;
    case 21:                                  /* gotoxy(row, col)          */
        term_gotoxy_active((int)a0, (int)a1);
        return 0;
    case 8:                                   /* getpid()                  */
        return (uint64_t)g_cur_pid;
    case 1:                                   /* exit(code)                */
        g_user_exit_code = (int)a0;
        sys_exit_resume();                    /* does not return           */
        return 0;
    case 15: {                                /* readdir(dir,i,name)       */
        char full[80]; fs_resolve((const char *)a0, full);
        int idx = (int)a1; char *name = (char *)a2;
        uint16_t dcl = 0; uint8_t at; uint16_t cl; uint32_t sz;
        if (!(full[0] == '/' && full[1] == 0)) {
            if (fat_resolve(full, &at, &cl, &sz) < 0 || !(at & 0x10)) return (uint64_t)-1;
            dcl = cl;
        }
        if (fat_list(dcl, idx, name, &at, &cl, &sz) < 0) return (uint64_t)-1;
        return (uint64_t)((at & 0x10) ? 1 : 0);
    }
    case 16: {                                /* taskinfo(i, buf) -> ps    */
        struct pinfo { int pid, ring, state; char name[16]; } *o = (struct pinfo *)a1;
        return (uint64_t)(int)proc_info((int)a0, &o->pid, &o->ring, &o->state, o->name);
    }
    case 18: {                                /* readfile(path,buf,max)    */
        char full[80]; fs_resolve((const char *)a0, full);
        uint8_t at; uint16_t cl; uint32_t sz;
        if (fat_resolve(full, &at, &cl, &sz) < 0 || (at & 0x10)) return (uint64_t)-1;
        return (uint64_t)(int)fat_read(cl, sz, (void *)a1, (uint32_t)a2);
    }
    case 22: {                                /* writefile(path,buf,n)     */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_write(full, (const void *)a1, (uint32_t)a2);
    }
    case 23: {                                /* mkdir(path)               */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_mkdir(full);
    }
    case 24: {                                /* unlink(path)              */
        char full[80]; fs_resolve((const char *)a0, full);
        return (uint64_t)(int)fat_unlink(full);
    }
    case 25:                                  /* chdir(path)               */
        return (uint64_t)(int)fs_chdir((const char *)a0);
    case 26:                                  /* getcwd(buf)               */
        fs_getcwd((char *)a0);
        return 0;
    case 27: {                                /* winsize() -> (cols<<16)|rows */
        int c = 0, r = 0; term_active_size(&c, &r);
        return (uint64_t)(((unsigned)c << 16) | (unsigned)(r & 0xFFFF));
    }
    default:
        return (uint64_t)-1;
    }
}

/* ---- minimal page-table walk: add a user page to the live address space ---- */
static uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static void flush_tlb(void)    { uint64_t v; __asm__ volatile("mov %%cr3,%0; mov %0,%%cr3" : "=r"(v) :: "memory"); }

/* Firmware write-protects its own page tables (CR0.WP), so temporarily allow
 * supervisor writes to read-only pages while we splice in the user mappings. */
static uint64_t wp_disable(void) {
    uint64_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0 & ~(1ULL << 16)) : "memory");
    return cr0;
}
static void wp_restore(uint64_t cr0) { __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory"); }

static uint64_t *next_level(uint64_t *tbl, int i) {
    if (!(tbl[i] & PTE_P)) {
        uint64_t *n = (uint64_t *)page_alloc(1);          /* identity-mapped, zeroed */
        tbl[i] = (uint64_t)n | PTE_P | PTE_RW | PTE_US;
        return n;
    }
    tbl[i] |= PTE_US;                                  /* open the path to ring 3 */
    return (uint64_t *)(tbl[i] & ~0xFFFULL);
}

static void map_user_page(uint64_t pml4_phys, uint64_t va, uint64_t phys, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;            /* CR3 is identity-mapped  */
    uint64_t *pdpt = next_level(pml4, (int)((va >> 39) & 0x1FF));
    uint64_t *pd   = next_level(pdpt, (int)((va >> 30) & 0x1FF));
    uint64_t *pt   = next_level(pd,   (int)((va >> 21) & 0x1FF));
    pt[(va >> 12) & 0x1FF] = (phys & ~0xFFFULL) | flags | PTE_P;
}

/* ---- reusable ring-3 program regions ------------------------------------
 * Two frame sets share the single USER_CODE_VA / USER_STK_TOP window so a shell
 * (level 0) can exec a child (level 1): only one is mapped at a time, swapped by
 * rewriting our own (writable) leaf page table.  A separate kernel syscall stack
 * per level keeps a suspended parent's syscall frame from being clobbered by the
 * child (see kernel/trap64.s). */
static uint8_t  *g_ucode, *g_ustk;             /* level 0 (console / shell)     */
static uint8_t  *g_ccode, *g_cstk;             /* level 1 (exec'd child)        */
static uint64_t *g_pt;                         /* cached leaf PT for the window */
static int       g_depth;                      /* 0 primary, 1 child running    */

static uint64_t *walk_pt(uint64_t pml4_phys, uint64_t va) {
    uint64_t *pml4 = (uint64_t *)pml4_phys;
    uint64_t *pdpt = (uint64_t *)(pml4[(va >> 39) & 0x1FF] & ~0xFFFULL);
    uint64_t *pd   = (uint64_t *)(pdpt[(va >> 30) & 0x1FF] & ~0xFFFULL);
    return (uint64_t *)(pd[(va >> 21) & 0x1FF] & ~0xFFFULL);
}

/* point the code+stack window at a given pair of physical frame runs */
static void map_region(uint8_t *code, uint8_t *stk) {
    int ci = (int)((USER_CODE_VA >> 12) & 0x1FF);
    int si = (int)((USER_STK_TOP  >> 12) & 0x1FF);
    for (int i = 0; i < USER_CODE_PAGES; i++)
        g_pt[ci + i]     = ((uint64_t)(code + (uint64_t)i * 4096)) | PTE_P | PTE_RW | PTE_US;
    for (int i = 0; i < USER_STK_PAGES; i++)
        g_pt[si - 1 - i] = ((uint64_t)(stk  + (uint64_t)i * 4096)) | PTE_P | PTE_RW | PTE_US;
    flush_tlb();
}

struct elf64_ehdr {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

static uint64_t *copy_page_table(const uint64_t *source) {
    uint64_t *copy = (uint64_t *)page_alloc(1);
    if (!copy) return 0;
    if (source) for (int i = 0; i < 512; i++) copy[i] = source[i];
    return copy;
}

static uint64_t make_table_entry(uint64_t *table) {
    return (uint64_t)table | PTE_P | PTE_RW | PTE_US;
}

struct linux_vm {
    uint64_t *pml4;
    uint64_t brk;
    uint64_t mmap_next;
};
static struct linux_vm g_linux_vm;

#define LINUX_FD_MAX 16
#define LINUX_O_ACCMODE 3
#define LINUX_O_CREAT 0100
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_DIRECTORY 0200000
enum linux_fd_kind { LINUX_FD_FILE = 1, LINUX_FD_DIR, LINUX_FD_NULL };
struct linux_fd {
    int used;
    int kind;
    int flags;
    uint32_t offset;
    uint16_t dir_cluster;
    char path[80];
};
static struct linux_fd g_linux_fds[LINUX_FD_MAX];

static void linux_fds_reset(void) {
    memset(g_linux_fds, 0, sizeof(g_linux_fds));
}

static int linux_resolve_at(int dirfd, const char *path, char *full) {
    if (!path || !path[0]) return -2;
    if (path[0] == '/' || dirfd == -100) { fs_resolve(path, full); return 0; }
    if (dirfd < 3 || dirfd >= LINUX_FD_MAX || !g_linux_fds[dirfd].used ||
        g_linux_fds[dirfd].kind != LINUX_FD_DIR) return -9;
    int n = 0;
    while (g_linux_fds[dirfd].path[n] && n < 79) { full[n] = g_linux_fds[dirfd].path[n]; n++; }
    if (n > 1 && n < 79) full[n++] = '/';
    int i = 0; while (path[i] && n < 79) full[n++] = path[i++];
    full[n] = 0;
    return path[i] ? -36 : 0;
}

static int linux_fd_alloc(int kind, int flags, const char *path, uint16_t dir_cluster, uint32_t offset) {
    for (int fd = 3; fd < LINUX_FD_MAX; fd++) {
        if (g_linux_fds[fd].used) continue;
        g_linux_fds[fd].used = 1;
        g_linux_fds[fd].kind = kind;
        g_linux_fds[fd].flags = flags;
        g_linux_fds[fd].offset = offset;
        g_linux_fds[fd].dir_cluster = dir_cluster;
        int i = 0; while (path && path[i] && i < 79) { g_linux_fds[fd].path[i] = path[i]; i++; }
        g_linux_fds[fd].path[i] = 0;
        return fd;
    }
    return -24;
}

static int linux_openat(int dirfd, const char *path, int flags) {
    char full[80]; int resolved = linux_resolve_at(dirfd, path, full);
    if (resolved < 0) return resolved;
    if (name_eq(full, "/dev/null")) return linux_fd_alloc(LINUX_FD_NULL, flags, full, 0, 0);
    uint8_t attr; uint16_t cluster; uint32_t size;
    int exists = fat_resolve(full, &attr, &cluster, &size) == 0;
    if (exists && (attr & 0x10)) {
        if (flags & LINUX_O_ACCMODE) return -21;
        return linux_fd_alloc(LINUX_FD_DIR, flags, full, cluster, 0);
    }
    if (flags & LINUX_O_DIRECTORY) return -20;
    if (!exists) {
        if (!(flags & LINUX_O_CREAT)) return -2;
        if (fat_write(full, "", 0) < 0) return -28;
        size = 0;
    } else if ((flags & LINUX_O_TRUNC) && (flags & LINUX_O_ACCMODE)) {
        if (fat_write(full, "", 0) < 0) return -5;
        size = 0;
    }
    return linux_fd_alloc(LINUX_FD_FILE, flags, full, 0, (flags & LINUX_O_APPEND) ? size : 0);
}

static int linux_close(int fd) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    g_linux_fds[fd].used = 0;
    return 0;
}

static int linux_read(int fd, void *buf, uint32_t len) {
    if (fd == 0) {
        char *out = (char *)buf; uint32_t count = 0;
        while (count < len) {
            int key = kbd64_getkey(); if (key < 0) key = hvkbd_getkey();
            if (key < 0) { wm_pump(); continue; }
            if (key == '\r') key = '\n';
            out[count++] = (char)key;
            usys_write((const char *)&out[count - 1], 1);
            if (key == '\n') break;
        }
        return (int)count;
    }
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used ||
        (g_linux_fds[fd].flags & LINUX_O_ACCMODE) == 1) return -9;
    if (g_linux_fds[fd].kind == LINUX_FD_NULL) return 0;
    if (g_linux_fds[fd].kind != LINUX_FD_FILE) return -21;
    int result = fat_pread(g_linux_fds[fd].path, buf, len, g_linux_fds[fd].offset);
    if (result > 0) g_linux_fds[fd].offset += (uint32_t)result;
    return result < 0 ? -5 : result;
}

static int linux_write(int fd, const void *buf, uint32_t len) {
    if (fd == 1 || fd == 2) { usys_write((const char *)buf, len); return (int)len; }
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used ||
        !(g_linux_fds[fd].flags & LINUX_O_ACCMODE)) return -9;
    if (g_linux_fds[fd].kind == LINUX_FD_NULL) return (int)len;
    if (g_linux_fds[fd].kind != LINUX_FD_FILE) return -21;
    if (g_linux_fds[fd].flags & LINUX_O_APPEND) {
        uint8_t attr; uint16_t cluster; uint32_t size;
        if (fat_resolve(g_linux_fds[fd].path, &attr, &cluster, &size) < 0) return -5;
        g_linux_fds[fd].offset = size;
    }
    int result = fat_pwrite(g_linux_fds[fd].path, buf, len, g_linux_fds[fd].offset);
    if (result > 0) g_linux_fds[fd].offset += (uint32_t)result;
    return result < 0 ? -5 : result;
}

static int64_t linux_lseek(int fd, int64_t offset, int whence) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind == LINUX_FD_DIR) return -29;
    if (g_linux_fds[fd].kind == LINUX_FD_NULL) return 0;
    int64_t base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = g_linux_fds[fd].offset;
    else if (whence == 2) {
        uint8_t attr; uint16_t cluster; uint32_t size;
        if (fat_resolve(g_linux_fds[fd].path, &attr, &cluster, &size) < 0) return -5;
        base = size;
    } else return -22;
    if (offset < -base || base + offset > 0xFFFFFFFFLL) return -22;
    g_linux_fds[fd].offset = (uint32_t)(base + offset);
    return base + offset;
}

struct linux_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    int64_t atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
};

static void linux_fill_stat(struct linux_stat *st, int kind, uint16_t cluster, uint32_t size) {
    memset(st, 0, sizeof(*st));
    st->ino = cluster ? cluster : 1;
    st->nlink = kind == LINUX_FD_DIR ? 2 : 1;
    st->mode = kind == LINUX_FD_DIR ? 0040755 : kind == LINUX_FD_FILE ? 0100644 : 0020666;
    st->size = kind == LINUX_FD_FILE ? size : 0;
    st->blksize = 512;
    st->blocks = (size + 511) / 512;
}

static int linux_stat_path(const char *path, struct linux_stat *st) {
    if (name_eq(path, "/dev/null")) { linux_fill_stat(st, LINUX_FD_NULL, 0, 0); return 0; }
    uint8_t attr; uint16_t cluster; uint32_t size;
    if (fat_resolve(path, &attr, &cluster, &size) < 0) return -2;
    linux_fill_stat(st, (attr & 0x10) ? LINUX_FD_DIR : LINUX_FD_FILE, cluster, size);
    return 0;
}

static int linux_fstat(int fd, void *statbuf) {
    if (!statbuf) return -14;
    if (fd >= 0 && fd <= 2) { linux_fill_stat((struct linux_stat *)statbuf, LINUX_FD_NULL, 0, 0); return 0; }
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    return linux_stat_path(g_linux_fds[fd].path, (struct linux_stat *)statbuf);
}

static int linux_ftruncate(int fd, uint64_t size) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind != LINUX_FD_FILE) return -22;
    if (!(g_linux_fds[fd].flags & LINUX_O_ACCMODE)) return -22;
    if (size > 0xFFFFFFFFULL) return -27;
    return fat_truncate(g_linux_fds[fd].path, (uint32_t)size) < 0 ? -5 : 0;
}

static int linux_newfstatat(int dirfd, const char *path, void *statbuf, int flags) {
    (void)flags;
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    return linux_stat_path(full, (struct linux_stat *)statbuf);
}

static int linux_getdents64(int fd, void *buf, uint32_t len) {
    if (fd < 3 || fd >= LINUX_FD_MAX || !g_linux_fds[fd].used) return -9;
    if (g_linux_fds[fd].kind != LINUX_FD_DIR) return -20;
    uint8_t *out = (uint8_t *)buf; uint32_t used = 0;
    for (;;) {
        char name[16]; uint8_t attr; uint16_t cluster; uint32_t size;
        if (fat_list(g_linux_fds[fd].dir_cluster, (int)g_linux_fds[fd].offset,
                     name, &attr, &cluster, &size) < 0) break;
        (void)size;
        uint32_t name_len = 0; while (name[name_len]) name_len++;
        uint16_t reclen = (uint16_t)((19 + name_len + 1 + 7) & ~7U);
        if (used + reclen > len) break;
        uint8_t *entry = out + used; memset(entry, 0, reclen);
        *(uint64_t *)(entry + 0) = cluster ? cluster : 1;
        *(int64_t *)(entry + 8) = (int64_t)g_linux_fds[fd].offset + 1;
        *(uint16_t *)(entry + 16) = reclen;
        entry[18] = (attr & 0x10) ? 4 : 8;
        for (uint32_t i = 0; i <= name_len; i++) entry[19 + i] = (uint8_t)name[i];
        used += reclen; g_linux_fds[fd].offset++;
    }
    return (int)used;
}

static int linux_mkdirat(int dirfd, const char *path) {
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    return fat_mkdir(full) < 0 ? -17 : 0;
}

static int linux_unlinkat(int dirfd, const char *path, int flags) {
    if (flags) return -22;
    char full[80]; int result = linux_resolve_at(dirfd, path, full);
    if (result < 0) return result;
    result = fat_unlink(full);
    return result == -2 ? -21 : result < 0 ? -2 : 0;
}

static uint64_t linux_getcwd(char *buf, uint64_t size) {
    if (!buf || !size) return (uint64_t)-22;
    char cwd[80]; fs_getcwd(cwd);
    uint64_t len = 0; while (cwd[len]) len++;
    if (len + 1 > size) return (uint64_t)-34;
    for (uint64_t i = 0; i <= len; i++) buf[i] = cwd[i];
    return (uint64_t)buf;
}

static int linux_arch_prctl(int code, uint64_t addr) {
    if (code == 0x1002) {                   /* ARCH_SET_FS */
        if (addr >= 0x0000800000000000ULL) return -22;
        write_fs_base(addr);
        return 0;
    }
    if (code == 0x1003) {                   /* ARCH_GET_FS */
        if (!addr) return -14;
        *(uint64_t *)addr = g_linux_fs_base;
        return 0;
    }
    return -22;
}

struct linux_timespec { int64_t sec, nsec; };

static int linux_clock_gettime(int clock_id, void *timespec) {
    if (!timespec) return -14;
    if (clock_id != 0 && clock_id != 1 && clock_id != 4) return -22;
    uint64_t ms = millis();
    struct linux_timespec *ts = (struct linux_timespec *)timespec;
    ts->sec = (int64_t)(ms / 1000);
    ts->nsec = (int64_t)((ms % 1000) * 1000000);
    return 0;
}

static uint64_t linux_random_state(void) {
    static uint64_t state;
    if (!state) {
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        state = ((uint64_t)hi << 32) | lo;
        state ^= (uint64_t)(uintptr_t)&state ^ millis();
    }
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}

static int linux_getrandom(void *buf, uint32_t len, uint32_t flags) {
    if (!buf || (flags & ~3U)) return -22;
    uint8_t *out = (uint8_t *)buf;
    uint64_t random = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (!(i & 7)) random = linux_random_state();
        out[i] = (uint8_t)(random >> ((i & 7) * 8));
    }
    return (int)len;
}

struct linux_iovec { const void *base; uint64_t len; };

static int linux_writev(int fd, const void *iov, int count) {
    if (!iov || count < 0 || count > 1024) return -22;
    const struct linux_iovec *vec = (const struct linux_iovec *)iov;
    int total = 0;
    for (int i = 0; i < count; i++) {
        if (vec[i].len > 0x7FFFFFFFULL) return total ? total : -22;
        int result = linux_write(fd, vec[i].base, (uint32_t)vec[i].len);
        if (result < 0) return total ? total : result;
        total += result;
        if ((uint64_t)result != vec[i].len) break;
    }
    return total;
}

static int linux_ioctl(int fd, uint64_t request, void *arg) {
    if (fd < 0 || fd > 2) return -25;
    if (request == 0x5413) {                /* TIOCGWINSZ */
        if (!arg) return -14;
        struct { uint16_t rows, cols, xpixels, ypixels; } *ws = arg;
        int cols = 0, rows = 0; term_active_size(&cols, &rows);
        ws->rows = (uint16_t)rows; ws->cols = (uint16_t)cols;
        ws->xpixels = 0; ws->ypixels = 0;
        return 0;
    }
    if (request == 0x5401) {                /* TCGETS */
        if (!arg) return -14;
        memset(arg, 0, 60);
        return 0;
    }
    return -25;
}

static uint64_t *linux_vm_pt(uint64_t va, int create) {
    if (!g_linux_vm.pml4) return 0;
    uint64_t *table = g_linux_vm.pml4;
    int shifts[3] = { 39, 30, 21 };
    for (int level = 0; level < 3; level++) {
        int index = (int)((va >> shifts[level]) & 0x1FF);
        if (!(table[index] & PTE_P)) {
            if (!create) return 0;
            uint64_t *next = copy_page_table(0);
            if (!next) return 0;
            table[index] = make_table_entry(next);
        } else if (table[index] & PTE_PS) {
            return 0;
        }
        table = (uint64_t *)(table[index] & ~0xFFFULL);
    }
    return table;
}

static uint64_t *linux_vm_pte(uint64_t va, int create) {
    uint64_t *pt = linux_vm_pt(va, create);
    return pt ? &pt[(va >> 12) & 0x1FF] : 0;
}

static int linux_map_page(uint64_t va, uint64_t prot) {
    uint64_t *pte = linux_vm_pte(va, 1);
    if (!pte || (*pte & ~0xFFFULL)) return -1;
    void *frame = page_alloc(1);
    if (!frame) return -1;
    *pte = (uint64_t)frame | PTE_US | (prot ? PTE_P : 0) | ((prot & 2) ? PTE_RW : 0);
    return 0;
}

static uint64_t page_up(uint64_t value) { return (value + 4095) & ~4095ULL; }

static int linux_vm_range_owned(uint64_t addr, uint64_t len) {
    if (!len || addr + len < addr) return 0;
    uint64_t end = addr + len;
    uint64_t brk_top = page_up(g_linux_vm.brk);
    if (addr >= LINUX_BRK_BASE && end <= brk_top) return 1;
    return addr >= LINUX_MMAP_BASE && end <= g_linux_vm.mmap_next;
}

static uint64_t linux_brk(uint64_t requested) {
    if (!g_linux_vm.pml4) return (uint64_t)-12;
    if (!requested) return g_linux_vm.brk;
    if (requested < LINUX_BRK_BASE || requested > LINUX_BRK_LIMIT) return g_linux_vm.brk;
    uint64_t old_top = page_up(g_linux_vm.brk);
    uint64_t new_top = page_up(requested);
    if (new_top > old_top) {
        uint64_t va = old_top;
        for (; va < new_top; va += 4096) {
            if (linux_map_page(va, 3) == 0) continue;
            for (uint64_t rollback = old_top; rollback < va; rollback += 4096) {
                uint64_t *pte = linux_vm_pte(rollback, 0);
                if (pte) *pte = 0;
            }
            flush_tlb();
            return g_linux_vm.brk;
        }
    } else {
        for (uint64_t va = new_top; va < old_top; va += 4096) {
            uint64_t *pte = linux_vm_pte(va, 0);
            if (pte) *pte = 0;
        }
        flush_tlb();
    }
    g_linux_vm.brk = requested;
    return requested;
}

static uint64_t linux_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                           uint64_t flags, uint64_t fd, uint64_t offset) {
    if (!g_linux_vm.pml4 || addr || !len || len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE ||
        (flags & 0x22) != 0x22 || fd != (uint64_t)-1 || offset)
        return (uint64_t)-22;
    len = page_up(len);
    uint64_t base = page_up(g_linux_vm.mmap_next);
    if (base > LINUX_MMAP_LIMIT || len > LINUX_MMAP_LIMIT - base) return (uint64_t)-12;
    uint64_t va = base;
    for (; va < base + len; va += 4096) {
        if (linux_map_page(va, prot) == 0) continue;
        for (uint64_t rollback = base; rollback < va; rollback += 4096) {
            uint64_t *pte = linux_vm_pte(rollback, 0);
            if (pte) *pte = 0;
        }
        flush_tlb();
        return (uint64_t)-12;
    }
    g_linux_vm.mmap_next = base + len;
    flush_tlb();
    return base;
}

static int linux_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    if (!g_linux_vm.pml4 || (addr & 4095) || !len ||
        len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE) return -22;
    len = page_up(len);
    if (!linux_vm_range_owned(addr, len)) return -12;
    for (uint64_t va = addr; va < addr + len; va += 4096) {
        uint64_t *pte = linux_vm_pte(va, 0);
        if (!pte || !(*pte & ~0xFFFULL)) return -12;
        *pte = (*pte & ~(PTE_P | PTE_RW)) | (prot ? PTE_P : 0) |
               ((prot & 2) ? PTE_RW : 0) | PTE_US;
    }
    flush_tlb();
    return 0;
}

static int linux_munmap(uint64_t addr, uint64_t len) {
    if (!g_linux_vm.pml4 || (addr & 4095) || !len ||
        len > LINUX_MMAP_LIMIT - LINUX_MMAP_BASE) return -22;
    len = page_up(len);
    if (!linux_vm_range_owned(addr, len)) return -12;
    for (uint64_t va = addr; va < addr + len; va += 4096) {
        uint64_t *pte = linux_vm_pte(va, 0);
        if (pte) *pte = 0;
    }
    flush_tlb();
    return 0;
}

static uint64_t *linux_address_space(const uint64_t code_pte[USER_CODE_PAGES],
                                     uint8_t *code, uint8_t *stk) {
    uint64_t *current = (uint64_t *)(read_cr3() & ~0xFFFULL);
    uint64_t *pml4 = copy_page_table(current);
    if (!pml4) return 0;

    int l4 = (int)((LINUX_LOAD_VA >> 39) & 0x1FF);
    int l3 = (int)((LINUX_LOAD_VA >> 30) & 0x1FF);
    int l2 = (int)((LINUX_LOAD_VA >> 21) & 0x1FF);
    uint64_t *old_pdpt = (uint64_t *)(current[l4] & ~0xFFFULL);
    if (!(current[l4] & PTE_P) || (current[l4] & PTE_PS)) return 0;
    uint64_t *pdpt = copy_page_table(old_pdpt);
    if (!pdpt || !(old_pdpt[l3] & PTE_P) || (old_pdpt[l3] & PTE_PS)) return 0;
    uint64_t *old_pd = (uint64_t *)(old_pdpt[l3] & ~0xFFFULL);
    uint64_t *pd = copy_page_table(old_pd);
    uint64_t *code_pt = copy_page_table(0);
    if (!pd || !code_pt) return 0;
    pml4[l4] = make_table_entry(pdpt);
    pdpt[l3] = make_table_entry(pd);
    pd[l2] = make_table_entry(code_pt);
    int code_index = (int)((LINUX_LOAD_VA >> 12) & 0x1FF);
    for (int i = 0; i < USER_CODE_PAGES; i++) {
        if (code_pte[i])
            code_pt[code_index + i] = (uint64_t)(code + (uint64_t)i * 4096) | code_pte[i];
    }

    int s4 = (int)((LINUX_STK_TOP >> 39) & 0x1FF);
    int s3 = (int)((LINUX_STK_TOP >> 30) & 0x1FF);
    int s2 = (int)((LINUX_STK_TOP >> 21) & 0x1FF);
    if (current[s4] & PTE_P) return 0;
    uint64_t *stack_pdpt = copy_page_table(0);
    uint64_t *stack_pd = copy_page_table(0);
    uint64_t *stack_pt = copy_page_table(0);
    if (!stack_pdpt || !stack_pd || !stack_pt) return 0;
    pml4[s4] = make_table_entry(stack_pdpt);
    stack_pdpt[s3] = make_table_entry(stack_pd);
    stack_pd[s2] = make_table_entry(stack_pt);
    int stack_index = (int)((LINUX_STK_TOP >> 12) & 0x1FF);
    for (int i = 0; i < USER_STK_PAGES; i++)
        stack_pt[stack_index - 1 - i] = (uint64_t)(stk + (uint64_t)i * 4096) | PTE_P | PTE_RW | PTE_US;

    pml4[(USER_CODE_VA >> 39) & 0x1FF] = 0;
    pml4[(LINUX_BRK_BASE >> 39) & 0x1FF] = 0;
    pml4[(LINUX_MMAP_BASE >> 39) & 0x1FF] = 0;
    return pml4;
}

static int run_linux_elf_frames(const uint8_t *img, uint64_t size,
                                uint8_t *code, uint8_t *stk, uint8_t *kstack_top) {
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;
    uint64_t cap = (uint64_t)USER_CODE_PAGES * 4096;
    if (!code || !stk || size < sizeof(*eh)) return -1;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != 2 || eh->ident[5] != 1 ||
        eh->type != 2 || eh->machine != 62 || eh->version != 1 ||
        eh->phentsize != sizeof(struct elf64_phdr) || !eh->phnum ||
        eh->phoff > size || (uint64_t)eh->phnum > (size - eh->phoff) / eh->phentsize)
        return -2;

    memset(code, 0, cap);
    uint64_t code_pte[USER_CODE_PAGES];
    for (int i = 0; i < USER_CODE_PAGES; i++) code_pte[i] = 0;
    const struct elf64_phdr *ph = (const struct elf64_phdr *)(img + eh->phoff);
    int loads = 0; uint64_t phdr_addr = 0;
    for (uint16_t i = 0; i < eh->phnum; i++) {
        if (ph[i].type != 1) continue;
        if (ph[i].filesz > ph[i].memsz || ph[i].offset > size ||
            ph[i].filesz > size - ph[i].offset || ph[i].vaddr < LINUX_LOAD_VA ||
            ph[i].memsz > cap || ph[i].vaddr - LINUX_LOAD_VA > cap - ph[i].memsz)
            return -3;
        uint64_t dst = ph[i].vaddr - LINUX_LOAD_VA;
        for (uint64_t j = 0; j < ph[i].filesz; j++) code[dst + j] = img[ph[i].offset + j];
        uint64_t first_page = dst / 4096;
        uint64_t end_page = page_up(dst + ph[i].memsz) / 4096;
        for (uint64_t page = first_page; page < end_page; page++)
            code_pte[page] |= PTE_P | PTE_US | ((ph[i].flags & 2) ? PTE_RW : 0);
        if (eh->phoff >= ph[i].offset &&
            eh->phoff + (uint64_t)eh->phnum * eh->phentsize <= ph[i].offset + ph[i].filesz)
            phdr_addr = ph[i].vaddr + eh->phoff - ph[i].offset;
        loads++;
    }
    if (!loads || !phdr_addr || eh->entry < LINUX_LOAD_VA || eh->entry >= LINUX_LOAD_VA + cap) return -4;

    uint64_t *process_pml4 = linux_address_space(code_pte, code, stk);
    if (!process_pml4) return -5;
    const char argv0[] = "linux-abi-selftest";
    uint64_t argp = LINUX_STK_TOP - sizeof(argv0);
    char *arg = (char *)(stk + 4096 - sizeof(argv0));
    for (unsigned i = 0; i < sizeof(argv0); i++) arg[i] = argv0[i];
    uint64_t randomp = (argp & ~15ULL) - 16;
    uint8_t *random_bytes = stk + 4096 - (LINUX_STK_TOP - randomp);
    uint64_t random0 = linux_random_state(), random1 = linux_random_state();
    for (int i = 0; i < 8; i++) {
        random_bytes[i] = (uint8_t)(random0 >> (i * 8));
        random_bytes[8 + i] = (uint8_t)(random1 >> (i * 8));
    }
    uint64_t rsp = randomp - 30 * sizeof(uint64_t);
    uint64_t *sp = (uint64_t *)(stk + 4096 - (LINUX_STK_TOP - rsp));
    sp[0] = 1; sp[1] = argp; sp[2] = 0;       /* argc, argv, argv terminator */
    sp[3] = 0;                                /* envp terminator             */
    sp[4] = 3;  sp[5] = phdr_addr;            /* AT_PHDR                      */
    sp[6] = 4;  sp[7] = eh->phentsize;        /* AT_PHENT                     */
    sp[8] = 5;  sp[9] = eh->phnum;            /* AT_PHNUM                     */
    sp[10] = 6; sp[11] = 4096;                /* AT_PAGESZ                    */
    sp[12] = 9; sp[13] = eh->entry;           /* AT_ENTRY                     */
    sp[14] = 11; sp[15] = 0;                  /* AT_UID                       */
    sp[16] = 12; sp[17] = 0;                  /* AT_EUID                      */
    sp[18] = 13; sp[19] = 0;                  /* AT_GID                       */
    sp[20] = 14; sp[21] = 0;                  /* AT_EGID                      */
    sp[22] = 23; sp[23] = 0;                  /* AT_SECURE                    */
    sp[24] = 25; sp[25] = randomp;            /* AT_RANDOM                    */
    sp[26] = 31; sp[27] = argp;               /* AT_EXECFN                    */
    sp[28] = 0; sp[29] = 0;                   /* AT_NULL                      */

    enum user_abi saved_abi = g_user_abi;
    int saved_exit = g_user_exit_code;
    g_user_abi = USER_ABI_LINUX;
    g_user_exit_code = 0;
    g_linux_vm.pml4 = process_pml4;
    g_linux_vm.brk = LINUX_BRK_BASE;
    g_linux_vm.mmap_next = LINUX_MMAP_BASE;
    g_linux_sigmask = 0;
    g_linux_clear_tid = 0;
    write_fs_base(0);
    linux_fds_reset();
    arch_set_syscall_stack((uint64_t)kstack_top);
    uint64_t kernel_cr3 = read_cr3();
    write_cr3((uint64_t)process_pml4);
    enter_user(eh->entry, rsp, 0);
    write_cr3(kernel_cr3);
    int exit_code = g_user_exit_code;
    write_fs_base(0);
    g_linux_vm.pml4 = 0;
    g_user_exit_code = saved_exit;
    g_user_abi = saved_abi;
    return exit_code;
}

static int run_linux_elf(const uint8_t *img, uint64_t size) {
    return run_linux_elf_frames(img, size, g_ucode, g_ustk,
                                g_ksys0 + sizeof(g_ksys0));
}

void arch_user_init(void) {
    g_ucode = (uint8_t *)page_alloc(USER_CODE_PAGES); g_ustk = (uint8_t *)page_alloc(USER_STK_PAGES);
    g_ccode = (uint8_t *)page_alloc(USER_CODE_PAGES); g_cstk = (uint8_t *)page_alloc(USER_STK_PAGES);
    if (!g_ucode || !g_ustk || !g_ccode || !g_cstk) { kputs("[user] region alloc failed\n"); g_ucode = 0; return; }

    uint64_t pml4 = read_cr3() & ~0xFFFULL;
    uint64_t cr0  = wp_disable();
    for (int i = 0; i < USER_CODE_PAGES; i++)
        map_user_page(pml4, USER_CODE_VA + (uint64_t)i * 4096, (uint64_t)(g_ucode + i * 4096), PTE_RW | PTE_US);
    for (int i = 0; i < USER_STK_PAGES; i++)
        map_user_page(pml4, USER_STK_TOP - (uint64_t)(i + 1) * 4096, (uint64_t)(g_ustk + i * 4096), PTE_RW | PTE_US);
    wp_restore(cr0);
    flush_tlb();
    g_pt = walk_pt(pml4, USER_CODE_VA);        /* cache leaf PT for fast swaps  */

    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    arch_setup_syscall_msrs();
    kputs("[user] ring-3 program region ready\n");

    blk_init();
    if (fat_mount() == 0) kputs("[fs] fat16 ram disk mounted\n");
    else                  kputs("[fs] fat16 mount failed\n");

    int elf_result = run_linux_elf(linux_hello_elf, sizeof(linux_hello_elf));
    if (elf_result == 0) kputs("[linux-abi] ELF64 loader + SysV stack ok\n");
    else { kputs("[linux-abi] self-test failed: "); kput_u32dec((uint32_t)elf_result); kputc('\n'); }

    int musl_result = arch_user_run("elf-fs-probe", exec_probe_prog,
                                    sizeof(exec_probe_prog), "");
    if (musl_result == 0) kputs("[musl] filesystem libc program exited cleanly\n");
    else { kputs("[musl] filesystem probe failed: "); kput_u32dec((uint32_t)musl_result); kputc('\n'); }
}

/* ---- process table: the ring-3 exec stack (surfaced to `ps` via SYS_TASKINFO) */
#define PROC_MAXLVL 2
static char g_pname[PROC_MAXLVL][16];
static int  g_ppid[PROC_MAXLVL];
static int  g_plevels;                          /* active ring-3 nesting levels */

/* i=0 -> the kernel; i=1..g_plevels -> the ring-3 exec stack.  0 ok, -1 at end. */
static int proc_info(int i, int *pid, int *ring, int *state, char *name) {
    if (i == 0) {
        *pid = 0; *ring = 0; *state = 0;
        const char *k = "kernel"; int j = 0; while (k[j]) { name[j] = k[j]; j++; } name[j] = 0;
        return 0;
    }
    int lvl = i - 1;
    if (lvl < 0 || lvl >= g_plevels) return -1;
    *pid = g_ppid[lvl]; *ring = 3;
    *state = (lvl == g_plevels - 1) ? 0 : 1;    /* innermost runs, callers wait */
    int j = 0; while (g_pname[lvl][j] && j < 15) { name[j] = g_pname[lvl][j]; j++; } name[j] = 0;
    return 0;
}

/* load an image into a frame set and run it to completion in ring 3.  argv[0]
 * (the program's single string argument) is staged in the code-region tail while
 * the CALLER is still mapped, then handed to umain via rdi (see trap64.s). */
static int run_prog(const char *name, const uint8_t *img, uint64_t size,
                    uint8_t *code, uint8_t *stk, uint8_t *kstack_top, const char *arg) {
    uint64_t cap = (uint64_t)USER_CODE_PAGES * 4096;
    if (size > cap) return -2;
    for (uint64_t i = 0; i < size; i++) code[i] = img[i];
    for (uint64_t i = size; i < cap; i++) code[i] = 0;      /* zero tail (.bss) */
    int ai = 0;                                             /* stage argv[0]     */
    if (arg) while (arg[ai] && ai < 255) { code[USER_ARG_OFF + ai] = (uint8_t)arg[ai]; ai++; }
    code[USER_ARG_OFF + ai] = 0;
    map_region(code, stk);
    arch_set_syscall_stack((uint64_t)kstack_top);
    int saved_exit = g_user_exit_code, saved_pid = g_cur_pid;
    g_user_exit_code = 0; g_cur_pid++;
    int lvl = g_depth;                                      /* record for `ps`   */
    if (lvl >= 0 && lvl < PROC_MAXLVL) {
        int j = 0; while (name[j] && j < 15) { g_pname[lvl][j] = name[j]; j++; } g_pname[lvl][j] = 0;
        g_ppid[lvl] = g_cur_pid;
    }
    int saved_levels = g_plevels; g_plevels = lvl + 1;
    enter_user(USER_CODE_VA, USER_STK_TOP - 16, USER_CODE_VA + USER_ARG_OFF);
    g_plevels = saved_levels;
    int ec = g_user_exit_code;
    g_user_exit_code = saved_exit; g_cur_pid = saved_pid;
    return ec;
}

/* primary program launched from the kernel console (`run`), nesting level 0 */
static int arch_user_run(const char *name, const uint8_t *img, uint64_t size, const char *arg) {
    if (!g_ucode) return -1;
    g_depth = 0;
    int ec = run_prog(name, img, size, g_ucode, g_ustk, g_ksys0 + sizeof(g_ksys0), arg);
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    return ec;
}

struct uprog_ent { const char *name; const unsigned char *img; unsigned size; };
static const struct uprog_ent g_uprogs[] = {
    { "hello", hello_prog, (unsigned)sizeof(hello_prog) },
    { "count", count_prog, (unsigned)sizeof(count_prog) },
    { "greet", greet_prog, (unsigned)sizeof(greet_prog) },
    { "sh",    sh_prog,    (unsigned)sizeof(sh_prog)    },
    { "vi",    vi_prog,    (unsigned)sizeof(vi_prog)    },
};

static int name_eq(const char *a, const char *b) {
    int j = 0; while (a[j] && a[j] == b[j]) j++;
    return a[j] == 0 && b[j] == 0;
}

static int find_prog(const char *name) {
    for (unsigned i = 0; i < sizeof(g_uprogs) / sizeof(g_uprogs[0]); i++)
        if (name_eq(g_uprogs[i].name, name)) return (int)i;
    return -1;
}

static int run_linux_path(const char *path, uint8_t *code, uint8_t *stk,
                          uint8_t *kstack_top) {
    char full[80];
    while (path[0] == '.' && path[1] == '/') path += 2;
    fs_resolve(path, full);

    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
    if (fat_resolve(full, &attr, &cluster, &size) < 0 || (attr & 0x10) || !size)
        return -1;

    uint8_t *image = (uint8_t *)kmalloc(size);
    if (!image) return -5;
    int read = fat_read(cluster, size, image, size);
    int result = read == (int)size
        ? run_linux_elf_frames(image, size, code, stk, kstack_top) : -1;
    kfree(image);
    return result;
}

int arch_user_run_named(const char *name, const char *arg) {
    if (name_eq(name, "musl")) {
        (void)arg;
        return run_linux_elf(musl_probe_elf, sizeof(musl_probe_elf));
    }
    int idx = find_prog(name);
    if (idx < 0)
        return run_linux_path(name, g_ucode, g_ustk, g_ksys0 + sizeof(g_ksys0));
    return arch_user_run(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size, arg);
}

const char *arch_user_prog_name(int i) {
    int count = (int)(sizeof(g_uprogs) / sizeof(g_uprogs[0]));
    if (i == count) return "musl";
    return (i >= 0 && i < count) ? g_uprogs[i].name : 0;
}

/* SYS_EXEC: run a named program as a nested (level-1) child of the caller.  Only
 * one level of nesting is supported (a shell launching a program). */
static int arch_exec_named(const char *name, const char *arg) {
    if (g_depth != 0 || !g_ucode) return -1;
    int idx = find_prog(name);
    g_depth = 1;
    int ec;
    if (idx >= 0)
        ec = run_prog(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size,
                      g_ccode, g_cstk, g_ksys1 + sizeof(g_ksys1), arg);
    else
        ec = run_linux_path(name, g_ccode, g_cstk, g_ksys1 + sizeof(g_ksys1));
    map_region(g_ucode, g_ustk);               /* remap the window back to the parent */
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    g_depth = 0;
    return ec;
}
