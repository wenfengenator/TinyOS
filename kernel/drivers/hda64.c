/* SPDX-License-Identifier: GPL-2.0-only */

/* Intel High Definition Audio: minimal synchronous PCM output for TinyOS.
 * Targets the analog HDA codec used by machines such as the ThinkPad T440s.
 * Format is fixed at 48 kHz, signed 16-bit, stereo. */
#include "boot64.h"

#define HDA_RATE 48000u
#define HDA_MAX_FRAMES 16384u
#define HDA_FORMAT 0x0011u

static inline void outl(uint16_t port, uint32_t value) { __asm__ volatile("outl %0,%1" :: "a"(value), "Nd"(port)); }
static inline uint32_t inl(uint16_t port) { uint32_t value; __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port)); return value; }
static uint32_t pci_read(int bus, int dev, int fn, int off) {
    outl(0xCF8, 0x80000000u | (uint32_t)(bus << 16) | (uint32_t)(dev << 11) | (uint32_t)(fn << 8) | (uint32_t)(off & 0xFC));
    return inl(0xCFC);
}
static void pci_write(int bus, int dev, int fn, int off, uint32_t value) {
    outl(0xCF8, 0x80000000u | (uint32_t)(bus << 16) | (uint32_t)(dev << 11) | (uint32_t)(fn << 8) | (uint32_t)(off & 0xFC));
    outl(0xCFC, value);
}
static void pci_power_d0(int bus, int dev, int fn) {
    if (!(pci_read(bus, dev, fn, 4) & (1u << 20))) return;  /* capabilities list */
    int cap = (int)(pci_read(bus, dev, fn, 0x34) & 0xFCu);
    for (int guard = 0; cap >= 0x40 && guard < 48; guard++) {
        uint32_t header = pci_read(bus, dev, fn, cap);
        if ((header & 0xFFu) == 1) {                       /* PCI power management */
            uint32_t pmcsr = pci_read(bus, dev, fn, cap + 4);
            if (pmcsr & 3u) { pci_write(bus, dev, fn, cap + 4, pmcsr & ~3u); sleep_ms(10); }
            return;
        }
        int next = (int)((header >> 8) & 0xFCu);
        if (!next || next == cap) return;
        cap = next;
    }
}

struct hda_bdl { uint32_t lo, hi, bytes, flags; } __attribute__((packed));
static volatile uint8_t *regs;
static int ready, stream_off, codec = -1, afg = -1, dac = -1, pin = -1;
static int afg_first, afg_count;
static uint32_t diag_stage, diag_gcap, diag_codecs;
static int master_volume = 80;
static int16_t *pcm;
static struct hda_bdl *bdl;

static uint8_t r8(int off) { return *(volatile uint8_t *)(regs + off); }
static uint16_t r16(int off) { return *(volatile uint16_t *)(regs + off); }
static uint32_t r32(int off) { return *(volatile uint32_t *)(regs + off); }
static void w8(int off, uint8_t value) { *(volatile uint8_t *)(regs + off) = value; }
static void w16(int off, uint16_t value) { *(volatile uint16_t *)(regs + off) = value; }
static void w32(int off, uint32_t value) { *(volatile uint32_t *)(regs + off) = value; }

static int wait32(int off, uint32_t mask, uint32_t want, uint32_t ms) {
    uint64_t end = millis() + ms;
    while ((r32(off) & mask) != want) { if (millis() > end) return 0; __asm__ volatile("pause"); }
    return 1;
}
static uint32_t hda_verb_once(int cad, int nid, uint32_t verb, uint32_t payload) {
    if (!regs) return 0xFFFFFFFFu;
    if (!wait32(0x68, 1, 0, 20)) return 0xFFFFFFFFu;
    w16(0x68, 2);                                      /* clear response-valid */
    uint32_t command = ((uint32_t)cad << 28) | ((uint32_t)nid << 20);
    command |= verb < 16 ? (verb << 16) | (payload & 0xFFFFu) : (verb << 8) | (payload & 0xFFu);
    w32(0x60, command); w16(0x68, 1);                  /* immediate-command busy */
    if (!wait32(0x68, 2, 2, 20)) return 0xFFFFFFFFu;
    uint32_t response = r32(0x64); w16(0x68, 2); return response;
}
static uint32_t hda_verb(int cad, int nid, uint32_t verb, uint32_t payload) {
    uint32_t response = 0xFFFFFFFFu;
    for (int attempt = 0; attempt < 4 && response == 0xFFFFFFFFu; attempt++) {
        response = hda_verb_once(cad, nid, verb, payload);
        if (response == 0xFFFFFFFFu) { w16(0x68, 2); sleep_ms(1); }
    }
    return response;
}
static uint32_t param(int nid, int which) { return hda_verb(codec, nid, 0xF00, (uint32_t)which); }
static void power_node(int nid) { hda_verb(codec, nid, 0x705, 0); }
static uint32_t widget_caps(int nid) { return param(nid, 9); }

