/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 SMP bring-up (R8).
 *
 * Enumerates CPUs from the ACPI MADT, then starts every application processor
 * with INIT-SIPI-SIPI via the local APIC.  Each AP runs ap_trampoline.s up to
 * long mode and calls ap_entry() here, which checks in under a spinlock and
 * parks.  No cross-core scheduler yet (deferred) — this proves bring-up, per-CPU
 * state, and shared-state locking. */
#include "boot64.h"

/* ---- spinlock (xchg-based test-and-set) ---- */
typedef volatile int spinlock_t;
static void spin_lock(spinlock_t *l)   { while (__sync_lock_test_and_set(l, 1)) while (*l) __asm__ volatile("pause"); }
static void spin_unlock(spinlock_t *l) { __sync_lock_release(l); }

static spinlock_t   g_console_lock;
static volatile int g_online = 1;          /* BSP counts itself */
static int          g_cpu_count = 1;
static uint8_t      g_apic_ids[256];
static volatile uint64_t g_lapic = 0xFEE00000ull;

/* ---- local APIC MMIO ---- */
static inline void     lapic_w(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(g_lapic + reg) = v; }
static inline uint32_t lapic_r(uint32_t reg) { return *(volatile uint32_t *)(uintptr_t)(g_lapic + reg); }
static uint32_t lapic_id(void) { return lapic_r(0x20) >> 24; }

static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static void busy(uint32_t n) { for (volatile uint32_t i = 0; i < n; i++) __asm__ volatile("pause"); }

#include "ap_tramp_array.h"      /* static const unsigned char ap_tramp[] */

/* ---- ACPI MADT: collect enabled Local APIC IDs, find the LAPIC base ---- */
static int parse_madt(uint64_t rsdp) {
    if (!rsdp) return 0;
    uint64_t xsdt = *(volatile uint64_t *)(uintptr_t)(rsdp + 24);   /* ACPI 2.0 XSDT */
    if (!xsdt) return 0;
    uint32_t xlen = *(volatile uint32_t *)(uintptr_t)(xsdt + 4);
    uint64_t madt = 0;
    for (uint32_t off = 36; off + 8 <= xlen; off += 8) {
        uint64_t tbl = *(volatile uint64_t *)(uintptr_t)(xsdt + off);
        const volatile char *sig = (const volatile char *)(uintptr_t)tbl;
        if (sig[0] == 'A' && sig[1] == 'P' && sig[2] == 'I' && sig[3] == 'C') { madt = tbl; break; }
    }
    if (!madt) return 0;

    g_lapic = *(volatile uint32_t *)(uintptr_t)(madt + 36);
    uint32_t mlen = *(volatile uint32_t *)(uintptr_t)(madt + 4);

    g_cpu_count = 0;
    for (uint32_t off = 44; off + 2 <= mlen && g_cpu_count < 256;) {
        const volatile uint8_t *e = (const volatile uint8_t *)(uintptr_t)(madt + off);
        uint8_t type = e[0], len = e[1];
        if (len < 2) break;
        if (type == 0) {                          /* Processor Local APIC */
            uint8_t apic_id = e[3];
            uint32_t flags = *(volatile uint32_t *)(uintptr_t)(madt + off + 4);
            if (flags & 1) g_apic_ids[g_cpu_count++] = apic_id;   /* enabled */
        }
        off += len;
    }
    return g_cpu_count > 0;
}

int smp_cpu_count(void)    { return g_cpu_count; }
int smp_online_count(void) { return g_online; }
uint64_t smp_lapic_base(void) { return g_lapic; }

/* ---- AP C entry (reached from the trampoline in long mode) ---- */
void ap_entry(void) {
    arch_load_cpu_tables();          /* valid GDT+IDT so a stray NMI/exception won't triple-fault */
    uint32_t id = lapic_id();
    spin_lock(&g_console_lock);
    g_online++;
    kputs("[smp] AP online: APIC id "); kput_u32dec(id);
    kputs("  ("); kput_u32dec((uint32_t)g_online); kputs(" cores up)\n");
    spin_unlock(&g_console_lock);
    for (;;) __asm__ volatile("hlt");             /* park (no SMP scheduler yet) */
}

/* ---- send INIT-SIPI-SIPI to one AP ---- */
static void start_ap(uint8_t apic_id, uint32_t vector) {
    lapic_w(0x310, (uint32_t)apic_id << 24);
    lapic_w(0x300, 0x00004500);                   /* INIT, assert, edge */
    while (lapic_r(0x300) & (1 << 12)) { }        /* delivery status */
    busy(200000);
    for (int s = 0; s < 2; s++) {                 /* SIPI x2 */
        lapic_w(0x310, (uint32_t)apic_id << 24);
        lapic_w(0x300, 0x00004600 | (vector & 0xFF));
        busy(20000);
        while (lapic_r(0x300) & (1 << 12)) { }
    }
}

void smp_init(boot_info_t *bi) {
    if (!parse_madt(bi->rsdp)) { kputs("[smp] no ACPI MADT; single core\n"); return; }

    uint32_t bsp = lapic_id();
    kputs("[smp] MADT reports "); kput_u32dec((uint32_t)g_cpu_count);
    kputs(" CPUs; LAPIC @ "); kput_u64hex(g_lapic);
    kputs("; BSP APIC id "); kput_u32dec(bsp); kputc('\n');
    if (g_cpu_count <= 1) return;

    lapic_w(0xF0, lapic_r(0xF0) | 0x100);         /* ensure LAPIC software-enabled */

    /* copy the trampoline to 0x8000 and fill the parameter page at 0x9000 */
    volatile uint8_t *dst = (volatile uint8_t *)0x8000;
    for (unsigned i = 0; i < sizeof(ap_tramp); i++) dst[i] = ap_tramp[i];

    void *stacks = kmalloc((uint64_t)g_cpu_count * 0x4000);   /* 16 KiB per AP */
    volatile uint64_t *p = (volatile uint64_t *)0x9000;
    p[0] = read_cr3();                            /* CR3 (PML4) */
    p[1] = (uint64_t)(uintptr_t)&ap_entry;        /* AP entry   */
    p[2] = (uint64_t)(uintptr_t)stacks;           /* stack base */
    p[3] = 0;                                     /* AP index   */

    uint32_t vector = 0x8000 >> 12;               /* SIPI vector -> 0x8000 */
    for (int i = 0; i < g_cpu_count; i++)
        if (g_apic_ids[i] != bsp) start_ap(g_apic_ids[i], vector);

    uint64_t deadline = millis() + 500;
    while (g_online < g_cpu_count && millis() < deadline) __asm__ volatile("pause");

    spin_lock(&g_console_lock);
    kputs("[smp] "); kput_u32dec((uint32_t)g_online);
    kputs(" / ");    kput_u32dec((uint32_t)g_cpu_count); kputs(" cores online\n");
    spin_unlock(&g_console_lock);
}
