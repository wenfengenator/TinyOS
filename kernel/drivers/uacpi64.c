/* SPDX-License-Identifier: GPL-2.0-only */

#include "boot64.h"
#include <uacpi/event.h>
#include <uacpi/uacpi.h>
#include <uacpi/sleep.h>
#include <uacpi/opregion.h>

struct tiny_lock { volatile uint32_t value; };
struct tiny_event { volatile uint32_t count; };
struct tiny_pci { uacpi_pci_address address; };
struct tiny_io { uacpi_io_addr base; uacpi_size len; };

static uint64_t g_uacpi_rsdp;
static int g_uacpi_ready;

static inline void port_out8(uint16_t p, uint8_t v) { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void port_out16(uint16_t p, uint16_t v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void port_out32(uint16_t p, uint32_t v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline uint8_t port_in8(uint16_t p) { uint8_t v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t port_in16(uint16_t p) { uint16_t v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t port_in32(uint16_t p) { uint32_t v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static uint32_t pci_read32(struct tiny_pci *p, uacpi_size off) {
    uint32_t addr = 0x80000000u | ((uint32_t)p->address.bus << 16) |
                    ((uint32_t)p->address.device << 11) |
                    ((uint32_t)p->address.function << 8) | ((uint32_t)off & 0xFCu);
    port_out32(0xCF8, addr);
    return port_in32(0xCFC);
}

static void pci_write32(struct tiny_pci *p, uacpi_size off, uint32_t value) {
    uint32_t addr = 0x80000000u | ((uint32_t)p->address.bus << 16) |
                    ((uint32_t)p->address.device << 11) |
                    ((uint32_t)p->address.function << 8) | ((uint32_t)off & 0xFCu);
    port_out32(0xCF8, addr);
    port_out32(0xCFC, value);
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) { *out = g_uacpi_rsdp; return UACPI_STATUS_OK; }
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) { (void)len; return (void *)(uintptr_t)addr; }
void uacpi_kernel_unmap(void *addr, uacpi_size len) { (void)addr; (void)len; }
void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *text) {
    if (level > UACPI_LOG_WARN) return;          /* drop INFO/TRACE table + op spam */
    kputs("[uacpi] "); kputs(text);
}
void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size ? size : 1); }
void uacpi_kernel_free(void *mem) { if (mem) kfree(mem); }
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return micros() * 1000ull; }
void uacpi_kernel_stall(uacpi_u8 usec) { uint64_t end = micros() + usec; while (micros() < end) __asm__ volatile("pause"); }
void uacpi_kernel_sleep(uacpi_u64 msec) { while (msec) { uint32_t n = msec > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)msec; sleep_ms(n); msec -= n; } }

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address addr, uacpi_handle *out) {
    if (addr.segment) return UACPI_STATUS_NOT_FOUND;
    struct tiny_pci *p = kmalloc(sizeof(*p)); if (!p) return UACPI_STATUS_OUT_OF_MEMORY;
    p->address = addr; *out = p; return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { kfree(h); }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v) { *v=pci_read32(h,o); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v) { *v=(uacpi_u16)(pci_read32(h,o)>>((o&2)*8)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size o, uacpi_u8 *v) { *v=(uacpi_u8)(pci_read32(h,o)>>((o&3)*8)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v) { pci_write32(h,o,v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v) { uint32_t x=pci_read32(h,o),s=(o&2)*8; pci_write32(h,o,(x&~(0xFFFFu<<s))|((uint32_t)v<<s)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size o, uacpi_u8 v) { uint32_t x=pci_read32(h,o),s=(o&3)*8; pci_write32(h,o,(x&~(0xFFu<<s))|((uint32_t)v<<s)); return UACPI_STATUS_OK; }

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    if (base + len > 0x10000u) return UACPI_STATUS_INVALID_ARGUMENT;
    struct tiny_io *io=kmalloc(sizeof(*io)); if(!io)return UACPI_STATUS_OUT_OF_MEMORY;
    io->base=base; io->len=len; *out=io; return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) { kfree(h); }
#define IO_CHECK(h,o,n) struct tiny_io *io=(h); if(!io || (o)+(n)>io->len)return UACPI_STATUS_INVALID_ARGUMENT
uacpi_status uacpi_kernel_io_read8(uacpi_handle h,uacpi_size o,uacpi_u8*v){IO_CHECK(h,o,1);*v=port_in8((uint16_t)(io->base+o));return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_io_read16(uacpi_handle h,uacpi_size o,uacpi_u16*v){IO_CHECK(h,o,2);*v=port_in16((uint16_t)(io->base+o));return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_io_read32(uacpi_handle h,uacpi_size o,uacpi_u32*v){IO_CHECK(h,o,4);*v=port_in32((uint16_t)(io->base+o));return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_io_write8(uacpi_handle h,uacpi_size o,uacpi_u8 v){IO_CHECK(h,o,1);port_out8((uint16_t)(io->base+o),v);return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_io_write16(uacpi_handle h,uacpi_size o,uacpi_u16 v){IO_CHECK(h,o,2);port_out16((uint16_t)(io->base+o),v);return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_io_write32(uacpi_handle h,uacpi_size o,uacpi_u32 v){IO_CHECK(h,o,4);port_out32((uint16_t)(io->base+o),v);return UACPI_STATUS_OK;}

uacpi_handle uacpi_kernel_create_mutex(void) { struct tiny_lock*l=kmalloc(sizeof(*l));if(l)l->value=0;return l; }
void uacpi_kernel_free_mutex(uacpi_handle h) { kfree(h); }
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h,uacpi_u16 timeout){struct tiny_lock*l=h;uint64_t end=millis()+timeout;do{if(__sync_bool_compare_and_swap(&l->value,0,1))return UACPI_STATUS_OK;__asm__ volatile("pause");}while(timeout==0xFFFFu||millis()<end);return UACPI_STATUS_TIMEOUT;}
void uacpi_kernel_release_mutex(uacpi_handle h){__sync_lock_release(&((struct tiny_lock*)h)->value);}
uacpi_handle uacpi_kernel_create_event(void){struct tiny_event*e=kmalloc(sizeof(*e));if(e)e->count=0;return e;}
void uacpi_kernel_free_event(uacpi_handle h){kfree(h);}
uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h,uacpi_u16 timeout){struct tiny_event*e=h;uint64_t end=millis()+timeout;do{uint32_t n=e->count;if(n&&__sync_bool_compare_and_swap(&e->count,n,n-1))return UACPI_TRUE;__asm__ volatile("pause");}while(timeout==0xFFFFu||millis()<end);return UACPI_FALSE;}
void uacpi_kernel_signal_event(uacpi_handle h){__sync_fetch_and_add(&((struct tiny_event*)h)->count,1);}
void uacpi_kernel_reset_event(uacpi_handle h){((struct tiny_event*)h)->count=0;}
uacpi_thread_id uacpi_kernel_get_thread_id(void){return (uacpi_thread_id)(uintptr_t)1;}
uacpi_interrupt_state uacpi_kernel_disable_interrupts(void){uint64_t f;__asm__ volatile("pushfq; popq %0; cli":"=r"(f)::"memory");return f;}
void uacpi_kernel_restore_interrupts(uacpi_interrupt_state f){if(f&(1u<<9))__asm__ volatile("sti":::"memory");}
uacpi_handle uacpi_kernel_create_spinlock(void){return uacpi_kernel_create_mutex();}
void uacpi_kernel_free_spinlock(uacpi_handle h){uacpi_kernel_free_mutex(h);}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h){uacpi_cpu_flags f=uacpi_kernel_disable_interrupts();while(!__sync_bool_compare_and_swap(&((struct tiny_lock*)h)->value,0,1))__asm__ volatile("pause");return f;}
void uacpi_kernel_unlock_spinlock(uacpi_handle h,uacpi_cpu_flags f){uacpi_kernel_release_mutex(h);uacpi_kernel_restore_interrupts(f);}
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request*r){(void)r;return UACPI_STATUS_UNIMPLEMENTED;}
uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq,uacpi_interrupt_handler fn,uacpi_handle ctx,uacpi_handle*out){(void)irq;(void)fn;(void)ctx;*out=(void*)1;return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler fn,uacpi_handle h){(void)fn;(void)h;return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type,uacpi_work_handler fn,uacpi_handle ctx){(void)type;fn(ctx);return UACPI_STATUS_OK;}
uacpi_status uacpi_kernel_wait_for_work_completion(void){return UACPI_STATUS_OK;}

static void mask_runtime_sci_sources(void) {
    (void)uacpi_disable_all_gpes();
    (void)uacpi_disable_fixed_event((uacpi_fixed_event)0);
}

/* EmbeddedControl address-space handler: ThinkPad _PTS/_WAK/_INI and battery/lid
 * methods read/write EC fields, so without this uACPI aborts them ("no handler
 * found"), leaving the EC unaware of the S3 transition — the machine sleeps but
 * never arms wake.  Bytes are relayed to TinyOS's EC driver (ECDT-discovered
 * ports, spec ch.12 RD_EC/WR_EC protocol). */
static uacpi_status ec_region_handler(uacpi_region_op op, uacpi_handle op_data) {
    switch (op) {
    case UACPI_REGION_OP_ATTACH: {
        uacpi_region_attach_data *d = op_data;
        d->out_region_context = UACPI_NULL;
        return UACPI_STATUS_OK;
    }
    case UACPI_REGION_OP_DETACH:
        return UACPI_STATUS_OK;
    case UACPI_REGION_OP_READ: {
        uacpi_region_rw_data *d = op_data;
        uint64_t val = 0;
        for (uacpi_u8 i = 0; i < d->byte_width; i++) {
            uint8_t b = 0;
            if (acpi_ec_region_read((uint8_t)(d->offset + i), &b) != 0)
                return UACPI_STATUS_HARDWARE_TIMEOUT;
            val |= (uint64_t)b << (8 * i);
        }
        d->value = val;
        return UACPI_STATUS_OK;
    }
    case UACPI_REGION_OP_WRITE: {
        uacpi_region_rw_data *d = op_data;
        for (uacpi_u8 i = 0; i < d->byte_width; i++) {
            if (acpi_ec_region_write((uint8_t)(d->offset + i),
                                     (uint8_t)(d->value >> (8 * i))) != 0)
                return UACPI_STATUS_HARDWARE_TIMEOUT;
        }
        return UACPI_STATUS_OK;
    }
    default:
        return UACPI_STATUS_UNIMPLEMENTED;
    }
}

static void ec_install(void) {
    /* Install on the root: uACPI recursively connects every EmbeddedControl
     * opregion regardless of _STA (which itself may need the EC). */
    uacpi_status s = uacpi_install_address_space_handler(
        uacpi_namespace_root(), UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        ec_region_handler, UACPI_NULL);
    if (s != UACPI_STATUS_OK) kputs("[uacpi] EC region handler install failed\n");
}

int tinyos_uacpi_init(uint64_t rsdp) {
    g_uacpi_rsdp=rsdp;
    uacpi_status st=uacpi_initialize(0);
    if(st==UACPI_STATUS_OK)st=uacpi_namespace_load();
    if(st==UACPI_STATUS_OK){ec_install();st=uacpi_namespace_initialize();}
    g_uacpi_ready=st==UACPI_STATUS_OK;
    if(g_uacpi_ready)mask_runtime_sci_sources();
    kputs(g_uacpi_ready?"[uacpi] AML namespace ready\n":"[uacpi] AML initialization failed status=");
    if(!g_uacpi_ready){kput_u32dec((uint32_t)st);kputc('\n');}
    return g_uacpi_ready?0:-(int)st;
}
static uacpi_status execute_optional_state_method(const char *path, uacpi_u64 state) {
    uacpi_object *arg = uacpi_object_create_integer(state);
    if (!arg) return UACPI_STATUS_OUT_OF_MEMORY;
    uacpi_object_array args = { .objects = &arg, .count = 1 };
    uacpi_status st = uacpi_eval(uacpi_namespace_root(), path, &args, 0);
    uacpi_object_unref(arg);
    return st == UACPI_STATUS_NOT_FOUND ? UACPI_STATUS_OK : st;
}
int tinyos_uacpi_prepare_s3(void){
    if(!g_uacpi_ready)return -1;
    /* _PTS (inside prepare) is required; _GTS is deprecated (removed in ACPI 5.0,
     * not called by Linux/Windows) so its failure — e.g. a flaky EC after a hard
     * power-cycle — must not abort the transition. */
    uacpi_status st=uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S3);
    if(st==UACPI_STATUS_OK)(void)execute_optional_state_method("_GTS",3);
    return (int)st;
}
int tinyos_uacpi_wake_s3(void){
    if(!g_uacpi_ready)return -1;
    uacpi_status st=uacpi_prepare_for_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    if(st==UACPI_STATUS_OK)st=uacpi_wake_from_sleep_state(UACPI_SLEEP_STATE_S3);
    mask_runtime_sci_sources();
    return (int)st;
}
/* Enter S3 via uACPI's ACPICA-equivalent sequence (clear WAK_STS + all events,
 * disable all GPEs then enable only wake GPEs, split SLP_TYP / SLP_TYP|SLP_EN
 * write with a cache flush between) — the same path Linux uses, which resumes
 * correctly on this laptop.  A real S3 powers the CPU off and never returns. */
int tinyos_uacpi_enter_s3(void){
    if(!g_uacpi_ready)return -1;
    return (int)uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S3);
}