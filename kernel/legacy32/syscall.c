/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos syscall implementations (hardware path).
 *
 * Reached from entry.s:syscall_stub with a `struct regs *`.  The Unicorn
 * emulator services the same call numbers itself (see tools/emulate.py); this
 * file is what actually runs on QEMU / real hardware. */
#include "kernel.h"

/* echo mode for stdin: 0 = show chars, 1 = mask with '*', 2 = silent */
static int g_echo = 0;

void syscall_dispatch(struct regs *r) {
    switch (r->eax) {
    case SYS_WRITE:
        if ((int)r->ebx == STDOUT_FD || (int)r->ebx == 2) {
            kprint_n((const char *)r->ecx, r->edx, UATTR);
            r->eax = r->edx;
        } else {
            r->eax = (uint32_t)fs_write((int)r->ebx, (const void *)r->ecx, r->edx);
        }
        break;

    case SYS_ECHO: g_echo = (int)r->ebx; r->eax = 0; break;
    case SYS_GETCHAR:   r->eax = (uint32_t)getchar_raw(); break;
    case SYS_CLEAR:     console_clear(); r->eax = 0; break;
    case SYS_GOTOXY:    console_gotoxy((int)r->ebx, (int)r->ecx); r->eax = 0; break;
    case SYS_WRITEFILE: r->eax = (uint32_t)vfs_writefile((const char *)r->ebx, (const void *)r->ecx, r->edx); break;

    case SYS_READ:
        if ((int)r->ebx == STDIN_FD) {
            char *buf = (char *)r->ecx; int max = (int)r->edx;
            int n = 0, cur = 0;
            int start = (g_echo == 0) ? console_getpos() : 0;
            for (;;) {
                int c = kbd_pop();                 /* PS/2 keyboard (IRQ1) */
                if (c < 0) c = serial_rx();         /* or serial console   */
                if (c < 0) { __asm__ volatile("sti; hlt; cli"); continue; }  /* block for input */
                if (c == '\r' || c == '\n') break;

                if (g_echo != 0) {                  /* login: masked/silent, append only */
                    if (c == 8 || c == 127) {
                        if (n) { n--; if (g_echo == 1) { char bs[4] = {'\b',' ','\b',0}; kprint(bs, UATTR); } }
                    } else if (c >= 32 && c < 127) {
                        if (n < max) buf[n++] = (char)c;
                        if (g_echo == 1) kprint("*", UATTR);
                    }
                    continue;                        /* ignore arrows/Delete while masked */
                }

                /* shell (plain echo): full in-line editor with a movable cursor */
                if (c == 8 || c == 127) {            /* Backspace: delete left of cursor */
                    if (cur > 0) { for (int i = cur - 1; i < n - 1; i++) buf[i] = buf[i+1]; n--; cur--; }
                    else continue;
                } else if (c == KEY_DEL) {           /* Delete: remove char at cursor */
                    if (cur < n) { for (int i = cur; i < n - 1; i++) buf[i] = buf[i+1]; n--; }
                    else continue;
                } else if (c == KEY_LEFT)  { if (cur > 0) cur--; else continue; }
                else if (c == KEY_RIGHT)   { if (cur < n) cur++; else continue; }
                else if (c == KEY_HOME)    { cur = 0; }
                else if (c == KEY_END)     { cur = n; }
                else if (c >= 32 && c < 127) {       /* insert a printable char at cursor */
                    if (n >= max) continue;
                    for (int i = n; i > cur; i--) buf[i] = buf[i-1];
                    buf[cur++] = (char)c; n++;
                } else continue;                     /* ignore Up/Down/PgUp/PgDn/etc. */

                console_setpos(start);               /* redraw line + reposition cursor */
                kprint_n(buf, (uint32_t)n, UATTR);
                kprint(" ", UATTR);
                console_setpos(start + cur);
            }
            kprint("\n", UATTR);                    /* echo the newline */
            r->eax = (uint32_t)n;
        } else {
            r->eax = (uint32_t)fs_read((int)r->ebx, (void *)r->ecx, r->edx);
        }
        break;

    case SYS_OPEN:   r->eax = (uint32_t)fs_open((const char *)r->ebx, (int)r->ecx); break;
    case SYS_CLOSE:  r->eax = (uint32_t)fs_close((int)r->ebx);                  break;
    case SYS_GETPID: r->eax = g_tasks[g_current].pid;                          break;
    case SYS_YIELD:  sched_yield(r); r->eax = 0;                                break;

    case SYS_SPAWN: {
        int i = (int)r->ebx;
        if (i >= 0 && i < NTASK && g_tasks[i].state == T_BLOCKED)
            g_tasks[i].state = T_READY;
        r->eax = 0;
        break;
    }

    case SYS_EXEC:  r->eax = (uint32_t)do_exec((const char *)r->ebx, (const char *)r->ecx); break;
    case SYS_FORK:  r->eax = (uint32_t)do_fork(r);                              break;
    case SYS_WAIT:  sched_wait(r, (int)r->ebx);                                 break;
    case SYS_STAT: {
        uint32_t sz = 0; int isdir = 0;
        int rc = vfs_stat((const char *)r->ebx, &sz, &isdir);
        if (rc == 0 && r->ecx) { uint32_t *o = (uint32_t *)r->ecx; o[0] = sz; o[1] = (uint32_t)isdir; }
        r->eax = (uint32_t)rc;
        break;
    }
    case SYS_LSEEK: r->eax = (uint32_t)fs_lseek((int)r->ebx, (int)r->ecx, (int)r->edx);   break;
    case SYS_READFILE: r->eax = (uint32_t)vfs_readfile((const char *)r->ebx, (void *)r->ecx, r->edx); break;

    case SYS_READDIR: {   /* (path, index, namebuf) over the unified VFS */
        r->eax = (uint32_t)vfs_readdir((const char *)r->ebx, (int)r->ecx, (char *)r->edx);
        break;
    }

    case SYS_TASKINFO: {
        uint32_t idx = r->ebx; struct { uint32_t pid, state; char name[16]; } *o = (void *)r->ecx;
        if (idx < (uint32_t)g_ntask && g_tasks[idx].state != T_UNUSED) {
            o->pid = g_tasks[idx].pid; o->state = g_tasks[idx].state;
            memcpy(o->name, g_tasks[idx].name, 16); r->eax = 0;
        } else r->eax = (uint32_t)-1;
        break;
    }

    case SYS_EXIT:
        sched_exit(r, r->ebx);
        break;

    default:
        r->eax = (uint32_t)-1;
        break;
    }
}
