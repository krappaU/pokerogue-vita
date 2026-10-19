// SPDX-License-Identifier: AGPL-3.0-only
// Compiles the game's module graph on all the cores before it is imported. The first start after a
// game update (new chunk names) used to fetch and compile the ~6 MB of chunks one after the other on
// the main thread, ~30 s on the Vita with two of its three cores idle. Here worker threads, each
// with its own QuickJS runtime, fetch and compile chunks as they find them in the imports of the
// chunks before; the main thread only shows progress. The results go to the compiled-code cache
// (js_host.c) and, as bytes, straight to the loader, so the import that follows is a deserialize.
// Anything that goes wrong here is ignored: the normal loader then does that chunk itself.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#endif

#include "host.h"

#define MAX_JOBS 64
#define WORKERS 3

typedef struct {
    char url[800];
} job;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    job jobs[MAX_JOBS];
    int njobs, next; // jobs[next .. njobs) are waiting
    int active;      // jobs being fetched or compiled
    int compiled, skipped;
    int alive; // workers still running
} P = {.lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER};

static void pause_ms(int ms) {
#ifdef __vita__
    sceKernelDelayThread(ms * 1000);
#else
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
#endif
}

static void enqueue(const char *url) {
    if (!module_chunk_url(url) || net_is_local(url)) return;
    char path[700];
    module_bytecode_path(url, path, sizeof path);
    struct stat st;
    pthread_mutex_lock(&P.lock);
    bool known = stat(path, &st) == 0;
    for (int i = 0; i < P.njobs && !known; i++) known = !strcmp(P.jobs[i].url, url);
    if (!known && P.njobs < MAX_JOBS) {
        host_log("[precompile] + %s", url);
        snprintf(P.jobs[P.njobs++].url, sizeof P.jobs[0].url, "%s", url);
        pthread_cond_broadcast(&P.cond);
    }
    pthread_mutex_unlock(&P.lock);
}

// Static and dynamic imports of a minified module: from"./x.js", import"./x.js", import(`./x.js`).
static void scan_imports(const char *src, size_t len, const char *base_url) {
    for (size_t i = 1; i + 6 < len; i++) {
        char q = src[i];
        if ((q != '"' && q != '\'' && q != '`') || src[i + 1] != '.' || src[i + 2] != '/') continue;
        size_t p = i;
        while (p > 0 && src[p - 1] == ' ') p--;
        if (p > 0 && src[p - 1] == '(') {
            p--;
            while (p > 0 && src[p - 1] == ' ') p--;
            if (p < 6 || strncmp(src + p - 6, "import", 6)) continue;
        } else if (!((p >= 4 && !strncmp(src + p - 4, "from", 4)) || (p >= 6 && !strncmp(src + p - 6, "import", 6)))) {
            continue;
        }
        size_t e = i + 1;
        while (e < len && e - i < 300 && src[e] != q && src[e] != '\n') e++;
        if (e >= len || src[e] != q || e - i < 6 || strncmp(src + e - 3, ".js", 3)) continue;
        char rel[320], abs[900];
        snprintf(rel, sizeof rel, "%.*s", (int)(e - i - 1), src + i + 1);
        url_resolve(base_url, rel, abs, sizeof abs);
        enqueue(abs);
    }
}

// Imports are checked at link time, not when compiling, so empty modules stand in for them.
static JSModuleDef *stub_loader(JSContext *ctx, const char *name, void *opaque) {
    (void)opaque;
    return JS_NewCModule(ctx, name, NULL);
}

