/* tinyos kernel — shared definitions */
#ifndef TINYOS_KERNEL_H
#define TINYOS_KERNEL_H

#include <stdint.h>

/* ---- physical memory layout ---- */
#define KERNEL_BASE   0x10000u
#define KSTACK_TOP    0x90000u      /* ring-0 stack top                       */
#define VGA_MEM       0xB8000u
#define CURSOR_ADDR   0x600u        /* shared console cursor (cell index)     */

/* per-task ring-3 stack tops: 0x80000, 0x7C000, 0x78000, ... (16 KB apart)  */
#define USTACK_TOP(i) (0x80000u - (uint32_t)(i) * 0x4000u)

/* ---- segment selectors ---- */
#define KCODE_SEL     0x08u
#define KDATA_SEL     0x10u
#define UCODE_SEL     0x1Bu         /* 0x18 | RPL3 */
#define UDATA_SEL     0x23u         /* 0x20 | RPL3 */
#define TSS_SEL       0x28u

/* ---- text attributes ---- */
#define KATTR         0x07          /* kernel: light grey  */
#define UATTR         0x0A          /* user:   light green */
#define LATTR         0x0E          /* login:  yellow      */

/* ---- syscalls (int 0x80; number in eax, args ebx/ecx/edx) ---- */
#define SYS_EXIT      1
#define SYS_READ      3   /* (fd, buf, len)  -> nbytes   fd 0 = stdin        */
#define SYS_WRITE     4   /* (fd, buf, len)  -> nbytes   fd 1 = console      */
#define SYS_OPEN      5   /* (path, flags)   -> fd or -1                     */
#define SYS_CLOSE     6   /* (fd)                                            */
#define SYS_YIELD     7   /* cooperative reschedule                          */
#define SYS_GETPID    8   /* -> pid                                          */
#define SYS_EXEC      9   /* (path) -> pid of new task, or -1                */
#define SYS_SPAWN     10  /* (task_index) -> unblock a preset task           */
#define SYS_STAT      11  /* (path, *size) -> 0/-1                           */
#define SYS_LSEEK     12  /* (fd, off, whence) -> new offset                 */
#define SYS_FORK      13  /* -> child pid (parent) / 0 (child) / -1          */
#define SYS_WAIT      14  /* (pid) -> exit code                              */
#define SYS_READDIR   15  /* (index, namebuf16) -> 0/-1  (fs + ramdisk)      */
#define SYS_TASKINFO  16  /* (index, buf) -> 0/-1  ("pid state name")        */
#define SYS_ECHO      17  /* (mode) 0=echo 1=mask(*) 2=silent                 */
#define SYS_READFILE  18  /* (path, buf, maxlen) -> nbytes (ram or FAT)       */
#define SYS_GETCHAR   19  /* -> one raw key (blocking, no echo)               */
#define SYS_CLEAR     20  /* clear screen, cursor home                        */
#define SYS_GOTOXY    21  /* (row, col) position the cursor                   */
#define SYS_WRITEFILE 22  /* (path, buf, len) -> write a ram-fs file          */

/* extended (non-ASCII) key codes the PS/2 driver emits for arrow/nav keys */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_DEL   0x84
#define KEY_HOME  0x85
#define KEY_END   0x86
#define KEY_PGUP  0x87
#define KEY_PGDN  0x88

#define O_APPEND      1

#define STDIN_FD      0
#define STDOUT_FD     1

/* register frame built by the int-0x80 stub (entry.s) */
struct regs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    uint32_t eip, cs, eflags, useresp, ss;
} __attribute__((packed));

/* ================= tasks / scheduler ================= */
#define NTASK 8
enum task_state { T_UNUSED = 0, T_READY = 1, T_RUNNING = 2, T_BLOCKED = 3, T_EXITED = 4 };

/* NOTE: the first 10 uint32 (the saved context) and the field offsets are
 * known to tools/emulate.py — keep them in sync. */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags; /* context 0..36 */
    uint32_t state;          /* 40 enum task_state */
    uint32_t pid;            /* 44 */
    uint32_t ppid;           /* 48 parent pid */
    uint32_t ustack;         /* 52 ring-3 stack top */
    uint32_t cr3;            /* 56 address space (page directory phys) */
    uint32_t kstack;         /* 60 ring-0 stack top */
    uint32_t exit_code;      /* 64 */
    int32_t  wait_on;        /* 68 pid this task is waiting for, or -1 */
    char     name[16];       /* 72 */
} task_t;                    /* 88 bytes */

