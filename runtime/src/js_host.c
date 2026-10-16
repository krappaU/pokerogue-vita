// SPDX-License-Identifier: AGPL-3.0-only
// QuickJS-ng host: runtime setup, ES module loading over HTTP, and the __native helper object.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

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

// ---------------------------------------------------------------- compiled-code cache
// Compiling the game's ~6 MB of JS takes ~25 s on the Vita. The compiled bytecode is kept in
// <data>/bytecode/, keyed by a hash of the URL, the source and the engine version, so a game
// update (new source) or a runtime update with a different QuickJS just compiles afresh.
// Entries older than 14 days are deleted at startup.
#define BYTECODE_SALT "pkr-bytecode-1"

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

static void bytecode_path(const char *url, const char *src, size_t len, char *out, size_t n) {
    uint64_t h = 14695981039346656037ull;
    const char *v = JS_GetVersion();
    h = fnv64(h, BYTECODE_SALT, strlen(BYTECODE_SALT));
    h = fnv64(h, v, strlen(v));
    h = fnv64(h, url, strlen(url) + 1);
    h = fnv64(h, src, len);
    snprintf(out, n, "%s/bytecode/%016llx.qbc", g_cfg.data_dir, (unsigned long long)h);
}

static void bytecode_cleanup(void) {
    char dir[600];
    snprintf(dir, sizeof dir, "%s/bytecode", g_cfg.data_dir);
    DIR *d = opendir(dir);
    if (!d) return;
    time_t now = time(NULL);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".qbc")) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && now - st.st_mtime > 14 * 24 * 3600) remove(path);
    }
    closedir(d);
}

static void bytecode_save(JSContext *ctx, JSValueConst fn, const char *path) {
    size_t blen = 0;
    uint8_t *bc = JS_WriteObject(ctx, &blen, fn, JS_WRITE_OBJ_BYTECODE);
    if (!bc) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return;
    }
    char dir[600], tmp[720];
    snprintf(dir, sizeof dir, "%s/bytecode", g_cfg.data_dir);
#ifdef __vita__
    mkdir(dir, 0777);
#else
    mkdir(dir, 0755);
