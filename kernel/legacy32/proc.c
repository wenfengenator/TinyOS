/* tinyos process management (hardware path): exec, fork, wait.
 *
 * The Unicorn emulator performs the equivalent operations itself at runtime
 * (see tools/emulate.py); this file is the real implementation for QEMU. */
#include "kernel.h"
#include "mm.h"

int rd_get(const char *name, uint8_t **ptr, uint32_t *size);

static uint32_t g_next_pid = 10;

/* load a program image from the ramdisk into a fresh, isolated address space
 * and create a ready task for it.  Returns the new pid, or -1. */
int do_exec(const char *path, const char *arg) {
    uint8_t *img; uint32_t size;
    if (rd_get(path, &img, &size) < 0) return -1;
    int idx = task_alloc();
    if (idx < 0) return -1;

    uint32_t pd = vm_create_user();                    /* supervisor kernel + empty user */
    for (uint32_t off = 0; off < size; off += PAGE_SIZE) {
        uint32_t fr = frame_alloc();
        uint32_t n = size - off; if (n > PAGE_SIZE) n = PAGE_SIZE;
        memcpy((void *)fr, img + off, n);
        vm_map(pd, USER_BASE + off, fr, PAGE_USER | PAGE_RW);
    }
    for (int i = 1; i <= 4; i++)                        /* 16 KB user stack */
        vm_map(pd, USER_STACK_TOP - (uint32_t)i * PAGE_SIZE, frame_alloc(), PAGE_USER | PAGE_RW);

    uint32_t af = frame_alloc();                        /* argument page @ 0x50000000 */
    char *dst = (char *)af; unsigned k = 0;
    if (arg) while (arg[k] && k < PAGE_SIZE - 1) { dst[k] = arg[k]; k++; }
    dst[k] = 0;
    vm_map(pd, ARG_ADDR, af, PAGE_USER | PAGE_RW);

    task_t *t = &g_tasks[idx];
    memset(t, 0, sizeof(*t));
    t->eip = USER_BASE; t->esp = USER_STACK_TOP - 16; t->ustack = USER_STACK_TOP - 16;
    t->eflags = 0x202; t->state = T_READY;
    t->pid = g_next_pid++; t->ppid = g_tasks[g_current].pid;
    t->cr3 = pd; t->kstack = KSTACK_TOP - (uint32_t)idx * 0x1000u; t->wait_on = -1;
    unsigned ln = kstrlen(path); if (ln > 15) ln = 15;
    memcpy(t->name, path, ln); t->name[ln] = 0;
    if (idx + 1 > g_ntask) g_ntask = idx + 1;
    return (int)t->pid;
}

/* duplicate the current task (address space copied) -> child pid to parent,
 * 0 to the child.  `r` is the parent's live register frame. */
int do_fork(struct regs *r) {
    int idx = task_alloc();
    if (idx < 0) return -1;
    task_t *p = &g_tasks[g_current];
    task_t *c = &g_tasks[idx];
    memset(c, 0, sizeof(*c));
    c->eax = 0;                                        /* child: fork() -> 0 */
    c->ebx = r->ebx; c->ecx = r->ecx; c->edx = r->edx;
    c->esi = r->esi; c->edi = r->edi; c->ebp = r->ebp;
    c->esp = r->useresp; c->eip = r->eip; c->eflags = r->eflags;
    c->pid = g_next_pid++; c->ppid = p->pid;
    c->cr3 = vm_clone(p->cr3);
    c->kstack = KSTACK_TOP - (uint32_t)idx * 0x1000u;
    c->wait_on = -1; c->state = T_READY;
    unsigned ln = kstrlen(p->name); if (ln > 15) ln = 15;
    memcpy(c->name, p->name, ln); c->name[ln] = 0;
    if (idx + 1 > g_ntask) g_ntask = idx + 1;
    return (int)c->pid;                                /* parent sees child pid */
}

