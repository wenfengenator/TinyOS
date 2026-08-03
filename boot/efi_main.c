/* tinyos x64 UEFI loader (R2).
 *
 * Self-contained: NO gnu-efi, NO mingw.  We declare just enough of the UEFI ABI
 * (Microsoft x64 calling convention) to:
 *   1. locate the Graphics Output Protocol and record its linear framebuffer,
 *   2. obtain the final memory map and call ExitBootServices,
 *   3. hand off to the 64-bit kernel entry (kstart -> kmain64).
 *
 * The loader and the kernel are linked into one PE32+ EFI application, so the
 * "jump to the kernel" is simply a call once boot services are gone. */

#include <stdint.h>
#include "boot64.h"

/* ---- minimal UEFI types ---- */
typedef uint16_t CHAR16;
typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS           0
#define EFI_BUFFER_TOO_SMALL  0x8000000000000005ULL
#define EFILOADERDATA         2                 /* EFI_MEMORY_TYPE            */

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } EFI_GUID;

/* ---- Simple Text Output (for an early pre-ExitBootServices greeting) ---- */
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset; EFI_TEXT_STRING OutputString;
    void *TestString, *QueryMode, *SetMode, *SetAttribute,
         *ClearScreen, *SetCursorPosition, *EnableCursor, *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ---- Boot Services (fields laid out to the exact UEFI offsets we use) ---- */
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(UINTN *, void *, UINTN *, UINTN *, uint32_t *);
typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(int, UINTN, void **);
typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(void *);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(EFI_HANDLE, UINTN);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(EFI_GUID *, void *, void **);
typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE, EFI_GUID *, void **);

typedef struct {
    char  Hdr[24];                                              /*   0 */
    void *RaiseTPL, *RestoreTPL;                                /*  24, 32 */
    void *AllocatePages, *FreePages;                            /*  40, 48 */
    EFI_GET_MEMORY_MAP GetMemoryMap;                            /*  56 */
    EFI_ALLOCATE_POOL  AllocatePool;                            /*  64 */
    EFI_FREE_POOL      FreePool;                                /*  72 */
    void *CreateEvent, *SetTimer, *WaitForEvent,               /*  80, 88, 96 */
         *SignalEvent, *CloseEvent, *CheckEvent;                /* 104,112,120 */
    void *InstallProtocolInterface, *ReinstallProtocolInterface,/* 128,136 */
         *UninstallProtocolInterface;                           /* 144 */
    EFI_HANDLE_PROTOCOL HandleProtocol;                         /* 152 */
    void *Reserved;                                             /* 160 */
    void *RegisterProtocolNotify, *LocateHandle,                /* 168,176 */
         *LocateDevicePath, *InstallConfigurationTable;         /* 184,192 */
    void *LoadImage, *StartImage, *Exit, *UnloadImage;          /* 200,208,216,224 */
    EFI_EXIT_BOOT_SERVICES ExitBootServices;                    /* 232 */
    void *GetNextMonotonicCount, *Stall, *SetWatchdogTimer;     /* 240,248,256 */
    void *ConnectController, *DisconnectController;             /* 264,272 */
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;/* 280,288,296 */
    void *ProtocolsPerHandle, *LocateHandleBuffer;             /* 304,312 */
    EFI_LOCATE_PROTOCOL LocateProtocol;                         /* 320 */
    /* remaining fields unused */
} EFI_BOOT_SERVICES;

typedef struct {
    char  Hdr[24];
    CHAR16   *FirmwareVendor;                 /* 24 */
    uint32_t  FirmwareRevision;               /* 32 (+pad) */
    EFI_HANDLE ConsoleInHandle;               /* 40 */
    void     *ConIn;                          /* 48 */
    EFI_HANDLE ConsoleOutHandle;              /* 56 */
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;  /* 64 */
    EFI_HANDLE StandardErrorHandle;           /* 72 */
    void     *StdErr;                         /* 80 */
    void     *RuntimeServices;                /* 88 */
    EFI_BOOT_SERVICES *BootServices;          /* 96 */
    UINTN     NumberOfTableEntries;           /* 104 */
    void     *ConfigurationTable;             /* 112 */
} EFI_SYSTEM_TABLE;

/* ---- Runtime Services: GetTime (UEFI wall clock) ---- */
typedef struct {
    uint16_t Year; uint8_t Month, Day, Hour, Minute, Second, Pad1;
    uint32_t Nanosecond; int16_t TimeZone; uint8_t Daylight, Pad2;
} EFI_TIME;
typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(EFI_TIME *, void *);
typedef struct {
    char Hdr[24];
    EFI_GET_TIME GetTime;                     /* 24: first runtime service we use */
    /* remaining fields unused */
} EFI_RUNTIME_SERVICES;

