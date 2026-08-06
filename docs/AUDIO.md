# TinyOS Audio

TinyOS provides a minimal Intel High Definition Audio driver for analog laptop
outputs, initially targeting the ThinkPad T440s. The userspace interface is the
header-only `programs/tcc_include/taudio.h` API, staged as
`/include/taudio.h`, and is compatible with on-device TinyCC. MicroPython uses
the same kernel interface through its built-in `taudio` module.

## Hardware test

Boot the current image and run:

```sh
audio
audio 35
```

The optional argument sets the global master volume from 0 (mute) to 100 and
then plays four short notes. The default after boot is 80. At boot, a detected
output is reported as:

```text
[hda] codec N afg N dac N pin N
```

If discovery fails, the kernel reports `[hda] no supported analog output` and
audio API calls return `-1` without blocking indefinitely.

## Game API

```c
#include <taudio.h>

if (taudio_ready()) {
    taudio_set_volume(60);
    taudio_tone(880, 60, 50);       /* Hz, milliseconds, volume percent */
}
```

`taudio_set_volume(0..100)` changes the global software master volume and
`taudio_get_volume()` reads it. The setting applies to both generated tones and
custom PCM from every application until changed or rebooted. The volume argument
to `taudio_tone()` remains a per-effect level and is multiplied by the master.

Custom clips use interleaved signed 16-bit stereo samples at 48 kHz:

```c
int16_t samples[frames * 2];
/* samples[0], samples[1] are the first left/right frame */
taudio_play(samples, frames);
```

Playback is synchronous in this first driver revision and is limited to 16,384
frames (about 341 ms) per call. Keep game effects short so they do not visibly
pause the update loop. `taudio_stop()` stops the active output stream.

The first implementation uses polled HDA immediate verbs and a single DMA
output stream. It does not yet provide mixing, interrupt-driven queues, volume
control through codec amplifier gain, input, HDMI audio, or automatic headphone
switching. Master volume is applied by software scaling before DMA.

## MicroPython API

The static `/PYTHON.BIN` interpreter includes `taudio` as a built-in native
module:

```python
import taudio

if taudio.ready():
    taudio.volume(60)
    taudio.tone(440, 120, 70)
```

The Python API provides `ready()`, `tone(hz, duration_ms, volume)`, `stop()`,
`volume()`, `volume(level)`, and `info()`. Frequency is limited to 20 through
12,000 Hz, duration to 1 through 300 ms, and volume to 0 through 100. `info()`
returns `(stage, gcap, codecs, codec, afg, dac, pin, volume)`.

Run the packaged smoke test with:

```sh
python /audio.py
```

Like the C API, Python tone playback is synchronous. See
[`MICROPYTHON.md`](MICROPYTHON.md) for how the native module is compiled into
the interpreter and crosses into the HDA driver.

## Pac-Man demo

`pacman` uses `taudio_tone()` for alternating pellet chirps, energizers, eaten
ghosts, player death, and level completion. Effects are intentionally short
because playback is synchronous. Set the master level before launching it:

```sh
audio 45
pacman
```

The packaged `/reversi.py` also uses `taudio` for startup, legal and illegal
moves, passes, mode changes, and game-over results.