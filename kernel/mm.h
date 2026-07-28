/* tinyos memory management: physical frame allocator + paging. */
#ifndef TINYOS_MM_H
#define TINYOS_MM_H
#include <stdint.h>

#define PAGE_SIZE     4096u
#define PAGE_PRESENT  0x1u
#define PAGE_RW       0x2u
#define PAGE_USER     0x4u

/* Physical layout:
 *   0x0000000 .. 0x0400000   low 4 MB: kernel image, stacks, VGA, tables
 *   0x0400000 .. 0x1000000   frame pool (12 MB) for page tables / user pages
 * The kernel identity-maps all 16 MB, so any physical frame is reachable at
 * its own address; user pages additionally live at high virtual addresses. */
#define PHYS_TOP      0x1000000u          /* 16 MB */
#define FRAME_BASE    0x0400000u          /*  4 MB */

#define USER_BASE     0x40000000u         /* where program images are mapped   */
#define USER_STACK_TOP 0xB0000000u        /* top of each task's user stack     */
#define ARG_ADDR       0x50000000u        /* exec argument string page         */

void     mm_init(void);
uint32_t frame_alloc(void);               /* zeroed 4 KB frame -> phys, or 0   */
void     frame_free(uint32_t pa);

uint32_t vm_create(void);                 /* new page dir (kernel user-mapped)  */
uint32_t vm_create_user(void);            /* isolated space for exec (supervisor kernel) */
void     vm_map(uint32_t pd, uint32_t va, uint32_t pa, uint32_t flags);
uint32_t vm_translate(uint32_t pd, uint32_t va);   /* -> phys or 0             */
uint32_t vm_clone(uint32_t src_pd);       /* copy an address space (for fork)  */
void     vm_switch(uint32_t pd);          /* load CR3                          */
void     paging_enable(void);             /* set CR0.PG (call once)            */

extern uint32_t g_kernel_pd;              /* base page dir, identity low 16 MB */

#endif