/* civil date -> Unix epoch seconds (proleptic Gregorian); the EFI_TIME face
 * value is used verbatim so `date` shows the same wall clock as the host. */
static uint64_t civil_to_epoch(int y, int m, int d, int hh, int mm, int ss) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    return (uint64_t)days * 86400ULL + (uint64_t)hh * 3600 + (uint64_t)mm * 60 + (uint64_t)ss;
}

/* ---- Graphics Output Protocol ---- */
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} EFI_GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    EFI_GOP_MODE_INFO   *Info;
    UINTN                SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN                FrameBufferSize;
} EFI_GOP_MODE;
struct _EFI_GRAPHICS_OUTPUT_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_GOP_QUERY_MODE)(struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t, UINTN *, EFI_GOP_MODE_INFO **);
typedef EFI_STATUS (EFIAPI *EFI_GOP_SET_MODE)(struct _EFI_GRAPHICS_OUTPUT_PROTOCOL *, uint32_t);
typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GOP_QUERY_MODE QueryMode;
    EFI_GOP_SET_MODE   SetMode;
    void *Blt;
    EFI_GOP_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---- Loaded Image + Simple File System + File protocols (asset loading) ---- */
typedef struct {
    uint32_t Revision; uint32_t _pad;
    EFI_HANDLE ParentHandle;
    void      *SystemTable;
    EFI_HANDLE DeviceHandle;        /* offset 24 */
    /* remaining fields unused */
} EFI_LOADED_IMAGE_PROTOCOL;

struct _EFI_FILE_PROTOCOL;
typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(struct _EFI_FILE_PROTOCOL *, struct _EFI_FILE_PROTOCOL **, CHAR16 *, uint64_t, uint64_t);
typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(struct _EFI_FILE_PROTOCOL *);
typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(struct _EFI_FILE_PROTOCOL *, UINTN *, void *);
typedef EFI_STATUS (EFIAPI *EFI_FILE_SETPOS)(struct _EFI_FILE_PROTOCOL *, uint64_t);
typedef EFI_STATUS (EFIAPI *EFI_FILE_GETINFO)(struct _EFI_FILE_PROTOCOL *, EFI_GUID *, UINTN *, void *);
typedef struct _EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_FILE_OPEN   Open;
    EFI_FILE_CLOSE  Close;
    void           *Delete;
    EFI_FILE_READ   Read;
    void           *Write, *GetPosition;
    EFI_FILE_SETPOS SetPosition;
    EFI_FILE_GETINFO GetInfo;
    void           *SetInfo, *Flush;
} EFI_FILE_PROTOCOL;

struct _EFI_SIMPLE_FS;
typedef EFI_STATUS (EFIAPI *EFI_VOLUME_OPEN)(struct _EFI_SIMPLE_FS *, EFI_FILE_PROTOCOL **);
typedef struct _EFI_SIMPLE_FS {
    uint64_t Revision;
    EFI_VOLUME_OPEN OpenVolume;
} EFI_SIMPLE_FS;

/* ---- Block I/O (to tell a USB boot from a fixed-disk boot for the installer) ---- */
typedef struct {
    uint32_t MediaId;
    uint8_t  RemovableMedia;
    uint8_t  MediaPresent;
    uint8_t  LogicalPartition;
    uint8_t  ReadOnly;
    uint8_t  WriteCaching;
    uint8_t  _pad[3];
    uint32_t BlockSize;
    /* remaining fields unused */
} EFI_BLOCK_IO_MEDIA;
typedef struct {
    uint64_t Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    /* remaining function pointers unused */
} EFI_BLOCK_IO_PROTOCOL;

/* ---- COM1 serial (headless verification, also used before ExitBootServices) ---- */
#define COM1 0x3F8
static inline void outb(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t inb(uint16_t p) { uint8_t r; __asm__ volatile("inb %1,%0" : "=a"(r) : "Nd"(p)); return r; }
static void serial_init_early(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80); outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x0B);
}
static void sputc(char c) { while ((inb(COM1 + 5) & 0x20) == 0) { } outb(COM1, (uint8_t)c); }
static void sputs(const char *s) { for (; *s; s++) { if (*s == '\n') sputc('\r'); sputc(*s); } }

/* boot handoff block: static so its address is stable across the stack switch */
static boot_info_t g_bi;

extern void kstart(boot_info_t *);   /* kernel/entry64.s */

