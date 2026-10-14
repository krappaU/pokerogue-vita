// SPDX-License-Identifier: AGPL-3.0-only
// Phase 0 feasibility benchmark for running PokeRogue on PS Vita via QuickJS-ng.
//
// Test 1: compile the real, current pokerogue.net startup chunks (about 6 MB of JS), then
//         serialize them to bytecode and load the bytecode back (the planned cache path).
// Test 2: boot Phaser 3.90 on a stubbed WebGL context and time frames of a synthetic
//         battle scene (see js/bench.js). GL calls are no-ops, so this is the JS-only cost.
//
// Builds for Linux (for comparison numbers) and for Vita (the numbers that matter).
// Output goes to stdout/screen and to a log file.

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "quickjs.h"

void fastpath_register(JSContext *ctx);

#ifdef __vita__
#include <psp2/ctrl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/io/stat.h>
#include "debugScreen.h"
#define DATA_DIR "app0:data/"
#define LOG_DIR "ux0:data/pokerogue-bench"
#define LOG_PATH LOG_DIR "/log.txt"
// Defaults are far too small for parsing 6 MB of minified JS.
int _newlib_heap_size_user = 192 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;
#define JS_STACK_SIZE (7 * 1024 * 1024)
#else
#define DATA_DIR "data/"
#define LOG_PATH "bench-log.txt"
#define JS_STACK_SIZE (7 * 1024 * 1024)
#endif

#define WARMUP_FRAMES 60
#define MEASURE_FRAMES 300

static FILE *log_fp;

static void logf_(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __vita__
    psvDebugScreenPrintf("%s", buf);
#endif
    fputs(buf, stdout);
    fflush(stdout);
    if (log_fp) {
        fputs(buf, log_fp);
        fflush(log_fp);
    }
}

static double now_ms(void) {
#ifdef __vita__
    return sceKernelGetProcessTimeWide() / 1000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

static double heap_mb(void) {
    struct mallinfo mi = mallinfo();
    return (double)(unsigned)mi.uordblks / (1024.0 * 1024.0);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(n + 1);
    if (buf && fread(buf, 1, n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = n;
    }
    return buf;
}

static void dump_exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *s = JS_ToCString(ctx, exc);
    logf_("  EXCEPTION: %s\n", s ? s : "?");
    JS_FreeCString(ctx, s);
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (!JS_IsUndefined(stack)) {
        const char *st = JS_ToCString(ctx, stack);
        if (st) logf_("%s\n", st);
        JS_FreeCString(ctx, st);
    }
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

static void run_jobs(JSRuntime *rt) {
    JSContext *c;
    while (JS_ExecutePendingJob(rt, &c) > 0) {}
}

// ---------------------------------------------------------------- test 1

// Compile-only still resolves static imports. Hand back an empty stub module so each chunk
// is measured on its own (linking/evaluation is not part of this test).
static int stub_init(JSContext *ctx, JSModuleDef *m) { return 0; }

static JSModuleDef *stub_loader(JSContext *ctx, const char *name, void *opaque) {
    return JS_NewCModule(ctx, name, stub_init);
}

static void test_compile(JSRuntime *rt) {
    logf_("\n== Test 1: compile live PokeRogue chunks ==\n");
    JSContext *ctx = JS_NewContext(rt);
    JS_SetModuleLoaderFunc(rt, NULL, stub_loader, NULL);
    size_t listlen;
    char *list = read_file(DATA_DIR "pokerogue/files.txt", &listlen);
    if (!list) {
        logf_("  missing files.txt\n");
        JS_FreeContext(ctx);
        return;
    }
    double heap0 = heap_mb();
    double t_parse = 0, t_write = 0, t_read = 0;
    size_t total_src = 0, total_bc = 0;
    char *save;
    for (char *name = strtok_r(list, "\r\n", &save); name; name = strtok_r(NULL, "\r\n", &save)) {
        char path[256];
        snprintf(path, sizeof path, DATA_DIR "pokerogue/%s", name);
        size_t len;
        char *src = read_file(path, &len);
        if (!src) {
            logf_("  cannot read %s\n", path);
            continue;
        }
        double t0 = now_ms();
        JSValue mod = JS_Eval(ctx, src, len, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        double t1 = now_ms();
        free(src);
        if (JS_IsException(mod)) {
            logf_("  %s: compile failed\n", name);
            dump_exception(ctx);
            continue;
        }
        size_t bclen = 0;
        uint8_t *bc = JS_WriteObject(ctx, &bclen, mod, JS_WRITE_OBJ_BYTECODE);
        double t2 = now_ms();
        JS_FreeValue(ctx, mod);
        double t3 = t2;
        if (bc) {
            JSValue back = JS_ReadObject(ctx, bc, bclen, JS_READ_OBJ_BYTECODE);
            t3 = now_ms();
            if (JS_IsException(back)) dump_exception(ctx);
            JS_FreeValue(ctx, back);
            js_free(ctx, bc);
        }
        logf_("  %-34s %7lu KB  parse %7.0f ms  bc %6lu KB  bc-load %6.0f ms\n", name, (unsigned long)(len / 1024),
              t1 - t0, (unsigned long)(bclen / 1024), t3 - t2);
        t_parse += t1 - t0;
        t_write += t2 - t1;
        t_read += t3 - t2;
        total_src += len;
        total_bc += bclen;
    }
    free(list);
    logf_("  TOTAL %lu KB source: parse %.0f ms, bytecode %lu KB, bytecode load %.0f ms (write %.0f ms)\n",
          (unsigned long)(total_src / 1024), t_parse, (unsigned long)(total_bc / 1024), t_read, t_write);
    logf_("  heap growth while compiling: %.1f MB\n", heap_mb() - heap0);
    JS_FreeContext(ctx);
    JS_SetModuleLoaderFunc(rt, NULL, NULL, NULL);
    JS_RunGC(rt);
}

// ---------------------------------------------------------------- test 2

static JSValue js_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    logf_("  [js] %s\n", s ? s : "");
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, now_ms());
}

static int eval_file(JSContext *ctx, const char *rel) {
    char path[256];
    snprintf(path, sizeof path, DATA_DIR "%s", rel);
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        logf_("  cannot read %s\n", path);
        return -1;
    }
    double t0 = now_ms();
    JSValue v = JS_Eval(ctx, src, len, rel, JS_EVAL_TYPE_GLOBAL);
    double t1 = now_ms();
    free(src);
    run_jobs(JS_GetRuntime(ctx));
    if (JS_IsException(v)) {
        logf_("  %s: failed\n", rel);
        dump_exception(ctx);
        return -1;
    }
    JS_FreeValue(ctx, v);
    logf_("  loaded %-16s %6lu KB in %6.0f ms\n", rel, (unsigned long)(len / 1024), t1 - t0);
    return 0;
}

