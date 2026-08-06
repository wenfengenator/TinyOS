/* SPDX-License-Identifier: GPL-2.0-only */

/* tinyos x64 tiny windowing system + terminal (Cycle 3).
 *
 * A keyboard-driven desktop compositor for the GOP framebuffer: it owns a list
 * of rectangular windows, composites them (with a highlighted focused window on
 * top) into a full-screen back buffer each frame, and routes keystrokes to the
 * focused window.  There is no mouse driver on the x64 side, so window focus and
 * movement are done from the keyboard (F1 = focus, F2 = move + arrows).
 *
 * A window is just a rectangle tagged with a kind (WIN_*); the compositor
 * dispatches to the matching draw and key handler with *direct* calls.  Storing
 * per-window function pointers in the struct would go through GOT-style
 * indirection that the -mi386pep EFI link step mis-resolves (it double-derefs
 * and jumps into a function prologue), so cross-object function pointers are
 * deliberately avoided.  The terminal widget lives in kernel/term64.c. */
#ifndef TINYOS_WM64_H
#define TINYOS_WM64_H

#include "boot64.h"

/* window kinds (direct-dispatched by the compositor) */
enum { WIN_TERMINAL, WIN_SYSMON, WIN_LOG };

typedef struct wm_window wm_window_t;
struct wm_window {
    int x, y, w, h;            /* whole-window pixel rect (title bar included) */
    const char *title;
    void *user;                /* widget private state                        */
    int  kind;                 /* WIN_* — selects the draw/key handler         */
};

/* desktop compositor + input loop; entered from kmain64 and never returns. */
void wm_run(boot_info_t *bi);

/* login screen (kernel/login64.c); blocks until valid credentials, then returns. */
void login_run(boot_info_t *bi);

/* window-list introspection (used by the terminal's `win` command). */
int         wm_window_count(void);
const char *wm_window_title(int i);
int         wm_focused_index(void);

/* UI font scale chosen from the framebuffer resolution (kernel/wm64.c). */
int         wm_ui_scale(void);          /* glyph magnification: 1 (low-res) or 2 */
int         wm_ui_cell(void);           /* glyph cell size in px = 8 * wm_ui_scale */
int         wm_ui_lineh(void);          /* text row pitch in px (cell + leading) */
void        wm_fill_rect(uint32_t *back, int x, int y, int w, int h, uint32_t rgb);  /* clipped fill into back buffer */

/* terminal widget (kernel/term64.c). */
void *term_create(boot_info_t *bi, int cols, int rows);
void  term_draw(wm_window_t *win, uint32_t *back, int cx, int cy, int cw, int ch);
void  term_key(wm_window_t *win, int key);
void  term_autostart_shell(void *user);   /* drop the desktop terminal into the linux shell */

#endif /* TINYOS_WM64_H */
