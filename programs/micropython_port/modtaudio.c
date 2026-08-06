/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdint.h>
#include <unistd.h>

#include "py/mperrno.h"
#include "py/runtime.h"

#define AUD_SYS_INFO   0x41554400L
#define AUD_SYS_TONE   0x41554402L
#define AUD_SYS_STOP   0x41554403L
#define AUD_SYS_DIAG   0x41554404L
#define AUD_SYS_VOLUME 0x41554405L

typedef struct {
    uint32_t stage, gcap, codecs;
    int32_t codec, afg, dac, pin;
    uint32_t volume;
} audio_info_t;

static mp_obj_t taudio_ready(void) {
    return mp_obj_new_bool(syscall(AUD_SYS_INFO) > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(taudio_ready_obj, taudio_ready);

static mp_obj_t taudio_tone(mp_obj_t hz_obj, mp_obj_t duration_obj, mp_obj_t volume_obj) {
    int hz = mp_obj_get_int(hz_obj);
    int duration = mp_obj_get_int(duration_obj);
    int volume = mp_obj_get_int(volume_obj);
    if (hz < 20 || hz > 12000) mp_raise_ValueError(MP_ERROR_TEXT("frequency must be 20..12000"));
    if (duration < 1 || duration > 300) mp_raise_ValueError(MP_ERROR_TEXT("duration must be 1..300"));
    if (volume < 0 || volume > 100) mp_raise_ValueError(MP_ERROR_TEXT("volume must be 0..100"));
    long result = syscall(AUD_SYS_TONE, (long)hz, (long)duration, (long)volume);
    if (result < 0) mp_raise_OSError(MP_EIO);
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_3(taudio_tone_obj, taudio_tone);

static mp_obj_t taudio_stop(void) {
    syscall(AUD_SYS_STOP);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(taudio_stop_obj, taudio_stop);

static mp_obj_t taudio_volume(size_t n_args, const mp_obj_t *args) {
    long requested = -1;
    if (n_args != 0) {
        requested = mp_obj_get_int(args[0]);
        if (requested < 0 || requested > 100) mp_raise_ValueError(MP_ERROR_TEXT("volume must be 0..100"));
    }
    long result = syscall(AUD_SYS_VOLUME, requested);
    return mp_obj_new_int(result);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(taudio_volume_obj, 0, 1, taudio_volume);

static mp_obj_t taudio_info(void) {
    audio_info_t info;
    if (syscall(AUD_SYS_DIAG, (long)&info) < 0) mp_raise_OSError(MP_ENODEV);
    mp_obj_t tuple[8] = {
        mp_obj_new_int_from_uint(info.stage), mp_obj_new_int_from_uint(info.gcap),
        mp_obj_new_int_from_uint(info.codecs), mp_obj_new_int(info.codec),
        mp_obj_new_int(info.afg), mp_obj_new_int(info.dac), mp_obj_new_int(info.pin),
        mp_obj_new_int_from_uint(info.volume),
    };
    return mp_obj_new_tuple(8, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_0(taudio_info_obj, taudio_info);

static const mp_rom_map_elem_t taudio_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_taudio) },
    { MP_ROM_QSTR(MP_QSTR_ready), MP_ROM_PTR(&taudio_ready_obj) },
    { MP_ROM_QSTR(MP_QSTR_tone), MP_ROM_PTR(&taudio_tone_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&taudio_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_volume), MP_ROM_PTR(&taudio_volume_obj) },
    { MP_ROM_QSTR(MP_QSTR_info), MP_ROM_PTR(&taudio_info_obj) },
};
static MP_DEFINE_CONST_DICT(taudio_globals, taudio_globals_table);

const mp_obj_module_t taudio_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&taudio_globals,
};
MP_REGISTER_MODULE(MP_QSTR_taudio, taudio_module);