static void compile_one(const char *url) {
    size_t len = 0;
    long status = 0;
    char *src = NULL;
    for (int attempt = 0; attempt < 2 && !src; attempt++) src = net_get_threadsafe(url, &len, &status);
    if (!src || status != 200) {
        free(src);
        host_log("[precompile] could not fetch %s (HTTP %ld)", url, status);
        return;
    }
    host_log("[precompile] %s (%lu KB)", url, (unsigned long)(len / 1024));
    scan_imports(src, len, url);
    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_SetModuleLoaderFunc(rt, NULL, stub_loader, NULL);
    JSContext *ctx = JS_NewContext(rt);
    JSValue fn = JS_Eval(ctx, src, len, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    int flags = bytecode_write_flags(src, len);
    free(src);
    size_t blen = 0;
    uint8_t *bc = JS_IsException(fn) ? NULL : JS_WriteObject(ctx, &blen, fn, flags);
    if (!bc) {
        JSValue exc = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, exc);
        host_log("[precompile] could not compile %s: %s", url, m ? m : "?");
        JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, fn);
    char path[700];
    module_bytecode_path(url, path, sizeof path);
    char *copy = bc ? malloc(blen) : NULL;
    if (copy) {
        memcpy(copy, bc, blen);
        module_bytecode_put(path, copy, blen); // the loader takes it from memory
    }
    // The main thread goes on once every chunk is compiled; the file is written meanwhile.
    pthread_mutex_lock(&P.lock);
    P.compiled += bc != NULL;
    P.active--;
    pthread_cond_broadcast(&P.cond);
    pthread_mutex_unlock(&P.lock);
    if (bc) module_bytecode_write(path, bc, blen);
    if (bc) js_free(ctx, bc);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
}

static void *worker(void *arg) {
    plat_thread_setup("pkr_compile", (int)(intptr_t)arg, 10); // below the main thread, which draws the progress
    for (;;) {
        pthread_mutex_lock(&P.lock);
        while (P.next >= P.njobs && P.active > 0) pthread_cond_wait(&P.cond, &P.lock);
        if (P.next >= P.njobs) { // nothing waiting and nobody who could add more
            P.alive--;
            pthread_cond_broadcast(&P.cond);
            pthread_mutex_unlock(&P.lock);
            return NULL;
        }
        char url[800];
        snprintf(url, sizeof url, "%s", P.jobs[P.next++].url);
        P.active++;
        pthread_mutex_unlock(&P.lock);
        compile_one(url);
    }
}

void precompile_graph(const char *entry_url) {
    if (!module_chunk_url(entry_url) || net_is_local(entry_url)) return;
    enqueue(entry_url);
    if (!P.njobs) return; // compiled already: the normal start
    double t0 = host_now_ms();
    host_log("[precompile] %s has no compiled code yet: compiling the module graph on %d threads", entry_url, WORKERS);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 6 * 1024 * 1024); // the parser recurses; the runtimes allow 4 MB
    pthread_mutex_lock(&P.lock);
    for (intptr_t i = 0; i < WORKERS; i++) {
        pthread_t t;
        if (pthread_create(&t, &attr, worker, (void *)i) == 0) {
            pthread_detach(t);
            P.alive++;
        }
    }
    pthread_mutex_unlock(&P.lock);
    pthread_attr_destroy(&attr);
    double shown = 0;
    pthread_mutex_lock(&P.lock);
    while (P.alive > 0 && (P.next < P.njobs || P.active > 0)) {
        if (host_now_ms() - shown > 250) {
            char msg[300];
            snprintf(msg, sizeof msg,
                     "PokeRogue\n\nPreparing game code\n%d of %d modules ready\n\nThe first start after an update takes a while.",
                     P.compiled, P.njobs);
            pthread_mutex_unlock(&P.lock);
            overlay_present(msg);
            pthread_mutex_lock(&P.lock);
            shown = host_now_ms();
        }
        pthread_mutex_unlock(&P.lock);
        pause_ms(50);
        pthread_mutex_lock(&P.lock);
    }
    int compiled = P.compiled, total = P.njobs;
    pthread_mutex_unlock(&P.lock);
    host_log("[precompile] %d of %d modules compiled in %.1f s", compiled, total, (host_now_ms() - t0) / 1000.0);
}
