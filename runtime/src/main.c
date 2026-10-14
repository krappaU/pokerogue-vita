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

// ---------------------------------------------------------------- frame stats

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
    if (perf_n < PERF_WINDOW) return;
    double sum = 0;
    for (int i = 0; i < perf_n; i++) sum += perf_js[i];
    qsort(perf_js, perf_n, sizeof *perf_js, cmp_d);
    double secs = (host_now_ms() - perf_t0) / 1000.0;
    struct mallinfo mi = mallinfo();
    size_t img_enc, img_dec;
    canvas_image_memory(&img_enc, &img_dec);
    host_log("[perf] %.1f fps | JS per frame: avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f | heap %.0f MB "
             "(images: %.0f MB files, %.0f MB decoded)",
             perf_n / secs, sum / perf_n, perf_js[perf_n / 2], perf_js[perf_n * 95 / 100], perf_js[perf_n - 1],
             (unsigned)mi.uordblks / 1048576.0, img_enc / 1048576.0, img_dec / 1048576.0);
    perf_n = 0;
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
    host_log("PokeRogue runtime (QuickJS-ng %s), page %s", JS_GetVersion(), g_cfg.base_url);

    if (!plat_init(g_cfg.win_w, g_cfg.win_h)) return 1;
    net_init();
    JSContext *ctx = host_js_init();
    if (!ctx) return 1;

    // Something on screen right away: loading and compiling the game's code takes a while on the Vita.
    {
        char msg[1200];
        snprintf(msg, sizeof msg, "PokeRogue\n\nConnecting to %s", g_cfg.base_url);
        overlay_present(msg);
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
        control_poll(ctx, &quit);
        net_poll(ctx);
        host_run_jobs(ctx);

        double t_js = host_now_ms();
        JSValue now = JS_NewFloat64(ctx, t_js);
        host_call_rt(ctx, "onFrame", 1, &now);
        host_run_jobs(ctx);
        perf_add(host_now_ms() - t_js);

        overlay_draw();
        if (pending_shot[0]) {
            save_screenshot(pending_shot);
            pending_shot[0] = 0;
        }
        plat_swap();
        gl_after_frame();
        frame_no++;
        if (g_cfg.exit_after_frames && frame_no >= g_cfg.exit_after_frames) quit = true;

#ifndef __vita__
        // Pace to 60 Hz (Xvfb has no vsync; on the Vita, vglSwapBuffers waits for vblank).
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
