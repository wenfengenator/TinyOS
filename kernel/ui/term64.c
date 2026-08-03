/* tinyos x64 terminal widget + built-in command console (Cycle 3).
 *
 * A terminal is a character grid (cols x rows of 8x8 glyphs) that lives inside a
 * window managed by kernel/wm64.c.  Keystrokes routed to it by the window
 * manager go through a tiny line editor; pressing Enter hands the line to the
 * console, whose built-in commands report on the running kernel (heap, cores,
 * uptime, the loaded asset, ...) and can launch the full-screen graphics demo.
 *
 * The 8x8 kernel font only has uppercase glyphs, so text renders folded to upper
 * case; command matching is therefore case-insensitive. */
#include "wm64.h"
#include "arch64.h"
#include "tinygl.h"
#include "fat64.h"                 /* RAM-disk FAT: fat_resolve/fat_read for `mnt put` */

typedef struct {
    boot_info_t *bi;
    int   cols, rows;
    char *grid;            /* cols*rows cells, space-filled                    */
    int   cx, cy;          /* cursor cell                                      */
    char  line[256];       /* current input line                               */
    int   llen;
    int   esc, esc_arg, esc_arg2, esc_semi;    /* ANSI escape state (CSI + up to 2 args) */
} terminal_t;

/* terminal whose console is currently running a ring-3 program; its stdout
 * (user SYS_WRITE) is routed here by usys_write() below. */
static terminal_t *g_active;

static inline void t_outb(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(p));
}

static char t_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static int  t_ieq(const char *a, const char *b) {
    int i = 0; for (; a[i] && b[i]; i++) if (t_lower(a[i]) != t_lower(b[i])) return 0;
    return a[i] == b[i];
}

/* ---- grid output ---- */
static void t_scroll(terminal_t *t) {
    for (int r = 1; r < t->rows; r++)
        for (int c = 0; c < t->cols; c++)
            t->grid[(r - 1) * t->cols + c] = t->grid[r * t->cols + c];
    for (int c = 0; c < t->cols; c++) t->grid[(t->rows - 1) * t->cols + c] = ' ';
    t->cy = t->rows - 1;
}
static void t_newline(terminal_t *t) { t->cx = 0; t->cy++; if (t->cy >= t->rows) t_scroll(t); }
static void t_putc(terminal_t *t, char c) {
    if (t->esc == 1) {                          /* just saw ESC */
        if (c == '[') { t->esc = 2; t->esc_arg = 0; t->esc_arg2 = 0; t->esc_semi = 0; } else t->esc = 0;
        return;
    }
    if (t->esc == 2) {                          /* CSI: ESC [ <n>[;<n>] <cmd> */
        if (c >= '0' && c <= '9') { t->esc_arg = t->esc_arg * 10 + (c - '0'); return; }
        if (c == ';') { t->esc_arg2 = t->esc_arg; t->esc_arg = 0; t->esc_semi = 1; return; }
        int n = t->esc_arg ? t->esc_arg : 1; t->esc = 0;
        if (c == 'D') { t->cx -= n; if (t->cx < 0) t->cx = 0; }                        /* left  */
        else if (c == 'C') { t->cx += n; if (t->cx >= t->cols) t->cx = t->cols - 1; }  /* right */
        else if (c == 'K') { for (int x = t->cx; x < t->cols; x++) t->grid[t->cy * t->cols + x] = ' '; }
        else if (c == 'H' || c == 'f') {        /* cursor to (row;col), 1-based */
            int row = t->esc_semi ? (t->esc_arg2 ? t->esc_arg2 : 1) : (t->esc_arg ? t->esc_arg : 1);
            int col = t->esc_semi ? (t->esc_arg ? t->esc_arg : 1) : 1;
            t->cy = row - 1; if (t->cy < 0) t->cy = 0; if (t->cy >= t->rows) t->cy = t->rows - 1;
            t->cx = col - 1; if (t->cx < 0) t->cx = 0; if (t->cx >= t->cols) t->cx = t->cols - 1;
        }
        else if (c == 'J') {                    /* 2 = whole screen, else cursor->end */
            int end = t->cols * t->rows;
            if (t->esc_arg == 2) { for (int i = 0; i < end; i++) t->grid[i] = ' '; t->cx = t->cy = 0; }
            else { for (int i = t->cy * t->cols + t->cx; i < end; i++) t->grid[i] = ' '; }
        }
        return;
    }
    if (c == 0x1b) { t->esc = 1; return; }      /* ESC begins an escape sequence */
    if (c == '\n') { t_newline(t); return; }
    if (c == '\r') { t->cx = 0; return; }
    if (c == '\t') { int n = 4 - (t->cx & 3); while (n--) t_putc(t, ' '); return; }
    if (c == '\b') { if (t->cx > 0) { t->cx--; t->grid[t->cy * t->cols + t->cx] = ' '; } return; }
    if (t->cx >= t->cols) t_newline(t);
    t->grid[t->cy * t->cols + t->cx] = c; t->cx++;
}
static void t_puts(terminal_t *t, const char *s) { for (; *s; s++) t_putc(t, *s); }
static void t_u32(terminal_t *t, uint32_t v) {
    char b[12]; int n = 0;
    if (!v) { t_putc(t, '0'); return; }
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) t_putc(t, b[n]);
}
static void t_u64hex(terminal_t *t, uint64_t v) {
    t_puts(t, "0x");
    for (int i = 60; i >= 0; i -= 4) { int d = (int)((v >> i) & 0xF); t_putc(t, (char)(d < 10 ? '0' + d : 'A' + d - 10)); }
}
static void t_clear(terminal_t *t) {
    for (int i = 0; i < t->cols * t->rows; i++) t->grid[i] = ' ';
    t->cx = t->cy = 0;
}
static void t_prompt(terminal_t *t) { t_puts(t, "> "); }

