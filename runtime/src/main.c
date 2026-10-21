// SPDX-License-Identifier: AGPL-3.0-only
// PokeRogue runtime entry point: config, frame loop, and the test control channel.
//
// Control channel (--control FILE; debug builds only): append one command per line to FILE while the
// runtime runs.
//   key <Key>        press and release a key (DOM key names: Space, Enter, Backspace, ArrowUp, a, ...)
//   keydown <Key> / keyup <Key>
//   tap <x> <y>      touch/click at window coordinates
//   shot <path.png>  save the next rendered frame
//   eval <js>        evaluate JS in the page and log the result
//   evalfile <path>  evaluate a JS file in the page and log the result
//   ime <text>       answer an open text-entry request (Linux stand-in for the Vita keyboard)
//   ime-cancel       cancel it
//   sendlog          upload the log to the log server (L + R + START on the Vita; devtools.c)
//   variants         shader variants off/on (L + R + TRIANGLE on the Vita; gl.c)
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
// 256 MB: with extended memory mode (ATTRIBUTE2=12) the console reported ~89 MB of main memory
// still free after a 200 MB heap and vitaGL's pools were set up (0.23 log); most of it goes here.
int _newlib_heap_size_user = 256 * 1024 * 1024;
#include <psp2/kernel/sysmem.h>
#define DEFAULT_JS_DIR "app0:js"
#define DEFAULT_DATA_DIR "ux0:data/pokerogue"
#ifdef PKR_DEBUG
#define DEFAULT_CONTROL "ux0:data/pokerogue/control.txt"
#else
#define DEFAULT_CONTROL NULL // player builds: no control channel (it reads a file every frame)
#endif
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
    .busy_after_ms = 2500,
    .max_textures = 4, // Phaser's shader picks a sprite's texture with an if per unit: 16 cost GPU time
#ifdef PKR_PROFILE_BUILD
    // profiling build (CMake PKR_PROFILE_BUILD): sample from the start, ten times as often, by time
    .sample = true,
    .profile_rate = 1000,
    .sample_ms = 1,
#endif
};

static FILE *log_fp;

// Log lines are written by a thread of their own: written and flushed to the memory card from the
// main loop, each line cost several ms on the Vita (the profiler's 45-line report: 0.3-0.7 s every
// 30 s, log71). host_log queues the line; the writer writes it at once, from another core, so
// little is lost if the app crashes. host_log_flush waits until everything queued is written
// (before the log is uploaded, and before the app quits or restarts).
#define LOG_BUF (256 * 1024)
static char log_buf[LOG_BUF];
static uint64_t log_queued, log_written; // bytes, ever
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER; // the watchdog and worker threads log too
static pthread_cond_t log_more = PTHREAD_COND_INITIALIZER, log_done = PTHREAD_COND_INITIALIZER;
static int log_thread_state; // 0: not started, 1: running, -1: couldn't start (written in place)

bool host_logging(void) { return log_fp != NULL; }

static void log_out(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    if (log_fp) {
        fwrite(p, 1, n, log_fp);
        fflush(log_fp);
    }
}

static void *log_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&log_lock);
    for (;;) {
        while (log_written == log_queued) pthread_cond_wait(&log_more, &log_lock);
        size_t at = (size_t)(log_written % LOG_BUF), n = (size_t)(log_queued - log_written);
        if (n > LOG_BUF - at) n = LOG_BUF - at; // up to the end of the ring; the rest next round
        pthread_mutex_unlock(&log_lock);
        log_out(log_buf + at, n);
        pthread_mutex_lock(&log_lock);
        log_written += n;
        pthread_cond_broadcast(&log_done);
    }
    return NULL;
}

void host_log_flush(void) {
    pthread_mutex_lock(&log_lock);
    uint64_t target = log_queued;
    while (log_thread_state == 1 && log_written < target) pthread_cond_wait(&log_done, &log_lock);
    pthread_mutex_unlock(&log_lock);
}

void host_log(const char *fmt, ...) {
#ifndef PKR_DEBUG
    if (!log_fp) return; // player builds: nothing to format or print without config.txt log=1
#endif
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (len < 0) return;
    size_t n = (size_t)len < sizeof buf - 1 ? (size_t)len : sizeof buf - 2;
    if (!n || buf[n - 1] != '\n') buf[n++] = '\n';
    pthread_mutex_lock(&log_lock);
    if (!log_thread_state) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        pthread_t t;
        log_thread_state = pthread_create(&t, &attr, log_thread, NULL) == 0 ? 1 : -1;
        if (log_thread_state == 1) {
            pthread_detach(t);
            // exit() closes every FILE after the atexit handlers: the writer must be done by then
            // (0.69's first build crashed in its fflush when main returned early, without a GPU).
            atexit(host_log_flush);
        }
        pthread_attr_destroy(&attr);
    }
    if (log_thread_state < 0) {
        log_out(buf, n);
    } else {
        while (log_queued + n - log_written > LOG_BUF) pthread_cond_wait(&log_done, &log_lock); // full: wait
        for (size_t i = 0; i < n;) {
            size_t at = (size_t)(log_queued % LOG_BUF), k = n - i < LOG_BUF - at ? n - i : LOG_BUF - at;
            memcpy(log_buf + at, buf + i, k);
            i += k, log_queued += k;
        }
        pthread_cond_signal(&log_more);
    }
    pthread_mutex_unlock(&log_lock);
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
    if (!host_logging()) return;
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
    double release_at;
} pending_release[8];
static int frame_no;
static int pending_tap_up = -1;
static float tap_x, tap_y;

