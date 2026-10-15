// SPDX-License-Identifier: AGPL-3.0-only
// PokeRogue runtime entry point: config, frame loop, and the test control channel.
//
// Control channel (--control FILE): append one command per line to FILE while the runtime runs.
//   key <Key>        press and release a key (DOM key names: Space, Enter, Backspace, ArrowUp, a, ...)
//   keydown <Key> / keyup <Key>
//   tap <x> <y>      touch/click at window coordinates
//   shot <path.png>  save the next rendered frame
//   eval <js>        evaluate JS in the page and log the result
//   evalfile <path>  evaluate a JS file in the page and log the result
//   ime <text>       answer an open text-entry request (Linux stand-in for the Vita keyboard)
//   ime-cancel       cancel it
//   quit

#include <errno.h>
#include <pthread.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gles.h"

#include "host.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
// QuickJS needs a deep native stack to parse the game's minified chunks; the default is 256 KB.
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
// newlib heap for the JS engine and decoded images. vitaGL allocates GPU memory separately.
int _newlib_heap_size_user = 200 * 1024 * 1024;
#include <psp2/kernel/sysmem.h>
#define DEFAULT_JS_DIR "app0:js"
#define DEFAULT_DATA_DIR "ux0:data/pokerogue"
#define DEFAULT_CONTROL "ux0:data/pokerogue/control.txt"
#else
#define DEFAULT_JS_DIR "js"
#define DEFAULT_DATA_DIR "data"
#define DEFAULT_CONTROL NULL
#endif

host_config g_cfg = {
    .base_url = "https://pokerogue.net/",
    .js_dir = DEFAULT_JS_DIR,
    .data_dir = DEFAULT_DATA_DIR,
    .control = DEFAULT_CONTROL,
    .win_w = 960,
    .win_h = 544,
    .exit_after_frames = 0,
};

static FILE *log_fp;

void host_log(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stderr);
    if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', stderr);
    if (log_fp) {
        fputs(buf, log_fp);
        if (buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', log_fp);
        fflush(log_fp);
    }
}

double host_now_ms(void) {
#ifdef __vita__
    return sceKernelGetProcessTimeWide() / 1000.0;
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    mkdir(tmp, 0777);
}

// ---------------------------------------------------------------- memory diagnostics

// Logs free system memory (Vita) and the largest block malloc can hand out right now.
void mem_report(const char *stage) {
    static const int sizes_mb[] = {256, 192, 160, 128, 96, 64, 48, 32, 16, 8, 4, 2, 1};
    int largest = 0;
    for (size_t i = 0; i < sizeof sizes_mb / sizeof *sizes_mb; i++) {
        // volatile + a write: otherwise the compiler removes the unused malloc/free pair and the
        // "allocation" always succeeds.
        char *volatile p = malloc((size_t)sizes_mb[i] << 20);
        if (p) {
            p[0] = 1;
            free(p);
            largest = sizes_mb[i];
            break;
        }
    }
#ifdef __vita__
    SceKernelFreeMemorySizeInfo info = {.size = sizeof info};
    sceKernelGetFreeMemorySize(&info);
    host_log("[mem] %s: largest malloc %d MB | system free: main %d MB, video %d MB, contiguous %d MB", stage, largest,
             info.size_user >> 20, info.size_cdram >> 20, info.size_phycont >> 20);
#else
    host_log("[mem] %s: largest malloc %d MB", stage, largest);
#endif
}

// ---------------------------------------------------------------- control channel

static long ctl_offset;
static char pending_shot[512];
static struct {
    char key[32];
    int release_frame;
} pending_release[8];
static int frame_no;
static int pending_tap_up = -1;
static float tap_x, tap_y;