#endif
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(bc, 1, blen, f) == blen;
    if (f) ok &= fclose(f) == 0;
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    js_free(ctx, bc);
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
    char bc_path[700];
    bytecode_path(url, src, len, bc_path, sizeof bc_path);
    struct stat st;
    bool have_bc = stat(bc_path, &st) == 0;
    snprintf(msg, sizeof msg, "PokeRogue\n\n%s game code\n%s (%lu KB)%s", have_bc ? "Loading" : "Preparing",
             base ? base + 1 : url, (unsigned long)(len / 1024),
             have_bc ? "" : "\n\nThe first start after an update takes a while.");
    overlay_present(msg);
    JSValue fn = JS_UNDEFINED;
    bool cached = false;
    size_t blen = 0;
    char *bc = read_file(bc_path, &blen);
    if (bc) {
        fn = JS_ReadObject(ctx, (const uint8_t *)bc, blen, JS_READ_OBJ_BYTECODE);
        free(bc);
        if (JS_IsException(fn) || JS_VALUE_GET_TAG(fn) != JS_TAG_MODULE) {
            host_log("[module] cached code for %s unusable; compiling", url);
            if (JS_IsException(fn)) JS_FreeValue(ctx, JS_GetException(ctx));
            else JS_FreeValue(ctx, fn);
            fn = JS_UNDEFINED;
            remove(bc_path);
        } else {
            cached = true;
        }
    }
    if (!cached) {
        fn = JS_Eval(ctx, src, len, url, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (!JS_IsException(fn)) bytecode_save(ctx, fn, bc_path);
    }
    free(src);
    if (JS_IsException(fn)) return NULL;
    JSModuleDef *m = JS_VALUE_GET_PTR(fn);
    JSValue meta = JS_GetImportMeta(ctx, m);
    JS_SetPropertyStr(ctx, meta, "url", JS_NewString(ctx, url));
    JS_FreeValue(ctx, meta);
    host_log("[module] %s (%lu KB) fetched %.0f ms, %s %.0f ms", url, (unsigned long)(len / 1024), t1 - t0,
             cached ? "loaded compiled code in" : "compiled", host_now_ms() - t1);
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

// writeFile(relPath, string | ArrayBuffer | TypedArray) -> bool. Writes to a temp file and
// renames, so a crash never leaves half a file. Missing folders on the way are created.
static JSValue n_write_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *rel = JS_ToCString(ctx, argv[0]);
    size_t len = 0;
    const char *s = NULL, *bin = NULL;
    if (JS_IsString(argv[1])) {
        s = JS_ToCStringLen(ctx, &len, argv[1]);
    } else {
        bin = (const char *)JS_GetArrayBuffer(ctx, &len, argv[1]);
        if (!bin) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            size_t off, tlen, bpe, total;
            JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[1], &off, &tlen, &bpe);
            if (!JS_IsException(ab)) {
                const char *base = (const char *)JS_GetArrayBuffer(ctx, &total, ab);
                if (base) bin = base + off, len = tlen;
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            JS_FreeValue(ctx, ab);
        }
    }
    const char *data = s ? s : bin;
    if (rel && data && !strstr(rel, "..")) { // parent folders
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/%s", g_cfg.data_dir, rel);
        for (char *p = dir + strlen(g_cfg.data_dir) + 1; (p = strchr(p, '/')); p++) {
            *p = 0;
            mkdir(dir, 0777);
            *p = '/';
        }
    }
    bool ok = false;
    if (rel && data && !strstr(rel, "..")) {
        char path[1024], tmp[1100];
        snprintf(path, sizeof path, "%s/%s", g_cfg.data_dir, rel);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *f = fopen(tmp, "wb");
        if (f) {
            ok = fwrite(data, 1, len, f) == len;
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
    if (osk_mutes_pad() || link_mutes_pad()) // the built-in keyboard or a shown link has the controls
        for (int i = 0; i < n; i++) memset(pads[i].axes, 0, sizeof pads[i].axes), memset(pads[i].buttons, 0, sizeof pads[i].buttons);
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

// gc() -> ms: runs QuickJS's cycle collector now (diagnostics: how long a collection takes).
static JSValue n_gc(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    double t0 = host_now_ms();
    JS_RunGC(JS_GetRuntime(ctx));
    return JS_NewFloat64(ctx, host_now_ms() - t0);
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

// btoa(string) -> string | null (null: a character above U+00FF). The JS versions built the result a
// character at a time; offline saves are base64 of the whole save (megabytes), which took seconds.
static JSValue n_btoa(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    // UTF-8 back to Latin-1 bytes (in place: never longer).
    uint8_t *b = js_malloc(ctx, len + 1);
    if (!b) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    size_t n = 0;
    bool ok = true;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) b[n++] = c;
        else if ((c & 0xE0) == 0xC0 && c <= 0xC3 && i + 1 < len) b[n++] = (uint8_t)(((c & 3) << 6) | (s[++i] & 0x3F));
        else { ok = false; break; }
    }
    JS_FreeCString(ctx, s);
    if (!ok) {
        js_free(ctx, b);
        return JS_NULL;
    }
    size_t olen = (n + 2) / 3 * 4;
    char *o = js_malloc(ctx, olen + 1);
    if (!o) {
        js_free(ctx, b);
        return JS_EXCEPTION;
    }
    size_t j = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)b[i] << 16 | (i + 1 < n ? (uint32_t)b[i + 1] << 8 : 0) | (i + 2 < n ? b[i + 2] : 0);
        o[j++] = b64[v >> 18 & 63];
        o[j++] = b64[v >> 12 & 63];
        o[j++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        o[j++] = i + 2 < n ? b64[v & 63] : '=';
    }
    js_free(ctx, b);
    JSValue r = JS_NewStringLen(ctx, o, j);
    js_free(ctx, o);
    return r;
}