static int find_codec_path(void) {
    uint16_t codecs = r16(0x0E);
    diag_codecs = codecs;
    kputs("[hda] codec bitmap "); kput_u64hex(codecs); kputc('\n');
    for (int cad = 0; cad < 15; cad++) if (codecs & (1u << cad)) {
        codec = cad; afg = dac = pin = -1;
        uint32_t vendor = param(0, 0);
        kputs("[hda] cad "); kput_u32dec((uint32_t)cad); kputs(" vendor "); kput_u64hex(vendor); kputc('\n');
        uint32_t root = param(0, 4); int first = (int)((root >> 16) & 0xFF), count = (int)(root & 0xFF);
        kputs("[hda] cad "); kput_u32dec((uint32_t)cad); kputs(" root "); kput_u64hex(root); kputc('\n');
        for (int node = first; node < first + count; node++) {
            uint32_t type = param(node, 5);
            kputs("[hda] fg "); kput_u32dec((uint32_t)node); kputs(" type "); kput_u64hex(type); kputc('\n');
            if ((type & 0xFF) == 1) { afg = node; break; }
        }
        if (afg < 0 && count == 1 && first > 0) {
            afg = first;
            kputs("[hda] sole function-group fallback "); kput_u32dec((uint32_t)afg); kputc('\n');
        }
        if (afg < 0) continue;
        uint32_t nodes = param(afg, 4); first = (int)((nodes >> 16) & 0xFF); count = (int)(nodes & 0xFF);
        kputs("[hda] afg nodes "); kput_u64hex(nodes); kputc('\n');
        if (!count || count > 128 || first < 2) continue;
        afg_first = first; afg_count = count;
        int pin_score = -1;
        for (int node = first; node < first + count; node++) {
            uint32_t wcaps = widget_caps(node);
            int type = (int)((wcaps >> 20) & 15);
            if (type == 0 && !(wcaps & (1u << 9)) && dac < 0) dac = node;
            uint32_t pcaps = param(node, 0x0C);
            uint32_t formats = param(node, 0x0B);
            if (dac < 0 && (formats & 1u) && !(pcaps & (1u << 4))) {
                dac = node;
                kputs("[hda] PCM converter fallback "); kput_u32dec((uint32_t)node); kputc('\n');
            }
            if ((type == 4 || (pcaps & (1u << 4))) && pcaps != 0xFFFFFFFFu) {
                uint32_t config = hda_verb(codec, node, 0xF1C, 0);
                int device = (int)((config >> 20) & 15), connectivity = (int)((config >> 30) & 3);
                if (device == 0 || device == 1 || device == 2) {
                    int score = 1;                    /* analog output-capable fallback */
                    if (connectivity != 1) score += 10;
                    if (device == 1) score += 30;     /* internal speaker */
                    else if (device == 2) score += 20;/* headphones */
                    else score += 10;                 /* line out */
                    kputs("[hda] analog pin "); kput_u32dec((uint32_t)node);
                    kputs(" cfg "); kput_u64hex(config); kputs(" caps "); kput_u64hex(pcaps); kputc('\n');
                    if (score > pin_score) { pin = node; pin_score = score; }
                }
            }
        }
        if (pin < 0 && vendor == 0x10EC0292u && first <= 0x14 && first + count > 0x14) {
            pin = 0x14; pin_score = 100;
            kputs("[hda] ALC292 T440s speaker pin quirk NID 20\n");
        }
        if (dac >= 0 && pin >= 0) return 1;
        if (cad + 1 < 15 && (codecs & (uint16_t)~((1u << (cad + 1)) - 1u)))
            codec = afg = dac = pin = -1;
    }
    return 0;
}

static uint32_t amp_gain(int nid, int output) {
    uint32_t caps = param(nid, output ? 0x12 : 0x0D);
    return caps == 0xFFFFFFFFu ? 0 : caps & 0x7Fu;       /* 0 dB offset */
}