static void control_line(JSContext *ctx, char *line, bool *quit) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "key") && arg) {
        plat_inject_key(arg, true);
        for (int i = 0; i < 8; i++) {
            if (!pending_release[i].key[0]) {
                snprintf(pending_release[i].key, sizeof pending_release[i].key, "%s", arg);
                pending_release[i].release_frame = frame_no + 4;
                break;
            }
        }
    } else if (!strcmp(line, "keydown") && arg) {
        plat_inject_key(arg, true);
    } else if (!strcmp(line, "keyup") && arg) {
        plat_inject_key(arg, false);
    } else if (!strcmp(line, "tap") && arg) {
        float x = 0, y = 0;
        if (sscanf(arg, "%f %f", &x, &y) == 2) {
            plat_inject_pointer(PEV_POINTER_DOWN, x, y);
            pending_tap_up = frame_no + 3;
            tap_x = x;
            tap_y = y;
        }
    } else if (!strcmp(line, "shot") && arg) {
        snprintf(pending_shot, sizeof pending_shot, "%s", arg);
    } else if (!strcmp(line, "eval") && arg) {
        char *res = NULL;
        host_eval_string(ctx, arg, "<control>", &res);
        host_log("[eval] %s", res ? res : "(exception)");
        free(res);
    } else if (!strcmp(line, "evalfile") && arg) {
        FILE *f = fopen(arg, "rb");
        if (!f) {
            host_log("[evalfile] cannot open %s", arg);
            return;
        }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc(n + 1);
        size_t got = fread(src, 1, n, f);
        src[got] = 0;
        fclose(f);
        char *res = NULL;
        host_eval_string(ctx, src, arg, &res);
        host_log("[evalfile] %s", res ? res : "(exception)");
        free(res);
        free(src);
    } else if (!strcmp(line, "ime")) {
        plat_ime_answer(arg ? arg : "");
    } else if (!strcmp(line, "ime-cancel")) {
        plat_ime_answer(NULL);
    } else if (!strcmp(line, "hud")) {
        g_cfg.hud = !g_cfg.hud;
    } else if (!strcmp(line, "quit")) {
        *quit = true;
    } else if (line[0]) {
        host_log("[control] unknown command: %s", line);
    }
}

