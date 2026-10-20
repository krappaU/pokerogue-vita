// SPDX-License-Identifier: AGPL-3.0-only
// Built-in sampling profiler, so that logs from the Vita show where the time goes. It samples
// while the debug line is on (SELECT while holding L+R, or hud=1 in config.txt), or after the
// control channel's "profile" command (without the debug line).
//
// At each of the interpreter's interrupt checks (QuickJS makes one every 10000 counted operations,
// a few dozen times a second) it records the JS call stack (JS_SampleStack,
// patches/quickjs-0001-*). Samples are therefore spread over JS work, not wall-clock time; time
// inside native calls is not seen. The main loop also reports how long each of its steps takes
// (game JS, drawing, buffer swap / GPU wait, network...).
//
// Every 30 s (while sampling) the log gets the busiest functions (self time = the sample's innermost frame;
// total = anywhere on the stack) and the main-loop split; <data>/profile.txt gets all stacks in
// collapsed form (one "frame;frame;frame count" line each, for flame graph tools).

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define REPORT_MS 30000
#define MAX_STACKS 4096
#define MAX_PHASES 16

typedef struct {
    char *key;
    unsigned count;
} entry;

static entry stacks[MAX_STACKS];
static int n_stacks;
static unsigned n_samples, n_dropped;
static bool started;
static const char *phase_names[MAX_PHASES];
static double phase_ms[MAX_PHASES], phase_total;
static double last_report;

void prof_init(void) {
    started = true;
    last_report = host_now_ms();
}

// Time-gated sampling (sample_ms): the next sample is due sample_ms after the last one, but never
// earlier than sample_ms into a main-loop step, so time spent in C between two JS runs (GL, swap,
// waiting) isn't charged to whatever JS runs first afterwards.
static double next_sample;
static double last_sample_t; // see slow_report

// A random interval averaging sample_ms: a fixed one lines up with the frame (each frame starts a
// step) and keeps sampling the same spot.
static double sample_gap(void) {
    static uint32_t seed = 12345;
    seed = seed * 1664525u + 1013904223u;
    return g_cfg.sample_ms * 2.0 * (double)(seed >> 8) / 16777216.0;
}

// Time spent in one step of the main loop (called by the loop when it moves to the next step).
void prof_phase(const char *name, double ms) {
    if (!started) return;
    last_sample_t = host_now_ms();
    if (g_cfg.sample_ms > 0) next_sample = last_sample_t + sample_gap();
    for (int i = 0; i < MAX_PHASES; i++) {
        if (phase_names[i] == name || !phase_names[i]) {
            phase_names[i] = name;
            phase_ms[i] += ms;
            phase_total += ms;
            return;
        }
    }
}

static unsigned hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
}

// Stack strings live in one fixed buffer, reset after each report: the heap is often nearly full
// on the Vita, and graphics allocations that fail there stall the game for a second.
#define ARENA_SIZE (1024 * 1024)
static char arena[ARENA_SIZE];
static size_t arena_used;

static char *arena_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    if (arena_used + n > ARENA_SIZE) return NULL;
    char *p = arena + arena_used;
    memcpy(p, s, n);
    arena_used += n;
    return p;
}

// Returns the stored copy of the stack (NULL if dropped).
static const char *count_stack(const char *key) {
    unsigned h = hash(key) % MAX_STACKS;
    for (int probe = 0; probe < MAX_STACKS; probe++, h = (h + 1) % MAX_STACKS) {
        if (!stacks[h].key) {
            if (n_stacks >= MAX_STACKS * 3 / 4 || !(stacks[h].key = arena_strdup(key))) { n_dropped++; return NULL; }
            stacks[h].count = 1;
            n_stacks++;
            return stacks[h].key;
        }
        if (!strcmp(stacks[h].key, key)) { stacks[h].count++; return stacks[h].key; }
    }
    return NULL;
}

// Slow frames: each sample of the current frame, with the time since the sample before it (or
// since the main-loop step began). A long native call or a garbage collection shows up as a long
// gap before the next sample, which is charged to that sample's stack (usually the caller's).
#define SLOW_FRAME_MS 250
#define SLOW_REPORTS 20 // per 30 s report, to bound the log
#define FRAME_SAMPLES 1024
static struct { const char *key; double ms; } fsamp[FRAME_SAMPLES];
static int n_fsamp, n_slow;