/* GOP GUID {9042a9de-23dc-4a38-96fb-7aded080516a} */
static EFI_GUID GOP_GUID =
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static EFI_GUID LOADED_IMAGE_GUID =
    { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID SIMPLE_FS_GUID =
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
/* EFI_BLOCK_IO_PROTOCOL GUID {964e5b21-6459-11d2-8e39-00a0c969723b} */
static EFI_GUID BLOCK_IO_GUID =
    { 0x964e5b21, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
/* EFI_DEVICE_PATH_PROTOCOL GUID {09576e91-6d3f-11d2-8e39-00a0c969723b} */
static EFI_GUID DEVICE_PATH_GUID =
    { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static EFI_GUID FILE_INFO_GUID =
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
/* ACPI 2.0 table GUID (identifies the RSDP in the UEFI configuration table) */
static EFI_GUID ACPI20_GUID =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };

typedef struct { EFI_GUID guid; void *table; } EFI_CONFIG_ENTRY;

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* Load /TINYWAD.DAT from the boot volume into a pool buffer (persists past
 * ExitBootServices).  Must run while boot services are still available. */
static void load_asset(EFI_BOOT_SERVICES *BS, EFI_HANDLE img, boot_info_t *bi) {
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (BS->HandleProtocol(img, &LOADED_IMAGE_GUID, (void **)&li) != EFI_SUCCESS || !li) {
        sputs("[efi] asset: no loaded-image protocol\n"); return;
    }
    EFI_SIMPLE_FS *fs = 0;
    if (BS->HandleProtocol(li->DeviceHandle, &SIMPLE_FS_GUID, (void **)&fs) != EFI_SUCCESS || !fs) {
        sputs("[efi] asset: no filesystem on boot device\n"); return;
    }
    EFI_FILE_PROTOCOL *root = 0;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS || !root) { sputs("[efi] asset: OpenVolume failed\n"); return; }
    EFI_FILE_PROTOCOL *file = 0;
    static CHAR16 name[] = u"TINYWAD.DAT";
    if (root->Open(root, &file, (CHAR16 *)name, 1 /*READ*/, 0) != EFI_SUCCESS || !file) {
        sputs("[efi] asset: TINYWAD.DAT not found\n"); return;
    }
    uint8_t info[256] __attribute__((aligned(8))); UINTN isz = sizeof(info);
    if (file->GetInfo(file, &FILE_INFO_GUID, &isz, info) != EFI_SUCCESS) {
        sputs("[efi] asset: GetInfo failed\n"); file->Close(file); return;
    }
    uint64_t fsize = *(uint64_t *)(info + 8);       /* EFI_FILE_INFO.FileSize */
    void *buf = 0;
    if (BS->AllocatePool(EFILOADERDATA, fsize, &buf) != EFI_SUCCESS || !buf) {
        sputs("[efi] asset: AllocatePool failed\n"); file->Close(file); return;
    }
    file->SetPosition(file, 0);
    UINTN rd = fsize;
    if (file->Read(file, &rd, buf) != EFI_SUCCESS) { sputs("[efi] asset: Read failed\n"); file->Close(file); return; }
    file->Close(file);
    bi->asset_base = (uint64_t)(uintptr_t)buf;
    bi->asset_size = rd;
    sputs("[efi] asset: TINYWAD.DAT loaded\n");
}

/* Load the loader's own \EFI\BOOT\BOOTX64.EFI into memory so the disk installer
 * can copy TinyOS onto the internal disk.  Same machinery as load_asset. */
static void load_self_efi(EFI_BOOT_SERVICES *BS, EFI_HANDLE img, boot_info_t *bi) {
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (BS->HandleProtocol(img, &LOADED_IMAGE_GUID, (void **)&li) != EFI_SUCCESS || !li) return;
    EFI_SIMPLE_FS *fs = 0;
    if (BS->HandleProtocol(li->DeviceHandle, &SIMPLE_FS_GUID, (void **)&fs) != EFI_SUCCESS || !fs) return;
    EFI_FILE_PROTOCOL *root = 0;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS || !root) return;
    EFI_FILE_PROTOCOL *file = 0;
    static CHAR16 name[] = u"\\EFI\\BOOT\\BOOTX64.EFI";
    if (root->Open(root, &file, (CHAR16 *)name, 1, 0) != EFI_SUCCESS || !file) {
        sputs("[efi] self: BOOTX64.EFI not found\n"); return;
    }
    uint8_t info[256] __attribute__((aligned(8))); UINTN isz = sizeof(info);
    if (file->GetInfo(file, &FILE_INFO_GUID, &isz, info) != EFI_SUCCESS) { file->Close(file); return; }
    uint64_t fsize = *(uint64_t *)(info + 8);
    void *buf = 0;
    if (BS->AllocatePool(EFILOADERDATA, fsize, &buf) != EFI_SUCCESS || !buf) { file->Close(file); return; }
    file->SetPosition(file, 0);
    UINTN rd = fsize;
    if (file->Read(file, &rd, buf) != EFI_SUCCESS) { file->Close(file); return; }
    file->Close(file);
    bi->efi_base = (uint64_t)(uintptr_t)buf;
    bi->efi_size = rd;
    sputs("[efi] self: BOOTX64.EFI loaded for the installer\n");
}

