/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 ACPI power management + Embedded Controller.
 *
 * Enough ACPI to (a) power the machine off cleanly (S5) and reset it, and (b)
 * talk to the laptop Embedded Controller so the desktop can show battery/AC
 * state.  No AML interpreter yet: the S5/S3 SLP_TYP values are lifted straight
 * out of the DSDT byte stream (the classic "_S5_" package scan), and the EC is
 * driven over the legacy 0x62/0x66 ports (or whatever the ECDT advertises).
 *
 * Everything here is bounded/polled with timeouts so a non-responsive EC on
 * unfamiliar firmware can never hang the machine (the whole reason the desktop
 * was freezing on real hardware). */
#include "boot64.h"
#include "arch64.h"
#include "s3_tramp_array.h"

static inline void  outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)  { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void  outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t r; __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static uint8_t cmos_read(uint8_t reg) { outb(0x70, (uint8_t)(0x80u | reg)); return inb(0x71); }
static void cmos_write(uint8_t reg, uint8_t value) { outb(0x70, (uint8_t)(0x80u | reg)); outb(0x71, value); }

/* ---- ACPI table discovery (XSDT walk, same shape as smp64.c/time64.c) ---- */
static uint64_t g_rsdp;

static uint64_t acpi_find(const char *sig) {
    if (!g_rsdp) return 0;
    uint64_t xsdt = *(volatile uint64_t *)(uintptr_t)(g_rsdp + 24);
    if (!xsdt) return 0;
    uint32_t xlen = *(volatile uint32_t *)(uintptr_t)(xsdt + 4);
    for (uint32_t off = 36; off + 8 <= xlen; off += 8) {
        uint64_t t = *(volatile uint64_t *)(uintptr_t)(xsdt + off);
        const volatile char *s = (const volatile char *)(uintptr_t)t;
        if (s[0] == sig[0] && s[1] == sig[1] && s[2] == sig[2] && s[3] == sig[3]) return t;
    }
    return 0;
}

/* ---- FADT: PM1 control ports + reset register + DSDT pointer ---- */
static uint16_t g_pm1a_cnt, g_pm1b_cnt;
static uint16_t g_pm1a_evt, g_pm1b_evt;         /* PM1 event (status) blocks */
static uint8_t g_pm1_evt_len;
static uint16_t g_gpe0_blk, g_gpe1_blk;
static uint8_t g_gpe0_len, g_gpe1_len;
static uint8_t g_old_gpe0_en[32], g_old_gpe1_en[32];
static uint32_t g_fadt_flags;
static uint16_t g_old_pm1a_cnt, g_old_pm1b_cnt, g_old_pm1a_en, g_old_pm1b_en;
static uint16_t g_reset_port; static uint8_t g_reset_val; static int g_reset_ok;
static int g_s5_ok, g_s3_ok;
static uint8_t g_s5a, g_s5b, g_s3a, g_s3b;
static uint64_t g_facs;
static uint32_t g_facs_len, g_old_wake32, g_old_ospm_flags;
static uint64_t g_old_wake64;
static int g_wake64_used;
static int g_s3_prepared;
static int g_s3_scheduler_was_running;
static int g_s3_interrupts_were_enabled;
static uint32_t g_last_s3_phase;
static uint8_t g_old_rtc_alarm_sec, g_old_rtc_alarm_min, g_old_rtc_alarm_hour, g_old_rtc_reg_b;
static int g_rtc_alarm_armed;
#define S3_WAKE_PHYS  0x7000u
#define S3_PARAM_PHYS 0x7F00u
#define S3_DIAG_MAGIC 0x53334447u
static char g_s3_trace[2048];
static uint32_t g_s3_trace_len;

/* Firmware often marks the low-memory identity map no-execute; the kernel never
 * noticed because it runs from high memory.  Once the wake trampoline enables
 * paging with these tables, the first fetch of its own low-memory code faults
 * (stuck at phase 6).  Clear NX (bit 63) down the walk to the trampoline page so
 * it stays executable; the trampoline's CR3 reload flushes the stale TLB. */
static void s3_make_wake_executable(void) {
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *tbl = (uint64_t *)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t idx = (S3_WAKE_PHYS >> shift) & 0x1FFu;
        uint64_t e = tbl[idx];
        if (!(e & 1ULL)) return;                       /* not present */
        tbl[idx] = e & ~(1ULL << 63);                  /* clear no-execute */
        if (shift == 12 || (e & (1ULL << 7))) return;  /* 4KB leaf or huge page */
        tbl = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
    }
}

/* The kernel's VM manager can remap the low trampoline page onto a high arena
 * frame (the walk for VA 0x7000 resolves to physical 0x100122000, not 0x7000).
 * That breaks the S3 design, which assumes 0x7000 is identity-mapped: firmware
 * enters real mode at *physical* 0x7000, but the instant s3_prot re-enables
 * paging with the kernel CR3 the linear-0x7000 fetch lands on the wrong frame
 * and triple-faults (stuck at phase 6).  Rewrite the leaf so VA 0x7000 maps
 * back to PA 0x7000 before the trampoline is copied there. */
static void s3_identity_map_wake(void) {
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *tbl = (uint64_t *)(uintptr_t)(cr3 & 0x000FFFFFFFFFF000ULL);
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t idx = (S3_WAKE_PHYS >> shift) & 0x1FFu;
        uint64_t e = tbl[idx];
        if (!(e & 1ULL)) return;                        /* unmapped: leave as-is */
        if (shift == 12) {                              /* 4KB leaf -> physical 0x7000 */
            tbl[idx] = (e & 0xFFFULL & ~(1ULL << 63)) | (uint64_t)S3_WAKE_PHYS;
            break;
        }
        if (e & (1ULL << 7)) {                          /* huge page -> identity (base 0) */
            tbl[idx] = e & 0xFFFULL & ~(1ULL << 63);
            break;
        }
        tbl = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
    }
    __asm__ volatile("invlpg (%0)" :: "r"((void *)(uintptr_t)S3_WAKE_PHYS) : "memory");
}

