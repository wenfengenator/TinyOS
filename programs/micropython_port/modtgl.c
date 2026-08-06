/* SPDX-License-Identifier: GPL-2.0-only */

#include <stdint.h>
#include <unistd.h>

#include "py/mperrno.h"
#include "py/runtime.h"
#include "../tcc_include/tinygl.h"

#define TGL_SYS_INFO       0x54474C00L
#define TGL_SYS_BO_CREATE  0x54474C01L
#define TGL_SYS_PRESENT    0x54474C02L
#define TGL_SYS_BO_DESTROY 0x54474C03L

typedef struct {
    uint64_t va;
    uint32_t w, h, stride, rsh, gsh, bsh;
    int32_t handle;
    uint32_t pad;
} bo_desc_t;

typedef struct {
    uint32_t w, h, stride, rsh, gsh, bsh;
} screen_info_t;

typedef struct {
    mp_obj_base_t base;
    int handle;
    uint32_t *pixels;
    int w, h, stride;
    unsigned rsh, gsh, bsh;
} surface_obj_t;

static uint32_t pack_rgb(const surface_obj_t *self, uint32_t color) {
    return (((color >> 16) & 255) << self->rsh) |
        (((color >> 8) & 255) << self->gsh) | ((color & 255) << self->bsh);
}

static surface_obj_t *get_surface(mp_obj_t self_obj) {
    surface_obj_t *self = MP_OBJ_TO_PTR(self_obj);
    if (self->handle < 0 || self->pixels == NULL) mp_raise_ValueError(MP_ERROR_TEXT("surface closed"));
    return self;
}

static mp_obj_t surface_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 2, 2, false);
    int w = mp_obj_get_int(args[0]), h = mp_obj_get_int(args[1]);
    if (w <= 0 || h <= 0) mp_raise_ValueError(MP_ERROR_TEXT("invalid size"));
    bo_desc_t desc;
    if (syscall(TGL_SYS_BO_CREATE, (long)w, (long)h, (long)&desc) < 0) mp_raise_OSError(MP_ENOMEM);
    surface_obj_t *self = mp_obj_malloc_with_finaliser(surface_obj_t, type);
    self->handle = desc.handle;
    self->pixels = (uint32_t *)(uintptr_t)desc.va;
    self->w = (int)desc.w; self->h = (int)desc.h; self->stride = (int)desc.stride;
    self->rsh = desc.rsh; self->gsh = desc.gsh; self->bsh = desc.bsh;
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t surface_size(mp_obj_t self_obj) {
    surface_obj_t *self = get_surface(self_obj);
    mp_obj_t size[2] = { mp_obj_new_int(self->w), mp_obj_new_int(self->h) };
    return mp_obj_new_tuple(2, size);
}
static MP_DEFINE_CONST_FUN_OBJ_1(surface_size_obj, surface_size);

static mp_obj_t surface_close(mp_obj_t self_obj) {
    surface_obj_t *self = MP_OBJ_TO_PTR(self_obj);
    if (self->handle >= 0) syscall(TGL_SYS_BO_DESTROY, (long)self->handle);
    self->handle = -1; self->pixels = NULL;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(surface_close_obj, surface_close);

static mp_obj_t surface_clear(mp_obj_t self_obj, mp_obj_t color_obj) {
    surface_obj_t *self = get_surface(self_obj);
    uint32_t color = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(color_obj));
    int count = self->w * self->h;
    for (int i = 0; i < count; i++) self->pixels[i] = color;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(surface_clear_obj, surface_clear);

static mp_obj_t surface_pixel(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    if ((unsigned)x < (unsigned)self->w && (unsigned)y < (unsigned)self->h)
        self->pixels[y * self->stride + x] = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_pixel_obj, 4, 4, surface_pixel);

static void fill_rect(surface_obj_t *self, int x, int y, int w, int h, uint32_t color) {
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)self->h) continue;
        uint32_t *row = self->pixels + yy * self->stride;
        for (int xx = x; xx < x + w; xx++) if ((unsigned)xx < (unsigned)self->w) row[xx] = color;
    }
}

