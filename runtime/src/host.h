// SPDX-License-Identifier: AGPL-3.0-only
// Shared declarations for the PokeRogue runtime: a minimal browser-like host around QuickJS-ng.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "quickjs.h"

// ---------------------------------------------------------------- config

typedef struct {
    const char *base_url;    // page URL the game is loaded from, e.g. https://pokerogue.net/
    const char *js_dir;      // runtime's own JS (bootstrap, DOM shim)
    const char *data_dir;    // writable: HTTP cache, localStorage, cookies
    const char *control;     // optional FIFO path for test commands (see main.c)
    int win_w, win_h;        // window / screen size
    int exit_after_frames;   // 0 = run forever
} host_config;

extern host_config g_cfg;

// ---------------------------------------------------------------- logging / time

void host_log(const char *fmt, ...);
double host_now_ms(void);

// ---------------------------------------------------------------- platform (platform_sdl.c)

typedef enum {
    PEV_KEY_DOWN,
    PEV_KEY_UP,
    PEV_POINTER_DOWN,
    PEV_POINTER_MOVE,
    PEV_POINTER_UP,
    PEV_QUIT,
} plat_event_type;

typedef struct {
    plat_event_type type;
    const char *key;   // DOM KeyboardEvent.key
    const char *code;  // DOM KeyboardEvent.code
    int key_code;      // legacy KeyboardEvent.keyCode (Phaser uses it)
    float x, y;        // pointer position in window pixels
    int pointer_id;
} plat_event;

typedef struct {
    bool connected;
    const char *id;
    float axes[4];
    float buttons[18]; // W3C "standard" layout; 17 = touchpad click (the game's "submit")
} plat_gamepad;

bool plat_init(int w, int h);
void plat_shutdown(void);
// Returns false when there are no more events.
bool plat_poll_event(plat_event *ev);
void plat_swap(void);
void plat_drawable_size(int *w, int *h);
int plat_gamepads(plat_gamepad *pads, int max);
// Inject a synthetic key press/release (control channel).
void plat_inject_key(const char *key, bool down);
// Inject a synthetic pointer event at window coordinates (control channel).
void plat_inject_pointer(plat_event_type type, float x, float y);

// Text entry through the system keyboard (Vita IME dialog). plat_ime_open returns false if a dialog
// can't be shown. plat_ime_poll returns 0 while the dialog is open, 1 when confirmed (text in out,
// UTF-8), -1 when cancelled. On Linux the control channel's "ime <text>" / "ime-cancel" answer it.
bool plat_ime_open(const char *title, const char *initial, int max_len, bool password);
int plat_ime_poll(char *out, size_t n);
void plat_ime_answer(const char *text); // Linux/control channel only (NULL = cancel)

// ---------------------------------------------------------------- JS host (js_host.c)

JSContext *host_js_init(void);
void host_js_shutdown(void);
int host_eval_file(JSContext *ctx, const char *path);
int host_eval_string(JSContext *ctx, const char *src, const char *name, char **result);
void host_report_exception(JSContext *ctx);
void host_run_jobs(JSContext *ctx);
// Call globalThis.__rt[name](args...), report exceptions.
void host_call_rt(JSContext *ctx, const char *name, int argc, JSValueConst *argv);
// Resolve `url` against `base` into out (size n).
void url_resolve(const char *base, const char *url, char *out, size_t n);

// ---------------------------------------------------------------- networking (net.c)

void net_init(void);
void net_shutdown(void);
void net_poll(JSContext *ctx);                 // deliver finished requests
void net_register(JSContext *ctx, JSValue native);
bool host_random_bytes(void *buf, size_t n);
// Blocking GET used by the module loader. Returns malloc'd body or NULL.
char *net_get_sync(const char *url, size_t *len, long *status);

// ---------------------------------------------------------------- bindings

void gl_register(JSContext *ctx, JSValue native);        // gl.c
void gl_after_frame(void);
void canvas_register(JSContext *ctx, JSValue native);    // canvas2d.c
void fastpath_register(JSContext *ctx, JSValue native);  // fastpath.c

// Status messages drawn before the game takes over the screen (overlay.c).
void overlay_set(const char *msg);
void overlay_present(const char *msg); // draw + swap immediately (main thread is about to block)
bool overlay_draw(void);
void overlay_disable(void);

// Bytes held by decoded-image objects: compressed files kept, and decoded pixels currently alive.
void canvas_image_memory(size_t *encoded, size_t *decoded);

// Pixel source lookup shared by gl.c (texImage2D) and canvas2d.c (drawImage).
// Returns RGBA8 pixels (straight alpha) of an Image/Canvas/ImageData-like object, or NULL.
// *owned is set when the caller must free() the returned buffer.
const uint8_t *pixels_from_js(JSContext *ctx, JSValueConst obj, int *w, int *h, bool *owned);
