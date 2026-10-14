// SPDX-License-Identifier: AGPL-3.0-only
// QuickJS-ng host: runtime setup, ES module loading over HTTP, and the __native helper object.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

static JSRuntime *rt;
static JSContext *g_ctx;

// ---------------------------------------------------------------- URLs

static size_t origin_len(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    return p - url;
}

// Removes "." and ".." segments from the path part of `url` in place.
static void normalize_path(char *url) {
    size_t o = origin_len(url);
    char *path = url + o;
    char *q = strpbrk(path, "?#");
    char tail[2048] = "";
    if (q) {
        snprintf(tail, sizeof tail, "%s", q);
        *q = 0;
    }
    char out[2048];
    size_t n = 0;
    out[0] = 0;
    char *save, *seg;
    bool trailing = path[0] && path[strlen(path) - 1] == '/';
    for (seg = strtok_r(path, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
        if (!strcmp(seg, ".")) continue;
        if (!strcmp(seg, "..")) {
            char *s = strrchr(out, '/');
            n = s ? (size_t)(s - out) : 0;
            out[n] = 0;
            continue;
        }
        n += snprintf(out + n, sizeof out - n, "/%s", seg);
    }
    if (trailing || n == 0) n += snprintf(out + n, sizeof out - n, "/");
    snprintf(path, 2048, "%s%s", out, tail);
}

void url_resolve(const char *base, const char *url, char *out, size_t n) {
    if (strstr(url, "://") || !strncmp(url, "data:", 5) || !strncmp(url, "blob:", 5)) {
        snprintf(out, n, "%s", url);
        return;
    }
    if (!strncmp(url, "//", 2)) {
        const char *colon = strchr(base, ':');
        snprintf(out, n, "%.*s%s", (int)(colon ? colon - base + 1 : 0), base, url);
        return;
    }
    size_t o = origin_len(base);
    if (url[0] == '/') {
        snprintf(out, n, "%.*s%s", (int)o, base, url);
    } else if (url[0] == '?' || url[0] == '#') {
        const char *cut = strpbrk(base + o, url[0] == '?' ? "?#" : "#");
        snprintf(out, n, "%.*s%s", (int)(cut ? cut - base : (long)strlen(base)), base, url);
    } else {
        const char *path_end = strpbrk(base + o, "?#");
        size_t blen = path_end ? (size_t)(path_end - base) : strlen(base);
        const char *slash = NULL;
        for (const char *p = base + o; p < base + blen; p++)
            if (*p == '/') slash = p;
        size_t dir = slash ? (size_t)(slash - base + 1) : blen;
        if (!slash) snprintf(out, n, "%.*s/%s", (int)dir, base, url);
        else snprintf(out, n, "%.*s%s", (int)dir, base, url);
    }
    normalize_path(out);
}

// ---------------------------------------------------------------- exceptions

void host_report_exception(JSContext *ctx) {
    JSValue exc = JS_GetException(ctx);
    const char *msg = JS_ToCString(ctx, exc);
    JSValue stack = JS_IsObject(exc) ? JS_GetPropertyStr(ctx, exc, "stack") : JS_UNDEFINED;
    const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    host_log("[js] uncaught: %s\n%s", msg ? msg : "?", st ? st : "");
    JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, st);
    JS_FreeValue(ctx, stack);
    JS_FreeValue(ctx, exc);
}

static void promise_rejection(JSContext *ctx, JSValueConst promise, JSValueConst reason, bool is_handled,
                              void *opaque) {
    if (is_handled) return;
    const char *msg = JS_ToCString(ctx, reason);
    JSValue stack = JS_IsObject(reason) ? JS_GetPropertyStr(ctx, reason, "stack") : JS_UNDEFINED;
    const char *st = JS_IsUndefined(stack) ? NULL : JS_ToCString(ctx, stack);
    host_log("[js] unhandled rejection: %s\n%s", msg ? msg : "?", st ? st : "");
    JS_FreeCString(ctx, msg);
    JS_FreeCString(ctx, st);
    JS_FreeValue(ctx, stack);
}

void host_run_jobs(JSContext *ctx) {
    JSContext *c;
    int r;
    while ((r = JS_ExecutePendingJob(rt, &c)) != 0) {
        if (r < 0) host_report_exception(c);
    }
}