static double ctl_wait_until; // "wait": no further commands before this time

static void toggle_variants(void);

static void control_line(JSContext *ctx, char *line, bool *quit) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0;
    if (!strcmp(line, "wait") && arg) {
        // the pause a script asked for, counted from when the previous command ran (scripts/drive.sh
        // turns its sleeps into these): with long frames, commands queued meanwhile kept their
        // spacing instead of running back to back
        ctl_wait_until = host_now_ms() + atof(arg) * 1000;
    } else if (!strcmp(line, "key") && arg) {
        plat_inject_key(arg, true);
        for (int i = 0; i < 8; i++) {
            if (!pending_release[i].key[0]) {
                snprintf(pending_release[i].key, sizeof pending_release[i].key, "%s", arg);
                // released after 120 ms like a finger's tap, and never in the frame that saw the
                // press: counted in frames, slow desktop frames (100-200 ms) either turned taps into
                // the game's 250 ms key repeat or put press and release into one game update
                pending_release[i].release_frame = frame_no + 1;
                pending_release[i].release_at = host_now_ms() + 120;
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
        if (ctx) host_eval_string(ctx, arg, "<control>", &res);
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
        if (ctx) host_eval_string(ctx, src, arg, &res);
        host_log("[evalfile] %s", res ? res : "(exception)");
        free(res);
        free(src);
#ifndef __vita__
    } else if (!strcmp(line, "ime")) {
        plat_ime_answer(arg ? arg : "");
    } else if (!strcmp(line, "ime-cancel")) {
        plat_ime_answer(NULL);
#endif
    } else if (!strcmp(line, "profile")) {
        // sampling without the debug line: drawing it makes Phaser rebuild its GL state every frame
        g_cfg.sample = !g_cfg.sample;
    } else if (!strcmp(line, "hud")) {
        g_cfg.hud = !g_cfg.hud;
    } else if (!strcmp(line, "variants")) {
        toggle_variants();
    } else if (!strcmp(line, "sendlog")) {
        dev_send_log();
    } else if (!strcmp(line, "quit")) {
        *quit = true;
    } else if (line[0]) {
        host_log("[control] unknown command: %s", line);
    }
}

// The control file is read by a thread of its own: opening it is a memory-card access (3 ms on the
// Vita, 30-45 ms while the game loads, 0.50 logs), and even twice a second it cost dev builds 3-4
// ms per frame on average in some windows (log83). The thread queues complete lines four times a
// second; control_poll runs them on the main thread.
typedef struct ctl_line {
    struct ctl_line *next;
    char text[];
} ctl_line;
static pthread_mutex_t ctl_lock = PTHREAD_MUTEX_INITIALIZER;
static ctl_line *ctl_head, *ctl_tail;

static ctl_line *ctl_pop(void) {
    pthread_mutex_lock(&ctl_lock);
    ctl_line *l = ctl_head;
    if (l && !(ctl_head = l->next)) ctl_tail = NULL;
    pthread_mutex_unlock(&ctl_lock);
    return l;
}

static void *control_reader(void *arg) {
    (void)arg;
    for (;;) {
        FILE *f = fopen(g_cfg.control, "r");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            if (size < ctl_offset) ctl_offset = 0; // truncated
            if (size > ctl_offset) {
                fseek(f, ctl_offset, SEEK_SET);
                char line[8192];
                while (fgets(line, sizeof line, f)) {
                    size_t n = strlen(line);
                    if (n && line[n - 1] != '\n') break; // partial line; read it next time
                    ctl_offset += n;
                    line[--n] = 0;
                    ctl_line *l = malloc(sizeof *l + n + 1);
                    if (!l) break;
                    memcpy(l->text, line, n + 1);
                    l->next = NULL;
                    pthread_mutex_lock(&ctl_lock);
                    if (ctl_tail) ctl_tail->next = l;
                    else ctl_head = l;
                    ctl_tail = l;
                    pthread_mutex_unlock(&ctl_lock);
                }
            }
            fclose(f);
        }
        struct timespec ts = {0, 250 * 1000000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void control_reader_start(void) {
    static bool started;
    if (started) return;
    started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024); // its line buffer is 8 KB
    pthread_t t;
    if (pthread_create(&t, &attr, control_reader, NULL) == 0) pthread_detach(t);
    else host_log("[control] couldn't start the reader thread");
    pthread_attr_destroy(&attr);
}

static void control_poll(JSContext *ctx, bool *quit) {
    if (pending_tap_up >= 0 && frame_no >= pending_tap_up) {
        plat_inject_pointer(PEV_POINTER_UP, tap_x, tap_y);
        pending_tap_up = -1;
    }
    for (int i = 0; i < 8; i++) {
        if (pending_release[i].key[0] && frame_no >= pending_release[i].release_frame &&
            host_now_ms() >= pending_release[i].release_at) {
            plat_inject_key(pending_release[i].key, false);
            pending_release[i].key[0] = 0;
        }
    }
    if (!g_cfg.control) return;
    // One key tap at a time: the next command waits until the tap's key is released. Commands
    // appended during a long frame would otherwise all run at its end and merge their taps.
    for (int i = 0; i < 8; i++)
        if (pending_release[i].key[0]) return;
    if (host_now_ms() < ctl_wait_until) return;
    control_reader_start();
    for (ctl_line *l; (l = ctl_pop());) {
        bool hold = !strncmp(l->text, "key ", 4) || !strncmp(l->text, "wait ", 5);
        control_line(ctx, l->text, quit);
        free(l);
        if (hold) break; // the rest waits for the release or the pause (see above)
    }
}

// ---------------------------------------------------------------- version

#ifndef PKR_VERSION
#define PKR_VERSION "dev"
#endif
#ifndef PKR_COMMIT
#define PKR_COMMIT "unknown"
#endif

#ifdef PKR_DEV
#define PKR_FLAVOR " dev"
#else
#define PKR_FLAVOR ""
#endif

const char *host_version_string(void) { return "PokeRogue Vita " PKR_VERSION PKR_FLAVOR " (" PKR_COMMIT ")"; }

// ---------------------------------------------------------------- frame stats

static JSContext *g_ctx_for_mem;

// Where main memory goes: the whole heap, and the parts we can attribute.
void mem_breakdown(const char *stage) {
    if (!host_logging()) return;
    struct mallinfo mi = mallinfo();
    size_t img_enc, img_dec, canv, fonts;
    int n_canv;
    canvas_image_memory(&img_enc, &img_dec);
    canvas_memory(&canv, &n_canv, &fonts);
    double js = 0;
    bool js_estimate = !strcmp(stage, "running");
    if (g_ctx_for_mem && js_estimate) {
        // Counting walks every JS object: ~40 ms on the desktop for 100 MB, likely most of a second
        // on the Vita, every 40 s of play. QuickJS's next collection is due at 1.5x what the last
        // one left, so that is known for free.
        js = JS_GetGCThreshold(JS_GetRuntime(g_ctx_for_mem)) / 1.5 / 1048576.0;
    } else if (g_ctx_for_mem) {
        JSMemoryUsage m;
        JS_ComputeMemoryUsage(JS_GetRuntime(g_ctx_for_mem), &m);
        js = m.malloc_size / 1048576.0;
    }
    size_t tex, tex_peak;
    int n_tex;
    gl_texture_memory(&tex, &tex_peak, &n_tex);
    size_t a_pcm, a_mp3;
    int a_voices;
    audio_memory(&a_pcm, &a_mp3, &a_voices);
    host_log("[mem] %s: audio %.1f MB decoded + %.1f MB streamed music, %d playing", stage, a_pcm / 1048576.0,
             a_mp3 / 1048576.0, a_voices);
    host_log("[mem] %s: heap in use %.0f MB | JS %.0f MB%s | canvases %.1f MB (%d) | fonts %.1f MB | images %.1f MB "
             "files + %.1f MB decoded | GPU textures %.1f MB (%d), peak %.1f MB",
             stage, (unsigned)mi.uordblks / 1048576.0, js, js_estimate ? " after the last collection" : "", canv / 1048576.0, n_canv, fonts / 1048576.0,
             img_enc / 1048576.0, img_dec / 1048576.0, tex / 1048576.0, n_tex, tex_peak / 1048576.0);
#ifdef __vita__
    // vitaGL's GPU memory pools (textures, buffers); when they're full it falls back to the heap.
    host_log("[mem] %s: GPU pools free: video %.1f of %.0f MB, RAM %.1f of %.0f MB, contiguous %.1f of %.0f MB, "
             "dialogs' %.1f of %.1f MB", stage,
             vglMemFree(VGL_MEM_VRAM) / 1048576.0, vglMemTotal(VGL_MEM_VRAM) / 1048576.0,
             vglMemFree(VGL_MEM_RAM) / 1048576.0, vglMemTotal(VGL_MEM_RAM) / 1048576.0,
             vglMemFree(VGL_MEM_PHYCONT) / 1048576.0, vglMemTotal(VGL_MEM_PHYCONT) / 1048576.0,
             vglMemFree(VGL_MEM_BUDGET) / 1048576.0, vglMemTotal(VGL_MEM_BUDGET) / 1048576.0);
#ifdef PKR_DEBUG
    if (n_tex) {
        size_t pool[GL_POOLS], big[3];
        gl_texture_pools(pool, big);
        host_log("[mem] %s: textures in video RAM %.1f MB, main RAM %.1f MB, contiguous %.1f MB, dialogs' %.1f MB, "
                 "heap %.1f MB | largest outside video RAM: %.1f, %.1f, %.1f MB", stage,
                 pool[GL_POOL_VIDEO] / 1048576.0, pool[GL_POOL_RAM] / 1048576.0, pool[GL_POOL_PHYCONT] / 1048576.0,
                 pool[GL_POOL_DIALOG] / 1048576.0, pool[GL_POOL_HEAP] / 1048576.0,
                 big[0] / 1048576.0, big[1] / 1048576.0, big[2] / 1048576.0);
    }
#endif
#endif
}

#define PERF_WINDOW 300
static double perf_js[PERF_WINDOW];
static int perf_n;
static double perf_t0;
// QuickJS's cycle collector runs inside an allocation once the JS heap has grown by half since the
// last run, and walks every object: a pause proportional to the whole heap. Frames that contained
// one are counted (detected by the GC threshold changing).
static int perf_gc;
static double perf_gc_max;
static double perf_swap_ms, perf_after_ms, perf_draw_ms; // non-JS parts of the frames in the window
// Time per main-loop step over the current [perf] window (steps other than JS, drawing and the
// swap are listed there when they take noticeable time).
#define N_PHASE_STATS 16
static struct {
    const char *name;
    double ms;
} phase_stats[N_PHASE_STATS];

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double t_main_start; // host_now_ms() at the start of main(), for the [perf] line's t=

// Logs JS time per frame (timers, rAF = Phaser's update + render, promise jobs) every 300 frames.
static void perf_add(double js_ms) {
    if (perf_n == 0) perf_t0 = host_now_ms();
    perf_js[perf_n++] = js_ms;
    double secs = (host_now_ms() - perf_t0) / 1000.0;
    // A window closes after 300 frames or 10 s, so slow stretches (loading) still get reported.
    if (perf_n < PERF_WINDOW && secs < 10) return;
    if (!host_logging()) {
        double d1, d2;
        int i1, i2;
        gl_draw_stats(&d1, &d2, &i1, &i2);
        plat_gpu_stats(&d1, &d2, &i1);
        perf_n = perf_gc = 0;
        perf_gc_max = perf_swap_ms = perf_after_ms = perf_draw_ms = 0;
        for (int i = 0; i < N_PHASE_STATS; i++) phase_stats[i].ms = 0;
        return;
    }
    double sum = 0;
    for (int i = 0; i < perf_n; i++) sum += perf_js[i];
    qsort(perf_js, perf_n, sizeof *perf_js, cmp_d);
    char net[320];
    net_stats(net, sizeof net);
    char dec[64];
    image_decode_stats(dec, sizeof dec);
    char gc[64] = "";
    if (perf_gc) snprintf(gc, sizeof gc, " | GC in %d frames (up to %.0f ms)", perf_gc, perf_gc_max);
    char other[160] = "";
    size_t olen = 0;
    static const char *counted[] = {"promise jobs", "game frame (JS)", "promise jobs after frame", "drawing", "swap"};
    for (int i = 0; i < N_PHASE_STATS && phase_stats[i].name; i++) {
        bool skip = false;
        for (size_t k = 0; k < sizeof counted / sizeof *counted; k++) skip |= !strcmp(phase_stats[i].name, counted[k]);
        double per = phase_stats[i].ms / perf_n;
        if (!skip && per >= 2 && olen < sizeof other - 48)
            olen += snprintf(other + olen, sizeof other - olen, "%s%s %.0f", olen ? ", " : " | also (ms/frame): ", phase_stats[i].name, per);
        phase_stats[i].ms = 0;
    }
    double gl_total, gl_max, gpu_avg, gpu_max;
    int restores, draws, gpu_n;
    gl_draw_stats(&gl_total, &gl_max, &restores, &draws);
    plat_gpu_stats(&gpu_avg, &gpu_max, &gpu_n);
    char glx[96] = "", gpu[64] = "";
    if (restores) snprintf(glx, sizeof glx, ", %d textures re-uploaded", restores);
    // Sampled frames (debug builds on the Vita): how long the GPU took for one frame.
    if (gpu_n) snprintf(gpu, sizeof gpu, " | GPU %.1f ms/frame (max %.0f, %d frames)", gpu_avg, gpu_max, gpu_n);
    host_log("[perf] %.1f fps | JS per frame: avg %.2f ms, p50 %.2f, p95 %.2f, max %.2f%s | draw %.1f, swap %.1f, after %.1f ms | "
             "in GL draw calls %.1f ms/frame (max %.0f), %d calls/frame%s%s%s | net: %s | %s | t=%.1f s",
             perf_n / secs, sum / perf_n, perf_js[perf_n / 2], perf_js[perf_n * 95 / 100], perf_js[perf_n - 1], gc,
             perf_draw_ms / perf_n, perf_swap_ms / perf_n, perf_after_ms / perf_n, gl_total / perf_n, gl_max, draws / perf_n,
             glx, gpu, other, net, dec, (host_now_ms() - t_main_start) / 1000.0);
    perf_gc = 0;
    perf_gc_max = 0;
    perf_swap_ms = perf_after_ms = perf_draw_ms = 0;
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
    for (int i = 0; i < N_PHASE_STATS; i++)
        if (phase_stats[i].name == loop_phase || !phase_stats[i].name) {
            phase_stats[i].name = loop_phase;
            phase_stats[i].ms += now - phase_t0;
            break;
        }
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
        snprintf(text, sizeof text, "v%s%s %s | %.0f fps | JS %.1f ms | %.0f MB", PKR_VERSION, PKR_FLAVOR, PKR_COMMIT,
                 secs > 0 ? frames / secs : 0, frames ? js_sum / frames : 0, (unsigned)mi.uordblks / 1048576.0);
        t0 = now, js_sum = 0, frames = 0;
    }
    overlay_hud_draw(text);
}

// Native drawing over the game's frame: the built-in keyboard and the debug line.
// L + R + TRIANGLE (debug builds) or "variants" on the control channel: shader variants off/on,
// with a notice for a few seconds.
static double variants_notice_until;
static bool variants_on = true;
static __attribute__((unused)) void toggle_variants(void) {
    variants_on = gl_toggle_variants();
    variants_notice_until = host_now_ms() + 3000;
}

static void draw_over_game(JSContext *ctx, double js_ms) {
    int pre_total, pre_left = gl_precompile_left(&pre_total);
    bool variants_notice = host_now_ms() < variants_notice_until;
    if (!osk_active() && !link_active() && !g_cfg.hud && !fields_draw_pending() && !pre_left && !variants_notice) return;
    host_call_rt(ctx, "beforeExternalGL", 0, NULL);
    fields_draw();
    osk_draw();
    link_draw();
    if (g_cfg.hud) draw_hud(ctx, js_ms);
    if (pre_left) {
        char text[80];
        snprintf(text, sizeof text, "Preparing shaders %d/%d: the game may stutter", pre_total - pre_left, pre_total);
        overlay_notice_draw(text);
    } else if (variants_notice) {
        overlay_notice_draw(variants_on ? "Shader variants on" : "Shader variants off (original shaders)");
    }
    host_call_rt(ctx, "afterExternalGL", 0, NULL);
}

// A frame of the game's JS can run for many seconds (building the title screen: ~40 s on the Vita),
// and the screen would show whatever was presented last, often black. After 2.5 s (config busy=<ms>), a
// "Loading" screen is drawn and presented from the JS interrupt callback, refreshed every
// BUSY_EVERY_MS; the game's GL state is put back afterwards. Only once the game's renderer has
// taken over from the startup overlay (which covers loading the code).
#define BUSY_EVERY_MS 400
// QuickJS collects cycles when its heap reaches 1.5x what was left after the last collection; with
// the game's ~60 MB of JS objects that takes 1.5-2.5 s on the Vita, often mid-battle (log65,
// log68). Under the loading screen (once per long frame) a collection costs nothing visible: if
// the heap is at least a third of the way to the next one, run it now, which also puts the next one
// further off.
static void gc_during_loading(void) {
    static double done_t0;
    if (!g_ctx_for_mem || done_t0 == phase_t0) return;
    done_t0 = phase_t0;
    JSRuntime *rt = JS_GetRuntime(g_ctx_for_mem);
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(rt, &m);
    double threshold = (double)JS_GetGCThreshold(rt), after_last = threshold / 1.5;
    if (m.malloc_size < after_last + (threshold - after_last) / 3) return;
    double t0 = host_now_ms();
    JS_RunGC(rt);
    double ms = host_now_ms() - t0;
    size_t before = m.malloc_size;
    JS_ComputeMemoryUsage(rt, &m);
    JS_SetGCThreshold(rt, m.malloc_size + m.malloc_size / 2);
    host_log("[gc] collected under the loading screen: %.0f ms, JS heap %.1f -> %.1f MB", ms, before / 1048576.0,
             m.malloc_size / 1048576.0);
}

static void busy_poll(void) {
    static double last;
    static int tick;
    if (!in_frame_js || !g_cfg.busy_after_ms || overlay_active()) return;
    double now = host_now_ms();
    if (now - phase_t0 < g_cfg.busy_after_ms || now - last < (g_cfg.busy_after_ms < BUSY_EVERY_MS ? g_cfg.busy_after_ms : BUSY_EVERY_MS))
        return;
    last = now;
    static double logged_t0;
    if (logged_t0 != phase_t0 && g_cfg.busy_after_ms >= 1000) host_log("[busy] loading screen (frame running for %.0f s)", (now - phase_t0) / 1000);
    logged_t0 = phase_t0;
    static const char *dots[] = {"Loading   ", "Loading.  ", "Loading.. ", "Loading..."};
    int pre_total, pre_left = gl_precompile_left(&pre_total);
    char text[80];
    if (pre_left) snprintf(text, sizeof text, "%s\n\nPreparing shaders %d/%d", dots[tick % 4], pre_total - pre_left, pre_total);
    else snprintf(text, sizeof text, "%s", dots[tick % 4]);
    tick++;
    overlay_busy_draw(text);
    plat_swap();
    gl_restore_state();
    // The game is busy anyway: a good time to build shader variants (gl.c, which leaves the GL
    // state as it was). The next poll redraws the screen right after a slow one.
    if (pre_left) gl_precompile(BUSY_EVERY_MS / 2);
    else gc_during_loading();
}

// Shader variants not built during loading (gl_precompile): between frames, at most one slow build
// (~1 s on the console when not in vitaGL's cache) every PRECOMPILE_GAP_MS, so input still gets
// through; draw_over_game shows a notice meanwhile.
#define PRECOMPILE_GAP_MS 300
static void precompile_between_frames(void) {
    static double next;
    double now = host_now_ms();
    if (now < next || !gl_precompile_left(NULL)) return;
    gl_precompile(8);
    if (host_now_ms() - now > 100) next = host_now_ms() + PRECOMPILE_GAP_MS;
}

static int js_interrupt(JSRuntime *rt, void *opaque) {
    (void)rt, (void)opaque;
    if (g_ctx_for_mem) prof_poll(g_ctx_for_mem);
    net_pump();
    busy_poll();
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
    gl_pixel_store(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_flip_vertically_on_write(1);
    for (size_t i = 0; i < (size_t)w * h; i++) px[i * 4 + 3] = 255;
    if (stbi_write_png(path, w, h, 4, px, w * 4)) host_log("[shot] %s", path);
    else host_log("[shot] failed to write %s", path);
    free(px);
}

// ---------------------------------------------------------------- config file

// Optional <data dir>/config.txt with key=value lines (overrides defaults; command line wins):
//   osk=1                           desktop: the built-in keyboard instead of control-channel answers
//   audio=0                         no sound
//   busy=2500                       show a "Loading" screen once one frame's JS has run this long (0: never)
//   local=game                      play an offline build of the game from <data>/game (no login,
//                                   saves stay on the console); automatic if <data>/game exists
//   online=1                        play the live site even if <data>/game exists
//   log=1                           write log.txt (player builds; debug builds always do)
//   gamefix=0                       the game's code exactly as it is, without gamefix.c's rewrites
// Debug builds only (dev, profiling and desktop builds; PKR_DEBUG in CMakeLists.txt):
//   url=http://192.168.1.10:8000/   load the game from another server (testing)
//   frames=600                      quit after N frames
//   interrupt=20                    stop JS that runs N s in one frame, logging where (debugging)
//   hud=1                           start with the debug line on (toggle: SELECT while holding L+R)
//   profile=0                       no built-in profiler (on by default; see profiler.c)
//   sample=1                        profile from the start (else only with the debug line)
//   profile_rate=1000               JS operations between profile samples (QuickJS's default 10000)
//   sample_ms=1                     at most one sample per ms: samples follow time, not JS operations
//   nettrace=1                      log the headers of requests to the game's API
//   maxtextures=16                  tell Phaser of at most N texture units (default 4: its shader has an if
//                                   per unit; 0 = as many as the GPU has)
//   plainshaders=1                  measuring only: every textured fragment shader just samples its texture
//                                   (wrong colours, effects gone), to see how much GPU time the shaders cost
//   gpuprofile=1                    every 10 s, GPU time per shader program for one frame ([gpu] lines)
//   shaderfix=0                     the game's shaders as they are, without js/webgl.js's rewrites (to compare)
//   precompile=0                    shader variants built only when first drawn, not ahead (to compare)
//   bench=shaders                   dev builds: the shader benchmark (bench/) instead of the game; its log
//                                   goes to the log server by itself, marked "shader benchmark"
// Dev builds only (devtools.c):
//   devserver=https://host/         log server for log upload (tools/devserver), devkey=<its key>
//   devbuilds=off|<url>             test builds: off, or another build.json than the dev-build release
// Is an offline build of the game in <data>/game? The start screen offers it (unless config.txt
// decides: online=1, or local=<dir>).
static char offline_dir[600], offline_version[64] = "?";
static bool offline_installed(void) {
    // An unpacked folder (copied by hand) or the downloaded zip.
    char probe[700];
    snprintf(offline_dir, sizeof offline_dir, "%s/game", g_cfg.data_dir);
    snprintf(probe, sizeof probe, "%s/index.html", offline_dir);
    FILE *f = fopen(probe, "rb");
    if (f) {
        fclose(f);
        snprintf(probe, sizeof probe, "%s/currentVersion.txt", offline_dir);
    } else {
        snprintf(offline_dir, sizeof offline_dir, "%s/game.zip", g_cfg.data_dir);
        if (!(f = fopen(offline_dir, "rb"))) return false;
        fclose(f);
        snprintf(probe, sizeof probe, "%s/game.zip.version", g_cfg.data_dir);
    }
    if ((f = fopen(probe, "rb"))) {
        size_t n = fread(offline_version, 1, sizeof offline_version - 1, f);
        offline_version[n] = 0;
        offline_version[strcspn(offline_version, "\r\n")] = 0;
        fclose(f);
    }
    return true;
}

// Called by screens drawn before the game loop (start screen) right before they swap buffers.
void host_frame_drawn(void) {
    if (pending_shot[0]) {
        save_screenshot(pending_shot);
        pending_shot[0] = 0;
    }
}

static void launcher_poll(bool *quit) {
    control_poll(NULL, quit);
    frame_no++;
}

static void load_config_file(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/config.txt", g_cfg.data_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "log")) {
            g_cfg.log = atoi(v) != 0;
        } else if (!strcmp(k, "gamefix")) {
            g_cfg.no_gamefix = atoi(v) == 0;
#ifdef PKR_DEBUG
        } else if (!strcmp(k, "url")) {
            static char url[1024];
            snprintf(url, sizeof url, "%s", v);
            g_cfg.base_url = url;
        } else if (!strcmp(k, "frames")) {
            g_cfg.exit_after_frames = atoi(v);
        } else if (!strcmp(k, "interrupt")) {
            g_cfg.interrupt_after_s = atoi(v);
        } else if (!strcmp(k, "hud")) {
            g_cfg.hud = atoi(v) != 0;
        } else if (!strcmp(k, "sample")) {
            g_cfg.sample = atoi(v) != 0;
        } else if (!strcmp(k, "profile_rate")) {
            g_cfg.profile_rate = atoi(v);
        } else if (!strcmp(k, "sample_ms")) {
            g_cfg.sample_ms = atof(v);
        } else if (!strcmp(k, "nettrace")) {
            g_cfg.net_trace = atoi(v) != 0;
        } else if (!strcmp(k, "maxtextures")) {
            g_cfg.max_textures = atoi(v);
        } else if (!strcmp(k, "plainshaders")) {
            g_cfg.plain_shaders = atoi(v) != 0;
        } else if (!strcmp(k, "gpuprofile")) {
            g_cfg.gpu_profile = atoi(v) != 0;
        } else if (!strcmp(k, "shaderfix")) {
            g_cfg.no_shader_fix = atoi(v) == 0;
        } else if (!strcmp(k, "precompile")) {
            g_cfg.no_precompile = atoi(v) == 0;
#ifdef PKR_DEV
        } else if (!strcmp(k, "bench") && !strcmp(v, "shaders")) {
            // The shader benchmark page the dev build carries (bench/), run instead of the game.
            static char dir[600];
#ifdef __vita__
            snprintf(dir, sizeof dir, "app0:bench");
#else
            snprintf(dir, sizeof dir, "%s/../bench", g_cfg.js_dir);
#endif
            g_cfg.local_dir = dir;
            g_cfg.base_url = LOCAL_ORIGIN "/";
#endif
        } else if (!strcmp(k, "profile")) {
            g_cfg.no_profile = atoi(v) == 0;
#endif
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
        } else if (!strcmp(k, "audio")) {
            g_cfg.no_audio = atoi(v) == 0;
        } else if (!strcmp(k, "busy")) {
            g_cfg.busy_after_ms = atoi(v);
        } else if (!strcmp(k, "devserver") && v[0]) {
            g_cfg.dev_server = strdup(v);
        } else if (!strcmp(k, "devkey") && v[0]) {
            g_cfg.dev_key = strdup(v);
        } else if (!strcmp(k, "devbuilds") && v[0]) {
            g_cfg.dev_builds = strdup(v);
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------- main

static void usage(void) {
    fprintf(stderr,
            "usage: pokerogue-rt [--url URL] [--js DIR] [--data DIR] [--control FILE] [--frames N] [--size WxH]\n");
}

bool g_relaunch;

// location.reload: the game asks for it after importing save data. The app restarts and goes
// straight back to the same game (online or offline) instead of the start screen.
static void relaunch(JSContext *ctx, int argc, char **argv) {
    host_call_rt(ctx, "flushStorage", 0, NULL);
    char path[600];
    snprintf(path, sizeof path, "%s/relaunch.txt", g_cfg.data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(g_cfg.local_dir ? "offline" : "online", f);
        fclose(f);
    }
    host_log("[app] restarting (the game asked to reload the page)");
    host_log_flush();
    plat_relaunch(argc, argv);
    host_log("[app] restart failed; carrying on");
    g_relaunch = false;
}

// After a relaunch: the mode to go back to ("online"/"offline"), or "" for the start screen.
static void relaunch_mode(char *out, size_t n) {
    char path[600];
    snprintf(path, sizeof path, "%s/relaunch.txt", g_cfg.data_dir);
    out[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
    remove(path);
}

int main(int argc, char **argv) {
    t_main_start = host_now_ms();
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
    char path[512], prev_log[512];
    snprintf(path, sizeof path, "%s/log.txt", g_cfg.data_dir);
    // The previous run's log is kept: after a crash it is the one that matters.
    snprintf(prev_log, sizeof prev_log, "%s/log-prev.txt", g_cfg.data_dir);
#ifndef PKR_DEBUG
    if (g_cfg.log)
#endif
    {
        remove(prev_log);
        rename(path, prev_log);
        log_fp = fopen(path, "w");
    }
    host_log("%s, built %s | QuickJS-ng %s | page %s", host_version_string(), __DATE__, JS_GetVersion(), g_cfg.base_url);


    mem_report("start");
    dev_early(); // may start the updater and quit
    if (!plat_init(g_cfg.win_w, g_cfg.win_h)) return 1;
    mem_report("after graphics init");

    net_init();
    mem_report("after network init");
    dev_start(prev_log);
    if (!g_cfg.no_audio) audio_init();

    // Online or offline: the start screen, unless config.txt decided. It can also download the
    // offline game.
    bool have_offline = offline_installed();
    char again[16];
    relaunch_mode(again, sizeof again);
    if (!again[0]) dev_offer_update(); // a newer test build in the dev-build release (restarts the app; also before bench=shaders)
    if (!strcmp(again, "offline") && have_offline && !g_cfg.local_dir) {
        g_cfg.local_dir = offline_dir;
        g_cfg.base_url = LOCAL_ORIGIN "/";
    }
    while (!g_cfg.local_dir && !g_cfg.force_online && strcmp(again, "online")) {
        int choice = launcher_run(have_offline, offline_version, launcher_poll);
        if (choice < 0) return 0;
        if (choice == 0) break;
        if (choice == 2 || choice == 3) { // download, or update to a newer release
            offline_install();
            have_offline = offline_installed();
            continue; // back to the start screen, now offering the installed game
        }
        g_cfg.local_dir = offline_dir;
        g_cfg.base_url = LOCAL_ORIGIN "/";
    }
    net_set_origin(g_cfg.base_url);
    if (g_cfg.local_dir) {
        char vp[700], ver[64] = "?";
        snprintf(vp, sizeof vp, strstr(g_cfg.local_dir, ".zip") ? "%s.version" : "%s/currentVersion.txt", g_cfg.local_dir);
        FILE *vf = fopen(vp, "rb");
        if (vf) {
            size_t n = fread(ver, 1, sizeof ver - 1, vf);
            ver[n] = 0;
            ver[strcspn(ver, "\r\n")] = 0;
            fclose(vf);
        }
        host_log("[local] offline game from %s (game version %s): no login, saves stay on this console", g_cfg.local_dir, ver);
    }
    else host_log("[online] playing %s", g_cfg.base_url);
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
#ifdef PKR_DEBUG
    if (!g_cfg.no_profile) prof_init();
#endif
    if (g_cfg.profile_rate > 0) JS_SetInterruptCounterInit(g_cfg.profile_rate);
#ifdef PKR_DEBUG
    {
        pthread_t wd;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 64 * 1024);
        if (pthread_create(&wd, &attr, watchdog, NULL) == 0) pthread_detach(wd);
        pthread_attr_destroy(&attr);
    }
#endif

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
#ifdef PKR_DEBUG
                g_cfg.hud = !g_cfg.hud;
#endif
                continue;
            }
            if (ev.type == PEV_TOGGLE_VARIANTS) {
#ifdef PKR_DEBUG
                toggle_variants();
#endif
                continue;
            }
            if (ev.type == PEV_SEND_LOG) {
                dev_send_log();
                continue;
            }
            if (link_event(&ev) || osk_event(&ev)) continue;
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
            int np = plat_gamepads(&pad, 1);
            osk_frame(np > 0 ? &pad : NULL);
            link_frame(np > 0 ? &pad : NULL);
        }
        set_phase("control channel");
        control_poll(ctx, &quit);
        dev_frame();
        set_phase("network");
        net_poll(ctx);
        set_phase("promise jobs");
        in_frame_js = true;
        size_t gc_threshold = JS_GetGCThreshold(JS_GetRuntime(ctx));
        double t_jobs = host_now_ms();
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
        // (promise jobs before the frame count too: during loads, fetched files are handled there)
        double js_ms = host_now_ms() - t_jobs;
        if (js_ms > 1000) host_log("[perf] slow frame: %.0f ms of JS", js_ms);
        if (JS_GetGCThreshold(JS_GetRuntime(ctx)) != gc_threshold) {
            double all = host_now_ms() - t_jobs;
            perf_gc++;
            if (all > perf_gc_max) perf_gc_max = all;
        }
        perf_add(js_ms);
        module_list_flush();
        prof_frame(js_ms);

        set_phase("drawing");
        double t_draw = host_now_ms();
        if (!overlay_draw()) draw_over_game(ctx, js_ms);
        if (pending_shot[0]) {
            save_screenshot(pending_shot);
            pending_shot[0] = 0;
        }
        set_phase("swap");
        double t_swap = host_now_ms();
        plat_swap();
        double t_after = host_now_ms();
        gl_after_frame();
        precompile_between_frames();
        perf_draw_ms += t_swap - t_draw, perf_swap_ms += t_after - t_swap, perf_after_ms += host_now_ms() - t_after;
        set_phase("input");
        frame_no++;
        if (g_relaunch) relaunch(ctx, argc, argv);
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
    host_log_flush();
    if (log_fp) fclose(log_fp);
    return 0;
}
