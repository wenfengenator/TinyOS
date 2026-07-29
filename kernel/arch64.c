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

/* ---- user address space layout (high canonical, clear of physical RAM) ---- */
#define USER_CODE_VA    0x0000700000000000ULL
#define USER_STK_TOP    0x0000700000100000ULL
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

uint64_t syscall_c(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2) {
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

int arch_user_run_named(const char *name, const char *arg) {
    int idx = find_prog(name);
    if (idx < 0) return -1;
    return arch_user_run(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size, arg);
}

const char *arch_user_prog_name(int i) {
    return (i >= 0 && i < (int)(sizeof(g_uprogs) / sizeof(g_uprogs[0]))) ? g_uprogs[i].name : 0;
}

/* SYS_EXEC: run a named program as a nested (level-1) child of the caller.  Only
 * one level of nesting is supported (a shell launching a program). */
static int arch_exec_named(const char *name, const char *arg) {
    if (g_depth != 0 || !g_ucode) return -1;
    int idx = find_prog(name);
    if (idx < 0) return -1;
    g_depth = 1;
    int ec = run_prog(g_uprogs[idx].name, g_uprogs[idx].img, g_uprogs[idx].size, g_ccode, g_cstk, g_ksys1 + sizeof(g_ksys1), arg);
    map_region(g_ucode, g_ustk);               /* remap the window back to the parent */
    arch_set_syscall_stack((uint64_t)(g_ksys0 + sizeof(g_ksys0)));
    g_depth = 0;
    return ec;
}