void host_call_rt(JSContext *ctx, const char *name, int argc, JSValueConst *argv) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue rtobj = JS_GetPropertyStr(ctx, global, "__rt");
    JSValue fn = JS_GetPropertyStr(ctx, rtobj, name);
    if (JS_IsFunction(ctx, fn)) {
        JSValue r = JS_Call(ctx, fn, rtobj, argc, argv);
        if (JS_IsException(r)) host_report_exception(ctx);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, rtobj);
    JS_FreeValue(ctx, global);
}

// ---------------------------------------------------------------- eval

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
        if (len) *len = n;
    }
    return buf;
}

int host_eval_file(JSContext *ctx, const char *path) {
    size_t len;
    char *src = read_file(path, &len);
    if (!src) {
        host_log("cannot read %s", path);
        return -1;
    }
    JSValue v = JS_Eval(ctx, src, len, path, JS_EVAL_TYPE_GLOBAL);
    free(src);
    if (JS_IsException(v)) {
        host_report_exception(ctx);
        return -1;
    }
    JS_FreeValue(ctx, v);
    host_run_jobs(ctx);
    return 0;
}

int host_eval_string(JSContext *ctx, const char *src, const char *name, char **result) {
    JSValue v = JS_Eval(ctx, src, strlen(src), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        host_report_exception(ctx);
        return -1;
    }
    if (result) {
        JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        const char *s = JS_IsException(json) || JS_IsUndefined(json) ? JS_ToCString(ctx, v) : JS_ToCString(ctx, json);
        *result = strdup(s ? s : "undefined");
        JS_FreeCString(ctx, s);
        JS_FreeValue(ctx, json);
    }
    JS_FreeValue(ctx, v);
    host_run_jobs(ctx);
    return 0;
}

// ---------------------------------------------------------------- modules

static char *module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    char out[2048];
    if (name[0] == '.' || name[0] == '/' || strstr(name, "://")) url_resolve(base && *base ? base : g_cfg.base_url, name, out, sizeof out);
    else snprintf(out, sizeof out, "%s", name);
    return js_strdup(ctx, out);
}

static JSModuleDef *module_load(JSContext *ctx, const char *url, void *opaque) {
    size_t len = 0;
    long status = 0;
    double t0 = host_now_ms();
    const char *base = strrchr(url, '/');
    char msg[600];
    snprintf(msg, sizeof msg, "PokeRogue\n\nDownloading game code\n%s", base ? base + 1 : url);
    overlay_present(msg);
    char *src = net_get_sync(url, &len, &status);
    if (!src || status != 200) {
        JS_ThrowReferenceError(ctx, "could not load module %s (HTTP %ld)", url, status);
        free(src);
        return NULL;
    }
    double t1 = host_now_ms();
    snprintf(msg, sizeof msg, "PokeRogue\n\nPreparing game code\n%s (%lu KB)\n\nThe first start takes a while.",
             base ? base + 1 : url, (unsigned long)(len / 1024));
    overlay_present(msg);
    JSValue fn = JS_Eval(ctx, src, len, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    free(src);
    if (JS_IsException(fn)) return NULL;
    JSModuleDef *m = JS_VALUE_GET_PTR(fn);
    JSValue meta = JS_GetImportMeta(ctx, m);
    JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
    JS_FreeValue(ctx, meta);
    host_log("[module] %s (%lu KB) fetched %.0f ms, compiled %.0f ms", url, (unsigned long)(len / 1024), t1 - t0,
             host_now_ms() - t1);
    if (len > 256 * 1024) mem_breakdown("after compiling");
    JS_FreeValue(ctx, fn);
    return m;
}

// ---------------------------------------------------------------- __native helpers

static JSValue n_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *s = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    host_log("%s", s ? s : "");
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue n_now(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, host_now_ms());
}

// readFile(relPath) -> string | null, relative to the data dir.
static JSValue n_read_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *rel = JS_ToCString(ctx, argv[0]);
    if (!rel || strstr(rel, "..")) {
        JS_FreeCString(ctx, rel);
        return JS_NULL;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_cfg.data_dir, rel);
    JS_FreeCString(ctx, rel);
    size_t len;
    char *s = read_file(path, &len);
    if (!s) return JS_NULL;
    JSValue v = JS_NewStringLen(ctx, s, len);
    free(s);
    return v;
}

