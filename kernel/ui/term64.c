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
        t_puts(t, "  ASSET  WIN  RUN <P>  SPAWN <ELF>  DEMO  PONG  SCHED  USCHED  STEST  REBOOT\n");
        t_puts(t, "DESKTOP: F1=FOCUS  F2=MOVE+ARROWS\n");
    } else if (t_ieq(cmd, "clear") || t_ieq(cmd, "cls")) {
        t_clear(t);
    } else if (t_ieq(cmd, "echo")) {
        t_puts(t, args); t_putc(t, '\n');
    } else if (t_ieq(cmd, "ver") || t_ieq(cmd, "about")) {
        t_puts(t, "TINYOS X64 - UEFI LONG MODE\n");
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
    } else if (t_ieq(cmd, "reboot")) {
        t_puts(t, "REBOOTING...\n");
        sleep_ms(300);
        t_outb(0x64, 0xFE);                     /* pulse the 8042 CPU reset line */
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