static int g_s3_trace_ready;

int arch_s3_save_context(volatile uint64_t *param);

static void s3_trace_text(const char *text) {
    while (*text && g_s3_trace_len + 1 < sizeof(g_s3_trace))
        g_s3_trace[g_s3_trace_len++] = *text++;
    g_s3_trace[g_s3_trace_len] = 0;
}

static void s3_trace_hex(uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    s3_trace_text("0x");
    int started = 0;
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned digit = (unsigned)((value >> shift) & 0xFu);
        if (digit || started || shift == 0) {
            char one[2] = { digits[digit], 0 };
            s3_trace_text(one);
            started = 1;
        }
    }
}

static void s3_trace_flush(void) {
    if (g_s3_trace_ready) {
        (void)dv_write("/S3TRACE.TXT", g_s3_trace, g_s3_trace_len);
        dv_sync();
    }
}

static void s3_trace_stage(const char *stage) {
    s3_trace_text(stage);
    s3_trace_text("\n");
    s3_trace_flush();
}

static void s3_trace_begin(void) {
    g_s3_trace_len = 0;
    g_s3_trace[0] = 0;
    g_s3_trace_ready = dv_mount();
    s3_trace_text("TinyOS ACPI S3 trace\nPM1a_EVT="); s3_trace_hex(g_pm1a_evt);
    s3_trace_text(" PM1a_CNT="); s3_trace_hex(g_pm1a_cnt);
    s3_trace_text(" FADT_FLAGS="); s3_trace_hex(g_fadt_flags);
    s3_trace_text("\nGPE0="); s3_trace_hex(g_gpe0_blk);
    s3_trace_text("/"); s3_trace_hex(g_gpe0_len);
    s3_trace_text(" GPE1="); s3_trace_hex(g_gpe1_blk);
    s3_trace_text("/"); s3_trace_hex(g_gpe1_len);
    s3_trace_text("\nFACS="); s3_trace_hex(g_facs);
    s3_trace_text(" wake32="); s3_trace_hex(S3_WAKE_PHYS);
    s3_trace_text("\n");
    s3_trace_stage(g_s3_trace_ready ? "TRACE_VOLUME=ready" : "TRACE_VOLUME=unavailable");
}

/* Pull SLP_TYPa/b out of a "_Sx_" package in the DSDT AML (no interpreter). */
static int dsdt_slp_typ(const uint8_t *d, uint32_t len, const char *nm, uint8_t *a, uint8_t *b) {
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (d[i] != nm[0] || d[i+1] != nm[1] || d[i+2] != nm[2] || d[i+3] != nm[3]) continue;
        /* must be a NameOp'd package: 08 "_Sx_" 12 ... (optionally \ before name) */
        if (!(d[i-1] == 0x08 || (i >= 2 && d[i-2] == 0x08 && d[i-1] == '\\'))) continue;
        const uint8_t *p = d + i + 4;
        if (*p != 0x12) continue;                       /* PackageOp */
        p++;
        p += ((*p >> 6) & 3) + 2;                        /* skip PkgLength + NumElements */
        if (*p == 0x0A) p++;                             /* BytePrefix */
        *a = *p++;
        if (*p == 0x0A) p++;
        *b = *p;
        return 1;
    }
    return 0;
}

static void fadt_init(void) {
    uint64_t fadt = acpi_find("FACP");
    if (!fadt) return;
    g_pm1a_cnt = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 64);   /* PM1a_CNT_BLK */
    g_pm1b_cnt = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 68);   /* PM1b_CNT_BLK */
    g_pm1a_evt = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 56);   /* PM1a_EVT_BLK (STS) */
    g_pm1b_evt = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 60);   /* PM1b_EVT_BLK (STS) */
    g_gpe0_blk = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 80);   /* GPE0_BLK */
    g_gpe1_blk = (uint16_t)*(volatile uint32_t *)(uintptr_t)(fadt + 84);   /* GPE1_BLK */
    g_pm1_evt_len = *(volatile uint8_t *)(uintptr_t)(fadt + 88);
    g_gpe0_len = *(volatile uint8_t *)(uintptr_t)(fadt + 92);
    g_gpe1_len = *(volatile uint8_t *)(uintptr_t)(fadt + 93);
    uint32_t flen = *(volatile uint32_t *)(uintptr_t)(fadt + 4);
    g_facs = *(volatile uint32_t *)(uintptr_t)(fadt + 36);                /* FIRMWARE_CTRL */
    if (flen >= 140) {
        uint64_t xfacs = *(volatile uint64_t *)(uintptr_t)(fadt + 132);   /* X_FIRMWARE_CTRL */
        if (xfacs) g_facs = xfacs;
    }
    /* ACPI 2.0+ FADTs may leave the legacy blocks 0 and use the extended GAS
     * fields instead: X_PM1a_EVT_BLK is a GAS at offset 148 (addr at +4). */
    if (!g_pm1a_evt && flen >= 160 && *(volatile uint8_t *)(uintptr_t)(fadt + 148) == 1)
        g_pm1a_evt = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 152);
    if (!g_pm1a_cnt && flen >= 172 && *(volatile uint8_t *)(uintptr_t)(fadt + 172) == 1)
        g_pm1a_cnt = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 176);
    if (!g_gpe0_blk && flen >= 232 && *(volatile uint8_t *)(uintptr_t)(fadt + 220) == 1)
        g_gpe0_blk = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 224);
    if (!g_gpe1_blk && flen >= 244 && *(volatile uint8_t *)(uintptr_t)(fadt + 232) == 1)
        g_gpe1_blk = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 236);
    g_fadt_flags = *(volatile uint32_t *)(uintptr_t)(fadt + 112);
    if ((g_fadt_flags & (1u << 10)) && flen >= 129) {                     /* RESET_REG_SUP */
        if (*(volatile uint8_t *)(uintptr_t)(fadt + 116) == 1)            /* SystemIO */
            g_reset_port = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 120);
        g_reset_val = *(volatile uint8_t *)(uintptr_t)(fadt + 128);
        g_reset_ok = g_reset_port != 0;
    }
    uint64_t dsdt = *(volatile uint32_t *)(uintptr_t)(fadt + 40);          /* DSDT */
    if (flen >= 148 && !dsdt) dsdt = *(volatile uint64_t *)(uintptr_t)(fadt + 140);  /* X_DSDT */
    if (dsdt) {
        const uint8_t *d = (const uint8_t *)(uintptr_t)dsdt;
        uint32_t dlen = *(volatile uint32_t *)(uintptr_t)(dsdt + 4);
        if (dlen > 36 && dlen < (64u << 20)) {
            g_s5_ok = dsdt_slp_typ(d, dlen, "_S5_", &g_s5a, &g_s5b);
            g_s3_ok = dsdt_slp_typ(d, dlen, "_S3_", &g_s3a, &g_s3b);
        }
    }
}

