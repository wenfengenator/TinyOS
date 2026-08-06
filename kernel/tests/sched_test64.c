/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 scheduler test battery (smoke + stress).
 *
 * Split out of kernel/sched/sched64.c so the scheduler core stays lean.  These
 * drive the LAPIC-timer scheduler with kernel threads and check the outcome:
 * concurrency (every thread advances), sleep/wake (a blocked thread yields and
 * only advances when woken), plus stress variants.  Details print to the kernel
 * log; sched_test_run returns 1 iff every test passed. */
#include "boot64.h"

#define TC_MAX 8
static volatile uint32_t g_tc[TC_MAX];

static void tc_worker(uint64_t id)  { for (;;) { g_tc[id]++; for (volatile int i = 0; i < 50; i++) __asm__ volatile("pause"); } }
static void tc_sleeper(uint64_t id) { for (;;) { g_tc[id]++; sched_block((int)id + 100); } }
static void tc_reset(void) { for (int i = 0; i < TC_MAX; i++) g_tc[i] = 0; }

/* every one of `nw` threads must make progress within `ms` */
static int test_concurrency(int nw, uint32_t ms) {
    tc_reset();
    sched_init();
    for (int i = 1; i <= nw; i++) sched_spawn(tc_worker, (uint64_t)i);
    sched_start();
    uint64_t t0 = millis(); while (millis() - t0 < ms) __asm__ volatile("pause");
    sched_stop();
    int ok = sched_switches() > 0;
    for (int i = 1; i <= nw; i++) if (!g_tc[i]) ok = 0;
    return ok;
}

/* a blocked thread yields the CPU; it advances only when the driver wakes it */
static int test_sleepwake(uint32_t ms) {
    tc_reset();
    sched_init();
    sched_spawn(tc_sleeper, 1);              /* g_tc[1], chan 101 */
    sched_spawn(tc_worker, 2);               /* g_tc[2] free-runs  */
    sched_start();
    uint32_t wakes = 0; uint64_t t0 = millis(), last = t0;
    while (millis() - t0 < ms) {
        uint64_t t = millis();
        if (t - last >= 15) { sched_wake(101); wakes++; last = t; }
        __asm__ volatile("pause");
    }
    sched_stop();
    return g_tc[1] > 0 && g_tc[2] > g_tc[1] * 4 && g_tc[1] <= wakes + 2;
}

/* hammer one channel with wakes: the sleeper completes many block/wake cycles */
static int test_blockwake_stress(uint32_t ms) {
    tc_reset();
    sched_init();
    sched_spawn(tc_sleeper, 1);              /* chan 101 */
    sched_start();
    uint64_t t0 = millis();
    while (millis() - t0 < ms) { sched_wake(101); __asm__ volatile("pause"); }
    sched_stop();
    return g_tc[1] > 10;
}

/* several sleepers on distinct channels, woken round-robin; each must wake */
static int test_multichan(uint32_t ms) {
    tc_reset();
    sched_init();
    int n = 4;
    for (int i = 1; i <= n; i++) sched_spawn(tc_sleeper, (uint64_t)i);   /* chans 101..104 */
    sched_start();
    uint64_t t0 = millis(), last = t0; int w = 0;
    while (millis() - t0 < ms) {
        uint64_t t = millis();
        if (t - last >= 10) { sched_wake(101 + (w % n)); w++; last = t; }
        __asm__ volatile("pause");
    }
    sched_stop();
    int ok = 1; for (int i = 1; i <= n; i++) if (!g_tc[i]) ok = 0;
    return ok;
}

static void report(const char *name, int ok, int *pass, int *total) {
    (*total)++; if (ok) (*pass)++;
    kputs("[sched-test] "); kputs(name); kputs(ok ? " PASS\n" : " FAIL\n");
}

int sched_test_run(int stress) {
    int pass = 0, total = 0;
    report("concurrency(4)....", test_concurrency(4, 60), &pass, &total);
    report("sleep/wake........", test_sleepwake(120), &pass, &total);
    if (stress) {
        report("stress threads(7).", test_concurrency(7, 120), &pass, &total);
        report("stress block/wake.", test_blockwake_stress(400), &pass, &total);
        report("stress multichan..", test_multichan(200), &pass, &total);
    }
    kputs("[sched-test] passed "); kput_u32dec((uint32_t)pass);
    kputc('/'); kput_u32dec((uint32_t)total); kputc('\n');
    return pass == total;
}