/* Determine whether we booted from removable media (USB stick) or a fixed disk
 * (the internal SSD).  The installer is only allowed from removable media so it
 * can never re-wipe the disk it was installed onto.  The boot partition's
 * RemovableMedia flag is unreliable (logical partitions report FALSE even on
 * USB), so the primary signal is a USB node in the boot device path. */
static int devpath_is_usb(EFI_BOOT_SERVICES *BS, EFI_HANDLE dev) {
    void *dp = 0;
    if (BS->HandleProtocol(dev, &DEVICE_PATH_GUID, &dp) != EFI_SUCCESS || !dp) return 0;
    uint8_t *p = (uint8_t *)dp;
    for (int guard = 0; guard < 64; guard++) {
        uint8_t type = p[0], sub = p[1];
        uint16_t len = (uint16_t)(p[2] | (p[3] << 8));
        if (type == 0x7F || len < 4) break;                       /* End / malformed */
        if (type == 0x03 && (sub == 0x05 || sub == 0x0F || sub == 0x10)) return 1; /* USB messaging node */
        p += len;
    }
    return 0;
}
static void detect_boot_media(EFI_BOOT_SERVICES *BS, EFI_HANDLE img, boot_info_t *bi) {
    bi->booted_removable = 0;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (BS->HandleProtocol(img, &LOADED_IMAGE_GUID, (void **)&li) != EFI_SUCCESS || !li) return;
    if (devpath_is_usb(BS, li->DeviceHandle)) bi->booted_removable = 1;
    else {                                                         /* fallback: RemovableMedia flag */
        EFI_BLOCK_IO_PROTOCOL *bio = 0;
        if (BS->HandleProtocol(li->DeviceHandle, &BLOCK_IO_GUID, (void **)&bio) == EFI_SUCCESS
            && bio && bio->Media && bio->Media->RemovableMedia) bi->booted_removable = 1;
    }
    sputs(bi->booted_removable ? "[efi] booted from removable media (USB)\n"
                              : "[efi] booted from fixed disk (install disabled)\n");
}