static int s3_prepare(void) {
    if (!g_s3_ok || !g_pm1a_cnt || !g_facs || sizeof(s3_tramp) > 0xF00u) return -1;
    uint8_t retained_rtc_phase = cmos_read(0x01);
    if (retained_rtc_phase >= 0xC1u && retained_rtc_phase <= 0xC7u) {
        /* 1=entered 2=real-mode 5=prot-mode 6=pre-paging 7=paging-on 3=long-mode 4=resumed */
        g_last_s3_phase = retained_rtc_phase & 0x0Fu;
        cmos_write(0x01, 0xFFu);                    /* consume marker; RTC don't-care */
    }
    volatile uint8_t *f = (volatile uint8_t *)(uintptr_t)g_facs;
    uint32_t flen = *(volatile uint32_t *)(uintptr_t)(g_facs + 4);
    if (f[0] != 'F' || f[1] != 'A' || f[2] != 'C' || f[3] != 'S' || flen < 24) return -1;
    g_facs_len = flen;

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)S3_WAKE_PHYS;
    for (uint32_t i = 0; i < 0x1000; i++) dst[i] = 0;
    for (uint32_t i = 0; i < sizeof(s3_tramp); i++) dst[i] = s3_tramp[i];
    for (uint32_t i = 0; i < sizeof(s3_tramp); i++) if (dst[i] != s3_tramp[i]) return -1;

    /* Do not publish FACS waking vectors yet.  They are installed atomically
     * with the eventual SLP_EN transition once resume restoration is complete. */
    volatile uint64_t *param = (volatile uint64_t *)(uintptr_t)S3_PARAM_PHYS;
    if (!g_last_s3_phase && (uint32_t)param[4] == S3_DIAG_MAGIC)
        g_last_s3_phase = (uint32_t)param[3];
    param[0] = param[1] = param[2] = 0;
    g_s3_prepared = 1;
    return 0;
}

/* ---- Embedded Controller (legacy 62/66, or the ECDT's ports) ---- */
#define EC_RD 0x80
static uint16_t g_ec_cmd = 0x66, g_ec_data = 0x62;
static int g_ec_present;

static void ecdt_init(void) {
    uint64_t e = acpi_find("ECDT");
    if (e) {
        if (*(volatile uint8_t *)(uintptr_t)(e + 36) == 1)                /* EC_CONTROL SystemIO */
            g_ec_cmd = (uint16_t)*(volatile uint64_t *)(uintptr_t)(e + 40);
        if (*(volatile uint8_t *)(uintptr_t)(e + 48) == 1)                /* EC_DATA SystemIO */
            g_ec_data = (uint16_t)*(volatile uint64_t *)(uintptr_t)(e + 52);
    }
}

/* Wait until status-port bit `bit` reads `val`; time-bounded so a dead EC can't
 * hang, yet generous enough for a real EC (transactions can take milliseconds). */
static int ec_wait(int bit, int val) {
    uint64_t end = millis() + 100;
    do {
        if (((inb(g_ec_cmd) >> bit) & 1) == val) return 0;
    } while (millis() < end);
    return -1;
}
static int ec_read(uint8_t off, uint8_t *out) {
    if (ec_wait(1, 0)) return -1;                        /* IBF clear */
    outb(g_ec_cmd, EC_RD);
    if (ec_wait(1, 0)) return -1;
    outb(g_ec_data, off);
    if (ec_wait(0, 1)) return -1;                        /* OBF set   */
    *out = inb(g_ec_data);
    return 0;
}
#define EC_WR 0x81
static int ec_write(uint8_t off, uint8_t val) {
    if (ec_wait(1, 0)) return -1;                        /* IBF clear */
    outb(g_ec_cmd, EC_WR);
    if (ec_wait(1, 0)) return -1;
    outb(g_ec_data, off);
    if (ec_wait(1, 0)) return -1;
    outb(g_ec_data, val);
    if (ec_wait(1, 0)) return -1;
    return 0;
}

/* Probe the EC once and dump a slab of its RAM to the kernel log so the battery
 * registers can be located on this specific laptop (offsets are DSDT-defined). */