static void configure_analog_path(void) {
    for (int node = afg_first; node < afg_first + afg_count; node++) {
        uint32_t wcaps = widget_caps(node);
        if (wcaps == 0xFFFFFFFFu || (wcaps & (1u << 9))) continue;
        power_node(node);
        if (wcaps & (1u << 2))
            hda_verb(codec, node, 3, 0xB000u | amp_gain(node, 1));
        if (wcaps & (1u << 1)) {
            uint32_t length = param(node, 0x0E) & 0x7Fu;
            if (length > 16) length = 16;
            uint32_t gain = amp_gain(node, 0);
            for (uint32_t index = 0; index < length; index++)
                hda_verb(codec, node, 3, 0x3000u | (index << 8) | gain);
        }
        int type = (int)((wcaps >> 20) & 15);
        if (type == 3 || type == 4) hda_verb(codec, node, 0x701, 0);
    }
}

static int probe_controller(int bus, int dev, int fn, int *input_streams) {
        kputs("[hda] PCI "); kput_u32dec((uint32_t)bus); kputc(':'); kput_u32dec((uint32_t)dev);
        kputc('.'); kput_u32dec((uint32_t)fn); kputc('\n');
        pci_power_d0(bus, dev, fn);
        uint32_t barlo = pci_read(bus, dev, fn, 0x10); uint64_t bar = barlo & ~0xFu;
        if ((barlo & 6) == 4) bar |= (uint64_t)pci_read(bus, dev, fn, 0x14) << 32;
        if (!bar) { kputs("[hda] invalid MMIO BAR\n"); return 0; }
        diag_stage = 3;
        pci_write(bus, dev, fn, 4, pci_read(bus, dev, fn, 4) | 6u);
        regs = (volatile uint8_t *)(uintptr_t)bar;
        w32(0x08, r32(0x08) & ~1u);
        if (!wait32(0x08, 1, 0, 100)) { kputs("[hda] controller reset assert timeout\n"); return 0; }
        diag_stage = 4;
        w32(0x08, r32(0x08) | 1u);
        if (!wait32(0x08, 1, 1, 100)) { kputs("[hda] controller reset release timeout\n"); return 0; }
        { uint64_t end = millis() + 250;
            while (!r16(0x0E) && millis() < end) { sleep_ms(1); } }
        uint16_t gcap = r16(0x00); int iss = (gcap >> 8) & 15, oss = (gcap >> 12) & 15;
        diag_gcap = gcap; diag_codecs = r16(0x0E); diag_stage = 5;
        kputs("[hda] GCAP "); kput_u64hex(gcap); kputs(" STATESTS "); kput_u64hex(r16(0x0E)); kputc('\n');
        if (!oss) { kputs("[hda] controller has no output streams\n"); return 0; }
        if (!find_codec_path()) { kputs("[hda] no analog output widget found on controller\n"); return 0; }
        *input_streams = iss;
        return 1;
}

static void stop_stream(void) {
    if (!regs || stream_off < 0) return;
    w32(stream_off, r32(stream_off) & ~2u);
    for (int spin = 0; spin < 100000 && (r32(stream_off) & 2u); spin++) __asm__ volatile("pause");
    w8(stream_off + 3, 0x1C);                    /* clear stream status */
}

int hda_init(void) {
    if (ready) return 1;
    diag_stage = 1;
    int found = 0, iss = 0;
    for (int b = 0; b < 256 && !found; b++) for (int d = 0; d < 32 && !found; d++) for (int f = 0; f < 8; f++) {
        uint32_t id = pci_read(b, d, f, 0); if ((uint16_t)id == 0xFFFF) { if (f == 0) break; continue; }
        uint32_t cls = pci_read(b, d, f, 8);
        if ((cls >> 24) == 0x04 && ((cls >> 16) & 0xFF) == 0x03) {
            diag_stage = 2;
            codec = afg = dac = pin = -1; afg_first = afg_count = 0;
            if (probe_controller(b, d, f, &iss)) found = 1;
        }
    }
    if (!found) { kputs("[hda] no PCI controller with analog output\n"); return 0; }
    diag_stage = 6;
    stream_off = 0x80 + iss * 0x20;
    pcm = (int16_t *)page_alloc(16); bdl = (struct hda_bdl *)page_alloc(1);
    if (!pcm || !bdl) { diag_stage = 7; return 0; }
    power_node(afg); power_node(dac); power_node(pin);
    configure_analog_path();
    hda_verb(codec, pin, 0x707, 0x40);            /* pin output enable */
    hda_verb(codec, pin, 0x70C, 0x02);            /* external amp enable */
    hda_verb(codec, pin, 0x701, 0);               /* first connection */
    hda_verb(codec, dac, 3, 0xB04A);              /* output amp: both channels, unmuted */
    hda_verb(codec, pin, 3, 0xB04A);
    ready = 1; diag_stage = 8;
    kputs("[hda] codec "); kput_u32dec((uint32_t)codec); kputs(" afg "); kput_u32dec((uint32_t)afg);
    kputs(" dac "); kput_u32dec((uint32_t)dac); kputs(" pin "); kput_u32dec((uint32_t)pin); kputc('\n');
    return 1;
}