// writeFile(relPath, string) -> bool. Writes to a temp file and renames, so a crash never leaves half a file.
static JSValue n_write_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *rel = JS_ToCString(ctx, argv[0]);
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
    bool ok = false;
    if (rel && s && !strstr(rel, "..")) {
        char path[1024], tmp[1100];
        snprintf(path, sizeof path, "%s/%s", g_cfg.data_dir, rel);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            ok = fwrite(s, 1, len, f) == len;
            ok &= fclose(f) == 0;
            if (ok) ok = rename(tmp, path) == 0;
        }
    }
    JS_FreeCString(ctx, rel);
    JS_FreeCString(ctx, s);
    return JS_NewBool(ctx, ok);
}

static JSValue n_gamepads(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    plat_gamepad pads[4];
    int n = plat_gamepads(pads, 4);
    JSValue arr = JS_NewArray(ctx);
    for (int i = 0; i < n; i++) {
        JSValue p = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, p, "id", JS_NewString(ctx, pads[i].id ? pads[i].id : "gamepad"));
        JSValue axes = JS_NewArray(ctx), buttons = JS_NewArray(ctx);
        for (int a = 0; a < 4; a++) JS_SetPropertyUint32(ctx, axes, a, JS_NewFloat64(ctx, pads[i].axes[a]));
        for (int b = 0; b < 18; b++) JS_SetPropertyUint32(ctx, buttons, b, JS_NewFloat64(ctx, pads[i].buttons[b]));
        JS_SetPropertyStr(ctx, p, "axes", axes);
        JS_SetPropertyStr(ctx, p, "buttons", buttons);
        JS_SetPropertyUint32(ctx, arr, i, p);
    }
    return arr;
}

static JSValue n_resolve_url(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *base = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    char out[4096];
    url_resolve(base ? base : "", url ? url : "", out, sizeof out);
    JS_FreeCString(ctx, base);
    JS_FreeCString(ctx, url);
    return JS_NewString(ctx, out);
}

// importModule(url) -> Promise: dynamic import from the bootstrap (a classic script).
static JSValue n_import(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url) return JS_EXCEPTION;
    JSValue p = JS_LoadModule(ctx, g_cfg.base_url, url);
    JS_FreeCString(ctx, url);
    return p;
}

// loadScript(name): evaluate one of the runtime's own JS files (from the js dir) as a classic script.
static JSValue n_load_script(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_EXCEPTION;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_cfg.js_dir, name);
    JS_FreeCString(ctx, name);
    size_t len;
    char *src = read_file(path, &len);
    if (!src) return JS_ThrowReferenceError(ctx, "loadScript: cannot read %s", path);
    JSValue v = JS_Eval(ctx, src, len, path, JS_EVAL_TYPE_GLOBAL);
    free(src);
    return v;
}

// utf8Decode(ArrayBuffer|TypedArray[, start, end]) -> string
static JSValue n_utf8_decode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off, tlen, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &tlen, &bpe);
        if (JS_IsException(ab)) return JS_EXCEPTION;
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p) return JS_NewString(ctx, "");
        p += off;
        len = tlen;
    }
    // Skip a UTF-8 BOM, like TextDecoder does.
    if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
        len -= 3;
    }
    return JS_NewStringLen(ctx, (const char *)p, len);
}

// utf8Encode(string) -> ArrayBuffer
static JSValue n_utf8_encode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)s, len);
    JS_FreeCString(ctx, s);
    return ab;
}

// imeOpen(title, initialText, maxLength, isPassword) -> bool
static JSValue n_ime_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *title = JS_ToCString(ctx, argv[0]), *init = JS_ToCString(ctx, argv[1]);
    int32_t max_len = 0;
    JS_ToInt32(ctx, &max_len, argv[2]);
    bool ok = plat_ime_open(title ? title : "", init ? init : "", max_len, JS_ToBool(ctx, argv[3]));
    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, init);
    return JS_NewBool(ctx, ok);
}

// imePoll() -> undefined while open, {ok: true, text} when confirmed, {ok: false} when cancelled
static JSValue n_ime_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char text[4096];
    int r = plat_ime_poll(text, sizeof text);
    if (r == 0) return JS_UNDEFINED;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "ok", JS_NewBool(ctx, r > 0));
    if (r > 0) JS_SetPropertyStr(ctx, o, "text", JS_NewString(ctx, text));
    return o;
}

