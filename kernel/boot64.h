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

/* composite + present one desktop frame (kernel/wm64.c); used by blocking syscalls */
void     wm_pump(void);

/* ---- multi-core SMP bring-up (kernel/smp64.c) ---- */
void     smp_init(boot_info_t *bi);
int      smp_cpu_count(void);      /* CPUs the firmware/ACPI reported */
int      smp_online_count(void);   /* cores that reached long-mode kernel code */

/* ---- Hyper-V VMBus synthetic keyboard (kernel/vmbus64.c) ---- */
int      hvkbd_init(void);         /* -> 1 if the Hyper-V keyboard channel is opened */
int      hvkbd_getkey(void);       /* ASCII/KEY_* code, or -1 */
int      hvkbd_present(void);      /* 1 if the synthetic keyboard channel was offered */
int      hvkbd_stage(void);        /* bring-up progress 0..8 (HUD: HVSTG) */
int      hvkbd_reason(void);       /* stage-2 fail reason: 1 post 2 no-reply 3 rejected */

/* 64-bit kernel entry, called by the loader (via kstart) after ExitBootServices */
void     kmain64(boot_info_t *bi);

#endif /* TINYOS_BOOT64_H */