// atob(string) -> string | null (null: not base64). The result is a byte string (U+0000..U+00FF).
static JSValue n_atob(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    // Bytes >= 0x80 take two bytes in UTF-8, so the output is at most 2 * 3/4 of the input.
    char *o = js_malloc(ctx, len * 3 / 2 + 4);
    if (!o) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    size_t j = 0, nchars = 0, pad = 0;
    uint32_t acc = 0;
    int bits = 0;
    bool ok = true;
    for (size_t i = 0; i < len && ok; i++) {
        uint8_t c = (uint8_t)s[i];
        int v;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r') continue;
        if (c == '=') { pad++; continue; }
        if (pad) { ok = false; break; } // data after padding
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else { ok = false; break; }
        nchars++;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            uint8_t byte = (uint8_t)(acc >> bits);
            if (byte < 0x80) o[j++] = (char)byte;
            else {
                o[j++] = (char)(0xC0 | byte >> 6);
                o[j++] = (char)(0x80 | (byte & 0x3F));
            }
        }
    }
    JS_FreeCString(ctx, s);
    if (ok && (nchars % 4 == 1 || pad > 2 || (pad && (nchars + pad) % 4))) ok = false;
    JSValue r = ok ? JS_NewStringLen(ctx, o, j) : JS_NULL;
    js_free(ctx, o);
    return r;
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
    bool ok = textin_open(title ? title : "", init ? init : "", max_len, JS_ToBool(ctx, argv[3]));
    JS_FreeCString(ctx, title);
    JS_FreeCString(ctx, init);
    return JS_NewBool(ctx, ok);
}

// imePoll() -> undefined while open, {ok: true, text} when confirmed, {ok: false} when cancelled
static JSValue n_ime_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char text[4096];
    int r = textin_poll(text, sizeof text);
    if (r == 0) return JS_UNDEFINED;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "ok", JS_NewBool(ctx, r > 0));
    if (r > 0) JS_SetPropertyStr(ctx, o, "text", JS_NewString(ctx, text));
    return o;
}

// setFieldOverlays([{x, y, w, h, pix?: ArrayBuffer, pw, ph}]): text of the game's input fields to
// draw over the game (game canvas coordinates; pix only when the text changed).
static JSValue n_set_field_overlays(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int64_t n = 0;
    if (JS_GetLength(ctx, argv[0], &n) < 0) return JS_EXCEPTION;
    for (int64_t i = 0; i < n && i < 8; i++) {
        JSValue o = JS_GetPropertyInt64(ctx, argv[0], i);
        double v[4] = {0};
        static const char *names[4] = {"x", "y", "w", "h"};
        for (int k = 0; k < 4; k++) {
            JSValue jv = JS_GetPropertyStr(ctx, o, names[k]);
            JS_ToFloat64(ctx, &v[k], jv);
            JS_FreeValue(ctx, jv);
        }
        JSValue pix = JS_GetPropertyStr(ctx, o, "pix");
        int32_t pw = 0, ph = 0;
        JSValue jw = JS_GetPropertyStr(ctx, o, "pw"), jh = JS_GetPropertyStr(ctx, o, "ph");
        JS_ToInt32(ctx, &pw, jw), JS_ToInt32(ctx, &ph, jh);
        size_t len = 0;
        uint8_t *p = NULL;
        if (!JS_IsUndefined(pix) && !JS_IsNull(pix)) {
            p = JS_GetArrayBuffer(ctx, &len, pix);
            if (!p) JS_FreeValue(ctx, JS_GetException(ctx)); // not an ArrayBuffer
            else if (len < (size_t)pw * ph * 4) p = NULL;
        }
        field_update((int)i, p, pw, ph, (float)v[0], (float)v[1], (float)v[2], (float)v[3]);
        JS_FreeValue(ctx, jw), JS_FreeValue(ctx, jh), JS_FreeValue(ctx, pix), JS_FreeValue(ctx, o);
    }
    fields_set((int)n);
    return JS_UNDEFINED;
}

static JSValue n_clock_offset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewFloat64(ctx, net_clock_offset_ms());
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

// showLink(url, caption): a link the game opens, as a QR code over the game (dialogs.c)
static JSValue n_show_link(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *url = JS_ToCString(ctx, argv[0]);
    const char *cap = argc > 1 ? JS_ToCString(ctx, argv[1]) : NULL;
    if (url) link_show(url, cap ? cap : "");
    JS_FreeCString(ctx, url);
    JS_FreeCString(ctx, cap);
    return JS_UNDEFINED;
}

