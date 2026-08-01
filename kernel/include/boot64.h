/* tinyos x64 boot handoff + kernel console API (shared by the UEFI loader in
 * boot/efi_main.c and the 64-bit kernel in kernel/kmain64.c, kernel/fb64.c). */
#ifndef TINYOS_BOOT64_H
#define TINYOS_BOOT64_H

#include <stdint.h>

/* TinyOS product version. */
#define TINYOS_VERSION "1.0.0"

/* Handoff block: the UEFI loader fills this before ExitBootServices and passes
 * a pointer to the 64-bit kernel entry. */
typedef struct {
    uint64_t fb_base;        /* linear framebuffer physical address           */
    uint64_t fb_size;        /* framebuffer size in bytes                     */
    uint32_t width;          /* visible width  in pixels                      */
    uint32_t height;         /* visible height in pixels                      */
    uint32_t pitch_px;       /* pixels per scan line (row stride)             */
    uint32_t pixel_format;   /* PF_RGBX8888 / PF_BGRX8888                      */
    uint64_t mmap_addr;      /* saved UEFI memory map (for later phases)       */
    uint64_t mmap_size;
    uint64_t mmap_desc_size;
    uint64_t asset_base;     /* large file (WAD) loaded from the ESP, or 0     */
    uint64_t asset_size;
    uint64_t rsdp;           /* ACPI RSDP physical address (for SMP/MADT), or 0 */
    uint32_t gop_modes;      /* count of GOP modes the firmware offered (diag)   */
    uint64_t boot_epoch;     /* UEFI GetTime() wall clock at boot (Unix sec), or 0 */
} boot_info_t;

/* 32-bpp pixel formats (values match EFI_GRAPHICS_PIXEL_FORMAT 0 and 1). */
#define PF_RGBX8888 0
#define PF_BGRX8888 1

/* non-ASCII key codes returned by kbd64_getkey() for nav + function keys */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83
#define KEY_F1    0x84
#define KEY_F2    0x85
#define KEY_F3    0x86
#define KEY_ESC   0x1B

/* ---- kernel console: serial (COM1) + GOP framebuffer (kernel/fb64.c) ---- */
void     serial_init(void);
void     fb_init(const boot_info_t *bi);
void     fb_fill(uint32_t rgb);
void     fb_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
uint32_t fb_peek(uint32_t x, uint32_t y);      /* read a pixel back (verification) */
void     kputc(char c);                        /* writes to serial + framebuffer   */
void     kputs(const char *s);
void     kput_u64hex(uint64_t v);
void     kput_u32dec(uint32_t v);

/* kernel-log scrollback for the WM log widget (kernel/fb64.c) */
int         klog_lines(void);                  /* retained line count               */
const char *klog_line(int i);                  /* line i (0 = oldest retained)      */
void        klog_wm_active(int on);            /* desktop up: stop the fb console   */

/* framebuffer helpers for the demo (kernel/fb64.c) */
uint32_t fb_pack(uint32_t rgb);                /* 0xRRGGBB -> device pixel         */
void     fb_get(uint32_t *w, uint32_t *h);     /* visible dimensions               */
void     fb_present(const uint32_t *back);     /* blit a tight w*h back buffer     */
void     fb_present_diff(const uint32_t *back, uint32_t *shadow,
                        uint32_t w, uint32_t h); /* blit only changed scanlines     */

/* display layer (kernel/disp64.c): a swappable backend behind a stable seam.
 * The default backend is the GOP / Hyper-V framebuffer (double buffer + scanline
 * diff); a future transport (virtio-gpu page-flip, synthvid) registers its own
 * backend and the WM / apps keep drawing into disp_back() + calling disp_present(). */
typedef struct display_backend {
    const char *name;
    int         outputs;                       /* number of display outputs (1 on GOP) */
    int       (*init)(void);                    /* allocate scanout(s); 0 on success   */
    uint32_t *(*back)(void);                    /* current draw target (w*h, tight)    */
    void      (*dims)(uint32_t *w, uint32_t *h);
    void      (*present)(void);                 /* present the back buffer (diff/flip) */
    void      (*present_full)(void);            /* unconditional full present          */
    void      (*flip)(void);                    /* page-flip if supported, else present */
} display_backend_t;

void        display_register(const display_backend_t *b);  /* a backend installs itself */
const char *display_backend_name(void);        /* active backend name               */
int         display_outputs(void);             /* active backend output count       */

int       disp_init(void);                     /* alloc scanout(s) via the backend  */
uint32_t *disp_back(void);                     /* the shared tight w*h back buffer  */
void      disp_dims(uint32_t *w, uint32_t *h); /* back-buffer dimensions            */
void      disp_present(void);                  /* present (scanline diff / flip)    */
void      disp_present_full(void);             /* unconditional full present        */
void      disp_flip(void);                     /* page-flip the scanout (else present) */
void     fb_text_to(uint32_t *buf, uint32_t x, uint32_t y,
                    const char *s, uint32_t fg_rgb, uint32_t bg_rgb);