int hda_ready(void) { return ready; }
int hda_get_diag(hda_diag_t *diag) {
    if (!diag) return -1;
    diag->stage = diag_stage; diag->gcap = diag_gcap; diag->codecs = diag_codecs;
    diag->codec = codec; diag->afg = afg; diag->dac = dac; diag->pin = pin;
    diag->volume = (uint32_t)master_volume;
    return ready;
}
int hda_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    master_volume = volume;
    return master_volume;
}
int hda_get_volume(void) { return master_volume; }
void hda_stop(void) { stop_stream(); }

int hda_play(const int16_t *samples, uint32_t frames) {
    if (!ready || !samples || !frames) return -1;
    if (frames > HDA_MAX_FRAMES) frames = HDA_MAX_FRAMES;
    uint32_t bytes = frames * 4u;
    if (master_volume == 100) memcpy(pcm, samples, bytes);
    else for (uint32_t i = 0; i < frames * 2u; i++)
        pcm[i] = (int16_t)((int32_t)samples[i] * master_volume / 100);
    stop_stream();
    w8(stream_off, 1); for (int spin = 0; spin < 100000 && !(r8(stream_off) & 1); spin++) __asm__ volatile("pause");
    w8(stream_off, 0); for (int spin = 0; spin < 100000 && (r8(stream_off) & 1); spin++) __asm__ volatile("pause");
    uint64_t pa = (uint64_t)(uintptr_t)pcm, bpa = (uint64_t)(uintptr_t)bdl;
    bdl->lo = (uint32_t)pa; bdl->hi = (uint32_t)(pa >> 32); bdl->bytes = bytes; bdl->flags = 1;
    w32(stream_off + 0x08, bytes); w16(stream_off + 0x0C, 0); w16(stream_off + 0x12, HDA_FORMAT);
    w32(stream_off + 0x18, (uint32_t)bpa); w32(stream_off + 0x1C, (uint32_t)(bpa >> 32));
    hda_verb(codec, dac, 0x706, 0x10); hda_verb(codec, dac, 0x200, HDA_FORMAT);
    w32(stream_off, (1u << 20) | 2u);
    uint64_t end = millis() + ((uint64_t)frames * 1000u + HDA_RATE - 1) / HDA_RATE + 20;
    while (millis() < end && !(r8(stream_off + 3) & 4)) {
        power_poll();
        if (wm_is_sleeping()) { stop_stream(); wm_sleep_wait(); return 0; }
        wm_pump();
        __asm__ volatile("pause");
    }
    stop_stream(); return (int)frames;
}

int hda_tone(uint32_t hz, uint32_t duration_ms, uint32_t volume) {
    if (!ready || hz < 20 || hz > 12000 || !duration_ms) return -1;
    if (duration_ms > 300) duration_ms = 300;
    if (volume > 100) volume = 100;
    uint32_t frames = HDA_RATE * duration_ms / 1000u; if (frames > HDA_MAX_FRAMES) frames = HDA_MAX_FRAMES;
    uint32_t period = HDA_RATE / hz; if (period < 4) period = 4;
    int32_t amp = (int32_t)(volume * 280); int32_t value = -amp, step = (amp * 4) / (int32_t)period;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t phase = i % period;
        if (phase == 0) value = -amp; else if (phase == period / 2) value = amp; else value += phase < period / 2 ? step : -step;
        pcm[i * 2] = pcm[i * 2 + 1] = (int16_t)value;
    }
    return hda_play(pcm, frames);
}