static void ec_probe(void) {
    uint8_t v;
    if (ec_read(0x00, &v) != 0) {
#if BOOT_DIAG
        kputs("[ec] no response on "); kput_u64hex(g_ec_cmd);
        kputs("/"); kput_u64hex(g_ec_data); kputc('\n');
#endif
        return;
    }
    g_ec_present = 1;
#if BOOT_DIAG
    kputs("[ec] present cmd="); kput_u64hex(g_ec_cmd); kputs(" data="); kput_u64hex(g_ec_data); kputc('\n');
    for (int base = 0; base < 0x100; base += 0x10) {         /* full 256-byte EC RAM (both batteries) */
        kputs("[ec] "); kput_u64hex((uint64_t)base); kputs(":");
        for (int j = 0; j < 0x10; j++) {
            if (ec_read((uint8_t)(base + j), &v) != 0) { kputs(" --"); continue; }
            kputc(' '); kputc("0123456789ABCDEF"[v >> 4]); kputc("0123456789ABCDEF"[v & 0xF]);
        }
        kputc('\n');
    }
#endif
}

void acpi_init(uint64_t rsdp) {
    g_rsdp = rsdp;
    fadt_init();
    s3_prepare();
    ecdt_init();
    ec_probe();
#if BOOT_DIAG
    kputs("[acpi] PM1a="); kput_u64hex(g_pm1a_cnt);
    kputs(" S5="); kputc(g_s5_ok ? '1' : '0');
    kputs(" typ="); kput_u64hex(g_s5a);
    kputs(" reset="); kputc(g_reset_ok ? '1' : '0'); kputc('\n');
    kputs("[acpi] wake power="); kputs((g_fadt_flags & (1u << 4)) ? "GPE" : "fixed");
    kputs(" GPE0="); kput_u64hex(g_gpe0_blk); kputc('/'); kput_u32dec(g_gpe0_len);
    kputs(" GPE1="); kput_u64hex(g_gpe1_blk); kputc('/'); kput_u32dec(g_gpe1_len); kputc('\n');
    kputs("[acpi] S3="); kputc(g_s3_ok ? '1' : '0');
    kputs(" typ="); kput_u64hex(g_s3a);
    kputs(" FACS="); kput_u64hex(g_facs);
    kputs(g_s3_prepared ? " wake trampoline prepared (entry gated)\n" : " unavailable\n");
#endif
    if (g_last_s3_phase) { kputs("[acpi] retained S3 phase="); kput_u32dec(g_last_s3_phase); kputc('\n'); }
    (void)tinyos_uacpi_init(rsdp);
}

uint32_t acpi_s3_phase(void) { return g_last_s3_phase; }

/* Persist the wake phase retained from the last S3 attempt so a headless test
 * can distinguish "firmware never resumed" (0) from "resumed but our device
 * restore hung" (2/3) — the CMOS phase marker otherwise only reaches serial. */
void acpi_s3_persist_boot_phase(void) {
    if (!dv_mount()) return;
    g_s3_trace_len = 0; g_s3_trace[0] = 0;
    s3_trace_text("S3 BOOT retained_phase="); s3_trace_hex(g_last_s3_phase);
    s3_trace_text(" SMI_EN="); s3_trace_hex(g_pm1a_evt ? inw((uint16_t)(g_pm1a_evt + 0x30)) : 0);
    s3_trace_text("\n");
    (void)dv_write("/S3BOOT.TXT", g_s3_trace, g_s3_trace_len);
    dv_sync();
}

/* ---- battery / AC (best-effort until the EC offsets are calibrated) ---- */
/* Public raw EC read for on-demand calibration dumps (see the `ec` command). */
int acpi_ec_read(uint8_t off, uint8_t *out) {
    if (!g_ec_present) return -1;
    return ec_read(off, out);
}

/* Ungated EC access for the uACPI EmbeddedControl region handler: _PTS/_WAK/_INI
 * on ThinkPads read and write EC fields, and this must work even if the boot
 * probe's single (possibly marginally-timed) read did not set g_ec_present. */
int acpi_ec_region_read(uint8_t off, uint8_t *out) { return ec_read(off, out); }
int acpi_ec_region_write(uint8_t off, uint8_t val) { return ec_write(off, val); }

/* T440s EC battery registers (firmware GJHT25WW), calibrated from plugged-vs-
 * unplugged EC dumps:
 *   0x47 bit4  = AC adapter present   (0x04 -> 0x14 when the charger is plugged)
 *   0xA0-0xAF  = MAIN pack (BAT0): remaining 0xA0, full 0xA2, percent 0xA6,
 *               current 0xA8, voltage 0xAA (mV)
 * The second (Power Bridge) pack is NOT exposed as a mirror block in readable EC
 * RAM (0xB0 is empty); the firmware serves it via an ACPI/SMBus battery-select. */
#define EC_AC_REG   0x46   /* H8 status byte: bit2=lid, bit4=AC (confirmed live on GJHT25WW) */
#define EC_AC_BIT   0x10
#define EC_PCT      0xA6
#define EC_VOLT_LO  0xAA
int acpi_battery(acpi_batt_t *b) {
    b->present = 0; b->ac_online = 0; b->charging = 0; b->percent = -1; b->voltage_mv = 0;
    for (int i = 0; i < 8; i++) b->raw[i] = 0;
    if (!g_ec_present) return -1;
    uint8_t v, lo, hi;
    if (ec_read(EC_AC_REG, &v) == 0) b->ac_online = (v & EC_AC_BIT) ? 1 : 0;
    if (ec_read(EC_PCT, &v) == 0 && v <= 100) { b->percent = v; b->present = 1; }
    if (ec_read(EC_VOLT_LO, &lo) == 0 && ec_read(EC_VOLT_LO + 1, &hi) == 0)
        b->voltage_mv = (int)(lo | (hi << 8));
    b->charging = b->ac_online && b->present && b->percent < 100;
    for (int i = 0; i < 8; i++)                          /* 0xA0..0xA7 block (the single battery) */
        if (ec_read((uint8_t)(0xA0 + i), &v) == 0) b->raw[i] = v;
    return 0;
}