void     fb_text_to_s(uint32_t *buf, uint32_t x, uint32_t y,        /* integer-scaled */
                     const char *s, uint32_t fg_rgb, uint32_t bg_rgb, uint32_t scale);

/* ---- heap: physical allocator over UEFI conventional memory (kernel/mm64.c) ---- */
void     heap_init(const boot_info_t *bi);
void    *kmalloc(uint64_t n);
void     kfree(void *p);
void    *page_alloc(uint64_t pages);           /* 4KB-aligned zeroed pages         */
void    *page_base(void);                      /* heap arena base (VA==EFI PhysStart)*/
void     page_free(void *p, uint64_t pages);   /* return a page run for reuse      */
void    *memset(void *d, int c, unsigned long n);
void    *memcpy(void *d, const void *s, unsigned long n);
uint64_t heap_total(void);                     /* arena size in bytes              */
uint64_t heap_used(void);                      /* bump high-water mark in bytes    */

/* ---- monotonic time via calibrated TSC (kernel/time64.c) ---- */
void     time_init(uint64_t rsdp);             /* rsdp: for the ACPI PM timer      */
void     time_set_epoch(uint64_t epoch);       /* seed the wall clock (UEFI GetTime) */
uint64_t wall_epoch(void);                     /* current Unix wall-clock seconds, or 0 */
uint64_t millis(void);
uint64_t micros(void);
void     sleep_ms(uint32_t ms);
uint64_t tsc_freq_hz(void);

/* ---- polled PS/2 keyboard (kernel/kbd64.c) ---- */
void     kbd64_init(void);
int      kbd64_getkey(void);                   /* ASCII/KEY_* code, or -1 if none  */
void     arch_load_cpu_tables(void);           /* load shared GDT+IDT on a secondary CPU */

/* ---- graphics demo (kernel/demo64.c) + pong game (kernel/pong64.c) ---- */
void     demo_run(boot_info_t *bi);
void     pong_run(boot_info_t *bi);

/* ---- preemptive scheduler (kernel/sched/sched64.c) ---- */
void     sched_init(void);
int      sched_spawn(void (*fn)(uint64_t), uint64_t arg);
int      usched_spawn_user(uint64_t entry, uint64_t user_rsp, uint8_t *kstack,
                           uint64_t ksize, uint64_t rbx, uint64_t r15v, uint64_t cr3, void *proc);
int      usched_spawn_forked(uint64_t entry, uint64_t user_rsp, uint8_t *kstack,
                             uint64_t ksize, uint64_t cr3, void *proc, const uint64_t *callee);
void     sched_start(void);
void     sched_stop(void);
uint64_t sched_switches(void);
void     sched_set_current_cr3(uint64_t cr3);   /* execve: retarget running task CR3 */
void     sched_demo(void);
int      sched_test_run(int stress);           /* scheduler test battery (smoke/stress) */
void     sched_block(int chan);                /* block the running task on a wait channel */
void     sched_wake(int chan);                 /* wake tasks blocked on a channel          */
void     sched_wait(volatile int *addr, int want, int chan); /* race-free block until *addr==want */
void     sched_exit(void);                     /* retire the running task (never returns)  */

/* composite + present one desktop frame (kernel/wm64.c); used by blocking syscalls */
void     wm_pump(void);

/* ---- multi-core SMP bring-up (kernel/smp64.c) ---- */
void     smp_init(boot_info_t *bi);
int      smp_cpu_count(void);      /* CPUs the firmware/ACPI reported */
int      smp_online_count(void);   /* cores that reached long-mode kernel code */
uint64_t smp_lapic_base(void);     /* Local APIC MMIO base (from ACPI MADT)     */

/* ---- virtio-blk persistent disk (kernel/vblk64.c) ---- */
int      vblk_init(void);                    /* 1 if a virtio-blk device is present */
int      vblk_read(uint32_t lba, void *buf);         /* one 512-byte sector          */
int      vblk_write(uint32_t lba, const void *buf);
/* ---- VMBus primitives shared by synthetic drivers (kernel/vmbus64.c) ---- */
int      vmbus_ready(void);                   /* VMBus connected + offers enumerated */
void    *vmbus_palloc(unsigned pages);
int      vmbus_msg_post(const void *msg, uint32_t len);
volatile unsigned char *vmbus_msg_poll(uint32_t iters);
void     vmbus_msg_done(void);
void     vmbus_signal_event(uint32_t connid);
int      vmbus_find_offer(const uint8_t *guid, uint32_t *relid, uint32_t *connid);