/* ---- entry point (UEFI calls this with the MS x64 ABI) ---- */
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_BOOT_SERVICES *BS = SystemTable->BootServices;

    serial_init_early();
    sputs("\n[efi] BOOTX64.EFI entered (x64 long mode, UEFI ABI)\n");
    if (SystemTable->ConOut && SystemTable->ConOut->OutputString) {
        static const CHAR16 hello[] = u"[efi] tinyos x64 loader starting\r\n";
        SystemTable->ConOut->OutputString(SystemTable->ConOut, (CHAR16 *)hello);
    }

    /* zero the handoff block (field-by-field to avoid an implicit memset) */
    g_bi.fb_base = g_bi.fb_size = 0;
    g_bi.width = g_bi.height = g_bi.pitch_px = g_bi.pixel_format = 0;
    g_bi.mmap_addr = g_bi.mmap_size = g_bi.mmap_desc_size = 0;

    /* 1) Graphics Output Protocol -> linear framebuffer */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    if (BS->LocateProtocol(&GOP_GUID, 0, (void **)&gop) == EFI_SUCCESS && gop) {
        /* choose the largest 32-bpp mode up to 1920x1080 for a roomy desktop */
        uint32_t best = gop->Mode->Mode, best_area = 0;
        for (uint32_t i = 0; i < gop->Mode->MaxMode; i++) {
            EFI_GOP_MODE_INFO *info = 0; UINTN isz = 0;
            if (gop->QueryMode(gop, i, &isz, &info) != EFI_SUCCESS || !info) continue;
            uint32_t w = info->HorizontalResolution, h = info->VerticalResolution, pf = info->PixelFormat;
            if ((pf != 0 && pf != 1) || w > 1920 || h > 1080) continue;   /* 32bpp, capped */
            uint32_t area = w * h;
            if (area > best_area) { best_area = area; best = i; }
        }
        if (best != gop->Mode->Mode) gop->SetMode(gop, best);
        EFI_GOP_MODE *m = gop->Mode;
        g_bi.fb_base      = m->FrameBufferBase;
        g_bi.fb_size      = m->FrameBufferSize;
        g_bi.width        = m->Info->HorizontalResolution;
        g_bi.height       = m->Info->VerticalResolution;
        g_bi.pitch_px     = m->Info->PixelsPerScanLine;
        g_bi.pixel_format = m->Info->PixelFormat;   /* 0=RGBX, 1=BGRX */
        g_bi.gop_modes    = gop->Mode->MaxMode;      /* how many modes were offered */
        sputs("[efi] GOP framebuffer acquired\n");
    } else {
        sputs("[efi] WARNING: no GOP; booting without a framebuffer\n");
    }

    /* 1b) load the WAD-sized asset while boot services are still up */
    load_asset(BS, ImageHandle, &g_bi);
    load_self_efi(BS, ImageHandle, &g_bi);       /* our own EFI, for the disk installer */
    detect_boot_media(BS, ImageHandle, &g_bi);   /* USB vs fixed-disk boot (install gate) */

    /* 1c) find the ACPI RSDP in the UEFI configuration table (for SMP/MADT) */
    {
        EFI_CONFIG_ENTRY *ct = (EFI_CONFIG_ENTRY *)SystemTable->ConfigurationTable;
        for (UINTN i = 0; i < SystemTable->NumberOfTableEntries; i++) {
            if (guid_eq(&ct[i].guid, &ACPI20_GUID)) {
                g_bi.rsdp = (uint64_t)(uintptr_t)ct[i].table;
                sputs("[efi] ACPI RSDP found\n");
                break;
            }
        }
    }

    /* 1d) UEFI wall clock (RuntimeServices GetTime; works on Hyper-V Gen2 + OVMF,
     * where a legacy CMOS RTC may be absent) */
    {
        EFI_RUNTIME_SERVICES *RT = (EFI_RUNTIME_SERVICES *)SystemTable->RuntimeServices;
        EFI_TIME t;
        for (unsigned i = 0; i < sizeof t; i++) ((uint8_t *)&t)[i] = 0;
        if (RT && RT->GetTime && RT->GetTime(&t, 0) == EFI_SUCCESS && t.Year >= 2000) {
            g_bi.boot_epoch = civil_to_epoch(t.Year, t.Month, t.Day, t.Hour, t.Minute, t.Second);
            sputs("[efi] UEFI wall-clock time acquired\n");
        }
    }

    /* 2) memory map + ExitBootServices.
     * Query the size, allocate with headroom, then (re)read the map and exit;
     * if the key went stale, re-read and retry (no allocation inside the loop). */
    UINTN map_size = 0, map_key = 0, desc_size = 0; uint32_t desc_ver = 0;
    void *map = 0;
    BS->GetMemoryMap(&map_size, 0, &map_key, &desc_size, &desc_ver);   /* -> BUFFER_TOO_SMALL */
    map_size += 4 * desc_size + 256;                                   /* headroom */
    if (BS->AllocatePool(EFILOADERDATA, map_size, &map) != EFI_SUCCESS) {
        sputs("[efi] FATAL: AllocatePool(memmap) failed\n");
        for (;;) __asm__ volatile("hlt");
    }

    int exited = 0;
    for (int tries = 0; tries < 8; tries++) {
        UINTN ms = map_size;
        if (BS->GetMemoryMap(&ms, map, &map_key, &desc_size, &desc_ver) != EFI_SUCCESS) {
            sputs("[efi] FATAL: GetMemoryMap failed\n");
            for (;;) __asm__ volatile("hlt");
        }
        if (BS->ExitBootServices(ImageHandle, map_key) == EFI_SUCCESS) {
            g_bi.mmap_addr = (uint64_t)(uintptr_t)map;
            g_bi.mmap_size = ms;
            g_bi.mmap_desc_size = desc_size;
            exited = 1;
            break;
        }
        /* map key stale (firmware changed the map) — loop and re-read */
    }
    if (!exited) {
        sputs("[efi] FATAL: ExitBootServices failed\n");
        for (;;) __asm__ volatile("hlt");
    }

    /* 3) boot services are gone; serial is still direct port I/O. Hand off. */
    sputs("[efi] ExitBootServices OK; entering 64-bit kernel\n");
    kstart(&g_bi);                 /* never returns */
    for (;;) __asm__ volatile("hlt");
    return EFI_SUCCESS;
}