extern task_t g_tasks[NTASK];
extern int32_t g_current;
extern int32_t g_ntask;

int   task_alloc(void);      /* find a free task slot, or -1 */
void  task_create(int idx, uint32_t entry, uint32_t pid, const char *name, int state);
int   schedule(void);        /* hardware path: pick next ready task */
void  ctx_switch(struct regs *r, int next);
void  sched_exit(struct regs *r, uint32_t code);
void  sched_yield(struct regs *r);
void  sched_wait(struct regs *r, int pid);
void  timer_tick(struct regs *r);

/* ================= in-RAM filesystem ================= */
#define NFILE 8
#define FNAME 24
#define FSIZE 256
typedef struct {
    char     name[FNAME];
    uint8_t  data[FSIZE];
    uint32_t size;
    uint32_t used;
} file_t;

#define NFD 16
typedef struct {
    int32_t  file;           /* index into g_files, or -1 */
    uint32_t off;
    int32_t  used;
} fd_t;

extern file_t g_files[NFILE];
extern fd_t   g_fds[NFD];

void fs_init(void);
int  fs_open(const char *name, int flags);
int  fs_read(int fd, void *buf, unsigned len);
int  fs_write(int fd, const void *buf, unsigned len);
int  fs_close(int fd);
int  fs_stat(const char *name, uint32_t *size);
int  fs_lseek(int fd, int off, int whence);

/* ================= ramdisk + process management ================= */
uint32_t rd_count(void);
const char *rd_name(uint32_t i);
uint32_t rd_isize(uint32_t i);
int  rd_get(const char *name, uint8_t **ptr, uint32_t *size);
int  block_read(uint32_t block, void *buf, uint32_t nblocks);
int  ata_read(uint32_t lba, uint32_t count, void *buf);
int  ata_write(uint32_t lba, uint32_t count, const void *buf);
int  fat_mount(void);
int  fat_mounted(void);
int  fat_list(uint32_t dir_cluster, int which, char *name, uint8_t *attr, uint16_t *cluster, uint32_t *size);
int  fat_lookup(uint32_t dir_cluster, const char *want, uint8_t *attr, uint16_t *cluster, uint32_t *size);
int  fat_read(uint16_t cluster, uint32_t size, void *buf, uint32_t maxlen);
int  fat_write(const char *sub, const void *buf, uint32_t len);
int  do_exec(const char *path, const char *arg);
int  do_fork(struct regs *r);

/* ================= console ================= */
void console_clear(void);
void console_gotoxy(int row, int col);
int  console_getpos(void);
void console_setpos(int idx);
int  getchar_raw(void);
void serial_init(void);
int  serial_rx(void);
void kprint(const char *s, uint8_t attr);
void kprint_n(const char *s, uint32_t len, uint8_t attr);
int  fs_writefile(const char *path, const void *buf, unsigned len);

/* ================= syscalls / entry ================= */
void syscall_dispatch(struct regs *r);
extern void syscall_stub(void);

/* ================= timer / preemption (hardware path) ================= */
extern volatile uint32_t g_ticks;
void pic_remap(void);
void pit_init(uint32_t hz);
void on_timer(void);
extern void irq0_stub(void);

/* ================= keyboard (IRQ1) ================= */
extern volatile uint8_t  g_kbd[];
extern volatile uint32_t g_kbd_head, g_kbd_tail;
void kbd_push(uint8_t c);
int  kbd_pop(void);
void on_keyboard(void);
extern void irq1_stub(void);
extern void switch_context(uint32_t *save_esp, uint32_t load_esp);

/* freestanding libc bits */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
int   kstrcmp(const char *a, const char *b);
unsigned kstrlen(const char *s);
int   kstrncmp(const char *a, const char *b, unsigned n);
void  kstrcpy(char *d, const char *s);
int   vfs_stat(const char *path, uint32_t *size, int *isdir);
int   vfs_readdir(const char *path, int index, char *name);
int   vfs_readfile(const char *path, void *buf, unsigned maxlen);
int   vfs_writefile(const char *path, const void *buf, unsigned len);

#endif