/* Laptop lid state.  On the ThinkPad H8 EC (per coreboot lid.asl) LIDS lives in
 * EC RAM 0x46 bit 2: 1 = open, 0 = closed.  Returns 1 (open) whenever the EC is
 * absent or the read fails, so callers never spuriously act as if the lid closed
 * (e.g. under QEMU, or if this offset turns out wrong on GJHT25WW firmware). */
#define EC_LID_REG  0x46
#define EC_LID_OPEN 0x04
int acpi_lid_open(void) {
    uint8_t v;
    if (!g_ec_present) return 1;
    if (ec_read(EC_LID_REG, &v) != 0) return 1;
    return (v & EC_LID_OPEN) ? 1 : 0;
}

/* Power-button poll.  PWRBTN_STS is bit 8 of PM1_STS; hardware sets it on a
 * press regardless of SCI enable, so we can poll it.  Returns 1 (and clears the
 * latch by writing 1-to-clear) on a fresh press, else 0. */
int acpi_power_button(void) {
    if (!g_pm1a_evt) return 0;
    uint16_t sts = inw(g_pm1a_evt);
    if (sts & (1u << 8)) { outw(g_pm1a_evt, (uint16_t)(1u << 8)); return 1; }
    return 0;
}

/* ---- power transitions ---- */
static void pm1_write(uint8_t slp_a, uint8_t slp_b) {
    /* Split the SLP_TYP and SLP_EN writes (ACPICA/uACPI do the same): some
     * firmware enters an S3 it cannot resume from if both are written at once.
     * WBINVD between so no dirty cache line is lost as the CPU context drops. */
    if (g_pm1a_cnt) {
        uint16_t v = inw(g_pm1a_cnt);
        outw(g_pm1a_cnt, (uint16_t)((v & ~((7u << 10) | (1u << 13))) | ((slp_a & 7u) << 10)));
    }
    if (g_pm1b_cnt) {
        uint16_t v = inw(g_pm1b_cnt);
        outw(g_pm1b_cnt, (uint16_t)((v & ~((7u << 10) | (1u << 13))) | ((slp_b & 7u) << 10)));
    }
    __asm__ volatile("wbinvd" ::: "memory");
    if (g_pm1a_cnt) outw(g_pm1a_cnt, (uint16_t)(inw(g_pm1a_cnt) | (1u << 13)));
    if (g_pm1b_cnt) outw(g_pm1b_cnt, (uint16_t)(inw(g_pm1b_cnt) | (1u << 13)));
}

static int rtc_decode(uint8_t value, int binary) {
    return binary ? value : (value & 0x0Fu) + ((value >> 4) & 0x07u) * 10;
}

static uint8_t rtc_encode(int value, int binary) {
    return binary ? (uint8_t)value : (uint8_t)(((value / 10) << 4) | (value % 10));
}

/* Arm an unconditional alarm ten seconds ahead. This gives s3test a wake
 * source even before model-specific lid/keyboard GPE methods are available. */
static int s3_arm_rtc_alarm(void) {
    uint64_t deadline = millis() + 100;
    while ((cmos_read(0x0A) & 0x80u) && millis() < deadline) __asm__ volatile("pause");
    if (cmos_read(0x0A) & 0x80u) return -1;

    uint8_t reg_b = cmos_read(0x0B);
    int binary = (reg_b & 0x04u) != 0;
    int hour24 = (reg_b & 0x02u) != 0;
    uint8_t raw_hour = cmos_read(0x04);
    int sec = rtc_decode(cmos_read(0x00), binary);
    int min = rtc_decode(cmos_read(0x02), binary);
    int hour;
    if (hour24) hour = rtc_decode(raw_hour, binary);
    else {
        int pm = (raw_hour & 0x80u) != 0;
        hour = rtc_decode((uint8_t)(raw_hour & 0x7Fu), binary) % 12;
        if (pm) hour += 12;
    }
    if (sec > 59 || min > 59 || hour > 23) return -1;
    sec += 10;
    if (sec >= 60) { sec -= 60; if (++min >= 60) { min = 0; if (++hour >= 24) hour = 0; } }

    g_old_rtc_alarm_sec = cmos_read(0x01);
    g_old_rtc_alarm_min = cmos_read(0x03);
    g_old_rtc_alarm_hour = cmos_read(0x05);
    g_old_rtc_reg_b = reg_b;
    cmos_write(0x0B, (uint8_t)(reg_b | 0x80u));          /* inhibit updates */
    cmos_write(0x01, rtc_encode(sec, binary));
    cmos_write(0x03, rtc_encode(min, binary));
    if (hour24) cmos_write(0x05, rtc_encode(hour, binary));
    else {
        int pm = hour >= 12, h12 = hour % 12; if (!h12) h12 = 12;
        cmos_write(0x05, (uint8_t)(rtc_encode(h12, binary) | (pm ? 0x80u : 0)));
    }
    cmos_write(0x0B, (uint8_t)((reg_b & ~0x80u) | 0x20u)); /* updates on, alarm IRQ enabled */
    (void)cmos_read(0x0C);                                /* clear stale alarm latch */
    g_rtc_alarm_armed = 1;
    return 0;
}

static void s3_restore_rtc_alarm(void) {
    if (!g_rtc_alarm_armed) return;
    cmos_write(0x0B, (uint8_t)(g_old_rtc_reg_b | 0x80u));
    /* Reg 0x01 (alarm-seconds) is deliberately NOT restored: the wake trampoline
     * stores its phase marker there and this runs during resume, so restoring it
     * would erase the marker.  The alarm is disabled via reg 0x0B below anyway. */
    cmos_write(0x03, g_old_rtc_alarm_min);
    cmos_write(0x05, g_old_rtc_alarm_hour);
    cmos_write(0x0B, g_old_rtc_reg_b);
    (void)cmos_read(0x0C);
    g_rtc_alarm_armed = 0;
}