static mp_obj_t surface_fill(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    fill_rect(self, mp_obj_get_int(args[1]), mp_obj_get_int(args[2]), mp_obj_get_int(args[3]),
        mp_obj_get_int(args[4]), pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[5])));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_fill_obj, 6, 6, surface_fill);

static mp_obj_t surface_blend(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    int w = mp_obj_get_int(args[3]), h = mp_obj_get_int(args[4]);
    uint32_t source = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[5]));
    unsigned alpha = (unsigned)mp_obj_get_int(args[6]); if (alpha > 255) alpha = 255;
    unsigned inverse = 255 - alpha;
    for (int yy = y; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)self->h) continue;
        uint32_t *row = self->pixels + yy * self->stride;
        for (int xx = x; xx < x + w; xx++) {
            if ((unsigned)xx >= (unsigned)self->w) continue;
            uint32_t destination = row[xx], output = 0;
            for (int shift = 0; shift < 32; shift += 8)
                output |= (((((source >> shift) & 255) * alpha + ((destination >> shift) & 255) * inverse) / 255) & 255) << shift;
            row[xx] = output;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_blend_obj, 7, 7, surface_blend);

static mp_obj_t surface_line(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x0 = mp_obj_get_int(args[1]), y0 = mp_obj_get_int(args[2]);
    int x1 = mp_obj_get_int(args[3]), y1 = mp_obj_get_int(args[4]);
    uint32_t color = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[5]));
    int dx = x1 - x0, dy = y1 - y0, sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int error = dx - dy;
    for (;;) {
        if ((unsigned)x0 < (unsigned)self->w && (unsigned)y0 < (unsigned)self->h)
            self->pixels[y0 * self->stride + x0] = color;
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice > -dy) { error -= dy; x0 += sx; }
        if (twice < dx) { error += dx; y0 += sy; }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_line_obj, 6, 6, surface_line);

static mp_obj_t surface_disc(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int cx = mp_obj_get_int(args[1]), cy = mp_obj_get_int(args[2]);
    int radius = mp_obj_get_int(args[3]);
    if (radius < 0) mp_raise_ValueError(MP_ERROR_TEXT("invalid radius"));
    uint32_t color = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[4]));
    for (int dy = -radius; dy <= radius; dy++) {
        int half_width = 0;
        while ((half_width + 1) * (half_width + 1) + dy * dy <= radius * radius) half_width++;
        fill_rect(self, cx - half_width, cy + dy, half_width * 2 + 1, 1, color);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_disc_obj, 5, 5, surface_disc);

