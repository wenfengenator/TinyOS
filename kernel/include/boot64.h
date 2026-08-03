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
    uint64_t efi_base;       /* loader's own \EFI\BOOT\BOOTX64.EFI in memory, or 0  */
    uint64_t efi_size;       /* its size in bytes (for the disk installer)          */
    uint64_t booted_removable; /* 1 = booted from removable media (USB), 0 = fixed disk */
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
void     fb_panic(uint64_t vec, uint64_t err, uint64_t rip,
                  uint64_t cr2, uint64_t cs, int64_t rip_off,
                  const int64_t *bt, int nbt, int64_t mapoff);  /* fatal-fault screen */
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
int      sched_is_running(void);               /* 1 while the LAPIC timer is live (safe to HLT) */
int      lapic_ready(void);                    /* LAPIC timer calibrated (idle-tick usable) */
void     lapic_idle_arm(void);                 /* arm a periodic tick to wake a native-console HLT */
void     lapic_idle_disarm(void);             /* mask it again after waking */
uint64_t cpu_idle_ms(void);                    /* accumulated CPU-halted milliseconds (for CPU%%) */
void     cpu_idle_add_ms(uint64_t ms);         /* add halted time from a HLT outside idle_wait */

/* composite + present one desktop frame (kernel/wm64.c); used by blocking syscalls */
void     wm_pump(void);
/* Note UI activity (keypress / terminal output) so wm_pump repaints at 30 FPS
 * briefly, then drops to ~2 FPS when idle to save power. */
void     ui_note_activity(void);
/* Low-power sleep: blank the panel + stop compositing (deep HLT idle) on lid
 * close, restore on lid open / keypress.  power_poll() samples the lid. */
void     wm_sleep_set(int on);
int      wm_is_sleeping(void);
void     power_poll(void);
int      acpi_lid_open(void);                  /* 1 = lid open (or EC absent), 0 = closed */
int      acpi_power_button(void);              /* 1 on a fresh power-button press (latch cleared) */
/* Handle a WM hotkey (F1 focus / F2 move / arrows for move+log-scroll) even while
 * a foreground app owns the keyboard; returns 1 if the key was consumed. */
int      wm_hotkey(int key);

/* ---- multi-core SMP bring-up (kernel/smp64.c) ---- */
void     smp_init(boot_info_t *bi);
int      smp_cpu_count(void);      /* CPUs the firmware/ACPI reported */
int      smp_online_count(void);   /* cores that reached long-mode kernel code */
uint64_t smp_lapic_base(void);     /* Local APIC MMIO base (from ACPI MADT)     */

/* ---- ACPI power management + Embedded Controller (kernel/acpi64.c) ---- */
typedef struct { int present, ac_online, charging, percent, voltage_mv; uint8_t raw[8]; } acpi_batt_t;
void     acpi_init(uint64_t rsdp);            /* parse FADT/ECDT/DSDT, probe EC       */
int      acpi_battery(acpi_batt_t *b);        /* fill battery/AC state; <0 if no EC   */
int      acpi_ec_read(uint8_t off, uint8_t *out);  /* raw EC RAM read (battery calibration) */
int      acpi_poweroff(void);                 /* ACPI S5 soft-off; <0 if unavailable  */
void     acpi_reset(void);                    /* reset register / 0xCF9 / 8042        */
int      acpi_s3_available(void);             /* 1 if S3 SLP_TYP + PM1 known          */

/* ---- virtio-blk persistent disk (kernel/vblk64.c) ---- */
int      vblk_init(void);                    /* 1 if a virtio-blk device is present */
int      vblk_read(uint32_t lba, void *buf);         /* one 512-byte sector          */
int      vblk_write(uint32_t lba, const void *buf);
/* ---- AHCI/SATA real-hardware disk (kernel/ahci64.c) ---- */
int      ahci_init(void);                            /* 1 if a SATA disk was found    */
int      ahci_read(uint64_t lba, uint32_t count, void *buf);
int      ahci_write(uint64_t lba, uint32_t count, const void *buf);
uint64_t ahci_sectors(void);                         /* disk size in 512-byte sectors */
int      ahci_present(void);

