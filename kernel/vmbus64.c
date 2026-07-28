/* tinyos x64 Hyper-V VMBus synthetic keyboard.
 *
 * Hyper-V Gen 2 VMs have no PS/2 i8042 — the keyboard is a synthetic VMBus
 * device.  This driver (polled, no IDT) walks the full path: detect Hyper-V ->
 * hypercall page -> SynIC -> VMBus connect (version negotiate) -> enumerate
 * offers -> open the keyboard channel (GPADL ring buffer) -> keyboard protocol
 * handshake -> poll keystrokes via hvkbd_getkey().
 *
 * NONE of this runs under QEMU (VMBus is a Hyper-V host construct); on non-
 * Hyper-V hosts it is a clean no-op.  Because it can only be validated on real
 * Hyper-V, hvkbd_stage() reports how far the handshake got (shown as HVSTG on
 * the demo HUD) and every step logs to serial, so a failure point is obvious:
 *   1 Hyper-V detected   2 hypercall+SynIC   3 VMBus version ok
 *   4 offers enumerated  5 keyboard found    6 GPADL created
 *   7 channel open       8 protocol ok (keys should flow) */
#include "boot64.h"

extern void *memset(void *, int, unsigned long);

/* ---- CPU helpers ---- */
static inline uint64_t rdmsr(uint32_t m) {
    uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v) {
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* ---- Hyper-V MSRs / hypercalls ---- */
#define HV_MSR_GUEST_OS_ID  0x40000000
#define HV_MSR_HYPERCALL    0x40000001
#define HV_MSR_SCONTROL     0x40000080
#define HV_MSR_SIEFP        0x40000082
#define HV_MSR_SIMP         0x40000083
#define HV_MSR_EOM          0x40000084
#define HV_MSR_SINT2        0x40000092

#define HVCALL_POST_MESSAGE 0x005C
#define HVCALL_SIGNAL_EVENT 0x005D
#define HV_HYPERCALL_FAST   (1ull << 16)

/* ---- VMBus ---- */
#define VMBUS_MESSAGE_CONNID 4
#define HVMSG_VMBUS          1
#define VMBUS_MESSAGE_SINT   2

#define CHANNELMSG_OFFERCHANNEL       1
#define CHANNELMSG_REQUESTOFFERS      3
#define CHANNELMSG_ALLOFFERS_DONE     4
#define CHANNELMSG_OPENCHANNEL        5
#define CHANNELMSG_OPENCHANNEL_RESULT 6
#define CHANNELMSG_GPADL_HEADER       8
#define CHANNELMSG_GPADL_CREATED     10
#define CHANNELMSG_INITIATE_CONTACT  14
#define CHANNELMSG_VERSION_RESPONSE  15
#define CHANNELMSG_UNLOAD            16
#define CHANNELMSG_UNLOAD_RESPONSE   17

#define VM_PKT_DATA_INBAND 6

/* synthetic keyboard protocol */
#define SK_PROTOCOL_REQUEST  1
#define SK_PROTOCOL_RESPONSE 2
#define SK_EVENT             3
#define SK_VERSION           0x00010000   /* synth keyboard: (major 1 << 16) | minor 0 */
#define SK_INFO_BREAK        2
#define SK_INFO_E0           4

/* Try WIN8_1 first (its 40-byte INITIATE_CONTACT is complete), then WIN10. */
static const uint32_t vmbus_versions[] = { 0x00030000, 0x00040000 };

/* synthetic keyboard interface type GUID f912ad6d-2b17-48ea-bd65-f927a61c7684 */
static const uint8_t KBD_GUID[16] = {
    0x6d,0xad,0x12,0xf9, 0x17,0x2b, 0xea,0x48, 0xbd,0x65, 0xf9,0x27,0xa6,0x1c,0x76,0x84
};

/* ring buffer: 1 control page + 2 data pages per direction */
#define RING_DATA_PAGES 2
#define SEND_PAGES      (1 + RING_DATA_PAGES)
#define RECV_PAGES      (1 + RING_DATA_PAGES)
#define RING_PAGES      (SEND_PAGES + RECV_PAGES)
#define RING_DATA_SZ    (RING_DATA_PAGES * 4096u)
#define GPADL_HANDLE    0x1000

static uint64_t g_hcall;
static uint8_t *g_simp, *g_post;
static int      g_kbd_relid = -1;
static uint32_t g_kbd_connid;
static uint8_t *g_send, *g_recv;
static int      g_open;
static int      g_stage;           /* how far bring-up got (HUD: HVSTG) */
static int      g_reason;          /* stage-2 failure reason (HUD: HVRSN) */
static uint64_t g_post_status;     /* last HvPostMessage raw status (diag) */
static int      g_poll_sint;       /* SINT a polled message arrived on     */
static uint32_t g_msg_connid = VMBUS_MESSAGE_CONNID;   /* working msg connection */

/* ---- page allocator (4 KiB aligned, zeroed; heap is identity-mapped) ---- */
static void *palloc_n(unsigned pages) {
    uint8_t *p = (uint8_t *)kmalloc((uint64_t)(pages + 1) * 4096);
    if (!p) return 0;
    uintptr_t a = ((uintptr_t)p + 0xFFF) & ~(uintptr_t)0xFFF;
    memset((void *)a, 0, pages * 4096);
    return (void *)a;
}
static void *palloc(void) { return palloc_n(1); }

/* ---- hypercalls: rcx=control, rdx=input GPA, r8=output GPA ---- */
static uint64_t hypercall(uint64_t control, uint64_t in_gpa, uint64_t out_gpa) {
    register uint64_t rcx asm("rcx") = control;
    register uint64_t rdx asm("rdx") = in_gpa;
    register uint64_t r8  asm("r8")  = out_gpa;
    register uint64_t rax asm("rax");
    __asm__ volatile("call *%[hp]" : "=a"(rax), "+r"(rcx), "+r"(rdx), "+r"(r8)
        : [hp] "r"(g_hcall) : "r9", "r10", "r11", "memory");
    return rax;
}
static void signal_host(uint32_t connid) {
    hypercall(HVCALL_SIGNAL_EVENT | HV_HYPERCALL_FAST, (uint64_t)connid, 0);
}

static int vmbus_post_cid(uint32_t connid, const void *msg, uint32_t len) {
    uint32_t *h = (uint32_t *)g_post;
    memset(g_post, 0, 256);
    h[0] = connid;
    h[2] = HVMSG_VMBUS;
    h[3] = len;
    const uint8_t *s = (const uint8_t *)msg;
    for (uint32_t i = 0; i < len; i++) g_post[16 + i] = s[i];
    g_post_status = hypercall(HVCALL_POST_MESSAGE, (uint64_t)(uintptr_t)g_post, 0);
    return (g_post_status & 0xFFFF) == 0;
}
static int vmbus_post(const void *msg, uint32_t len) { return vmbus_post_cid(g_msg_connid, msg, len); }

/* poll ALL 16 SynIC message slots (a reply may land on any SINT) */
static volatile uint8_t *vmbus_poll(uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++) {
        for (int s = 0; s < 16; s++) {
            volatile uint8_t *slot = g_simp + s * 256;
            if (*(volatile uint32_t *)slot != 0) { g_poll_sint = s; return slot + 16; }
        }
        __asm__ volatile("pause");
    }
    return 0;
}
static void vmbus_done(void) {
    volatile uint8_t *slot = g_simp + g_poll_sint * 256;
    *(volatile uint32_t *)slot = 0;
    wrmsr(HV_MSR_EOM, 0);
}

