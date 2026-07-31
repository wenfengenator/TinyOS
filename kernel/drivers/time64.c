/* tinyos x64 monotonic time: TSC calibrated against the ACPI PM timer.
 *
 * The 8254 PIT is unreliable on some hypervisors (Hyper-V Gen 2 especially), so
 * we calibrate the CPU timestamp counter against the ACPI Power-Management timer
 * — a fixed 3.579545 MHz counter located via the FADT — and fall back to the PIT
 * only if no PM timer exists.  millis()/micros() then derive from rdtsc().  All
 * interrupt-free (polled), so no IDT is required. */
#include "boot64.h"

static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline uint32_t inl(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo;
}

#define PIT_HZ    1193182u
#define PMTMR_HZ  3579545u

static uint64_t g_tsc_hz;
static uint64_t g_base;
static uint32_t g_pmtmr_port;
static int      g_pmtmr_32;

/* ---- ACPI: locate a table by signature through the XSDT ---- */
static uint64_t acpi_find(uint64_t rsdp, const char *sig) {
    if (!rsdp) return 0;
    uint64_t xsdt = *(volatile uint64_t *)(uintptr_t)(rsdp + 24);
    if (!xsdt) return 0;
    uint32_t xlen = *(volatile uint32_t *)(uintptr_t)(xsdt + 4);
    for (uint32_t off = 36; off + 8 <= xlen; off += 8) {
        uint64_t t = *(volatile uint64_t *)(uintptr_t)(xsdt + off);
        const volatile char *s = (const volatile char *)(uintptr_t)t;
        if (s[0] == sig[0] && s[1] == sig[1] && s[2] == sig[2] && s[3] == sig[3]) return t;
    }
    return 0;
}

static void find_pmtmr(uint64_t rsdp) {
    uint64_t fadt = acpi_find(rsdp, "FACP");
    if (!fadt) return;
    g_pmtmr_port = *(volatile uint32_t *)(uintptr_t)(fadt + 76);                 /* PM_TMR_BLK */
    g_pmtmr_32   = (*(volatile uint32_t *)(uintptr_t)(fadt + 112) >> 8) & 1;     /* TMR_VAL_EXT */
    uint32_t flen = *(volatile uint32_t *)(uintptr_t)(fadt + 4);
    if (!g_pmtmr_port && flen >= 216) {                                          /* X_PM_TMR_BLK */
        if (*(volatile uint8_t *)(uintptr_t)(fadt + 208) == 1)                   /* SystemIO */
            g_pmtmr_port = (uint32_t)*(volatile uint64_t *)(uintptr_t)(fadt + 212);
    }
}

static uint32_t pmtmr_read(void) {
    uint32_t v = inl(g_pmtmr_port);
    return g_pmtmr_32 ? v : (v & 0x00FFFFFFu);
}

/* Calibrate the TSC against the PM timer over ~20 ms.  Returns 1 on success. */
static int calibrate_pmtmr(void) {
    if (!g_pmtmr_port) return 0;
    uint32_t mask   = g_pmtmr_32 ? 0xFFFFFFFFu : 0x00FFFFFFu;
    uint32_t target = PMTMR_HZ / 50;                     /* 20 ms of PM ticks */
    uint32_t start  = pmtmr_read();
    uint64_t tsc0   = rdtsc();
    uint32_t elapsed = 0, guard = 0;
    while (elapsed < target) {
        elapsed = (pmtmr_read() - start) & mask;
        if (++guard > 50000000u) return 0;              /* timer not advancing */
    }
    uint64_t tsc1 = rdtsc();
    g_tsc_hz = (tsc1 - tsc0) * (uint64_t)PMTMR_HZ / elapsed;
    return g_tsc_hz > 1000000ull;
}

/* Fallback: calibrate against PIT channel 2 (unreliable on some hypervisors). */
static int calibrate_pit(void) {
    uint8_t p61 = inb(0x61);
    outb(0x61, (uint8_t)((p61 & ~0x02) | 0x01));        /* gate on, speaker off */
    outb(0x43, 0xB0);                                   /* ch2, lo/hi, mode 0   */
    uint16_t count = (uint16_t)(PIT_HZ / 100);          /* ~10 ms               */
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)(count >> 8));
    p61 = inb(0x61);
    outb(0x61, (uint8_t)(p61 & ~0x01));
    outb(0x61, (uint8_t)(p61 | 0x01));                  /* restart the counter  */
    uint64_t t0 = rdtsc();
    uint32_t guard = 0;
    while (!(inb(0x61) & 0x20)) { if (++guard > 50000000u) return 0; }
    uint64_t t1 = rdtsc();
    g_tsc_hz = (t1 - t0) * 100u;
    return g_tsc_hz > 1000000ull;
}

void time_init(uint64_t rsdp) {
    find_pmtmr(rsdp);
    if (!calibrate_pmtmr() && !calibrate_pit())
        g_tsc_hz = 1000000000ull;                       /* last resort: assume 1 GHz */
    g_base = rdtsc();
}

/* wall clock: seeded once from the UEFI GetTime() epoch captured by the loader,
 * then advanced by the monotonic millisecond counter. 0 until seeded. */
static uint64_t g_epoch_base, g_epoch_millis;
void time_set_epoch(uint64_t epoch) { g_epoch_base = epoch; g_epoch_millis = millis(); }
uint64_t wall_epoch(void) {
    return g_epoch_base ? g_epoch_base + (millis() - g_epoch_millis) / 1000 : 0;
}

uint64_t tsc_freq_hz(void) { return g_tsc_hz; }

uint64_t micros(void) {
    uint64_t d = rdtsc() - g_base;
    return d / (g_tsc_hz / 1000000ull ? g_tsc_hz / 1000000ull : 1);
}
uint64_t millis(void) {
    uint64_t d = rdtsc() - g_base;
    return d / (g_tsc_hz / 1000ull ? g_tsc_hz / 1000ull : 1);
}
void sleep_ms(uint32_t ms) {
    uint64_t target = millis() + ms;
    while (millis() < target) { __asm__ volatile("pause"); }
}