/* ---- USB host (xHCI) + Mass Storage block device (kernel/drivers/usb64.c) ---- */
int      usb_init(void);                             /* 1 if a USB mass-storage device is ready */
int      usb_present(void);
int      usb_gen(void);                              /* bumps on connect/disconnect (mount invalidation) */
uint64_t usb_sectors(void);                          /* stick size in 512-byte sectors */
int      usb_read(uint64_t lba, uint32_t count, void *buf);
int      usb_write(uint64_t lba, uint32_t count, const void *buf);

/* ---- FAT32 reader for the internal-disk ESP over AHCI (kernel/fs/esp64.c) ---- */
typedef void (*esp_dirent_cb)(const char *name, uint32_t size, int isdir,
                              uint32_t first_cluster, void *ctx);
int      esp_mount(void);                            /* 1 if the internal ESP mounted */
uint32_t esp_root_cluster(void);
void     esp_list(uint32_t dir_cluster, esp_dirent_cb cb, void *ctx);
int      esp_find(const char *path, uint32_t *clus, uint32_t *size, int *isdir);
int      esp_install(const uint8_t *efi, uint32_t efi_len);   /* clean-install to the SATA disk */

/* ---- FAT32 read/write data volume on the internal disk (kernel/fs/datavol64.c) ---- */
/* Generic FAT32 engine over a selectable 512B block backend (AHCI or USB).
 * NOTE: a backend *tag* is used rather than function pointers on purpose — this
 * PE/EFI kernel is built -fPIC and runtime function-address materialization
 * (&func passed as a value) is miscompiled to a GOT-style load that the mi386pep
 * linker resolves to the code itself, so runtime function pointers crash. */
#define FV_AHCI 0
#define FV_USB  1
typedef struct fatvol {
    int      mnt;
    int      backend;                                /* FV_AHCI | FV_USB */
    uint64_t part, fat, data;
    uint32_t spc, root, fatsz, maxclus, alloc_hint;
} fatvol_t;
int      fv_mount(fatvol_t *v, int backend);         /* find + mount a FAT32 volume */
int      fv_resolve(fatvol_t *v, const char *path, uint32_t *clus, uint32_t *size, int *isdir);
int      fv_read(fatvol_t *v, uint32_t first, uint32_t size, void *buf, uint32_t max);
int      fv_readdir(fatvol_t *v, uint32_t dir_clus, int idx, char *name);
int      fv_readdir_m(fatvol_t *v, uint32_t dir_clus, int idx, char *name,
                      uint8_t *attr, uint32_t *clus, uint32_t *size);
int      fv_write(fatvol_t *v, const char *path, const void *data, uint32_t len);
int      fv_pread(fatvol_t *v, const char *path, void *buf, uint32_t len, uint32_t off);
int      fv_pwrite(fatvol_t *v, const char *path, const void *buf, uint32_t len, uint32_t off);
int      fv_truncate(fatvol_t *v, const char *path, uint32_t newsize);
int      fv_mkdir(fatvol_t *v, const char *path);
int      fv_unlink(fatvol_t *v, const char *path);
int      fv_rmdir(fatvol_t *v, const char *path);
fatvol_t *dv_vol(void);                              /* internal /mnt volume, or 0 if unavailable */
fatvol_t *uv_vol(void);                              /* USB /usb volume, or 0 if unavailable */

int      dv_mount(void);                             /* 1 if the data partition mounted */
int      dv_ready(void);
int      dv_resolve(const char *path, uint32_t *clus, uint32_t *size, int *isdir);
int      dv_read(uint32_t first, uint32_t size, void *buf, uint32_t max);
int      dv_readdir(uint32_t dir_clus, int idx, char *name);
int      dv_write(const char *path, const void *data, uint32_t len);
int      dv_mkdir(const char *path);
int      dv_unlink(const char *path);
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