/* Launch the musl mini-busybox interactive shell (/MBB.BIN sh) inside this
 * terminal.  mbb polls the keyboard itself (linux_read fd0) and pumps the
 * compositor, so the desktop stays live; its stdout is routed here via g_active.
 * Blocks until the user types 'exit'. */
static void console_run_shell(terminal_t *t) {
    g_active = t;
    int rc = arch_run_linux("/MBB.BIN", "sh");
    g_active = 0;
    if (rc < 0) t_puts(t, "linux shell unavailable (/MBB.BIN missing)\n");
    else        t_puts(t, "\n[back at tinyos console - type 'sh' to re-enter the linux shell]\n");
}

/* desktop autostart hook (wm64.c): drop the terminal straight into the shell */
void term_autostart_shell(void *user) { if (user) console_run_shell((terminal_t *)user); }

/* ring-3 stdout sink (declared in arch64.h): user SYS_WRITE lands here */
void usys_write(const char *p, unsigned long n) {
    if (n) ui_note_activity();                 /* terminal output -> repaint at full rate */
    if (g_active) for (unsigned long i = 0; i < n; i++) t_putc(g_active, p[i]);
    else          for (unsigned long i = 0; i < n; i++) kputc((char)p[i]);
}

/* terminal control for full-screen ring-3 programs (SYS_CLEAR / SYS_GOTOXY) */
void term_clear_active(void) { if (g_active) t_clear(g_active); }
void term_gotoxy_active(int row, int col) {
    terminal_t *t = g_active;
    if (!t) return;
    if (row < 0) row = 0; else if (row >= t->rows) row = t->rows - 1;
    if (col < 0) col = 0; else if (col >= t->cols) col = t->cols - 1;
    t->cy = row; t->cx = col;
}
void term_active_size(int *cols, int *rows) {
    terminal_t *t = g_active;
    if (cols) *cols = t ? t->cols : 80;
    if (rows) *rows = t ? t->rows : 25;
}

/* directory-entry callback for the `esp` command: one line per file/dir */
static void esp_ls_cb(const char *name, uint32_t size, int isdir, uint32_t first, void *ctx) {
    (void)first;
    terminal_t *t = (terminal_t *)ctx;
    t_puts(t, isdir ? "  <DIR>  " : "         ");
    t_puts(t, name);
    if (!isdir) { t_puts(t, "  "); t_u32(t, size); t_puts(t, " B"); }
    t_putc(t, '\n');
}

