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

static inline void  outb(uint16_t p, uint8_t v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p)  { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static inline void  outw(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint16_t inw(uint16_t p) { uint16_t r; __asm__ volatile("inw %1,%0" : "=a"(r) : "Nd"(p)); return r; }

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
static uint16_t g_pm1a_evt;                    /* PM1a event (status) block */
static uint16_t g_reset_port; static uint8_t g_reset_val; static int g_reset_ok;
static int g_s5_ok, g_s3_ok;
static uint8_t g_s5a, g_s5b, g_s3a, g_s3b;

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
    uint32_t flen = *(volatile uint32_t *)(uintptr_t)(fadt + 4);
    /* ACPI 2.0+ FADTs may leave the legacy blocks 0 and use the extended GAS
     * fields instead: X_PM1a_EVT_BLK is a GAS at offset 148 (addr at +4). */
    if (!g_pm1a_evt && flen >= 160 && *(volatile uint8_t *)(uintptr_t)(fadt + 148) == 1)
        g_pm1a_evt = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 152);
    if (!g_pm1a_cnt && flen >= 172 && *(volatile uint8_t *)(uintptr_t)(fadt + 172) == 1)
        g_pm1a_cnt = (uint16_t)*(volatile uint64_t *)(uintptr_t)(fadt + 176);
    uint32_t flags = *(volatile uint32_t *)(uintptr_t)(fadt + 112);
    if ((flags & (1u << 10)) && flen >= 129) {                            /* RESET_REG_SUP */
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

/* Wait until status-port bit `bit` reads `val`; bounded so a dead EC can't hang. */
static int ec_wait(int bit, int val) {
    for (int i = 0; i < 5000; i++)
        if (((inb(g_ec_cmd) >> bit) & 1) == val) return 0;
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

/* Probe the EC once and dump a slab of its RAM to the kernel log so the battery
 * registers can be located on this specific laptop (offsets are DSDT-defined). */
static void ec_probe(void) {
    uint8_t v;
    if (ec_read(0x00, &v) != 0) { kputs("[ec] no response on "); kput_u64hex(g_ec_cmd);
        kputs("/"); kput_u64hex(g_ec_data); kputc('\n'); return; }
    g_ec_present = 1;
    kputs("[ec] present cmd="); kput_u64hex(g_ec_cmd); kputs(" data="); kput_u64hex(g_ec_data); kputc('\n');
    for (int base = 0; base < 0x100; base += 0x10) {         /* full 256-byte EC RAM (both batteries) */
        kputs("[ec] "); kput_u64hex((uint64_t)base); kputs(":");
        for (int j = 0; j < 0x10; j++) {
            if (ec_read((uint8_t)(base + j), &v) != 0) { kputs(" --"); continue; }
            kputc(' '); kputc("0123456789ABCDEF"[v >> 4]); kputc("0123456789ABCDEF"[v & 0xF]);
        }
        kputc('\n');
    }
}

void acpi_init(uint64_t rsdp) {
    g_rsdp = rsdp;
    fadt_init();
    ecdt_init();
    ec_probe();
    kputs("[acpi] PM1a="); kput_u64hex(g_pm1a_cnt);
    kputs(" S5="); kputc(g_s5_ok ? '1' : '0');
    kputs(" typ="); kput_u64hex(g_s5a);
    kputs(" reset="); kputc(g_reset_ok ? '1' : '0'); kputc('\n');
}

/* ---- battery / AC (best-effort until the EC offsets are calibrated) ---- */
/* Public raw EC read for on-demand calibration dumps (see the `ec` command). */
int acpi_ec_read(uint8_t off, uint8_t *out) {
    if (!g_ec_present) return -1;
    return ec_read(off, out);
}

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
    if (g_pm1a_cnt) outw(g_pm1a_cnt, (uint16_t)(((slp_a & 7) << 10) | (1 << 13)));  /* SLP_TYP|SLP_EN */
    if (g_pm1b_cnt) outw(g_pm1b_cnt, (uint16_t)(((slp_b & 7) << 10) | (1 << 13)));
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

int acpi_s3_available(void) { return g_s3_ok && g_pm1a_cnt; }
