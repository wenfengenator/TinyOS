/* TinyOS audio user API: Intel HDA, 48 kHz signed 16-bit stereo PCM.
 * Plain C and header-only so on-device TinyCC applications can include it. */
#ifndef TINYOS_AUDIO_H
#define TINYOS_AUDIO_H

#include <stdint.h>
#include <unistd.h>

extern long syscall(long number, ...);

#define TAUDIO_RATE       48000
#define TAUDIO_MAX_FRAMES 16384

#define AUD_SYS_INFO 0x41554400L
#define AUD_SYS_PLAY 0x41554401L
#define AUD_SYS_TONE 0x41554402L
#define AUD_SYS_STOP 0x41554403L
#define AUD_SYS_DIAG 0x41554404L
#define AUD_SYS_VOLUME 0x41554405L

typedef struct taudio_info {
    uint32_t stage, gcap, codecs;
    int32_t codec, afg, dac, pin;
    uint32_t volume;
} taudio_info_t;

static inline int taudio_ready(void) {
    return (int)syscall(AUD_SYS_INFO);
}

/* Play interleaved left/right samples. Playback is synchronous and clips are
 * bounded to TAUDIO_MAX_FRAMES (about 341 ms). Returns frames played or -1. */
static inline int taudio_play(const int16_t *stereo, int frames) {
    if (!stereo || frames <= 0) return -1;
    if (frames > TAUDIO_MAX_FRAMES) frames = TAUDIO_MAX_FRAMES;
    return (int)syscall(AUD_SYS_PLAY, (long)stereo, (long)frames);
}

/* Convenience sound effect: 20..12000 Hz, 1..300 ms, volume 0..100. */
static inline int taudio_tone(int hz, int duration_ms, int volume) {
    return (int)syscall(AUD_SYS_TONE, (long)hz, (long)duration_ms, (long)volume);
}

static inline void taudio_stop(void) {
    syscall(AUD_SYS_STOP);
}

static inline int taudio_info(taudio_info_t *info) {
    return info ? (int)syscall(AUD_SYS_DIAG, (long)info) : -1;
}

static inline int taudio_set_volume(int volume) {
    return (int)syscall(AUD_SYS_VOLUME, (long)volume);
}

static inline int taudio_get_volume(void) {
    return (int)syscall(AUD_SYS_VOLUME, -1L);
}

#endif