/* ---- Hyper-V synthetic SCSI (StorVSC) persistent disk (kernel/storvsc64.c) ---- */
int      storvsc_init(void);                  /* 1 if a persistent SCSI disk is ready */
int      storvsc_read(uint32_t lba, void *buf);
int      storvsc_write(uint32_t lba, const void *buf);
int      storvsc_read_many(uint32_t lba, void *buf, uint32_t sectors);
int      storvsc_write_many(uint32_t lba, const void *buf, uint32_t sectors);
int      storvsc_stage(void);                 /* bring-up progress 0..12 (diagnostic) */
int      storvsc_diag(void);                  /* last I/O (status<<16|srb<<8|scsi); ~0=timeout */
int      storvsc_ver(void);                   /* negotiated vstor protocol version (e.g. 0x0602) */
int      storvsc_blksz(void);                 /* disk block size from READ CAPACITY (want 512) */

/* virtio-net NIC (Cycle 8 N1): raw Ethernet frame transport for the TCP/IP stack */
int      vnet_init(void);                     /* 1 if a virtio-net device is present */
void     vnet_mac(uint8_t out[6]);            /* our MAC address                     */
int      vnet_tx(const void *frame, uint32_t len);    /* send one Ethernet frame     */
int      vnet_rx(void *buf, uint32_t max);            /* recv one frame, 0 if none    */
int      vnet_selftest(void);                 /* N1 gate: ARP round-trip (0 = ok)    */

/* Hyper-V synthetic NIC (netvsc + RNDIS over VMBus; kernel/drivers/netvsc64.c) */
int      netvsc_init(void);                   /* 1 if a Hyper-V NIC came up          */
void     netvsc_mac(uint8_t out[6]);
int      netvsc_tx(const void *frame, uint32_t len);
int      netvsc_rx(void *buf, uint32_t max);
int      netvsc_stage(void);                  /* bring-up progress 1..9 (diagnostic) */
int      netvsc_diag(void);

int      nic_init(void);                      /* bring up a NIC (virtio or netvsc)   */

/* minimal TCP/IP stack (Cycle 8 N2): ARP/IPv4/ICMP/UDP/DNS over virtio-net */
int      net_init(void);                      /* bind to the NIC (MAC, addressing)   */
int      net_selftest(void);                  /* N2 gate: ping gateway + DNS (0 = ok) */
uint32_t net_local_ip(void);                  /* our IPv4 address (host order)        */
uint32_t net_resolve(const char *name);       /* DNS A-record lookup (host order, 0=fail) */
int      net_ip_send(uint32_t dst, uint8_t proto, const void *payload, int plen);
int      net_ping(uint32_t dst, uint32_t *rtt_us); /* one ICMP echo; 0=reply (rtt in us), -1=timeout */
void     net_ifconfig(uint32_t *ip, uint32_t *mask, uint32_t *gw, uint32_t *dns, uint8_t *mac, int *up);
void     net_setcfg(uint32_t ip, uint32_t mask, uint32_t gw, uint32_t dns); /* static IP config */
int      net_poll(void);                      /* service one RX/loopback packet       */
void     net_drain(void);                     /* drain the RX backlog (busy-network robustness) */

/* TCP (Cycle 8 N3): connection-oriented transport + the socket engine */
void     tcp_input(uint32_t src, const uint8_t *seg, int len);
int      tcp_selftest(void);                  /* N3 gate: loopback connect/data/close */
int      tcp_connect(uint32_t dst, uint16_t dport);   /* active open; TCB index or -1 */
int      tcp_listen(uint16_t port);                   /* passive open; listener index  */
int      tcp_accept(int lidx);                        /* next established child, or -1  */
int      tcp_send(int idx, const void *data, int len);
int      tcp_recv(int idx, void *buf, int max);       /* bytes, 0 = EOF, -1 = error     */
int      tcp_close(int idx);
void     tcp_peer(int idx, uint32_t *ip, uint16_t *port);   /* remote endpoint of a TCB  */

/* ---- Hyper-V VMBus synthetic keyboard (kernel/vmbus64.c) ---- */
int      hvkbd_init(void);         /* -> 1 if the Hyper-V keyboard channel is opened */
int      hvkbd_getkey(void);       /* ASCII/KEY_* code, or -1 */
int      hvkbd_present(void);      /* 1 if the synthetic keyboard channel was offered */
int      hvkbd_stage(void);        /* bring-up progress 0..8 (HUD: HVSTG) */
int      hvkbd_reason(void);       /* stage-2 fail reason: 1 post 2 no-reply 3 rejected */
void     vmbus_synic_set_masked(int masked);  /* silence the SynIC message SINT (scheduler) */

/* 64-bit kernel entry, called by the loader (via kstart) after ExitBootServices */
void     kmain64(boot_info_t *bi);

#endif /* TINYOS_BOOT64_H */
