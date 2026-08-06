/* SPDX-License-Identifier: GPL-2.0-only */

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
/* The Linux user LOAD window [0x400000,0x600000) is remapped away from identity
 * in every process CR3 (to the program's ELF pages).  A kernel structure whose
 * PHYSICAL address lands in that window is therefore shadowed when the kernel
 * dereferences it (as an identity pointer) while a process CR3 is active — page
 * tables walked during fork/exec/reap would read ELF bytes as PTEs and free live
 * pages.  QEMU hides this only because its arena happens to start above 6 MiB;
 * real firmware places it lower.  Keep the allocator out of that physical hole
 * so every kernel pointer is identity-valid under any CR3. */
#define LOAD_HOLE_LO 0x400000ULL
#define LOAD_HOLE_HI 0x600000ULL
static int run_in_hole(uint64_t start, uint64_t bytes) {
    return start < LOAD_HOLE_HI && start + bytes > LOAD_HOLE_LO;
}

void *page_alloc(uint64_t pages) {
    if (!g_arena || !pages) return 0;
    uint8_t *p = 0;
    uint64_t alo = (uint64_t)g_arena, ahi = alo + g_arena_size, total = g_arena_size / 4096;
    for (page_run **pp = &g_page_free; *pp; ) {
        page_run *r = *pp;
        /* Validate the node before trusting it: a corrupted free list (garbage
         * count/next from a use-after-free elsewhere) would otherwise carve a
         * wild pointer and #GP in the zeroing loop.  Drop the tail and continue. */
        if ((uint64_t)r < alo || (uint64_t)r >= ahi || ((uint64_t)r & 0xFFFu) ||
            r->count == 0 || r->count > total || (uint64_t)r + r->count * 4096u > ahi) {
            kputs("[mm] corrupt page free-list node dropped @"); kput_u64hex((uint64_t)r); kputc('\n');
            *pp = 0;
            break;
        }
        if (run_in_hole((uint64_t)r, r->count * 4096u)) { pp = &r->next; continue; }
        if (r->count == pages) { p = (uint8_t *)r; *pp = r->next; break; }
        if (r->count > pages) { r->count -= pages; p = (uint8_t *)r + r->count * 4096u; break; }
        pp = &r->next;
    }
    if (!p) {
        uint64_t start = align_up((uint64_t)g_brk, 4096);
        if (run_in_hole(start, pages * 4096u)) start = LOAD_HOLE_HI;  /* jump past the LOAD hole */
        if (start + pages * 4096u > ahi) return 0;
        g_brk = (uint8_t *)(start + pages * 4096u);
        p = (uint8_t *)start;
    }
    for (uint64_t i = 0; i < pages * 4096u; i++) p[i] = 0;
    return p;
}

/* Return a page run to the free list (no coalescing); the freed pages hold the
 * list node.  Used to reclaim dead processes' frames.  Rejects out-of-arena and
 * misaligned pointers so a corrupted page-table walk can't link a bogus/aliased
 * page into the allocator (defense in depth). */
void page_free(void *p, uint64_t pages) {
    if (!p || !pages || !g_arena) return;
    uint64_t a = (uint64_t)p;
    if (a & 0xFFF) return;                                          /* not page-aligned */
    if (a < (uint64_t)g_arena || a + pages * 4096u > (uint64_t)g_arena + g_arena_size) return;
    if (run_in_hole(a, pages * 4096u)) return;                     /* never in the LOAD hole */
    /* Reject a double/overlapping free: linking a page that is still in the free
     * list (or overlaps one) makes page_alloc hand the same frame out twice,
     * which is the classic path to a poisoned list.  Log it so the buggy caller
     * is visible instead of corrupting the allocator on real hardware. */
    for (page_run *r = g_page_free; r; r = r->next) {
        uint64_t rs = (uint64_t)r, re = rs + r->count * 4096u;
        if (rs < (uint64_t)g_arena || re > (uint64_t)g_arena + g_arena_size) break;  /* list already bad */
        if (a < re && a + pages * 4096u > rs) {
            /* Load-base-independent caller id: symbolize (ra - pf) against pf's
             * objdump address to name the teardown site doing the double-free. */
            kputs("[mm] double/overlap free rejected @"); kput_u64hex(a);
            kputs(" ra="); kput_u64hex((uint64_t)__builtin_return_address(0));
            kputs(" pf="); kput_u64hex((uint64_t)(void *)page_free); kputc('\n');
            return;
        }
    }
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
