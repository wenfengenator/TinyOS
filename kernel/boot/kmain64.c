/* tinyos x64 kernel entry (R3).
 *
 * Reached from the UEFI loader (boot/efi_main.c) via the assembly trampoline
 * kstart (kernel/entry64.s) *after* ExitBootServices.  The firmware has already
 * placed us in 64-bit long mode with paging + an identity map, so this stage
 * just brings up the serial + framebuffer console, proves it, and halts.
 *
 * Later requirements grow the kernel from here (IDT, timer, keyboard, heap,
 * SMP, and the Doom-enabling infrastructure). */
#include "boot64.h"
#include "wm64.h"
#include "arch64.h"

void kmain64(boot_info_t *bi) {
    serial_init();
    kputs("\n[tinyos64] long-mode kernel entry (post-ExitBootServices)\n");

    if (bi->fb_base) {
        fb_init(bi);
        kputs("[tinyos64] framebuffer ");
        kput_u32dec(bi->width); kputc('x'); kput_u32dec(bi->height);
        kputs(" pitch "); kput_u32dec(bi->pitch_px);
        kputs(" fmt ");   kput_u32dec(bi->pixel_format);
        kputs(" @ ");     kput_u64hex(bi->fb_base);
        kputc('\n');

        /* Colour swatches in the top-right prove real GOP pixel writes and the
         * pixel-format packing; read three back and report over serial so the
         * framebuffer path is verifiable headlessly. */
        fb_rect(bi->width - 96, 8, 24, 24, 0xFF0000);   /* red   */
        fb_rect(bi->width - 64, 8, 24, 24, 0x00FF00);   /* green */
        fb_rect(bi->width - 32, 8, 24, 24, 0x0000FF);   /* blue  */
        kputs("[tinyos64] fb readback R="); kput_u64hex(fb_peek(bi->width - 96 + 12, 20));
        kputs(" G="); kput_u64hex(fb_peek(bi->width - 64 + 12, 20));
        kputs(" B="); kput_u64hex(fb_peek(bi->width - 32 + 12, 20));
        kputc('\n');
    } else {
        kputs("[tinyos64] WARNING: firmware provided no framebuffer\n");
    }

    if (bi->mmap_addr) {
        kputs("[tinyos64] uefi memory map @ "); kput_u64hex(bi->mmap_addr);
        kputs(" size "); kput_u32dec((uint32_t)bi->mmap_size);
        kputs(" desc "); kput_u32dec((uint32_t)bi->mmap_desc_size);
        kputc('\n');
    }

    kputs("[tinyos64] SERIAL + GOP CONSOLE ONLINE\n");

    /* ---- R5 infrastructure: time, heap, keyboard ---- */
    time_init(bi->rsdp);
    time_set_epoch(bi->boot_epoch);            /* wall clock from UEFI GetTime() */
    heap_init(bi);
    kbd64_init();
    kputs("[tinyos64] tsc ");  kput_u32dec((uint32_t)(tsc_freq_hz() / 1000000));
    kputs(" MHz; heap ");      kput_u32dec((uint32_t)(heap_total() / (1024 * 1024)));
    kputs(" MB\n");

    if (bi->asset_base) {
        const uint8_t *a = (const uint8_t *)(uintptr_t)bi->asset_base;
        uint64_t sum = 0;
        for (uint64_t i = 0; i < bi->asset_size; i++) sum += a[i];
        kputs("[tinyos64] asset "); kput_u32dec((uint32_t)bi->asset_size);
        kputs(" B @ "); kput_u64hex(bi->asset_base);
        kputs(" sum "); kput_u64hex(sum); kputc('\n');
    } else {
        kputs("[tinyos64] no asset loaded\n");
    }

    smp_init(bi);
    hvkbd_init();
    arch_init();

    kputs("[tinyos64] TINYOS X64 UEFI BOOT OK\n");
    arch_user_init();
    if (disp_init() != 0) kputs("[disp] shared back buffer unavailable\n");
    login_run(bi);
    wm_run(bi);

    kputs("[tinyos64] halting.\n");
    for (;;) __asm__ volatile("hlt");
}
