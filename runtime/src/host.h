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
    const char *local_dir;   // config.txt local=<dir>: play an offline build from this folder
    bool force_online;       // config.txt online=1
    bool log;                // config.txt log=1: write log.txt in a player build (debug builds always do)
    bool no_profile;         // config.txt profile=0
    bool no_audio;           // config.txt audio=0: no sound (the game gets a silent Web Audio)
    int busy_after_ms;       // config.txt busy=<ms>: loading screen during long frames (0: never)
    bool net_trace;          // config.txt nettrace=1: log API request headers
    bool osk;                // always use the built-in keyboard (config.txt osk=1)
    bool hud;                // debug line over the game (config.txt hud=1; SELECT while holding L+R)
    bool sample;             // profiler sampling without the debug line (control channel "profile",
                             // config.txt sample=1; on from the start in profiling builds)
    int max_textures;        // texture units Phaser is told of (default 4; debug builds: maxtextures=N)
    bool plain_shaders;      // config.txt plainshaders=1 (debug builds): trivial fragment shaders, to measure
    bool gpu_profile;        // config.txt gpuprofile=1 (debug builds): GPU time per shader, one frame every 10 s
    bool no_shader_fix;      // config.txt shaderfix=0 (debug builds): the game's shaders without js/webgl.js's rewrites
    int profile_rate;        // JS operations between samples (config.txt profile_rate=N; 0: QuickJS's 10000)
    double sample_ms;        // at most one sample per this many ms, so samples follow time rather than
                             // operation counts (config.txt sample_ms=1; 0: every check)
    int interrupt_after_s;   // 0 = never interrupt long-running JS (config.txt interrupt=N)
    const char *dev_server;  // config.txt devserver=<url>: log upload (devtools.c)
    const char *dev_key;     // config.txt devkey=<secret>, sent with each log
    const char *dev_builds;  // config.txt devbuilds=<url of build.json> or off: test builds
} host_config;

extern host_config g_cfg;

// ---------------------------------------------------------------- logging / time

void host_log(const char *fmt, ...);
bool host_logging(void); // is there a log to write to (player builds: only with config.txt log=1)
double host_now_ms(void);
void mem_breakdown(const char *stage); // logs where main memory goes

// ---------------------------------------------------------------- platform (platform_sdl.c)