static double eval_num(JSContext *ctx, const char *code) {
    JSValue v = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    double d = -1;
    if (JS_IsException(v)) dump_exception(ctx);
    else JS_ToFloat64(ctx, &d, v);
    JS_FreeValue(ctx, v);
    return d;
}

static void eval_print(JSContext *ctx, const char *code) {
    JSValue v = JS_Eval(ctx, code, strlen(code), "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        dump_exception(ctx);
        return;
    }
    const char *s = JS_ToCString(ctx, v);
    if (s && *s) logf_("%s\n", s);
    JS_FreeCString(ctx, s);
    JS_FreeValue(ctx, v);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double sim_time = 1000.0;

static int tick(JSContext *ctx, JSValueConst tickfn) {
    sim_time += 1000.0 / 60.0;
    JSValue arg = JS_NewFloat64(ctx, sim_time);
    JSValue r = JS_Call(ctx, tickfn, JS_UNDEFINED, 1, &arg);
    if (JS_IsException(r)) {
        dump_exception(ctx);
        return -1;
    }
    JS_FreeValue(ctx, r);
    run_jobs(JS_GetRuntime(ctx));
    return 0;
}

static void test_phaser(JSRuntime *rt) {
    logf_("\n== Test 2: Phaser 3.90 synthetic battle scene (GL stubbed) ==\n");
    JSContext *ctx = JS_NewContext(rt);
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__log", JS_NewCFunction(ctx, js_log, "__log", 1));
    JS_SetPropertyStr(ctx, global, "__now", JS_NewCFunction(ctx, js_now, "__now", 0));
    double heap0 = heap_mb();

    fastpath_register(ctx);
    if (eval_file(ctx, "js/shim.js") || eval_file(ctx, "phaser.min.js") || eval_file(ctx, "js/bench.js") ||
        eval_file(ctx, "js/fastpath.js"))
        goto out;
    logf_("  heap after load: +%.1f MB\n", heap_mb() - heap0);

    JSValue tickfn = JS_GetPropertyStr(ctx, global, "__tick");
    double tb = now_ms();
    int i;
    for (i = 0; i < 600; i++) {
        if (tick(ctx, tickfn)) break;
        if (eval_num(ctx, "__bench.ready ? 1 : (__bench.error ? -1 : 0)") != 0) break;
    }
    eval_print(ctx, "__bench.error || ''");
    if (eval_num(ctx, "__bench.ready ? 1 : 0") != 1) {
        logf_("  Phaser did not become ready after %d ticks\n", i);
        eval_print(ctx, "'missing: ' + [...__missing].join(', ')");
        JS_FreeValue(ctx, tickfn);
        goto out;
    }
    logf_("  Phaser booted in %.0f ms (%d ticks), heap +%.1f MB\n", now_ms() - tb, i + 1, heap_mb() - heap0);

    eval_print(ctx, "__bench.verifyFastpath()");

    // Each scenario runs with Phaser's own JS batching, then with the native fast path.
    static const char *scenarios[] = {"idle", "battle", "heavy"};
    static const char *modes[] = {"js", "native"};
    static double ft[MEASURE_FRAMES];
    for (size_t s = 0; s < sizeof scenarios / sizeof *scenarios; s++) {
        char code[128];
        snprintf(code, sizeof code, "__bench.start('%s'), __bench.objectCount()", scenarios[s]);
        int objs = (int)eval_num(ctx, code);
        for (int m = 0; m < 2; m++) {
            snprintf(code, sizeof code, "__fastpath.set(%d), 0", m);
            eval_num(ctx, code);
            JS_RunGC(rt);
            for (int f = 0; f < WARMUP_FRAMES; f++)
                if (tick(ctx, tickfn)) goto done;
            double calls0 = eval_num(ctx, "__bench.glCalls()");
            double sum = 0;
            for (int f = 0; f < MEASURE_FRAMES; f++) {
                double t0 = now_ms();
                if (tick(ctx, tickfn)) goto done;
                ft[f] = now_ms() - t0;
                sum += ft[f];
            }
            double calls = (eval_num(ctx, "__bench.glCalls()") - calls0) / MEASURE_FRAMES;
            qsort(ft, MEASURE_FRAMES, sizeof *ft, cmp_double);
            double avg = sum / MEASURE_FRAMES;
            logf_("  %-7s %-6s %4d objs  avg %6.2f ms  p50 %6.2f  p95 %6.2f  max %6.2f  (~%.0f fps)  gl %.0f/frame\n",
                  scenarios[s], modes[m], objs, avg, ft[MEASURE_FRAMES / 2], ft[MEASURE_FRAMES * 95 / 100],
                  ft[MEASURE_FRAMES - 1], 1000.0 / avg, calls);
        }
    }

    for (int m = 0; m < 2; m++) {
        char code[64];
        snprintf(code, sizeof code, "__fastpath.set(%d), 0", m);
        eval_num(ctx, code);
        logf_("\n  breakdown (heavy scene, %s batching, profiled):\n", modes[m]);
        eval_print(ctx, "__bench.enableProfile(), ''");
        for (int f = 0; f < 120; f++)
            if (tick(ctx, tickfn)) goto done;
        eval_print(ctx, "__bench.profileReport(120)");
    }
done:
    logf_("  heap at end: +%.1f MB\n", heap_mb() - heap0);
    eval_print(ctx, "__missing.size ? 'APIs touched but not shimmed: ' + [...__missing].join(', ') : ''");
    JS_FreeValue(ctx, tickfn);
out:
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
}

int main(int argc, char **argv) {
#ifdef __vita__
    psvDebugScreenInit();
    sceIoMkdir(LOG_DIR, 0777);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
#endif
    log_fp = fopen(LOG_PATH, "w");
    logf_("PokeRogue Vita - Phase 0 benchmark (QuickJS-ng %s)\n", JS_GetVersion());
#ifdef __vita__
    logf_("CPU %d MHz, bus %d MHz\n", scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency());
#endif
    double t_start = now_ms();

    JSRuntime *rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, JS_STACK_SIZE);
    JS_UpdateStackTop(rt);

    test_compile(rt);
    test_phaser(rt);

    JS_FreeRuntime(rt);
    logf_("\nAll done in %.1f s. Log: %s\n", (now_ms() - t_start) / 1000.0, LOG_PATH);
    if (log_fp) fclose(log_fp);

#ifdef __vita__
    psvDebugScreenPrintf("\nPress START to exit.\n");
    SceCtrlData pad;
    do {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        sceKernelDelayThread(50 * 1000);
    } while (!(pad.buttons & SCE_CTRL_START));
    sceKernelExitProcess(0);
#endif
    return 0;
}