/* ---- the console: run one command line ---- */
static void console_exec(terminal_t *t, char *line) {
    char *p = line; while (*p == ' ') p++;
    char cmd[32]; int ci = 0;
    while (*p && *p != ' ' && ci < 31) cmd[ci++] = *p++;
    cmd[ci] = 0;
    while (*p == ' ') p++;
    const char *args = p;
    if (!cmd[0]) return;

    if (t_ieq(cmd, "help") || t_ieq(cmd, "?")) {
        t_puts(t, "COMMANDS:\n");
        t_puts(t, "  SH (LINUX SHELL)  HELP  CLEAR  ECHO <T>  VER\n");
        t_puts(t, "  MEM  CPU  UPTIME  TIME\n");
        t_puts(t, "  ASSET  WIN  RUN <P>  SPAWN <ELF>  DEMO  TGL  TGL3D  COMP  BO  PONG  SCHED  USCHED  STEST  EC  DISK  ESP  MNT  USB  INSTALL  REBOOT  POWEROFF\n");
        t_puts(t, "DESKTOP: F1=FOCUS  F2=MOVE+ARROWS\n");
    } else if (t_ieq(cmd, "clear") || t_ieq(cmd, "cls")) {
        t_clear(t);
    } else if (t_ieq(cmd, "echo")) {
        t_puts(t, args); t_putc(t, '\n');
    } else if (t_ieq(cmd, "ver") || t_ieq(cmd, "about")) {
        t_puts(t, "TINYOS X64 v" TINYOS_VERSION " - UEFI LONG MODE\n");
        t_puts(t, "TINY WINDOWING + TERMINAL CONSOLE\n");
    } else if (t_ieq(cmd, "mem")) {
        t_puts(t, "HEAP TOTAL "); t_u32(t, (uint32_t)(heap_total() / (1024 * 1024)));
        t_puts(t, " MB  USED ");  t_u32(t, (uint32_t)(heap_used()  / (1024 * 1024)));
        t_puts(t, " MB\n");
    } else if (t_ieq(cmd, "cpu") || t_ieq(cmd, "cores")) {
        t_puts(t, "CORES ONLINE "); t_u32(t, (uint32_t)smp_online_count());
        t_puts(t, " / ");            t_u32(t, (uint32_t)smp_cpu_count());
        t_putc(t, '\n');
    } else if (t_ieq(cmd, "uptime")) {
        t_puts(t, "UP "); t_u32(t, (uint32_t)(millis() / 1000)); t_puts(t, " S\n");
    } else if (t_ieq(cmd, "time")) {
        t_puts(t, "TSC "); t_u32(t, (uint32_t)(tsc_freq_hz() / 1000000));
        t_puts(t, " MHZ  UPTIME MS "); t_u32(t, (uint32_t)millis()); t_putc(t, '\n');
    } else if (t_ieq(cmd, "asset")) {
        if (t->bi->asset_base) {
            t_puts(t, "ASSET "); t_u32(t, (uint32_t)(t->bi->asset_size / 1024));
            t_puts(t, " KB @ "); t_u64hex(t, t->bi->asset_base); t_putc(t, '\n');
        } else {
            t_puts(t, "NO ASSET LOADED\n");
        }
    } else if (t_ieq(cmd, "win")) {
        int n = wm_window_count(), f = wm_focused_index();
        for (int i = 0; i < n; i++) {
            t_puts(t, i == f ? " * " : "   ");
            t_puts(t, wm_window_title(i)); t_putc(t, '\n');
        }
    } else if (t_ieq(cmd, "run")) {
        if (!args[0]) { t_puts(t, "usage: run <program> [arg]  (try: run hello)\n"); }
        else {
            char pname[32]; int pi = 0;               /* split "prog arg" */
            const char *ap = args;
            while (*ap && *ap != ' ' && pi < 31) pname[pi++] = *ap++;
            pname[pi] = 0;
            while (*ap == ' ') ap++;
            g_active = t;
            int rc = arch_user_run_named(pname, ap);
            g_active = 0;
            if (rc == -1) { t_puts(t, "no such program: "); t_puts(t, pname); t_putc(t, '\n'); }
        }
    } else if (t_ieq(cmd, "spawn")) {
        if (!args[0]) { t_puts(t, "usage: spawn <path> [args]  (runs a Linux ELF as a scheduled process)\n"); }
        else {
            char pth[64]; int pi = 0;                 /* split "path args" */
            const char *ap = args;
            while (*ap && *ap != ' ' && pi < 63) pth[pi++] = *ap++;
            pth[pi] = 0;
            while (*ap == ' ') ap++;
            g_active = t;
            int rc = arch_run_linux(pth, ap);
            g_active = 0;
            if (rc < 0) { t_puts(t, "spawn failed: "); t_puts(t, pth); t_putc(t, '\n'); }
            else { t_puts(t, "exit code "); t_u32(t, (uint32_t)rc); t_putc(t, '\n'); }
        }
    } else if (t_ieq(cmd, "sh") || t_ieq(cmd, "shell") || t_ieq(cmd, "bash")) {
        console_run_shell(t);
    } else if (t_ieq(cmd, "tgl")) {
        t_puts(t, "LAUNCHING TINYGL 2D DEMO - Q/ESC RETURNS\n");
        tgl_demo();
    } else if (t_ieq(cmd, "tgl3d")) {
        t_puts(t, "LAUNCHING TINYGL 3D CUBE - Q/ESC RETURNS\n");
        tgl_mesh_demo();
    } else if (t_ieq(cmd, "comp")) {
        t_puts(t, "LAUNCHING TINYGL COMPOSITOR - Q/ESC RETURNS\n");
        tgl_comp_demo();
    } else if (t_ieq(cmd, "bo")) {
        t_puts(t, "LAUNCHING TINYGL BUFFER-OBJECT DEMO - Q/ESC RETURNS\n");
        tgl_bo_demo();
    } else if (t_ieq(cmd, "demo")) {
        t_puts(t, "LAUNCHING GRAPHICS DEMO - Q/ESC RETURNS\n");
        demo_run(t->bi);
    } else if (t_ieq(cmd, "pong")) {
        t_puts(t, "LAUNCHING PONG - Q/ESC RETURNS\n");
        pong_run(t->bi);
    } else if (t_ieq(cmd, "sched")) {
        t_puts(t, "LAUNCHING PREEMPTIVE SCHEDULER DEMO - Q/ESC RETURNS\n");
        sched_demo();
    } else if (t_ieq(cmd, "usched")) {
        t_puts(t, "SCHED TEST: A=BLOCKING READ, B=FREE-RUN (RING-3 x2)...\n");
        uint32_t a = 0, b = 0, sw = 0;
        int rc = arch_usched_selftest(&a, &b, &sw);
        t_puts(t, "  A(READS)="); t_u32(t, a);
        t_puts(t, " B(RUNS)="); t_u32(t, b);
        t_puts(t, " SWITCHES="); t_u32(t, sw);
        t_putc(t, '\n');
        t_puts(t, rc == 0 ? "  OK: A BLOCKED ON read() WHILE B KEPT RUNNING\n"
                          : "  FAILED\n");
    } else if (t_ieq(cmd, "stest")) {
        t_puts(t, "SCHEDULER + PROCESS TEST SUITE (smoke + stress) - SEE KERNEL LOG\n");
        int s = sched_test_run(1);
        int p = arch_proc_test(1);
        int c = arch_procsched_test();
        int w = arch_waitproc_test();
        int f = arch_fork_test();
        t_puts(t, (s && p && c == 0 && w == 0 && f == 0) ? "  ALL TESTS PASSED\n" : "  SOME TESTS FAILED (SEE KERNEL LOG)\n");
    } else if (t_ieq(cmd, "esp")) {
        if (!esp_mount()) { t_puts(t, "NO ESP (no EFI System Partition found on the disk)\n"); }
        else {
            const char *dir = args[0] ? args : "/";
            uint32_t clus = esp_root_cluster(); int isdir = 1;
            if (!(dir[0] == '/' && dir[1] == 0) && !esp_find(dir, &clus, 0, &isdir)) {
                t_puts(t, "no such path: "); t_puts(t, dir); t_putc(t, '\n');
            } else if (!isdir) {
                t_puts(t, dir); t_puts(t, " is a file\n");
            } else {
                t_puts(t, "ESP "); t_puts(t, dir); t_puts(t, ":\n");
                esp_list(clus, esp_ls_cb, t);
            }
        }
    } else if (t_ieq(cmd, "ec")) {
        uint8_t v;
        if (acpi_ec_read(0, &v) != 0) { t_puts(t, "NO EC\n"); }
        else {
            static const char hx[] = "0123456789ABCDEF";
            for (int base = 0; base < 0x100; base += 0x10) {
                t_putc(t, hx[(base >> 4) & 0xF]); t_putc(t, hx[base & 0xF]); t_putc(t, ':');
                for (int j = 0; j < 16; j++) {
                    t_putc(t, ' ');
                    if (acpi_ec_read((uint8_t)(base + j), &v) == 0) {
                        t_putc(t, hx[v >> 4]); t_putc(t, hx[v & 0xF]);
                    } else t_puts(t, "--");
                }
                t_putc(t, '\n');
            }
        }
    } else if (t_ieq(cmd, "disk")) {
        if (!ahci_present()) ahci_init();
        if (!ahci_present()) { t_puts(t, "NO SATA DISK (AHCI)\n"); }
        else {
            uint64_t ns = ahci_sectors();
            t_puts(t, "SATA DISK "); t_u32(t, (uint32_t)(ns / 2048));
            t_puts(t, " MB  ("); t_u64hex(t, ns); t_puts(t, " SECTORS)\n");
            static uint8_t mbr[512] __attribute__((aligned(16)));
            static uint8_t sec[512] __attribute__((aligned(16)));
            static const char HX[] = "0123456789ABCDEF";
            if (ahci_read(0, 1, mbr) != 0) { t_puts(t, "READ LBA0 FAILED\n"); }
            else if (ahci_read(1, 1, sec) == 0 && sec[0]=='E'&&sec[1]=='F'&&sec[2]=='I'&&sec[3]==' '&&
                     sec[4]=='P'&&sec[5]=='A'&&sec[6]=='R'&&sec[7]=='T') {
                uint64_t parr = 0; uint32_t nparts = 0, psize = 0;
                for (int k = 0; k < 8; k++) parr   |= (uint64_t)sec[72 + k] << (8 * k);
                for (int k = 0; k < 4; k++) nparts |= (uint32_t)sec[80 + k] << (8 * k);
                for (int k = 0; k < 4; k++) psize  |= (uint32_t)sec[84 + k] << (8 * k);
                if (!psize) psize = 128;
                uint32_t per = 512 / psize; if (!per) per = 1;
                t_puts(t, "GPT: "); t_u32(t, nparts); t_puts(t, " ENTRIES\n");
                static uint8_t pb[512] __attribute__((aligned(16)));
                int shown = 0;
                for (uint32_t i = 0; i < nparts && shown < 8; i++) {
                    if (i % per == 0 && ahci_read(parr + i / per, 1, pb) != 0) break;
                    uint8_t *e = pb + (i % per) * psize;
                    int empty = 1; for (int k = 0; k < 16; k++) if (e[k]) { empty = 0; break; }
                    if (empty) continue;
                    uint64_t f = 0, l = 0;
                    for (int k = 0; k < 8; k++) { f |= (uint64_t)e[32 + k] << (8 * k); l |= (uint64_t)e[40 + k] << (8 * k); }
                    t_puts(t, " P"); t_u32(t, i); t_putc(t, ' ');
                    t_u64hex(t, f); t_puts(t, ".."); t_u64hex(t, l); t_putc(t, ' ');
                    for (int c = 0; c < 36; c++) { uint16_t ch = (uint16_t)(e[56 + c*2] | (e[57 + c*2] << 8)); if (!ch) break; if (ch>=0x20&&ch<0x7F) t_putc(t,(char)ch); }
                    t_putc(t, '\n'); shown++;
                }
                if (!shown) t_puts(t, " (no partitions)\n");
            } else if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
                t_puts(t, "MBR PARTITIONS:\n");
                for (int i = 0; i < 4; i++) {
                    const uint8_t *e = mbr + 0x1BE + i * 16;
                    uint8_t type = e[4];
                    uint32_t start = e[8] | (e[9]<<8) | (e[10]<<16) | ((uint32_t)e[11]<<24);
                    uint32_t cnt   = e[12] | (e[13]<<8) | (e[14]<<16) | ((uint32_t)e[15]<<24);
                    if (!type && !start) continue;
                    t_puts(t, " P"); t_u32(t, (uint32_t)i);
                    t_puts(t, " type=0x"); t_putc(t, HX[type>>4]); t_putc(t, HX[type&0xF]);
                    t_puts(t, " start="); t_u32(t, start); t_puts(t, " sect="); t_u32(t, cnt);
                    if (type == 0xEF) t_puts(t, "  <-- EFI");
                    else if (type==0x0C||type==0x0B||type==0x06||type==0x0E) t_puts(t, "  (FAT)");
                    else if (type==0x83) t_puts(t, "  (Linux)");
                    else if (type==0x07) t_puts(t, "  (NTFS)");
                    t_putc(t, '\n');
                }
            } else {
                t_puts(t, "NO GPT/MBR  LBA0:");
                for (int i = 0; i < 16; i++) { t_putc(t, ' '); t_putc(t, HX[mbr[i]>>4]); t_putc(t, HX[mbr[i]&0xF]); }
                t_putc(t, '\n');
            }
        }
    } else if (t_ieq(cmd, "mnt")) {
        if (!dv_mount()) { t_puts(t, "no data partition (run 'install yes' first, then reboot)\n"); }
        else {
            char sub[16], a1[128], a2[128]; int n;
            const char *p = args; while (*p == ' ') p++;
            n = 0; while (*p && *p != ' ' && n < 15) sub[n++] = *p++; sub[n] = 0; while (*p == ' ') p++;
            n = 0; while (*p && *p != ' ' && n < 127) a1[n++] = *p++; a1[n] = 0; while (*p == ' ') p++;
            n = 0; while (*p && n < 127) a2[n++] = *p++; a2[n] = 0;
            uint32_t a2len = 0; while (a2[a2len]) a2len++;

            if (!sub[0] || t_ieq(sub, "ls")) {
                const char *path = a1[0] ? a1 : "/";
                uint32_t c; int isd;
                if (!dv_resolve(path, &c, 0, &isd) || !isd) t_puts(t, "no such directory\n");
                else { char nm[256];
                    for (int i = 0; i < 512; i++) { if (dv_readdir(c, i, nm) != 0) break;
                        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
                        t_puts(t, "  "); t_puts(t, nm); t_putc(t, '\n'); } }
            } else if (t_ieq(sub, "cat")) {
                uint32_t c, sz; int isd;
                if (!dv_resolve(a1, &c, &sz, &isd) || isd) { t_puts(t, "no such file\n"); }
                else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                    if (b) { int r = dv_read(c, sz, b, sz); for (int i = 0; i < r; i++) t_putc(t, (char)b[i]);
                        t_putc(t, '\n'); page_free(b, pg); } }
            } else if (t_ieq(sub, "write")) {
                t_puts(t, dv_write(a1, a2, a2len) == 0 ? "written\n" : "write failed\n");
            } else if (t_ieq(sub, "mkdir")) {
                t_puts(t, dv_mkdir(a1) == 0 ? "created\n" : "mkdir failed\n");
            } else if (t_ieq(sub, "rm")) {
                t_puts(t, dv_unlink(a1) == 0 ? "removed\n" : "rm failed\n");
            } else if (t_ieq(sub, "put")) {                  /* RAM-fs a1 -> data a2 (persist) */
                uint8_t at; uint16_t cl; uint32_t sz;
                if (fat_resolve(a1, &at, &cl, &sz) < 0 || (at & 0x10)) t_puts(t, "no such ramfs file\n");
                else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                    if (b) { int rc = (fat_read(cl, sz, b, sz) == (int)sz) ? dv_write(a2[0] ? a2 : a1, b, sz) : -1;
                        t_puts(t, rc == 0 ? "copied to data\n" : "copy failed\n"); page_free(b, pg); } }
            } else if (t_ieq(sub, "get")) {                  /* data a1 -> RAM-fs a2 */
                uint32_t c, sz; int isd;
                if (!dv_resolve(a1, &c, &sz, &isd) || isd) t_puts(t, "no such data file\n");
                else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                    if (b) { int rc = (dv_read(c, sz, b, sz) == (int)sz) ? (fat_write(a2[0] ? a2 : a1, b, sz) >= 0 ? 0 : -1) : -1;
                        t_puts(t, rc == 0 ? "copied to ramfs\n" : "copy failed\n"); page_free(b, pg); } }
            } else {
                t_puts(t, "usage: mnt [ls|cat|write|put|get|mkdir|rm] <args>\n");
            }
        }
    } else if (t_ieq(cmd, "usb")) {
        fatvol_t *uvp = uv_vol();
        if (!usb_present() && !usb_init()) { t_puts(t, "no USB device (plug in a USB stick and reboot)\n"); }
        else if (!uvp) { t_puts(t, "USB stick has no FAT32 filesystem\n"); }
        else {
            char sub[16], a1[128], a2[128]; int n;
            const char *p = args; while (*p == ' ') p++;
            n = 0; while (*p && *p != ' ' && n < 15) sub[n++] = *p++; sub[n] = 0; while (*p == ' ') p++;
            n = 0; while (*p && *p != ' ' && n < 127) a1[n++] = *p++; a1[n] = 0; while (*p == ' ') p++;
            n = 0; while (*p && n < 127) a2[n++] = *p++; a2[n] = 0;
            uint32_t a2len = 0; while (a2[a2len]) a2len++;

            if (!sub[0] || t_ieq(sub, "ls")) {
                const char *path = a1[0] ? a1 : "/";
                uint32_t c; int isd;
                if (!fv_resolve(uvp, path, &c, 0, &isd) || !isd) t_puts(t, "no such directory\n");
                else { char nm[256];
                    for (int i = 0; i < 512; i++) { if (fv_readdir(uvp, c, i, nm) != 0) break;
                        if (nm[0] == '.' && (nm[1] == 0 || (nm[1] == '.' && nm[2] == 0))) continue;
                        t_puts(t, "  "); t_puts(t, nm); t_putc(t, '\n'); } }
            } else if (t_ieq(sub, "cat")) {
                uint32_t c, sz; int isd;
                if (!fv_resolve(uvp, a1, &c, &sz, &isd) || isd) { t_puts(t, "no such file\n"); }
                else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                    if (b) { int r = fv_read(uvp, c, sz, b, sz); for (int i = 0; i < r; i++) t_putc(t, (char)b[i]);
                        t_putc(t, '\n'); page_free(b, pg); } }
            } else if (t_ieq(sub, "write")) {
                t_puts(t, fv_write(uvp, a1, a2, a2len) == 0 ? "written\n" : "write failed\n");
            } else if (t_ieq(sub, "mkdir")) {
                t_puts(t, fv_mkdir(uvp, a1) == 0 ? "created\n" : "mkdir failed\n");
            } else if (t_ieq(sub, "rm")) {
                t_puts(t, fv_unlink(uvp, a1) == 0 ? "removed\n" : "rm failed\n");
            } else if (t_ieq(sub, "get")) {                  /* USB a1 -> internal data a2 */
                uint32_t c, sz; int isd;
                if (!fv_resolve(uvp, a1, &c, &sz, &isd) || isd) t_puts(t, "no such usb file\n");
                else if (!dv_mount()) t_puts(t, "no data partition (install first)\n");
                else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                    if (b) { int rc = (fv_read(uvp, c, sz, b, sz) == (int)sz) ? dv_write(a2[0] ? a2 : a1, b, sz) : -1;
                        t_puts(t, rc == 0 ? "copied usb -> data\n" : "copy failed\n"); page_free(b, pg); } }
            } else if (t_ieq(sub, "put")) {                  /* internal data a1 -> USB a2 */
                if (!dv_mount()) t_puts(t, "no data partition (install first)\n");
                else { uint32_t c, sz; int isd;
                    if (!dv_resolve(a1, &c, &sz, &isd) || isd) t_puts(t, "no such data file\n");
                    else { uint64_t pg = (sz + 4095) / 4096 + 1; uint8_t *b = (uint8_t *)page_alloc(pg);
                        if (b) { int rc = (dv_read(c, sz, b, sz) == (int)sz) ? fv_write(uvp, a2[0] ? a2 : a1, b, sz) : -1;
                            t_puts(t, rc == 0 ? "copied data -> usb\n" : "copy failed\n"); page_free(b, pg); } } }
            } else if (t_ieq(sub, "wtest")) {               /* raw write->read->verify (non-destructive) */
                uint64_t lba = usb_sectors() ? usb_sectors() - 1 : 0;
                if (a1[0]) { lba = 0; for (const char *q = a1; *q >= '0' && *q <= '9'; q++) lba = lba*10 + (uint32_t)(*q - '0'); }
                static uint8_t save[512], patt[512], back[512]; static uint8_t seq;
                seq++;
                int rs = usb_read(lba, 1, save);
                for (int i = 0; i < 512; i++) patt[i] = (uint8_t)(seq ^ (i * 7 + 0x3D));
                int rw = usb_write(lba, 1, patt);
                for (int i = 0; i < 512; i++) back[i] = 0;
                int rr = usb_read(lba, 1, back);
                int match = 0, unchanged = 0;
                for (int i = 0; i < 512; i++) { if (back[i] == patt[i]) match++; if (back[i] == save[i]) unchanged++; }
                if (rs == 0) usb_write(lba, 1, save);         /* restore original */
                t_puts(t, "wtest lba="); t_u32(t, (uint32_t)lba);
                t_puts(t, " rd="); t_u32(t, (uint32_t)rs); t_puts(t, " wr="); t_u32(t, (uint32_t)rw);
                t_puts(t, " rd2="); t_u32(t, (uint32_t)rr);
                t_puts(t, " match="); t_u32(t, (uint32_t)match);
                t_puts(t, "/512 (unchanged="); t_u32(t, (uint32_t)unchanged); t_puts(t, ")\n");
                t_puts(t, match == 512 ? "  => WRITE OK (persists)\n" :
                          unchanged == 512 ? "  => WRITE DID NOT PERSIST (sector unchanged)\n" :
                          "  => WRITE PARTIAL/WRONG\n");
                t_puts(t, "  wrote:"); for (int i = 0; i < 8; i++) { t_putc(t, ' '); t_u32(t, patt[i]); }
                t_puts(t, "\n  read :"); for (int i = 0; i < 8; i++) { t_putc(t, ' '); t_u32(t, back[i]); }
                t_putc(t, '\n');
            } else {
                t_puts(t, "  get <usbfile> [dataname]  copy USB -> internal data (/mnt)\n");
                t_puts(t, "  put <datafile> [usbname]  copy internal data -> USB\n");
            }
        }
    } else if (t_ieq(cmd, "install")) {
        if (!t->bi->booted_removable) {
            t_puts(t, "install disabled: TinyOS is already installed and running from the internal disk.\n");
            t_puts(t, "To reinstall, boot from the TinyOS USB stick and run 'install yes' from there.\n");
        } else if (!t->bi->efi_base || !t->bi->efi_size) {
            t_puts(t, "installer: loader did not load BOOTX64.EFI (boot from the TinyOS USB)\n");
        } else if (!ahci_present() && !ahci_init()) {
            t_puts(t, "installer: no SATA disk found\n");
        } else if (!t_ieq(args, "yes")) {
            t_puts(t, "*** WARNING ***  'install yes' ERASES the ENTIRE internal disk and\n");
            t_puts(t, "installs TinyOS as the default boot.  ALL DATA (incl. Ubuntu) IS LOST.\n");
            t_puts(t, "EFI image "); t_u32(t, (uint32_t)(t->bi->efi_size / 1024)); t_puts(t, " KB ready.  Type 'install yes' to proceed.\n");
        } else {
            t_puts(t, "installing TinyOS to the internal disk...\n");
            int rc = esp_install((const uint8_t *)t->bi->efi_base, (uint32_t)t->bi->efi_size);
            if (rc == 0) t_puts(t, "INSTALL OK.  Remove the USB and reboot to boot TinyOS from the SSD.\n");
            else { t_puts(t, "INSTALL FAILED rc=-"); t_u32(t, (uint32_t)(-rc)); t_putc(t, '\n'); }
        }
    } else if (t_ieq(cmd, "poweroff") || t_ieq(cmd, "shutdown") || t_ieq(cmd, "off")) {
        t_puts(t, "POWERING OFF...\n");
        sleep_ms(300);
        if (acpi_poweroff() < 0) t_puts(t, "ACPI S5 UNAVAILABLE (POWER OFF MANUALLY)\n");
    } else if (t_ieq(cmd, "reboot")) {
        t_puts(t, "REBOOTING...\n");
        sleep_ms(300);
        acpi_reset();                           /* ACPI reset reg -> 0xCF9 -> 8042 */
    } else {
        t_puts(t, "UNKNOWN: "); t_puts(t, cmd); t_puts(t, "  (TRY HELP)\n");
    }
}

