// SPDX-License-Identifier: AGPL-3.0-only
// Built-in sampling profiler, so that logs from the Vita show where the time goes. It samples
// while the debug line is on (SELECT while holding L+R, or hud=1 in config.txt).
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

// Time spent in one step of the main loop (called by the loop when it moves to the next step).
void prof_phase(const char *name, double ms) {
    if (!started) return;
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

static void count_stack(const char *key) {
    unsigned h = hash(key) % MAX_STACKS;
    for (int probe = 0; probe < MAX_STACKS; probe++, h = (h + 1) % MAX_STACKS) {
        if (!stacks[h].key) {
            if (n_stacks >= MAX_STACKS * 3 / 4 || !(stacks[h].key = arena_strdup(key))) { n_dropped++; return; }
            stacks[h].count = 1;
            n_stacks++;
            return;
        }
        if (!strcmp(stacks[h].key, key)) { stacks[h].count++; return; }
    }
}

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
    if (!g_cfg.hud) return;
    static char buf[4096];
    if (JS_SampleStack(ctx, buf, sizeof buf) > 0 && buf[0]) {
        shorten(buf);
        count_stack(buf);
        n_samples++;
    }
}

// ---------------------------------------------------------------- report

typedef struct { const char *name; size_t len; unsigned self, total; } fstat;

static int by_self(const void *a, const void *b) { return (int)((const fstat *)b)->self - (int)((const fstat *)a)->self; }
static int by_total(const void *a, const void *b) { return (int)((const fstat *)b)->total - (int)((const fstat *)a)->total; }

static fstat *find_fn(fstat *fs, int *n, int cap, const char *name, size_t len) {
    for (int i = 0; i < *n; i++)
        if (fs[i].len == len && !memcmp(fs[i].name, name, len)) return &fs[i];
    if (*n >= cap) return NULL;
    fs[*n] = (fstat){name, len, 0, 0};
    return &fs[(*n)++];
}

static void report(void) {
    unsigned total = n_samples;
    if (!total) return;
    int cap = 2048, n = 0;
    fstat *fs = calloc(cap, sizeof *fs);
    if (!fs) return;
    for (int i = 0; i < MAX_STACKS; i++) {
        if (!stacks[i].key) continue;
        const char *k = stacks[i].key, *p = k;
        // Each function once per stack for "total"; the last frame gets "self".
        const char *seen[64];
        size_t seen_len[64];
        int ns = 0;
        for (;;) {
            const char *e = strchr(p, ';');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            bool dup = false;
            for (int j = 0; j < ns; j++) dup |= seen_len[j] == len && !memcmp(seen[j], p, len);
            fstat *f = find_fn(fs, &n, cap, p, len);
            if (f && !dup) f->total += stacks[i].count;
            if (ns < 64) seen[ns] = p, seen_len[ns++] = len;
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
    free(fs);

    char path[600];
    snprintf(path, sizeof path, "%s/profile.txt", g_cfg.data_dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# %s: %u samples; collapsed stacks (frame;frame;... count)\n", host_version_string(), total);
        for (int i = 0; i < MAX_STACKS; i++)
            if (stacks[i].key) fprintf(f, "%s %u\n", stacks[i].key, stacks[i].count);
        fclose(f);
    }
}

// Called once per frame: reports every 30 s, then starts counting afresh.
void prof_frame(void) {
    if (!started) return;
    double now = host_now_ms();
    if (now - last_report < REPORT_MS) return;
    last_report = now;
    report();
    for (int i = 0; i < MAX_STACKS; i++) stacks[i].key = NULL;
    arena_used = 0;
    n_stacks = 0, n_samples = 0, n_dropped = 0, phase_total = 0;
    for (int i = 0; i < MAX_PHASES; i++) phase_ms[i] = 0;
}
