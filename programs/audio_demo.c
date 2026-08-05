/* TinyOS Intel HDA smoke test. Build on-device:
 *   ./tcc.bin /audio.c -o /audio.bin ; ./audio.bin */
#include <taudio.h>
#include <unistd.h>

static void say(const char *s) {
    int n = 0; while (s[n]) n++;
    if (n) { ssize_t written = write(1, s, (size_t)n); (void)written; }
}

static void say_uint(uint32_t value) {
    char text[11]; int n = 0;
    do { text[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n--) { ssize_t written = write(1, text + n, 1); (void)written; }
}

static void say_int(int32_t value) {
    if (value < 0) { say("-"); say_uint((uint32_t)(-(int64_t)value)); }
    else say_uint((uint32_t)value);
}

static void report_diag(void) {
    taudio_info_t info;
    if (taudio_info(&info) < 0) return;
    say("audio: diagnostic stage="); say_uint(info.stage);
    say(" gcap="); say_uint(info.gcap); say(" codecs="); say_uint(info.codecs);
    say(" codec="); say_int(info.codec); say(" afg="); say_int(info.afg);
    say(" dac="); say_int(info.dac); say(" pin="); say_int(info.pin);
    say(" volume="); say_uint(info.volume); say("\n");
}

static int parse_volume(const char *text) {
    int value = 0, digits = 0;
    while (*text >= '0' && *text <= '9') { value = value * 10 + *text++ - '0'; digits++; }
    return digits && !*text && value <= 100 ? value : -1;
}

int main(int argc, char **argv) {
    static const int notes[] = { 262, 330, 392, 523 };
    if (argc > 2) { say("usage: audio [volume 0-100]\n"); return 1; }
    if (argc == 2) {
        int volume = parse_volume(argv[1]);
        if (volume < 0) { say("audio: volume must be 0..100\n"); return 1; }
        taudio_set_volume(volume);
    }
    if (!taudio_ready()) { say("audio: no supported Intel HDA analog output\n"); report_diag(); return 1; }
    report_diag();
    say("audio: playing C major test tones\n");
    for (int i = 0; i < 4; i++) if (taudio_tone(notes[i], 140, 55) < 0) {
        say("audio: playback failed\n"); return 1;
    }
    say("audio: done\n"); return 0;
}