// "name@preload-helper-CoW3Hs31.js:64:120" -> "name@preload-helper:64:120": the build hash makes
// stacks long (and they're stored for each distinct stack).
static void shorten(char *s) {
    char *w = s;
    for (char *r = s; *r;) {
        if (*r == '-') {
            char *e = r + 1;
            while ((*e >= 'a' && *e <= 'z') || (*e >= 'A' && *e <= 'Z') || (*e >= '0' && *e <= '9') || *e == '_' || *e == '-') e++;
            if (e - r - 1 == 8 && !strncmp(e, ".js:", 4)) { // "-XXXXXXXX.js:"
                r = e + 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

// Called from the JS interrupt handler.
void prof_poll(JSContext *ctx) {
    if (!started) return;
    // Only while the debug line is on: in the Vita3K emulator, sampling during normal play caused
    // one-second stalls (cause unknown; each sample itself is fast).
    if (!g_cfg.hud && !g_cfg.sample) return;
    if (g_cfg.sample_ms > 0) {
        double now = host_now_ms();
        if (now < next_sample) return;
        next_sample = now + sample_gap();
    }
    static char buf[4096];
    if (JS_SampleStack(ctx, buf, sizeof buf) > 0 && buf[0]) {
        shorten(buf);
        const char *key = count_stack(buf);
        n_samples++;
        double now = host_now_ms();
        if (n_fsamp < FRAME_SAMPLES) fsamp[n_fsamp].key = key, fsamp[n_fsamp++].ms = now - last_sample_t;
        last_sample_t = now;
    }
}

// The innermost `n` frames of a stack ("a;b;c;d" -> "c;d" for n = 2).
static const char *stack_tail(const char *key, int n) {
    const char *p = key + strlen(key);
    while (p > key) {
        if (p[-1] == ';' && --n == 0) break;
        p--;
    }
    return p;
}

// A frame whose JS ran for SLOW_FRAME_MS or more: where its time went, by stack.
static void slow_report(double js_ms) {
    if (n_slow++ >= SLOW_REPORTS) return;
    static struct { const char *key; double ms; } agg[FRAME_SAMPLES];
    int na = 0;
    double sampled = 0;
    for (int i = 0; i < n_fsamp; i++) {
        sampled += fsamp[i].ms;
        int k = 0;
        while (k < na && agg[k].key != fsamp[i].key) k++;
        if (k == na) agg[na].key = fsamp[i].key, agg[na++].ms = 0;
        agg[k].ms += fsamp[i].ms;
    }
    host_log("[slow] frame with %.0f ms of JS: %d samples over %.0f ms; the most time in:", js_ms, n_fsamp, sampled);
    for (int r = 0; r < 3 && na; r++) {
        int best = 0;
        for (int k = 1; k < na; k++)
            if (agg[k].ms > agg[best].ms) best = k;
        if (agg[best].ms < 20) break;
        host_log("[slow]   %4.0f ms  %.300s", agg[best].ms, agg[best].key ? stack_tail(agg[best].key, 8) : "(stack not stored)");
        agg[best] = agg[--na];
    }
}

// ---------------------------------------------------------------- report

typedef struct { const char *name; size_t len; unsigned self, total; int stack; } fstat;

static int by_self(const void *a, const void *b) { return (int)((const fstat *)b)->self - (int)((const fstat *)a)->self; }
static int by_total(const void *a, const void *b) { return (int)((const fstat *)b)->total - (int)((const fstat *)a)->total; }

// Functions by name, through a hash index (FN_SLOTS > cap): a linear search over ~2000 functions
// for each frame of ~3000 stacks took seconds on the Vita, every 30 s.
#define FN_SLOTS 4096
static fstat *find_fn(fstat *fs, int *n, int cap, int *index, const char *name, size_t len) {
    unsigned h = 2166136261u;
    for (size_t i = 0; i < len; i++) h = (h ^ (unsigned char)name[i]) * 16777619u;
    for (unsigned k = h % FN_SLOTS;; k = (k + 1) % FN_SLOTS) {
        int i = index[k];
        if (i < 0) {
            if (*n >= cap) return NULL;
            fs[*n] = (fstat){name, len, 0, 0, 0};
            index[k] = *n;
            return &fs[(*n)++];
        }
        if (fs[i].len == len && !memcmp(fs[i].name, name, len)) return &fs[i];
    }
}

// ---------------------------------------------------------------- groups
// Each sample's stack sorted into one group, to see at a glance where a battle frame's JS goes
// (docs/native-renderer.md). Frames are "name@file:line:col"; the runtime's own files end in ".js",
// the game's and Phaser's bundles don't (shorten() drops their hash and extension). Phaser's
// Game.step frames ("step@") come first; the frame after them says which part of the frame it is.
enum { G_PIPELINES, G_TEXT, G_RENDER_NATIVE, G_RENDER_JS, G_TWEENS, G_ANIMS, G_UPDATE, G_EVENTS, G_OURS,
       G_OTHER, G_COUNT };
static const char *group_names[G_COUNT] = {
    "render: PokeRogue's sprite pipelines", "render: text", "render: in native code",
    "render: Phaser JS", "update: tweens", "update: sprite animations", "update: other",
    "frame events", "runtime JS outside the frame", "other",
};

// strstr within the first len bytes (newlib doesn't always declare memmem)
static bool has(const char *f, size_t len, const char *s) {
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= len; i++)
        if (!memcmp(f + i, s, n)) return true;
    return false;
}
static bool frame_is(const char *f, size_t len, const char *name) {
    size_t n = strlen(name);
    return len > n && !memcmp(f, name, n) && f[n] == '@';
}
static bool frame_in_ours(const char *f, size_t len) {
    const char *at = memchr(f, '@', len);
    return at && has(at, len - (size_t)(at - f), ".js:");
}

static int group_of(const char *key) {
    const char *fr[64];
    size_t fl[64];
    int n = 0;
    for (const char *p = key; n < 64;) {
        const char *e = strchr(p, ';');
        fr[n] = p, fl[n] = e ? (size_t)(e - p) : strlen(p), n++;
        if (!e) break;
        p = e + 1;
    }
    int i = 0;
    while (i < n && !frame_is(fr[i], fl[i], "step")) i++;
    if (i == n) return frame_in_ours(fr[n - 1], fl[n - 1]) ? G_OURS : G_OTHER;
    while (i < n && frame_is(fr[i], fl[i], "step")) i++;
    if (i == n) return G_OTHER;
    bool render = frame_is(fr[i], fl[i], "render") || frame_is(fr[i], fl[i], "preRender") ||
                  frame_is(fr[i], fl[i], "postRender");
    bool update = frame_is(fr[i], fl[i], "update");
    for (int k = i; k < n; k++) {
        if (render && (frame_is(fr[k], fl[k], "onBind") || frame_is(fr[k], fl[k], "onBatch") ||
                       frame_is(fr[k], fl[k], "batchQuad") || frame_is(fr[k], fl[k], "onPreRender")) &&
            !frame_in_ours(fr[k], fl[k]) && !has(fr[k], fl[k], "@native"))
            return G_PIPELINES;
        if (render && (frame_is(fr[k], fl[k], "textRender") || frame_is(fr[k], fl[k], "updateText") ||
                       frame_is(fr[k], fl[k], "TextWebGLRenderer")))
            return G_TEXT;
        if (update && frame_is(fr[k], fl[k], "tweenStep")) return G_TWEENS;
        if (update && frame_is(fr[k], fl[k], "fastSceneUpdate")) return G_ANIMS;
    }
    if (render) return has(fr[n - 1], fl[n - 1], "@native") ? G_RENDER_NATIVE : G_RENDER_JS;
    if (update) return G_UPDATE;
    return G_EVENTS;
}

static void report_groups(unsigned total) {
    unsigned g[G_COUNT] = {0};
    for (int i = 0; i < MAX_STACKS; i++)
        if (stacks[i].key) g[group_of(stacks[i].key)] += stacks[i].count;
    char line[600];
    size_t ll = 0;
    line[0] = 0;
    for (int k = 0; k < G_COUNT; k++)
        if (g[k] * 200 >= total) // 0.5% and up
            ll += snprintf(line + ll, sizeof line - ll, "%s%s %.0f%%", ll ? ", " : "", group_names[k], 100.0 * g[k] / total);
    host_log("[prof] groups: %s", line);
}

// <data>/profile.txt, written by a thread of its own: in small pieces to the memory card from the
// main loop, it was part of a 1-5 s stall every 30 s on the Vita.
static int profile_writing; // one at a time: skipped while the last one is still being written
static void *profile_thread(void *arg) {
    char path[600];
    snprintf(path, sizeof path, "%s/profile.txt", g_cfg.data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(arg, f);
        fclose(f);
    }
    free(arg);
    __atomic_store_n(&profile_writing, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void write_profile(unsigned total) {
    if (__atomic_load_n(&profile_writing, __ATOMIC_ACQUIRE)) return;
    size_t size = 200;
    for (int i = 0; i < MAX_STACKS; i++)
        if (stacks[i].key) size += strlen(stacks[i].key) + 12;
    char *buf = malloc(size), *w = buf;
    if (!buf) return;
    w += snprintf(w, size, "# %s: %u samples; collapsed stacks (frame;frame;... count)\n", host_version_string(), total);
    for (int i = 0; i < MAX_STACKS; i++)
        if (stacks[i].key) w += snprintf(w, size - (size_t)(w - buf), "%s %u\n", stacks[i].key, stacks[i].count);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 * 1024);
    pthread_t t;
    __atomic_store_n(&profile_writing, 1, __ATOMIC_RELEASE);
    if (pthread_create(&t, &attr, profile_thread, buf) == 0) pthread_detach(t);
    else free(buf), __atomic_store_n(&profile_writing, 0, __ATOMIC_RELEASE);
    pthread_attr_destroy(&attr);
}

static void report(void) {
    unsigned total = n_samples;
    if (!total) return;
    int cap = 2048, n = 0;
    fstat *fs = calloc(cap, sizeof *fs);
    int *index = malloc(FN_SLOTS * sizeof *index);
    if (!fs || !index) {
        free(fs), free(index);
        return;
    }
    for (int i = 0; i < FN_SLOTS; i++) index[i] = -1;
    for (int i = 0; i < MAX_STACKS; i++) {
        if (!stacks[i].key) continue;
        const char *k = stacks[i].key, *p = k;
        // Each function once per stack for "total" (marked with the stack's slot); the last frame
        // gets "self".
        for (;;) {
            const char *e = strchr(p, ';');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            fstat *f = find_fn(fs, &n, cap, index, p, len);
            if (f && f->stack != i + 1) f->total += stacks[i].count, f->stack = i + 1;
            if (!e) {
                if (f) f->self += stacks[i].count;
                break;
            }
            p = e + 1;
        }
    }
    char line[400];
    host_log("[prof] %u JS samples (%u stacks%s). Main loop:", total, n_stacks, n_dropped ? ", some dropped" : "");
    double tt = phase_total > 0 ? phase_total : 1;
    size_t ll = 0;
    line[0] = 0;
    for (int i = 0; i < MAX_PHASES && phase_names[i]; i++)
        if (phase_ms[i] >= tt / 200)
            ll += snprintf(line + ll, sizeof line - ll, "%s%s %.0f%%", ll ? ", " : "", phase_names[i], 100.0 * phase_ms[i] / tt);
    host_log("[prof]   %s", line);
    report_groups(total);
    qsort(fs, n, sizeof *fs, by_self);
    host_log("[prof] self (innermost JS function):");
    for (int i = 0; i < n && i < 20; i++)
        host_log("[prof]   %4.1f%%  %.*s", 100.0 * fs[i].self / total, (int)fs[i].len, fs[i].name);
    qsort(fs, n, sizeof *fs, by_total);
    host_log("[prof] total (on the stack):");
    for (int i = 0, shown = 0; i < n && shown < 15; i++) {
        if (fs[i].total * 100 >= total * 99) continue; // skip frames that are on every stack (main loop)
        host_log("[prof]   %4.1f%%  %.*s", 100.0 * fs[i].total / total, (int)fs[i].len, fs[i].name);
        shown++;
    }
    free(fs), free(index);
    write_profile(total);
}

// Called once per frame with its JS time: reports a slow frame at once, and everything every 30 s,
// then starts counting afresh.
void prof_frame(double js_ms) {
    if (!started) return;
    if (js_ms >= SLOW_FRAME_MS && n_fsamp) slow_report(js_ms);
    n_fsamp = 0;
    double now = host_now_ms();
    if (now - last_report < REPORT_MS) return;
    last_report = now;
    report();
    host_log("[prof] (this report took %.0f ms)", host_now_ms() - now);
    for (int i = 0; i < MAX_STACKS; i++) stacks[i].key = NULL;
    arena_used = 0;
    n_slow = 0;
    n_stacks = 0, n_samples = 0, n_dropped = 0, phase_total = 0;
    for (int i = 0; i < MAX_PHASES; i++) phase_ms[i] = 0;
}