// memoryUsage() -> QuickJS's own accounting, in bytes and counts.
static JSValue n_memory_usage(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSMemoryUsage m;
    JS_ComputeMemoryUsage(JS_GetRuntime(ctx), &m);
    JSValue o = JS_NewObject(ctx);
#define M(f) JS_SetPropertyStr(ctx, o, #f, JS_NewFloat64(ctx, (double)m.f))
    M(malloc_size); M(malloc_count); M(memory_used_size); M(atom_count); M(atom_size); M(str_count); M(str_size);
    M(obj_count); M(obj_size); M(prop_count); M(prop_size); M(shape_count); M(shape_size); M(js_func_count);
    M(js_func_size); M(js_func_code_size); M(js_func_pc2line_size); M(array_count); M(fast_array_count);
    M(fast_array_elements); M(binary_object_count); M(binary_object_size);
#undef M
    return o;
}

// showMessage(text | null): full-screen status message (only before the game takes over the screen)
static JSValue n_show_message(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *msg = JS_IsNull(argv[0]) || JS_IsUndefined(argv[0]) ? NULL : JS_ToCString(ctx, argv[0]);
    overlay_set(msg);
    JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

// randomBytes(n) -> ArrayBuffer of cryptographically secure random bytes
static JSValue n_random_bytes(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint32_t n = 0;
    JS_ToUint32(ctx, &n, argv[0]);
    if (n > 65536) return JS_ThrowRangeError(ctx, "randomBytes: at most 65536 bytes");
    uint8_t *buf = malloc(n ? n : 1);
    bool ok = host_random_bytes(buf, n);
    JSValue ab = ok ? JS_NewArrayBufferCopy(ctx, buf, n) : JS_ThrowInternalError(ctx, "no random source");
    free(buf);
    return ab;
}

static const JSCFunctionListEntry native_funcs[] = {
    JS_CFUNC_DEF("randomBytes", 1, n_random_bytes),
    JS_CFUNC_DEF("showMessage", 1, n_show_message),
    JS_CFUNC_DEF("memoryUsage", 0, n_memory_usage),
    JS_CFUNC_DEF("imeOpen", 4, n_ime_open),
    JS_CFUNC_DEF("imePoll", 0, n_ime_poll),
    JS_CFUNC_DEF("loadScript", 1, n_load_script),
    JS_CFUNC_DEF("utf8Decode", 1, n_utf8_decode),
    JS_CFUNC_DEF("utf8Encode", 1, n_utf8_encode),
    JS_CFUNC_DEF("log", 1, n_log),
    JS_CFUNC_DEF("now", 0, n_now),
    JS_CFUNC_DEF("readFile", 1, n_read_file),
    JS_CFUNC_DEF("writeFile", 2, n_write_file),
    JS_CFUNC_DEF("gamepads", 0, n_gamepads),
    JS_CFUNC_DEF("resolveURL", 2, n_resolve_url),
    JS_CFUNC_DEF("importModule", 1, n_import),
};

void host_js_set_interrupt(JSInterruptHandler *cb) { JS_SetInterruptHandler(rt, cb, NULL); }

JSContext *host_js_init(void) {
    rt = JS_NewRuntime();
    JS_SetMaxStackSize(rt, 4 * 1024 * 1024);
    JS_UpdateStackTop(rt);
    JS_SetModuleLoaderFunc(rt, module_normalize, module_load, NULL);
    JS_SetHostPromiseRejectionTracker(rt, promise_rejection, NULL);
    JSContext *ctx = JS_NewContext(rt);
    g_ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue native = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, native, native_funcs, sizeof native_funcs / sizeof *native_funcs);
    JS_SetPropertyStr(ctx, native, "baseURL", JS_NewString(ctx, g_cfg.base_url));
    JS_SetPropertyStr(ctx, native, "jsDir", JS_NewString(ctx, g_cfg.js_dir));
    JS_SetPropertyStr(ctx, native, "screenWidth", JS_NewInt32(ctx, g_cfg.win_w));
    JS_SetPropertyStr(ctx, native, "screenHeight", JS_NewInt32(ctx, g_cfg.win_h));
    net_register(ctx, native);
    gl_register(ctx, native);
    canvas_register(ctx, native);
    fastpath_register(ctx, native);
    JS_SetPropertyStr(ctx, global, "__native", native);
    JS_FreeValue(ctx, global);
    return ctx;
}

void host_js_shutdown(void) {
    // Freeing a runtime that still holds the game's object graph is slow and pointless at exit.
    g_ctx = NULL;
}
