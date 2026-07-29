/* tinyos x64 boot handoff + kernel console API (shared by the UEFI loader in
 * boot/efi_main.c and the 64-bit kernel in kernel/kmain64.c, kernel/fb64.c). */
#ifndef TINYOS_BOOT64_H
#define TINYOS_BOOT64_H

#include <stdint.h>

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

/* framebuffer helpers for the demo (kernel/fb64.c) */
uint32_t fb_pack(uint32_t rgb);                /* 0xRRGGBB -> device pixel         */
void     fb_get(uint32_t *w, uint32_t *h);     /* visible dimensions               */
void     fb_present(const uint32_t *back);     /* blit a tight w*h back buffer     */
void     fb_text_to(uint32_t *buf, uint32_t x, uint32_t y,
                    const char *s, uint32_t fg_rgb, uint32_t bg_rgb);
void     fb_text_to_s(uint32_t *buf, uint32_t x, uint32_t y,        /* integer-scaled */
                     const char *s, uint32_t fg_rgb, uint32_t bg_rgb, uint32_t scale);

/* ---- heap: physical allocator over UEFI conventional memory (kernel/mm64.c) ---- */
void     heap_init(const boot_info_t *bi);
void    *kmalloc(uint64_t n);
void     kfree(void *p);
void    *page_alloc(uint64_t pages);           /* 4KB-aligned zeroed pages         */
void    *memset(void *d, int c, unsigned long n);
void    *memcpy(void *d, const void *s, unsigned long n);
uint64_t heap_total(void);                     /* arena size in bytes              */
uint64_t heap_used(void);                      /* bump high-water mark in bytes    */

/* ---- monotonic time via calibrated TSC (kernel/time64.c) ---- */
void     time_init(uint64_t rsdp);             /* rsdp: for the ACPI PM timer      */
uint64_t millis(void);
uint64_t micros(void);
void     sleep_ms(uint32_t ms);
uint64_t tsc_freq_hz(void);

/* ---- polled PS/2 keyboard (kernel/kbd64.c) ---- */
void     kbd64_init(void);
int      kbd64_getkey(void);                   /* ASCII/KEY_* code, or -1 if none  */

/* ---- graphics demo (kernel/demo64.c) + pong game (kernel/pong64.c) ---- */
void     demo_run(boot_info_t *bi);
void     pong_run(boot_info_t *bi);

/* ---- preemptive scheduler demo (kernel/sched64.c) ---- */
void     sched_demo(void);

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
int      storvsc_stage(void);                 /* bring-up progress 0..12 (diagnostic) */
int      storvsc_diag(void);                  /* last I/O (status<<16|srb<<8|scsi); ~0=timeout */
int      storvsc_ver(void);                   /* negotiated vstor protocol version (e.g. 0x0602) */
int      storvsc_blksz(void);                 /* disk block size from READ CAPACITY (want 512) */

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
