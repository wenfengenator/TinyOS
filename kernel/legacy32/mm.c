/* tinyos memory management: physical frame allocator + paging.
 *
 * The kernel identity-maps the low 16 MB in every address space (so kernel code
 * can reach any physical frame at its own address and the emulator's kernel
 * structures stay physical=virtual).  Each task additionally gets private user
 * pages at high virtual addresses, giving real inter-task isolation. */
#include "kernel.h"
#include "mm.h"

/* ---- physical frame allocator (bitmap over 4..16 MB) ---- */
#define NFRAMES ((PHYS_TOP - FRAME_BASE) / PAGE_SIZE)
uint32_t frame_bitmap[NFRAMES / 32];

static int bit_get(int i) { return (frame_bitmap[i >> 5] >> (i & 31)) & 1; }
static void bit_set(int i) { frame_bitmap[i >> 5] |=  (1u << (i & 31)); }
static void bit_clr(int i) { frame_bitmap[i >> 5] &= ~(1u << (i & 31)); }

uint32_t frame_alloc(void) {
    for (int i = 0; i < (int)NFRAMES; i++)
        if (!bit_get(i)) {
            bit_set(i);
            uint32_t pa = FRAME_BASE + (uint32_t)i * PAGE_SIZE;
            memset((void *)pa, 0, PAGE_SIZE);     /* identity-mapped -> reachable */
            return pa;
        }
    return 0;
}
void frame_free(uint32_t pa) {
    if (pa < FRAME_BASE || pa >= PHYS_TOP) return;
    bit_clr((int)((pa - FRAME_BASE) / PAGE_SIZE));
}

/* ---- paging ---- */
/* base page directory + page tables (identity-map the low 16 MB).
 * Two flavours: *_u are user-accessible (for in-kernel system tasks that live
 * in low memory); *_s are supervisor-only (for exec'd programs, so ring-3 code
 * cannot reach kernel memory or other tasks' frames -> real isolation). */
static uint32_t kpd_u[1024]    __attribute__((aligned(PAGE_SIZE)));
static uint32_t kpt_u[4][1024] __attribute__((aligned(PAGE_SIZE)));
uint32_t kpd_s[1024]    __attribute__((aligned(PAGE_SIZE)));
static uint32_t kpt_s[4][1024] __attribute__((aligned(PAGE_SIZE)));
uint32_t g_kernel_pd;

void mm_init(void) {
    memset(frame_bitmap, 0, sizeof(frame_bitmap));
    for (int t = 0; t < 4; t++) {
        for (int i = 0; i < 1024; i++) {
            uint32_t pa = (uint32_t)t * 0x400000u + (uint32_t)i * PAGE_SIZE;
            kpt_u[t][i] = pa | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            kpt_s[t][i] = pa | PAGE_PRESENT | PAGE_RW;
        }
        kpd_u[t] = (uint32_t)kpt_u[t] | PAGE_PRESENT | PAGE_RW | PAGE_USER;
        kpd_s[t] = (uint32_t)kpt_s[t] | PAGE_PRESENT | PAGE_RW;
    }
    for (int i = 4; i < 1024; i++) { kpd_u[i] = 0; kpd_s[i] = 0; }
    g_kernel_pd = (uint32_t)kpd_u;
}

static uint32_t *pd_entry(uint32_t pd, uint32_t va) { return &((uint32_t *)pd)[va >> 22]; }

void vm_map(uint32_t pd, uint32_t va, uint32_t pa, uint32_t flags) {
    uint32_t *pde = pd_entry(pd, va);
    if (!(*pde & PAGE_PRESENT)) {
        uint32_t pt = frame_alloc();
        *pde = pt | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    }
    uint32_t *pt = (uint32_t *)(*pde & ~0xFFFu);
    pt[(va >> 12) & 0x3FF] = (pa & ~0xFFFu) | (flags & 0xFFF) | PAGE_PRESENT;
}

uint32_t vm_translate(uint32_t pd, uint32_t va) {
    uint32_t pde = *pd_entry(pd, va);
    if (!(pde & PAGE_PRESENT)) return 0;
    uint32_t pte = ((uint32_t *)(pde & ~0xFFFu))[(va >> 12) & 0x3FF];
    if (!(pte & PAGE_PRESENT)) return 0;
    return (pte & ~0xFFFu) | (va & 0xFFFu);
}

uint32_t vm_create(void) {
    uint32_t pd = frame_alloc();
    for (int i = 0; i < 4; i++) ((uint32_t *)pd)[i] = kpd_u[i];   /* user-accessible kernel id-map */
    for (int i = 4; i < 1024; i++) ((uint32_t *)pd)[i] = 0;
    return pd;
}

/* isolated address space for exec'd programs: kernel identity is
 * supervisor-only, so ring-3 code can only touch its own USER pages. */
uint32_t vm_create_user(void) {
    uint32_t pd = frame_alloc();
    for (int i = 0; i < 4; i++) ((uint32_t *)pd)[i] = kpd_s[i];
    for (int i = 4; i < 1024; i++) ((uint32_t *)pd)[i] = 0;
    return pd;
}

/* copy every user mapping (VA >= USER_BASE) into a brand-new address space */
uint32_t vm_clone(uint32_t src) {
    uint32_t dst = vm_create_user();
    for (uint32_t i = USER_BASE >> 22; i < 1024; i++) {
        uint32_t pde = ((uint32_t *)src)[i];
        if (!(pde & PAGE_PRESENT)) continue;
        uint32_t *spt = (uint32_t *)(pde & ~0xFFFu);
        for (int j = 0; j < 1024; j++) {
            if (!(spt[j] & PAGE_PRESENT)) continue;
            uint32_t va = (i << 22) | ((uint32_t)j << 12);
            uint32_t np = frame_alloc();
            memcpy((void *)np, (void *)(spt[j] & ~0xFFFu), PAGE_SIZE);
            vm_map(dst, va, np, PAGE_USER | PAGE_RW);
        }
    }
    return dst;
}

void vm_switch(uint32_t pd) { __asm__ volatile("mov %0, %%cr3" :: "r"(pd) : "memory"); }

void paging_enable(void) {
    vm_switch(g_kernel_pd);
    uint32_t cr0; __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
}