/* ---- ring buffers ---- */
static void ring_out(uint8_t *data, uint32_t *w, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) { data[*w] = s[i]; *w = (*w + 1) % RING_DATA_SZ; }
}
static void ring_send_inband(const void *payload, uint32_t plen) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_send;
    uint8_t *data = g_send + 4096;
    uint32_t w = ctl[0], start = w;
    uint32_t pad = (uint32_t)((-(int)plen) & 7);
    uint16_t desc[8]; memset(desc, 0, sizeof(desc));
    desc[0] = VM_PKT_DATA_INBAND;
    desc[1] = 2;                                  /* data offset (16/8) */
    desc[2] = (uint16_t)((16 + plen + pad) / 8);  /* len8 */
    ring_out(data, &w, desc, 16);
    ring_out(data, &w, payload, plen);
    uint8_t z[8] = {0}; ring_out(data, &w, z, pad);
    uint64_t prev = start; ring_out(data, &w, &prev, 8);
    __asm__ volatile("mfence" ::: "memory");
    ctl[0] = w;
    __asm__ volatile("mfence" ::: "memory");
    signal_host(g_kbd_connid);
}
static uint32_t ring_recv(void *buf, uint32_t maxlen) {
    volatile uint32_t *ctl = (volatile uint32_t *)g_recv;
    const uint8_t *data = g_recv + 4096;
    uint32_t r = ctl[1], w = ctl[0];
    if (r == w) return 0;
    __asm__ volatile("mfence" ::: "memory");
    uint8_t hdr[16];
    for (uint32_t i = 0; i < 16; i++) hdr[i] = data[(r + i) % RING_DATA_SZ];
    uint16_t off8 = *(uint16_t *)(hdr + 2), len8 = *(uint16_t *)(hdr + 4);
    if (!len8) return 0;
    uint32_t ppos = (r + off8 * 8u) % RING_DATA_SZ;
    uint32_t plen = (len8 - off8) * 8u;
    if (plen > maxlen) plen = maxlen;
    uint8_t *d = (uint8_t *)buf;
    for (uint32_t i = 0; i < plen; i++) d[i] = data[(ppos + i) % RING_DATA_SZ];
    ctl[1] = (r + len8 * 8u + 8u) % RING_DATA_SZ;
    __asm__ volatile("mfence" ::: "memory");
    return plen;
}