static void control_poll(JSContext *ctx, bool *quit) {
    if (pending_tap_up >= 0 && frame_no >= pending_tap_up) {
        plat_inject_pointer(PEV_POINTER_UP, tap_x, tap_y);
        pending_tap_up = -1;
    }
    for (int i = 0; i < 8; i++) {
        if (pending_release[i].key[0] && frame_no >= pending_release[i].release_frame) {
            plat_inject_key(pending_release[i].key, false);
            pending_release[i].key[0] = 0;
        }
    }
    if (!g_cfg.control) return;
    FILE *f = fopen(g_cfg.control, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size < ctl_offset) ctl_offset = 0; // truncated
    if (size > ctl_offset) {
        fseek(f, ctl_offset, SEEK_SET);
        char line[8192];
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line);
            if (n && line[n - 1] != '\n') break; // partial line; read it next frame
            ctl_offset += n;
            line[--n] = 0;
            control_line(ctx, line, quit);
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------- version

#ifndef PKR_VERSION
#define PKR_VERSION "dev"
#endif
#ifndef PKR_COMMIT
#define PKR_COMMIT "unknown"
#endif

const char *host_version_string(void) { return "PokeRogue Vita " PKR_VERSION " (" PKR_COMMIT ")"; }

// ---------------------------------------------------------------- frame stats

static JSContext *g_ctx_for_mem;

// Where main memory goes: the whole heap, and the parts we can attribute.
void mem_breakdown(const char *stage) {
    struct mallinfo mi = mallinfo();
    size_t img_enc, img_dec, canv, fonts;
    int n_canv;
    canvas_image_memory(&img_enc, &img_dec);
    canvas_memory(&canv, &n_canv, &fonts);
    double js = 0;
    if (g_ctx_for_mem) {
        JSMemoryUsage m;
        JS_ComputeMemoryUsage(JS_GetRuntime(g_ctx_for_mem), &m);
        js = m.malloc_size / 1048576.0;
    }
    size_t tex, tex_peak;
    int n_tex;
    gl_texture_memory(&tex, &tex_peak, &n_tex);
    host_log("[mem] %s: heap in use %.0f MB | JS %.0f MB | canvases %.1f MB (%d) | fonts %.1f MB | images %.1f MB "
             "files + %.1f MB decoded | GPU textures %.1f MB (%d), peak %.1f MB",
             stage, (unsigned)mi.uordblks / 1048576.0, js, canv / 1048576.0, n_canv, fonts / 1048576.0,
             img_enc / 1048576.0, img_dec / 1048576.0, tex / 1048576.0, n_tex, tex_peak / 1048576.0);
#ifdef __vita__
    // vitaGL's GPU memory pools (textures, buffers); when they're full it falls back to the heap.
    host_log("[mem] %s: GPU pools free: video %.1f of %.0f MB, RAM %.1f of %.0f MB, contiguous %.1f of %.0f MB", stage,
             vglMemFree(VGL_MEM_VRAM) / 1048576.0, vglMemTotal(VGL_MEM_VRAM) / 1048576.0,
             vglMemFree(VGL_MEM_RAM) / 1048576.0, vglMemTotal(VGL_MEM_RAM) / 1048576.0,
             vglMemFree(VGL_MEM_PHYCONT) / 1048576.0, vglMemTotal(VGL_MEM_PHYCONT) / 1048576.0);
#endif
}

#define PERF_WINDOW 300
static double perf_js[PERF_WINDOW];
static int perf_n;
static double perf_t0;

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

// Logs JS time per frame (timers, rAF = Phaser's update + render, promise jobs) every 300 frames.
static void perf_add(double js_ms) {
    if (perf_n == 0) perf_t0 = host_now_ms();
    perf_js[perf_n++] = js_ms;
    double secs = (host_now_ms() - perf_t0) / 1000.0;
    // A window closes after 300 frames or 10 s, so slow stretches (loading) still get reported.
    if (perf_n < PERF_WINDOW && secs < 10) return;
    double sum = 0;
    for (int i = 0; i < perf_n; i++) sum += perf_js[i];
    qsort(perf_js, perf_n, sizeof *perf_js, cmp_d);
    char net[160];
    net_stats(net, sizeof net);
    host_log("[perf] %.1f fps | JS per frame: avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f | net: %s", perf_n / secs,
             sum / perf_n, perf_js[perf_n / 2], perf_js[perf_n * 95 / 100], perf_js[perf_n - 1], net);
    static int windows;
    if (windows++ % 4 == 0) mem_breakdown("running");
    perf_n = 0;
}

// Watchdog: the main loop marks which step it is in. A background thread logs when one step takes
// longer than 5 s. With interrupt=N in config.txt, JS that runs for N s in one frame is stopped,
// which logs a stack trace of where it was (and may break the game: setup can legitimately take a
// minute on the Vita).
static const char *volatile loop_phase = "startup";
static volatile double phase_t0;
static volatile bool in_frame_js;

static void set_phase(const char *name) {
    double now = host_now_ms();
    prof_phase(loop_phase, now - phase_t0);
    loop_phase = name;
    phase_t0 = now;
}

static void *watchdog(void *arg) {
    (void)arg;
    const char *reported = NULL;
    double reported_t0 = 0;
    for (;;) {
#ifdef __vita__
        sceKernelDelayThread(1000 * 1000);
#else
        struct timespec ts = {1, 0};
        nanosleep(&ts, NULL);
#endif
        const char *ph = loop_phase;
        double t0 = phase_t0, stuck = (host_now_ms() - t0) / 1000.0;
        if (stuck >= 5 && !(ph == reported && t0 == reported_t0)) {
            host_log("[watchdog] main loop stuck for %.0f s in: %s", stuck, ph);
            reported = ph, reported_t0 = t0;
        }
    }
    return NULL;
}

// Debug line over the game: version, frame rate, JS time per frame, heap use. Refreshed twice a
// second. Phaser caches GL state, so it is told to set its state up again afterwards.
static void draw_hud(JSContext *ctx, double js_ms) {
    static char text[200];
    static double t0, js_sum;
    static int frames;
    double now = host_now_ms();
    if (t0 == 0) t0 = now;
    frames++;
    js_sum += js_ms;
    if (now - t0 >= 500 || !text[0]) {
        struct mallinfo mi = mallinfo();
        double secs = (now - t0) / 1000.0;
        snprintf(text, sizeof text, "v%s %s | %.0f fps | JS %.1f ms | %.0f MB", PKR_VERSION, PKR_COMMIT,
                 secs > 0 ? frames / secs : 0, frames ? js_sum / frames : 0, (unsigned)mi.uordblks / 1048576.0);
        t0 = now, js_sum = 0, frames = 0;
    }
    overlay_hud_draw(text);
}

// Native drawing over the game's frame: the built-in keyboard and the debug line.
static void draw_over_game(JSContext *ctx, double js_ms) {
    if (!osk_active() && !g_cfg.hud && !fields_draw_pending()) return;
    host_call_rt(ctx, "beforeExternalGL", 0, NULL);
    fields_draw();
    osk_draw();
    if (g_cfg.hud) draw_hud(ctx, js_ms);
    host_call_rt(ctx, "afterExternalGL", 0, NULL);
}

static int js_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt, (void)opaque;
    if (g_ctx_for_mem) prof_poll(g_ctx_for_mem);
    net_pump();
    if (!in_frame_js || !g_cfg.interrupt_after_s || host_now_ms() - phase_t0 < g_cfg.interrupt_after_s * 1000.0)
        return 0;
    host_log("[watchdog] interrupting JS that has run for %d s in one frame", g_cfg.interrupt_after_s);
    in_frame_js = false;
    return 1;
}

