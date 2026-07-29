/* tinyos x64 heap: a simple allocator over UEFI conventional memory.
 *
 * After ExitBootServices every EfiConventionalMemory region is ours to use, and
 * OVMF identity-maps physical RAM, so we can pick the largest such region and
 * carve a heap out of it.  The allocator is a bump pointer plus a first-fit free
 * list (freed blocks are reused, no coalescing) — enough for the Doom-enabling
 * demo, which allocates a few multi-MB buffers. */
#include "boot64.h"

/* EFI_MEMORY_DESCRIPTOR (we stride the array by the firmware's desc_size). */
typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t phys;
    uint64_t virt;
    uint64_t pages;
    uint64_t attr;
} efi_mdesc_t;

#define EFI_CONVENTIONAL_MEMORY 7
#define HEAP_ALIGN 16u

typedef struct free_blk { uint64_t size; struct free_blk *next; } free_blk;

static uint8_t  *g_arena;          /* start of the heap arena          */
static uint64_t  g_arena_size;     /* total arena bytes                */
static uint8_t  *g_brk;            /* bump pointer                     */
static free_blk *g_free;           /* free list                        */

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

void heap_init(const boot_info_t *bi) {
    g_arena = 0; g_arena_size = 0; g_free = 0;

    if (!bi->mmap_addr || !bi->mmap_desc_size) return;

    /* pick the largest EfiConventionalMemory region at/above 1 MB */
    uint64_t best_base = 0, best_pages = 0;
    for (uint64_t off = 0; off + bi->mmap_desc_size <= bi->mmap_size; off += bi->mmap_desc_size) {
        const efi_mdesc_t *d = (const efi_mdesc_t *)(uintptr_t)(bi->mmap_addr + off);
        if (d->type != EFI_CONVENTIONAL_MEMORY) continue;
        if (d->phys < 0x100000) continue;                 /* skip low memory */
        if (d->pages > best_pages) { best_pages = d->pages; best_base = d->phys; }
    }
    if (!best_pages) return;

    g_arena      = (uint8_t *)(uintptr_t)best_base;
    g_arena_size = best_pages * 4096u;
    g_brk        = g_arena;
    g_free       = 0;
}

uint64_t heap_total(void) { return g_arena_size; }
uint64_t heap_used(void)  { return g_arena ? (uint64_t)(g_brk - g_arena) : 0; }

/* Page allocator: hand out 4KB-aligned, zeroed pages (for page tables and user
 * frames).  Shares the bump pointer with kmalloc; never freed. */
void *page_alloc(uint64_t pages) {
    if (!g_arena || !pages) return 0;
    uint64_t start = align_up((uint64_t)g_brk, 4096);
    uint64_t need  = pages * 4096u;
    if (start + need > (uint64_t)g_arena + g_arena_size) return 0;
    g_brk = (uint8_t *)(start + need);
    uint8_t *p = (uint8_t *)start;
    for (uint64_t i = 0; i < need; i++) p[i] = 0;
    return p;
}

void *kmalloc(uint64_t n) {
    if (!g_arena || !n) return 0;
    uint64_t need = align_up(n, HEAP_ALIGN) + sizeof(free_blk);

    /* first-fit reuse of a freed block */
    free_blk **pp = &g_free;
    for (free_blk *b = g_free; b; pp = &b->next, b = b->next) {
        if (b->size >= need) { *pp = b->next; return (uint8_t *)b + sizeof(free_blk); }
    }
    /* otherwise bump */
    if ((uint64_t)(g_brk - g_arena) + need > g_arena_size) return 0;
    free_blk *blk = (free_blk *)g_brk;
    blk->size = need;
    g_brk += need;
    return (uint8_t *)blk + sizeof(free_blk);
}

void kfree(void *p) {
    if (!p) return;
    free_blk *blk = (free_blk *)((uint8_t *)p - sizeof(free_blk));
    blk->next = g_free;
    g_free = blk;
}