static void s3_arm_gpe_block(uint16_t block, uint8_t len, uint8_t *saved) {
    uint8_t half = len / 2;
    if (!block || !half || half > 32) return;
    for (uint8_t i = 0; i < half; i++) {
        saved[i] = inb((uint16_t)(block + half + i));
        outb((uint16_t)(block + i), 0xFFu);       /* clear stale W1C status */
        outb((uint16_t)(block + half + i), 0xFFu);
    }
}

static void s3_restore_gpe_block(uint16_t block, uint8_t len, const uint8_t *saved) {
    uint8_t half = len / 2;
    if (!block || !half || half > 32) return;
    for (uint8_t i = 0; i < half; i++)
        outb((uint16_t)(block + half + i), saved[i]);
}

static int s3_arm_fixed_wake(void) {
    uint16_t enable_bits = (1u << 8) | (1u << 10);       /* power button + RTC */
    uint16_t half = g_pm1_evt_len >= 4 ? g_pm1_evt_len / 2 : 2;
    if (g_pm1a_cnt) g_old_pm1a_cnt = inw(g_pm1a_cnt);
    if (g_pm1b_cnt) g_old_pm1b_cnt = inw(g_pm1b_cnt);
    if (g_pm1a_evt) { outw(g_pm1a_evt, 0xFFFFu);   /* clear all fixed status (W1C) */
        g_old_pm1a_en = inw((uint16_t)(g_pm1a_evt + half));
        outw((uint16_t)(g_pm1a_evt + half), (uint16_t)(g_old_pm1a_en | enable_bits)); }
    if (g_pm1b_evt) { outw(g_pm1b_evt, 0xFFFFu);
        g_old_pm1b_en = inw((uint16_t)(g_pm1b_evt + half));
        outw((uint16_t)(g_pm1b_evt + half), (uint16_t)(g_old_pm1b_en | enable_bits)); }
    s3_arm_gpe_block(g_gpe0_blk, g_gpe0_len, g_old_gpe0_en);
    s3_arm_gpe_block(g_gpe1_blk, g_gpe1_len, g_old_gpe1_en);
    if (s3_arm_rtc_alarm() != 0) return -1;
    kputs("[acpi] wake armed PM1a_EN=");
    kput_u64hex(g_pm1a_evt ? inw((uint16_t)(g_pm1a_evt + half)) : 0);
    kputs(" PM1b_EN=");
    kput_u64hex(g_pm1b_evt ? inw((uint16_t)(g_pm1b_evt + half)) : 0);
    kputs(" GPE0_EN0=");
    kput_u64hex((g_gpe0_blk && g_gpe0_len >= 2) ? inb((uint16_t)(g_gpe0_blk + g_gpe0_len / 2)) : 0);
    kputs(" GPE1_EN0=");
    kput_u64hex((g_gpe1_blk && g_gpe1_len >= 2) ? inb((uint16_t)(g_gpe1_blk + g_gpe1_len / 2)) : 0);
    kputc('\n');
    return 0;
}

/* Persist the actual wake-enable machine state to the trace so a headless
 * hardware test shows exactly what was armed before sleep: SCI_EN proves the
 * platform is in ACPI mode (else no ACPI wake source can fire), and the EN/STS
 * readbacks prove the power-button/RTC/GPE arming actually took. */
static void s3_trace_wake_state(void) {
    uint16_t half = g_pm1_evt_len >= 4 ? g_pm1_evt_len / 2 : 2;
    uint16_t cnt = g_pm1a_cnt ? inw(g_pm1a_cnt) : 0;
    s3_trace_text("SCI_EN="); s3_trace_hex(cnt & 1u);
    s3_trace_text(" PM1a_CNT="); s3_trace_hex(cnt);
    s3_trace_text(" PM1a_STS="); s3_trace_hex(g_pm1a_evt ? inw(g_pm1a_evt) : 0);
    s3_trace_text(" PM1a_EN="); s3_trace_hex(g_pm1a_evt ? inw((uint16_t)(g_pm1a_evt + half)) : 0);
    s3_trace_text("\nGPE0_STS="); s3_trace_hex((g_gpe0_blk && g_gpe0_len >= 2) ? inb(g_gpe0_blk) : 0);
    s3_trace_text(" GPE0_EN="); s3_trace_hex((g_gpe0_blk && g_gpe0_len >= 2) ? inb((uint16_t)(g_gpe0_blk + g_gpe0_len / 2)) : 0);
    s3_trace_text(" SLP_TYPa="); s3_trace_hex(g_s3a);
    s3_trace_text(" SLP_TYPb="); s3_trace_hex(g_s3b);
    s3_trace_text(" WAKEVEC="); s3_trace_hex(g_facs ? *(volatile uint32_t *)(uintptr_t)(g_facs + 12) : 0);
    s3_trace_text(" FACSFLAGS="); s3_trace_hex((g_facs && g_facs_len >= 24) ? *(volatile uint32_t *)(uintptr_t)(g_facs + 20) : 0);
    s3_trace_text(" OSPMFLAGS="); s3_trace_hex((g_facs && g_facs_len >= 40) ? *(volatile uint32_t *)(uintptr_t)(g_facs + 36) : 0);
    s3_trace_text(" XWAKE="); s3_trace_hex((g_facs && g_facs_len >= 32) ? *(volatile uint64_t *)(uintptr_t)(g_facs + 24) : 0);
    s3_trace_text(" WAKE64="); s3_trace_hex((uint32_t)g_wake64_used);
    s3_trace_text(" TRAMP="); s3_trace_hex(*(volatile uint32_t *)(uintptr_t)S3_WAKE_PHYS);
    /* Dump the live page-table walk to the trampoline page (the trampoline reloads
     * this same CR3), so a phase-6 stall reveals whether 0x7000 is present /
     * no-execute (bit63) / a huge page (bit7) / identity-mapped. */
    { uint64_t cr3v; __asm__ volatile("mov %%cr3,%0" : "=r"(cr3v));
      s3_trace_text(" CR3="); s3_trace_hex(cr3v);
      s3_trace_text(" WALK=");
      uint64_t *t = (uint64_t *)(uintptr_t)(cr3v & 0x000FFFFFFFFFF000ULL);
      for (int shift = 39; ; shift -= 9) {
          uint64_t e = t[(S3_WAKE_PHYS >> shift) & 0x1FFu];
          s3_trace_hex(e);
          if (!(e & 1ULL) || (e & (1ULL << 7)) || shift == 12) break;
          s3_trace_text("/");
          t = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ULL);
      } }
    s3_trace_text("\n"); s3_trace_flush();
}