static mp_obj_t surface_text(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    size_t length;
    const char *text = mp_obj_str_get_data(args[3], &length);
    uint32_t color = pack_rgb(self, (uint32_t)mp_obj_get_int_truncated(args[4]));
    int scale = mp_obj_get_int(args[5]);
    if (scale < 1 || scale > 8) mp_raise_ValueError(MP_ERROR_TEXT("scale must be 1..8"));
    for (size_t index = 0; index < length; index++, x += 8 * scale) {
        unsigned character = (unsigned char)text[index];
        if (character < 0x20 || character > 0x7e) continue;
        const unsigned char *glyph = tgl_font8x8[character - 0x20];
        for (int row = 0; row < 8; row++) {
            unsigned bits = glyph[row];
            for (int column = 0; column < 8; column++) {
                if (bits & (1u << column))
                    fill_rect(self, x + column * scale, y + row * scale, scale, scale, color);
            }
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_text_obj, 6, 6, surface_text);

static mp_obj_t surface_vgradient(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x = mp_obj_get_int(args[1]), y = mp_obj_get_int(args[2]);
    int w = mp_obj_get_int(args[3]), h = mp_obj_get_int(args[4]);
    uint32_t top = (uint32_t)mp_obj_get_int_truncated(args[5]);
    uint32_t bottom = (uint32_t)mp_obj_get_int_truncated(args[6]);
    for (int row = 0; row < h; row++) {
        unsigned t = h > 1 ? (unsigned)(row * 255 / (h - 1)) : 0;
        unsigned r = (((top >> 16) & 255) * (255 - t) + ((bottom >> 16) & 255) * t) / 255;
        unsigned g = (((top >> 8) & 255) * (255 - t) + ((bottom >> 8) & 255) * t) / 255;
        unsigned b = ((top & 255) * (255 - t) + (bottom & 255) * t) / 255;
        fill_rect(self, x, y + row, w, 1, pack_rgb(self, (r << 16) | (g << 8) | b));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_vgradient_obj, 7, 7, surface_vgradient);

static mp_obj_t surface_present(size_t n_args, const mp_obj_t *args) {
    surface_obj_t *self = get_surface(args[0]);
    int x = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    int y = n_args > 2 ? mp_obj_get_int(args[2]) : 0;
    return mp_obj_new_int(syscall(TGL_SYS_PRESENT, (long)self->handle, (long)x, (long)y));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(surface_present_obj, 1, 3, surface_present);

static const mp_rom_map_elem_t surface_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&surface_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_size), MP_ROM_PTR(&surface_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&surface_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&surface_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_pixel), MP_ROM_PTR(&surface_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_fill), MP_ROM_PTR(&surface_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_blend), MP_ROM_PTR(&surface_blend_obj) },
    { MP_ROM_QSTR(MP_QSTR_line), MP_ROM_PTR(&surface_line_obj) },
    { MP_ROM_QSTR(MP_QSTR_disc), MP_ROM_PTR(&surface_disc_obj) },
    { MP_ROM_QSTR(MP_QSTR_text), MP_ROM_PTR(&surface_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_vgradient), MP_ROM_PTR(&surface_vgradient_obj) },
    { MP_ROM_QSTR(MP_QSTR_present), MP_ROM_PTR(&surface_present_obj) },
};
static MP_DEFINE_CONST_DICT(surface_locals, surface_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(tgl_surface_type, MP_QSTR_Surface, MP_TYPE_FLAG_NONE,
    make_new, surface_make_new, locals_dict, &surface_locals);

static mp_obj_t tgl_module_screen_size(void) {
    screen_info_t info;
    if (syscall(TGL_SYS_INFO, (long)&info) < 0) mp_raise_OSError(MP_ENODEV);
    mp_obj_t size[2] = { mp_obj_new_int_from_uint(info.w), mp_obj_new_int_from_uint(info.h) };
    return mp_obj_new_tuple(2, size);
}
static MP_DEFINE_CONST_FUN_OBJ_0(tgl_screen_size_obj, tgl_module_screen_size);

static const mp_rom_map_elem_t tgl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_tgl) },
    { MP_ROM_QSTR(MP_QSTR_screen_size), MP_ROM_PTR(&tgl_screen_size_obj) },
    { MP_ROM_QSTR(MP_QSTR_Surface), MP_ROM_PTR(&tgl_surface_type) },
    { MP_ROM_QSTR(MP_QSTR_KEY_UP), MP_ROM_INT(0x80) },
    { MP_ROM_QSTR(MP_QSTR_KEY_DOWN), MP_ROM_INT(0x81) },
    { MP_ROM_QSTR(MP_QSTR_KEY_LEFT), MP_ROM_INT(0x82) },
    { MP_ROM_QSTR(MP_QSTR_KEY_RIGHT), MP_ROM_INT(0x83) },
    { MP_ROM_QSTR(MP_QSTR_KEY_ESC), MP_ROM_INT(0x1b) },
};
static MP_DEFINE_CONST_DICT(tgl_globals, tgl_globals_table);

const mp_obj_module_t tgl_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&tgl_globals,
};
MP_REGISTER_MODULE(MP_QSTR_tgl, tgl_module);