static void save_screenshot(const char *path) {
    int w, h;
    plat_drawable_size(&w, &h);
    uint8_t *px = malloc((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    for (size_t i = 0; i < (size_t)w * h; i++) px[i * 4 + 3] = 255;
    if (stbi_write_png(path, w, h, 4, px, w * 4)) host_log("[shot] %s", path);
    else host_log("[shot] failed to write %s", path);
    free(px);
}

// ---------------------------------------------------------------- config file

// Optional <data dir>/config.txt with key=value lines (overrides defaults; command line wins):
//   url=http://192.168.1.10:8000/   load the game from another server (testing)
//   frames=600                      quit after N frames
//   interrupt=20                    stop JS that runs N s in one frame, logging where (debugging)
//   hud=1                           start with the debug line on (toggle: SELECT while holding L+R)
//   osk=1                           built-in keyboard instead of the system one
//   profile=0                       no built-in profiler (on by default; see profiler.c)
//   nettrace=1                      log the headers of requests to the game's API
//   local=game                      play an offline build of the game from <data>/game (no login,
//                                   saves stay on the console); automatic if <data>/game exists
//   online=1                        play the live site even if <data>/game exists
// An offline build of the game in <data>/game is used automatically unless config.txt says
// online=1 (or names another folder with local=).
static void detect_local_game(void) {
    if (g_cfg.local_dir || g_cfg.force_online) return;
    static char dir[600];
    char probe[700];
    snprintf(dir, sizeof dir, "%s/game", g_cfg.data_dir);
    snprintf(probe, sizeof probe, "%s/index.html", dir);
    FILE *f = fopen(probe, "rb");
    if (!f) return;
    fclose(f);
    g_cfg.local_dir = dir;
    g_cfg.base_url = LOCAL_ORIGIN "/";
}

static void load_config_file(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/config.txt", g_cfg.data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char url[1024];
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "url")) {
            snprintf(url, sizeof url, "%s", v);
            g_cfg.base_url = url;
        } else if (!strcmp(k, "frames")) {
            g_cfg.exit_after_frames = atoi(v);
        } else if (!strcmp(k, "interrupt")) {
            g_cfg.interrupt_after_s = atoi(v);
        } else if (!strcmp(k, "hud")) {
            g_cfg.hud = atoi(v) != 0;
        } else if (!strcmp(k, "osk")) {
            g_cfg.osk = atoi(v) != 0;
        } else if (!strcmp(k, "local") && v[0]) {
            // A folder with an offline build of the game (relative paths: inside the data folder).
            static char dir[600];
            if (v[0] == '/' || strchr(v, ':')) snprintf(dir, sizeof dir, "%s", v);
            else snprintf(dir, sizeof dir, "%s/%s", g_cfg.data_dir, v);
            size_t n = strlen(dir);
            while (n > 1 && dir[n - 1] == '/') dir[--n] = 0;
            g_cfg.local_dir = dir;
            g_cfg.base_url = LOCAL_ORIGIN "/";
        } else if (!strcmp(k, "online")) {
            g_cfg.force_online = atoi(v) != 0;
        } else if (!strcmp(k, "nettrace")) {
            g_cfg.net_trace = atoi(v) != 0;
        } else if (!strcmp(k, "profile")) {
            g_cfg.no_profile = atoi(v) == 0;
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------- main

static void usage(void) {
    fprintf(stderr,
            "usage: pokerogue-rt [--url URL] [--js DIR] [--data DIR] [--control FILE] [--frames N] [--size WxH]\n");
}

int main(int argc, char **argv) {
    // The data dir may come from the command line; the config file lives inside it.
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--data")) g_cfg.data_dir = argv[i + 1];
    load_config_file();
    detect_local_game();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--url") && v) g_cfg.base_url = argv[++i];
        else if (!strcmp(a, "--js") && v) g_cfg.js_dir = argv[++i];
        else if (!strcmp(a, "--data") && v) g_cfg.data_dir = argv[++i];
        else if (!strcmp(a, "--control") && v) g_cfg.control = argv[++i];
        else if (!strcmp(a, "--frames") && v) g_cfg.exit_after_frames = atoi(argv[++i]);
        else if (!strcmp(a, "--size") && v) sscanf(argv[++i], "%dx%d", &g_cfg.win_w, &g_cfg.win_h);
        else {
            usage();
            return 2;
        }
    }
    mkdirs(g_cfg.data_dir);
    char path[512];
    snprintf(path, sizeof path, "%s/log.txt", g_cfg.data_dir);
    log_fp = fopen(path, "w");
    host_log("%s, built %s | QuickJS-ng %s | page %s", host_version_string(), __DATE__, JS_GetVersion(), g_cfg.base_url);
    if (g_cfg.local_dir) {
        char vp[700], ver[64] = "?";
        snprintf(vp, sizeof vp, "%s/currentVersion.txt", g_cfg.local_dir);
        FILE *vf = fopen(vp, "rb");
        if (vf) {
            size_t n = fread(ver, 1, sizeof ver - 1, vf);
            ver[n] = 0;
            ver[strcspn(ver, "\r\n")] = 0;
            fclose(vf);
        }
        host_log("[local] offline game from %s (game version %s): no login, saves stay on this console", g_cfg.local_dir, ver);
    }

    mem_report("start");
    if (!plat_init(g_cfg.win_w, g_cfg.win_h)) return 1;
    mem_report("after graphics init");
    net_init();
    mem_report("after network init");
    JSContext *ctx = host_js_init();
    if (!ctx) return 1;
    g_ctx_for_mem = ctx;

    // Something on screen right away: loading and compiling the game's code takes a while on the Vita.
    {
        char msg[1200];
        if (g_cfg.local_dir) snprintf(msg, sizeof msg, "PokeRogue\n\nOffline game\n%s", g_cfg.local_dir);
        else snprintf(msg, sizeof msg, "PokeRogue\n\nConnecting to %s", g_cfg.base_url);
        overlay_present(msg);
    }

    host_js_set_interrupt(js_interrupt);
    set_phase("loading the game code");
    if (!g_cfg.no_profile) prof_init();
    {
        pthread_t wd;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        if (pthread_create(&wd, &attr, watchdog, NULL) == 0) pthread_detach(wd);
        pthread_attr_destroy(&attr);
    }

    snprintf(path, sizeof path, "%s/bootstrap.js", g_cfg.js_dir);
    if (host_eval_file(ctx, path)) {
        host_log("bootstrap failed");
        return 1;
    }

    bool quit = false;
    const double frame_ms = 1000.0 / 60.0;
    double next = host_now_ms();
    while (!quit) {
        plat_event ev;
        while (plat_poll_event(&ev)) {
            if (ev.type == PEV_QUIT) {
                quit = true;
                continue;
            }
            if (ev.type == PEV_TOGGLE_HUD) {
                g_cfg.hud = !g_cfg.hud;
                continue;
            }
            if (osk_event(&ev)) continue;
            static const char *names[] = {"keydown", "keyup", "pointerdown", "pointermove", "pointerup"};
            JSValue args[7] = {
                JS_NewString(ctx, names[ev.type]),
                JS_NewString(ctx, ev.key ? ev.key : ""),
                JS_NewString(ctx, ev.code ? ev.code : ""),
                JS_NewInt32(ctx, ev.key_code),
                JS_NewFloat64(ctx, ev.x),
                JS_NewFloat64(ctx, ev.y),
                JS_NewInt32(ctx, ev.pointer_id),
            };
            host_call_rt(ctx, "onInput", 7, args);
            for (int i = 0; i < 7; i++) JS_FreeValue(ctx, args[i]);
        }
        {
            plat_gamepad pad;
            osk_frame(plat_gamepads(&pad, 1) > 0 ? &pad : NULL);
        }
        set_phase("control channel");
        control_poll(ctx, &quit);
        set_phase("network");
        net_poll(ctx);
        set_phase("promise jobs");
        in_frame_js = true;
        host_run_jobs(ctx);

        double t_js = host_now_ms();
        JSValue now = JS_NewFloat64(ctx, t_js);
        set_phase("game frame (JS)");
        in_frame_js = true;
        host_call_rt(ctx, "onFrame", 1, &now);
        set_phase("promise jobs after frame");
        in_frame_js = true;
        host_run_jobs(ctx);
        in_frame_js = false;
        double js_ms = host_now_ms() - t_js;
        if (js_ms > 1000) host_log("[perf] slow frame: %.0f ms of JS", js_ms);
        perf_add(js_ms);
        prof_frame();

        set_phase("drawing");
        if (!overlay_draw()) draw_over_game(ctx, js_ms);
        if (pending_shot[0]) {
            save_screenshot(pending_shot);
            pending_shot[0] = 0;
        }
        set_phase("swap");
        plat_swap();
        gl_after_frame();
        set_phase("input");
        frame_no++;
        if (g_cfg.exit_after_frames && frame_no >= g_cfg.exit_after_frames) quit = true;

#ifndef __vita__
        // Pace to 60 Hz (Xvfb has no vsync; on the Vita, vglSwapBuffers waits for vblank).
        set_phase("idle (frame pacing)");
        next += frame_ms;
        double now_ms = host_now_ms();
        if (next > now_ms) {
            double wait = next - now_ms;
            struct timespec ts = {(time_t)(wait / 1000), (long)(fmod(wait, 1000.0) * 1e6)};
            nanosleep(&ts, NULL);
        } else if (now_ms - next > 100) {
            next = now_ms; // fell far behind: don't try to catch up
        }
#endif
    }

    host_js_shutdown();
    net_shutdown();
    plat_shutdown();
    if (log_fp) fclose(log_fp);
    return 0;
}