/* ---- window manager callbacks ---- */
void *term_create(boot_info_t *bi, int cols, int rows) {
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    terminal_t *t = (terminal_t *)kmalloc(sizeof(terminal_t));
    if (!t) return 0;
    t->bi = bi; t->cols = cols; t->rows = rows;
    t->grid = (char *)kmalloc((uint64_t)cols * rows);
    if (!t->grid) return 0;
    for (int i = 0; i < cols * rows; i++) t->grid[i] = ' ';
    t->cx = t->cy = 0; t->llen = 0; t->esc = 0;
    t_puts(t, "tinyos x64 terminal\n");
    t_puts(t, "type 'sh' for a linux shell, or 'help' for native commands.\n\n");
    t_prompt(t);
    return t;
}

void term_key(wm_window_t *win, int key) {
    terminal_t *t = (terminal_t *)win->user;
    if (!t) return;
    if (key == '\n' || key == '\r') {
        t->line[t->llen] = 0;
        t_putc(t, '\n');
        console_exec(t, t->line);
        t->llen = 0;
        t_prompt(t);
    } else if (key == '\b') {
        if (t->llen > 0) { t->llen--; t_putc(t, '\b'); }
    } else if (key >= 0x20 && key < 0x7F) {
        if (t->llen < (int)sizeof(t->line) - 1) {
            t->line[t->llen++] = (char)key;
            t_putc(t, (char)key);
        }
    }
    /* arrows / F-keys reach here only when not consumed by the WM; ignore them */
}

void term_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch) {
    (void)cw; (void)ch;
    terminal_t *t = (terminal_t *)win->user;
    if (!t) return;
    int cell = wm_ui_cell(), sc = wm_ui_scale();
    int lineh = wm_ui_lineh();               /* vertical row pitch (adds leading) */
    char rowbuf[256];
    for (int r = 0; r < t->rows; r++) {
        const char *g = &t->grid[r * t->cols];
        int last = -1;
        for (int c = 0; c < t->cols && c < 255; c++) { rowbuf[c] = g[c]; if (g[c] != ' ') last = c; }
        if (last < 0) continue;                 /* blank row: content bg shows through */
        rowbuf[last + 1] = 0;
        fb_text_to_s(back, (uint32_t)cx, (uint32_t)(cy + r * lineh), rowbuf, 0x33FF66, 0x08111C, sc);
    }
    /* blinking block cursor */
    if ((millis() / 500) & 1)
        fb_text_to_s(back, (uint32_t)(cx + t->cx * cell), (uint32_t)(cy + t->cy * lineh), "_", 0xFFFFFF, 0x08111C, sc);
}