// notice(text): a message over the game until a button is pressed (dialogs.c)
static JSValue n_notice(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *t = JS_ToCString(ctx, argv[0]);
    if (t) dialog_notice(t);
    JS_FreeCString(ctx, t);
    return JS_UNDEFINED;
}

// pickFile(title, relDir, extensions): file picker over the game on <data>/relDir (dialogs.c)
static JSValue n_pick_file(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *title = JS_ToCString(ctx, argv[0]), *rel = JS_ToCString(ctx, argv[1]), *ext = JS_ToCString(ctx, argv[2]);
    if (title && rel && ext && !strstr(rel, "..")) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s/%s", g_cfg.data_dir, rel);
        mkdir(dir, 0777);
        dialog_pick_open(title, dir, ext);
    }
    JS_FreeCString(ctx, title), JS_FreeCString(ctx, rel), JS_FreeCString(ctx, ext);
    return JS_UNDEFINED;
}

// pickPoll() -> undefined (nothing yet) | null (cancelled) | [name, ArrayBuffer]
static JSValue n_pick_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *path;
    int r = dialog_pick_poll(&path);
    if (!r) return JS_UNDEFINED;
    if (r < 0) return JS_NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return JS_NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n > 0 ? n : 1);
    bool ok = buf && fread(buf, 1, n, f) == (size_t)n;
    fclose(f);
    JSValue ret = JS_NULL;
    if (ok) {
        ret = JS_NewArray(ctx);
        const char *slash = strrchr(path, '/');
        JS_SetPropertyUint32(ctx, ret, 0, JS_NewString(ctx, slash ? slash + 1 : path));
        JS_SetPropertyUint32(ctx, ret, 1, JS_NewArrayBufferCopy(ctx, buf, n));
    }
    free(buf);
    return ret;
}

// relaunch(): location.reload, done by restarting the app after this frame (main.c)
static JSValue n_relaunch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    g_relaunch = true;
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
    JS_CFUNC_DEF("showLink", 2, n_show_link),
    JS_CFUNC_DEF("notice", 1, n_notice),
    JS_CFUNC_DEF("pickFile", 3, n_pick_file),
    JS_CFUNC_DEF("pickPoll", 0, n_pick_poll),
    JS_CFUNC_DEF("relaunch", 0, n_relaunch),
    JS_CFUNC_DEF("showMessage", 1, n_show_message),
    JS_CFUNC_DEF("memoryUsage", 0, n_memory_usage),
    JS_CFUNC_DEF("imeOpen", 4, n_ime_open),
    JS_CFUNC_DEF("imePoll", 0, n_ime_poll),
    JS_CFUNC_DEF("setFieldOverlays", 1, n_set_field_overlays),
    JS_CFUNC_DEF("clockOffset", 0, n_clock_offset),
    JS_CFUNC_DEF("loadScript", 1, n_load_script),
    JS_CFUNC_DEF("utf8Decode", 1, n_utf8_decode),
    JS_CFUNC_DEF("utf8Encode", 1, n_utf8_encode),
    JS_CFUNC_DEF("btoa", 1, n_btoa),
    JS_CFUNC_DEF("gc", 0, n_gc),
    JS_CFUNC_DEF("atob", 1, n_atob),
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
    bytecode_cleanup();
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
    JS_SetPropertyStr(ctx, native, "dataDir", JS_NewString(ctx, g_cfg.data_dir));
    JS_SetPropertyStr(ctx, native, "screenWidth", JS_NewInt32(ctx, g_cfg.win_w));
    JS_SetPropertyStr(ctx, native, "screenHeight", JS_NewInt32(ctx, g_cfg.win_h));
    net_register(ctx, native);
    gl_register(ctx, native);
    canvas_register(ctx, native);
    fastpath_register(ctx, native);
    audio_register(ctx, native);
    savecrypt_register(ctx, native);
    JS_SetPropertyStr(ctx, global, "__native", native);
    JS_FreeValue(ctx, global);
    return ctx;
}

void host_js_shutdown(void) {
    // Freeing a runtime that still holds the game's object graph is slow and pointless at exit.
    g_ctx = NULL;
}