typedef enum {
    PEV_KEY_DOWN,
    PEV_KEY_UP,
    PEV_POINTER_DOWN,
    PEV_POINTER_MOVE,
    PEV_POINTER_UP,
    PEV_QUIT,
    PEV_TOGGLE_HUD, // debug line on/off
    PEV_SEND_LOG,   // upload the log to the log server (devtools.c)
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
// Starts the audio output thread: it calls mix(out, frames) for interleaved 16-bit stereo at `rate`.
bool plat_audio_start(int rate, void (*mix)(int16_t *out, int frames));
// From inside a worker thread: pin it to a CPU core and set its priority relative to the main
// thread's (negative = more urgent) (Vita).
void plat_thread_setup(const char *name, int core, int priority);
void plat_keep_awake(void);                 // reset the console's idle/auto-sleep timer
long long plat_free_space(const char *path); // free bytes on the storage holding path, -1 if unknown
bool offline_install(void);
// Screens drawn outside the game loop (offline_install.c).
bool ui_confirm(const char *msg);       // X yes, O no
void ui_message_wait(const char *msg);  // until X or O
// Finds "key":"value" / "key":number after `from` in a JSON text (flat objects; no escapes).
bool json_str(const char *from, const char *key, char *out, size_t n);
long long json_num(const char *from, const char *key);
bool ui_download(const char *url, const char *path, const char *what, char *err, size_t errn); // with progress, O cancels
// Dev server (devtools.c): test builds and log upload. Dev builds only (PKR_DEV): a player's build
// never replaces its own code or sends data anywhere but the game's servers.
#ifdef PKR_DEV
void dev_early(void);                  // before graphics init: the steps that need the system's installer
void dev_start(const char *prev_log);  // after net_init: uploads the previous run's log
bool dev_offer_update(void);           // start screen: installs a newer test build (restarts the app)
void dev_send_log(void);               // L + R + START in game: uploads the current log
void dev_send_bench_log(void);         // the benchmark page (bench/) is done: uploads its log
void dev_frame(void);                  // main loop: shows upload results
#else
static inline void dev_early(void) {}
static inline void dev_start(const char *prev_log) { (void)prev_log; }
static inline bool dev_offer_update(void) { return false; }
static inline void dev_send_log(void) {}
static inline void dev_send_bench_log(void) {}
static inline void dev_frame(void) {}
#endif                 // offline_install.c: download <data>/game.zip (or an update)
void offline_update_check_start(void);      // background check for a newer offline game release
const char *offline_update_available(const char *installed); // its tag, or NULL
void plat_shutdown(void);
// Returns false when there are no more events.
bool plat_poll_event(plat_event *ev);
void plat_swap(void);
void plat_gpu_stats(double *avg_ms, double *max_ms, int *samples); // debug builds: sampled GPU time per frame
void plat_drawable_size(int *w, int *h);
int plat_gamepads(plat_gamepad *pads, int max);
// Inject a synthetic key press/release (control channel).
void plat_inject_key(const char *key, bool down);
// Inject a synthetic pointer event at window coordinates (control channel).
void plat_inject_pointer(plat_event_type type, float x, float y);

#ifndef __vita__
// Desktop text entry answered through the control channel ("ime <text>", "ime-cancel"; osk.c): the
// Vita uses the built-in keyboard only. plat_ime_poll returns 0 while a field waits for its text,
// 1 when answered (text in out), -1 when cancelled.
bool plat_ime_open(const char *title, const char *initial, int max_len, bool password);
int plat_ime_poll(char *out, size_t n);
void plat_ime_answer(const char *text); // NULL = cancel
#endif

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
void host_js_set_interrupt(JSInterruptHandler *cb);
// Compiled-code cache (js_host.c), used by precompile.c
bool module_chunk_url(const char *url);                           // "/assets/name-HASH.js": compiled code is keyed by URL
void module_bytecode_path(const char *url, char *out, size_t n);  // the cache file of such a chunk
int bytecode_write_flags(const char *src, size_t len);           // JS_WriteObject flags for a chunk
bool module_bytecode_write(const char *path, const void *bc, size_t len); // any thread
void module_bytecode_put(const char *path, char *buf, size_t len);        // hand a compiled chunk to the loader (takes buf)
void module_list_flush(void);                                     // from the main loop
// Compiles a module and its imports on worker threads, filling the cache (precompile.c).
void precompile_graph(const char *entry_url);
void mem_report(const char *stage); // largest malloc + system free memory, to the log

// Built-in sampling profiler (profiler.c): reports to the log every 30 s and to <data>/profile.txt.
void prof_init(void);
void prof_phase(const char *name, double ms); // time spent in one main-loop step
void prof_poll(JSContext *ctx); // from the JS interrupt handler
void prof_frame(void);          // once per frame
void net_poll(JSContext *ctx);                 // deliver finished requests
void net_pump(void);                           // progress transfers only (from long JS runs)
void net_set_origin(const char *page_url);
double net_clock_offset_ms(void);              // server time - console clock (0 if unknown/close)     // the page origin sent as Origin/Referer
bool net_download_file(const char *url, const char *path, bool (*progress)(long long done, long long total, void *ud),
                       void *ud, char *err, size_t errn);
void net_stats(char *out, size_t n);           // one-line summary for the log
void image_decode_stats(char *out, size_t n);   // images decoded since the last call (canvas2d.c)
void net_register(JSContext *ctx, JSValue native);
bool host_random_bytes(void *buf, size_t n);
// Blocking GET used by the module loader. Returns malloc'd body or NULL.
char *net_get_sync(const char *url, size_t *len, long *status);
char *net_get_threadsafe(const char *url, size_t *len, long *status); // own connection, any thread
long net_post_threadsafe(const char *url, const char *const *headers, const void *body, size_t len, char *err, size_t errn);
#define LOCAL_ORIGIN "https://offline.invalid" // origin of the game in local mode (local=<dir>)
bool net_is_local(const char *url);

// ---------------------------------------------------------------- bindings

void gl_register(JSContext *ctx, JSValue native);        // gl.c
void gl_buffer_sub_data(int target, int offset, const void *p, size_t len); // gl.c
void gl_draw_stats(double *total_ms, double *max_frame_ms, int *restores, int *draws); // gl.c
void gl_after_frame(void);
void gl_restore_state(void); // after drawing over the game mid-frame
void gl_texture_memory(size_t *bytes, size_t *peak, int *count);
enum { GL_POOL_VIDEO, GL_POOL_RAM, GL_POOL_PHYCONT, GL_POOL_DIALOG, GL_POOL_HEAP, GL_POOLS };
void gl_texture_pools(size_t bytes[GL_POOLS], size_t largest_out[3]); // Vita: texture bytes per memory pool
void canvas_register(JSContext *ctx, JSValue native);    // canvas2d.c
void fastpath_register(JSContext *ctx, JSValue native);  // fastpath.c
void audio_init(void);                                    // audio.c: decoder thread + output
void audio_register(JSContext *ctx, JSValue native);      // audio.c: __native.audio*
void savecrypt_register(JSContext *ctx, JSValue native);  // savecrypt.c: __native.aesEncrypt/aesDecrypt
void audio_memory(size_t *pcm, size_t *mp3, int *active_voices);

// Status messages drawn before the game takes over the screen (overlay.c).
void overlay_set(const char *msg);
void overlay_present(const char *msg); // draw + swap immediately (main thread is about to block)
bool overlay_draw(void);
void overlay_disable(void);
bool overlay_active(void);
void overlay_hud_draw(const char *text); // debug line over the game (top-right)
void overlay_busy_draw(const char *text); // full-screen message during a long frame (caller swaps)
void overlay_blit_text(uint8_t *px, int W, int H, int x, int y, const char *text, int scale, const uint8_t rgba[4]);
void overlay_image_draw(const uint8_t *rgba, int w, int h, bool changed, int *ox, int *oy);
void overlay_rgba_at(int slot, const uint8_t *rgba, int w, int h, bool changed, float x, float y, float dw, float dh);
void gl_game_to_screen(float gx, float gy, float *sx, float *sy, float *scale);
void host_frame_drawn(void); // screenshots from screens outside the game loop
int launcher_run(bool offline_ok, const char *offline_version, void (*poll)(bool *quit)); // 0 online, 1 offline, -1 quit
bool fields_draw(void); // the game's text input fields (js/input.js -> __native.setFieldOverlays)
void fields_set(int n);
bool fields_draw_pending(void);
void field_update(int i, const uint8_t *rgba, int w, int h, float x, float y, float dw, float dh);

// Text entry: the built-in keyboard (osk.c; on the desktop, the control channel unless osk=1).
bool textin_open(const char *title, const char *initial, int max_len, bool password);
int textin_poll(char *out, size_t n); // 0 open, 1 confirmed, -1 cancelled / not open
bool osk_active(void);
bool osk_mutes_pad(void);             // game input muted (keyboard open, or its buttons still held)
bool osk_event(const plat_event *ev); // true if the keyboard consumed the event
void osk_frame(const plat_gamepad *pad); // buttons and stick, once per frame
bool osk_draw(void);                  // draws the keyboard if open (caller wraps GL state for Phaser)

// Links the game opens, shown as a QR code (dialogs.c).
void link_show(const char *url, const char *caption);
bool link_active(void);
bool link_mutes_pad(void);             // game input muted (code shown, or its closing button still held)
bool link_event(const plat_event *ev); // true if consumed
void link_frame(const plat_gamepad *pad);
bool link_draw(void);
void dialog_notice(const char *text);
void dialog_pick_open(const char *title, const char *dir, const char *exts);
int dialog_pick_poll(const char **path); // 0 nothing, 1 chosen, -1 cancelled (reported once)
// Restarts the app (location.reload), straight back into the online or offline game.
void plat_relaunch(int argc, char **argv);
extern bool g_relaunch;
const char *host_version_string(void);  // "PokeRogue Vita 0.18 (abc1234)"

// Bytes held by decoded-image objects: compressed files kept, and decoded pixels currently alive.
void canvas_image_memory(size_t *encoded, size_t *decoded);
void canvas_memory(size_t *canvas_bytes, int *canvas_count, size_t *font_bytes);

// Pixel source lookup shared by gl.c (texImage2D) and canvas2d.c (drawImage).
// Returns RGBA8 pixels (straight alpha) of an Image/Canvas/ImageData-like object, or NULL.
// *owned is set when the caller must free() the returned buffer.
const uint8_t *pixels_from_js(JSContext *ctx, JSValueConst obj, int *w, int *h, bool *owned);
JSValue bitmap_ref(JSContext *ctx, JSValueConst obj); // Bitmap behind an image source, or undefined