static void s3_restore_interrupts(void) {
    if (!g_s3_scheduler_was_running && g_s3_interrupts_were_enabled) __asm__ volatile("sti");
}

static void s3_restore_fixed_wake(void) {
    if (g_facs) {
        *(volatile uint32_t *)(uintptr_t)(g_facs + 12) = g_old_wake32;
        if (g_facs_len >= 32) *(volatile uint64_t *)(uintptr_t)(g_facs + 24) = g_old_wake64;
        if (g_wake64_used) *(volatile uint32_t *)(uintptr_t)(g_facs + 36) = g_old_ospm_flags;
    }
    s3_restore_rtc_alarm();
    s3_restore_gpe_block(g_gpe0_blk, g_gpe0_len, g_old_gpe0_en);
    s3_restore_gpe_block(g_gpe1_blk, g_gpe1_len, g_old_gpe1_en);
    uint16_t half = g_pm1_evt_len >= 4 ? g_pm1_evt_len / 2 : 2;
    if (g_pm1a_cnt) outw(g_pm1a_cnt, g_old_pm1a_cnt);
    if (g_pm1b_cnt) outw(g_pm1b_cnt, g_old_pm1b_cnt);
    if (g_pm1a_evt) outw((uint16_t)(g_pm1a_evt + half), g_old_pm1a_en);
    if (g_pm1b_evt) outw((uint16_t)(g_pm1b_evt + half), g_old_pm1b_en);
}

/* Called from s3_context.s after all callee-saved state is on the resume
 * stack.  Success does not return: firmware restarts at the FACS vector. */
int acpi_s3_commit(void) {
    volatile uint64_t *param = (volatile uint64_t *)(uintptr_t)S3_PARAM_PHYS;
    if (!g_s3_prepared || param[0] > 0xFFFFFFFFu || !param[1] || !param[2]) return -3;

    /* The waking vector and wake sources were published/armed in acpi_s3_enter,
     * before the disk was suspended, so the trace could record the armed state. */
    if (g_pm1a_evt) outw(g_pm1a_evt, 0xFFFFu);  /* clear all fixed status incl WAK_STS */
    if (g_pm1b_evt) outw(g_pm1b_evt, 0xFFFFu);
    kputs("[acpi] entering S3 via wake vector 0x7000\n");
    /* Use uACPI's ACPICA-equivalent sleep sequence (what Linux does, which
     * resumes fine on this laptop); fall back to the direct register write only
     * if uACPI declines before actually sleeping. */
    if (tinyos_uacpi_enter_s3() != 0) pm1_write(g_s3a, g_s3b);
    /* A real S3 powers the CPU off above, so this never runs.  If we are still
     * executing after a few seconds the platform did NOT enter S3; return so the
     * caller restores devices and reports it instead of hanging forever. */
    uint64_t deadline = millis() + 3000;
    while (millis() < deadline) __asm__ volatile("pause");
    return -8;
}