/* ---- Phase 2: open the keyboard channel ---- */
static int open_kbd_channel(void) {
    uint8_t *ring = (uint8_t *)palloc_n(RING_PAGES);
    if (!ring) { kputs("[hv] ring alloc failed\n"); return 0; }
    g_send = ring;
    g_recv = ring + SEND_PAGES * 4096;

    /* GPADL_HEADER: hand the ring buffer pages to the host */
    uint8_t msg[240]; memset(msg, 0, sizeof(msg));
    uint32_t *m = (uint32_t *)msg;
    m[0] = CHANNELMSG_GPADL_HEADER;
    m[2] = (uint32_t)g_kbd_relid;
    m[3] = GPADL_HANDLE;
    uint16_t *m16 = (uint16_t *)(msg + 16);
    m16[0] = (uint16_t)(8 + 8 * RING_PAGES);      /* range_buflen */
    m16[1] = 1;                                   /* rangecount   */
    uint32_t *rng = (uint32_t *)(msg + 20);
    rng[0] = RING_PAGES * 4096;                   /* byte_count   */
    rng[1] = 0;                                   /* byte_offset  */
    uint64_t base = (uint64_t)(uintptr_t)ring, *pfn = (uint64_t *)(msg + 28);
    for (int i = 0; i < RING_PAGES; i++) pfn[i] = (base >> 12) + i;
    if (!vmbus_post(msg, 28 + 8 * RING_PAGES)) { kputs("[hv] GPADL_HEADER post failed\n"); return 0; }

    volatile uint8_t *p = vmbus_poll(20000000);
    if (!p || *(volatile uint32_t *)p != CHANNELMSG_GPADL_CREATED) { kputs("[hv] no GPADL_CREATED\n"); if (p) vmbus_done(); return 0; }
    uint32_t gs = *(volatile uint32_t *)(p + 16); vmbus_done();   /* creation_status @16 (12 is the echoed handle) */
    kputs("[hv] GPADL created status "); kput_u32dec(gs); kputc('\n');
    if (gs) return 0;
    g_stage = 6;

    /* OPENCHANNEL */
    memset(msg, 0, sizeof(msg));
    m[0] = CHANNELMSG_OPENCHANNEL;
    m[2] = (uint32_t)g_kbd_relid;
    m[3] = 1;                                     /* openid */
    m[4] = GPADL_HANDLE;
    m[5] = 0;                                     /* target_vp */
    m[6] = SEND_PAGES;                            /* recv ring page offset */
    if (!vmbus_post(msg, 28 + 120)) { kputs("[hv] OPENCHANNEL post failed\n"); return 0; }

    p = vmbus_poll(20000000);
    if (!p || *(volatile uint32_t *)p != CHANNELMSG_OPENCHANNEL_RESULT) { kputs("[hv] no OPENCHANNEL_RESULT\n"); if (p) vmbus_done(); return 0; }
    uint32_t os = *(volatile uint32_t *)(p + 16); vmbus_done();   /* status @16 (12 is openid) */
    kputs("[hv] channel open status "); kput_u32dec(os); kputc('\n');
    if (os) return 0;
    g_open = 1; g_stage = 7;

    /* keyboard protocol handshake */
    struct { uint32_t type, version; } req = { SK_PROTOCOL_REQUEST, SK_VERSION };
    ring_send_inband(&req, sizeof(req));
    for (int t = 0; t < 60; t++) {
        uint8_t b[64];
        uint32_t n = ring_recv(b, sizeof(b));
        if (n >= 8 && *(uint32_t *)b == SK_PROTOCOL_RESPONSE) {
            uint32_t acc = *(uint32_t *)(b + 4) & 1;
            kputs("[hv] keyboard protocol response accepted="); kput_u32dec(acc); kputc('\n');
            g_stage = 8; return 1;
        }
        for (volatile int d = 0; d < 2000000; d++) __asm__ volatile("pause");
    }
    kputs("[hv] no protocol response (keys may still arrive)\n");
    g_stage = 8;
    return 1;
}

