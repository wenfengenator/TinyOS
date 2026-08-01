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
void    *page_base(void)  { return g_arena; }

typedef struct page_run { struct page_run *next; uint64_t count; } page_run;
static page_run *g_page_free;

/* Page allocator: hand out 4KB-aligned, zeroed page runs (for page tables and
 * user frames).  Reuses freed runs (first-fit, carving from a larger run's tail)
 * before bumping, so fork/exit churn does not exhaust the arena. */
void *page_alloc(uint64_t pages) {
    if (!g_arena || !pages) return 0;
    uint8_t *p = 0;
    for (page_run **pp = &g_page_free; *pp; pp = &(*pp)->next) {
        page_run *r = *pp;
        if (r->count == pages) { p = (uint8_t *)r; *pp = r->next; break; }
        if (r->count > pages) { r->count -= pages; p = (uint8_t *)r + r->count * 4096u; break; }
    }
    if (!p) {
        uint64_t start = align_up((uint64_t)g_brk, 4096);
        if (start + pages * 4096u > (uint64_t)g_arena + g_arena_size) return 0;
        g_brk = (uint8_t *)(start + pages * 4096u);
        p = (uint8_t *)start;
    }
    for (uint64_t i = 0; i < pages * 4096u; i++) p[i] = 0;
    return p;
}

/* Return a page run to the free list (no coalescing); the freed pages hold the
 * list node.  Used to reclaim dead processes' frames. */
void page_free(void *p, uint64_t pages) {
    if (!p || !pages) return;
    page_run *r = (page_run *)p;
    r->count = pages;
    r->next = g_page_free;
    g_page_free = r;
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