int acpi_s3_enter(void) {
    if (!g_s3_prepared) return -1;

    s3_trace_begin();
    s3_trace_stage("ENTER=begin");
    g_s3_scheduler_was_running = sched_is_running();
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    g_s3_interrupts_were_enabled = (flags & (1u << 9)) != 0;
    if (g_s3_scheduler_was_running) sched_stop();
    else __asm__ volatile("cli");
    s3_trace_stage("CPU=interrupts-disabled");
    if (smp_s3_quiesce() != 0) {
        s3_trace_stage("AP=quiesce-failed");
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return -2;
    }
    s3_trace_stage("AP=quiesced");
    hda_suspend();
    s3_trace_stage("HDA=suspended");
    if (usb_suspend() != 0) {
        s3_trace_stage("USB=suspend-failed");
        hda_resume();
        smp_s3_abort();
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return -3;
    }
    s3_trace_stage("USB=suspended");
    int aml_rc = tinyos_uacpi_prepare_s3();
    s3_trace_text("AML_PTS="); s3_trace_hex((uint32_t)aml_rc); s3_trace_text("\n"); s3_trace_flush();
    if (aml_rc != 0) {
        usb_resume();
        hda_resume();
        smp_s3_abort();
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return -6;
    }

    fb_s3_suspend();

    /* Re-install the wake trampoline at 0x7000 right before sleeping, in case
     * anything touched low memory since boot (the trace's TRAMP field then shows
     * the first bytes so we can confirm it is intact at sleep time).  Restore the
     * page's identity mapping first so the copy lands at physical 0x7000 and the
     * paging-enable in s3_prot keeps fetching it (WALK then ends in 0x7xxx). */
    s3_identity_map_wake();
    { volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)S3_WAKE_PHYS;
      for (uint32_t i = 0; i < sizeof(s3_tramp); i++) dst[i] = s3_tramp[i]; }
    s3_make_wake_executable();

    /* Publish the waking vector.  The 32-bit vector (offset 12) is entered in
     * real mode; UEFI firmware commonly ignores it and honours the 64-bit X
     * vector (offset 24) with 64BIT_WAKE_F (ospm_flags offset 36 bit 0) set,
     * which re-enters us in long mode (our s3_long trampoline path).  Use it
     * whenever the firmware advertises 64BIT_WAKE_SUPPORTED_F (flags off.20 bit 1). */
    g_wake64_used = 0;
    if (g_facs) {
        volatile uint32_t *wake32 = (volatile uint32_t *)(uintptr_t)(g_facs + 12);
        g_old_wake32 = *wake32;
        *wake32 = S3_WAKE_PHYS;
        if (g_facs_len >= 32) {
            volatile uint64_t *wake64 = (volatile uint64_t *)(uintptr_t)(g_facs + 24);
            uint32_t fflags = (g_facs_len >= 24) ? *(volatile uint32_t *)(uintptr_t)(g_facs + 20) : 0;
            g_old_wake64 = *wake64;
            if ((g_facs_len >= 40) && (fflags & (1u << 1))) {   /* 64BIT_WAKE_SUPPORTED_F */
                volatile uint32_t *ospm = (volatile uint32_t *)(uintptr_t)(g_facs + 36);
                g_old_ospm_flags = *ospm;
                *wake64 = S3_WAKE_PHYS;                          /* enter at 0x7000 ... */
                *ospm = g_old_ospm_flags | (1u << 0);            /* ... in long mode */
                g_wake64_used = 1;
            } else {
                *wake64 = 0;                 /* force the 32-bit real-mode vector */
            }
        }
    }
    if (s3_arm_fixed_wake() != 0) {
        s3_trace_stage("WAKE=arm-failed");
        s3_restore_fixed_wake();
        usb_resume();
        hda_resume();
        smp_s3_abort();
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return -7;
    }
    s3_trace_wake_state();
    { char dr[420]; fb_igd_dump(dr, sizeof dr); s3_trace_text("DISP_PRE="); s3_trace_text(dr); s3_trace_text("\n"); s3_trace_flush(); }
    s3_trace_stage("AHCI=flushing-next");
    if (ahci_suspend() != 0) {
        /* The controller may still be usable on failure; preserve the result. */
        s3_trace_stage("AHCI=suspend-failed");
        s3_restore_fixed_wake();
        usb_resume();
        hda_resume();
        smp_s3_abort();
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return -3;
    }

    volatile uint64_t *param = (volatile uint64_t *)(uintptr_t)S3_PARAM_PHYS;
    param[3] = 1;
    param[4] = S3_DIAG_MAGIC;
    int rc = arch_s3_save_context(param);

    if (rc != 0) {
        s3_restore_fixed_wake();
        ahci_resume();
        /* Disk is back: record whether we even entered S3 (rc==-8 => the SLP_EN
         * write did not sleep the machine; any other rc => aborted earlier). */
        s3_trace_text("SLEEP=did-not-enter rc="); s3_trace_hex((uint32_t)(-rc));
        s3_trace_text("\n"); s3_trace_flush();
        usb_resume(); hda_resume();
        smp_s3_abort();
        if (g_s3_scheduler_was_running) sched_start();
        s3_restore_interrupts();
        return rc;
    }

    /* A real wake returns through the saved frame with rc=0.  Reinstall CPU
     * tables/APIC before interrupts or scheduled userspace run. */
    arch_init();
    param[3] = 4; g_last_s3_phase = 4;
    kputs("[acpi] resumed from S3\n");
    /* Bring the panel back FIRST so a successful resume is visible even if a
     * later device restart stalls on real hardware — this separates "firmware
     * never resumed" (black screen) from "resumed but a restore step hung". */
    fb_s3_resume();
    kbd64_init();
    s3_restore_fixed_wake();
    if (g_pm1a_evt) outw(g_pm1a_evt, (uint16_t)(1u << 15));
    if (g_pm1b_evt) outw(g_pm1b_evt, (uint16_t)(1u << 15));
    int ap_rc = smp_s3_restart();
    int ahci_rc = ahci_resume();
    /* Disk is back: append the raw post-resume display registers so a blind reboot
     * + cat /mnt/S3TRACE.TXT reveals exactly what the S3 reset cleared. */
    s3_trace_text("DISP_POST="); s3_trace_text(fb_igd_post_dump()); s3_trace_text("\n"); s3_trace_flush();
    int usb_rc = usb_resume();
    int hda_rc = hda_resume();
    int aml_wake_rc = tinyos_uacpi_wake_s3();
    cmos_write(0x01, 0xC4u);            /* phase 4: resume fully completed (survives a power-cycle) */
    sched_s3_resume(g_s3_scheduler_was_running);
    s3_restore_interrupts();
    if (ap_rc != 0 || ahci_rc != 0 || usb_rc != 0 || hda_rc != 0 || aml_wake_rc != 0) return -4;
    return rc;
}

int acpi_poweroff(void) {
    if (!g_s5_ok || !g_pm1a_cnt) return -1;
    __asm__ volatile("cli");
    pm1_write(g_s5a, g_s5b);
    for (;;) __asm__ volatile("hlt");                    /* should not reach here */
}

void acpi_reset(void) {
    if (g_reset_ok) { outb(g_reset_port, g_reset_val); for (volatile int i = 0; i < 100000; i++) {} }
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);                /* PCI reset control    */
    for (volatile int i = 0; i < 100000; i++) {}
    outb(0x64, 0xFE);                                    /* 8042 CPU reset line  */
}

int acpi_s3_available(void) { return g_s3_prepared; }