/* set-1 scancode -> ASCII (matches kbd64.c) */
static const char sc_ascii[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s',
    'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
    'b','n','m',',','.','/',0,'*',0,' ',0,
};

int hvkbd_getkey(void) {
    if (!g_open) return -1;
    uint8_t b[64];
    uint32_t n = ring_recv(b, sizeof(b));
    if (n < 12 || *(uint32_t *)b != SK_EVENT) return -1;
    uint16_t make = *(uint16_t *)(b + 4);
    uint32_t info = *(uint32_t *)(b + 8);
    if (info & SK_INFO_BREAK) return -1;              /* release */
    if (info & SK_INFO_E0) {
        switch (make) { case 0x48: return KEY_UP; case 0x50: return KEY_DOWN;
                        case 0x4B: return KEY_LEFT; case 0x4D: return KEY_RIGHT; default: return -1; }
    }
    if (make < 128) { char c = sc_ascii[make]; if (c) return (unsigned char)c; }
    return -1;
}

int hvkbd_present(void) { return g_kbd_relid >= 0; }
int hvkbd_stage(void)   { return g_stage; }
int hvkbd_reason(void)  { return g_reason; }

/* ---- Phase 1: bring-up + enumerate ---- */
int hvkbd_init(void) {
    uint32_t a, b, c, d;
    cpuid(0x40000000, &a, &b, &c, &d);
    if (!(b == 0x7263694D && c == 0x666F736F && d == 0x76482074)) {   /* "Microsoft Hv" */
        kputs("[hv] not running under Hyper-V; skipping VMBus keyboard\n");
        return 0;
    }
    g_stage = 1;
    kputs("[hv] Hyper-V detected\n");

    wrmsr(HV_MSR_GUEST_OS_ID, 0x1000000000000000ull);
    void *hp = palloc();
    wrmsr(HV_MSR_HYPERCALL, ((uint64_t)(uintptr_t)hp) | 1);
    g_hcall = (uint64_t)(uintptr_t)hp;

    g_simp = (uint8_t *)palloc();
    uint8_t *siefp = (uint8_t *)palloc();
    g_post = (uint8_t *)palloc();
    wrmsr(HV_MSR_SIMP,  ((uint64_t)(uintptr_t)g_simp) | 1);
    wrmsr(HV_MSR_SIEFP, ((uint64_t)(uintptr_t)siefp) | 1);
    wrmsr(HV_MSR_SCONTROL, 1);
    wrmsr(HV_MSR_SINT2, 0x30 | (1u << 17));
    g_stage = 2;
    kputs("[hv] hypercall + SynIC ready\n");

    void *ipage = palloc(), *m1 = palloc(), *m2 = palloc();

    /* Gen 2 UEFI firmware already used VMBus, so the host may think a client is
     * connected and ignore our INITIATE_CONTACT.  Reset with UNLOAD first. */
    {
        struct { uint32_t t, p; } ul = { CHANNELMSG_UNLOAD, 0 };
        vmbus_post(&ul, sizeof(ul));
        volatile uint8_t *p = vmbus_poll(10000000);
        if (p) vmbus_done();          /* drain UNLOAD_RESPONSE (residual firmware state) */
    }

    /* INITIATE_CONTACT: offset 16 = interrupt_page GPA (WIN8.1/WIN10-v4).  Try
     * message connection ids 4 then 1, scanning all SINTs, logging everything. */
    static const uint32_t connids[2] = { 4, 1 };
    uint32_t agreed = 0;
    for (unsigned v = 0; v < sizeof(vmbus_versions) / sizeof(vmbus_versions[0]) && !agreed; v++) {
        for (int ci = 0; ci < 2 && !agreed; ci++) {
            uint8_t ic[40]; memset(ic, 0, sizeof(ic));
            uint32_t *w = (uint32_t *)ic;
            w[0] = CHANNELMSG_INITIATE_CONTACT;
            w[2] = vmbus_versions[v];
            *(uint64_t *)(ic + 16) = (uint64_t)(uintptr_t)ipage;
            *(uint64_t *)(ic + 24) = (uint64_t)(uintptr_t)m1;
            *(uint64_t *)(ic + 32) = (uint64_t)(uintptr_t)m2;
            int posted = vmbus_post_cid(connids[ci], ic, sizeof(ic));
            if (!posted) { g_reason = 1; continue; }
            volatile uint8_t *p = vmbus_poll(10000000);
            if (!p) { g_reason = 2; continue; }
            uint32_t mtype = *(volatile uint32_t *)p; uint8_t ok = p[8];
            vmbus_done();
            if (mtype == CHANNELMSG_VERSION_RESPONSE && ok) {
                agreed = vmbus_versions[v]; g_reason = 0; g_msg_connid = connids[ci];
                kputs("[hv] VMBus connected: version "); kput_u64hex(agreed);
                kputs(" (cid "); kput_u32dec(connids[ci]); kputs(")\n");
            } else g_reason = 3;
        }
    }
    if (!agreed) { kputs("[hv] VMBus connect FAILED (stage 2)\n"); return 0; }
    g_stage = 3;

    struct { uint32_t type, pad; } req = { CHANNELMSG_REQUESTOFFERS, 0 };
    if (!vmbus_post(&req, sizeof(req))) { kputs("[hv] REQUESTOFFERS failed\n"); return 0; }

    int offers = 0;
    for (;;) {
        volatile uint8_t *p = vmbus_poll(20000000);
        if (!p) { kputs("[hv] offer poll timeout\n"); break; }
        uint32_t mtype = *(volatile uint32_t *)p;
        if (mtype == CHANNELMSG_ALLOFFERS_DONE) { vmbus_done(); break; }
        if (mtype == CHANNELMSG_OFFERCHANNEL) {
            const volatile uint8_t *if_type = p + 8;
            uint32_t relid  = *(volatile uint32_t *)(p + 8 + 176);   /* child_relid   */
            uint32_t connid = *(volatile uint32_t *)(p + 8 + 184);   /* connection_id */
            int is_kbd = 1;
            for (int i = 0; i < 16; i++) if (if_type[i] != KBD_GUID[i]) { is_kbd = 0; break; }
            if (is_kbd) { g_kbd_relid = (int)relid; g_kbd_connid = connid; }
            offers++;
        }
        vmbus_done();
        if (offers > 64) break;
    }
    g_stage = 4;

    if (g_kbd_relid < 0) { kputs("[hv] synthetic keyboard NOT offered\n"); return 0; }
    g_stage = 5;
    kputs("[hv] keyboard relid "); kput_u32dec((uint32_t)g_kbd_relid);
    kputs(" connid "); kput_u32dec(g_kbd_connid); kputs("; opening channel\n");
    return open_kbd_channel();
}
