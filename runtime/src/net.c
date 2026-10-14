// SPDX-License-Identifier: AGPL-3.0-only
// HTTP for fetch/XHR and the module loader, on libcurl.
//
// Requests run on curl's multi interface and are polled once per frame, so no threads are needed.
// Responses for versioned static files (hashed /assets/ chunks, or URLs carrying ?t=/?v= cache
// busters from the game's manifest) are cached on disk, which is what makes later boots fast.
// The page itself, the manifest and all API calls always go to the network.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <curl/curl.h>

#include "host.h"

// Cloudflare in front of pokerogue.net rejects requests that don't look like a browser.
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36"

typedef struct buf {
    char *data;
    size_t len, cap;
} buf;

static void buf_add(buf *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}

typedef struct req {
    int id;
    CURL *easy;
    struct curl_slist *headers;
    buf body, resp_headers;
    char *upload;
    char url[4096];
    bool cacheable;
    JSValue callback;
    // Served from disk cache: delivered on the next poll without touching the network.
    bool from_cache;
    struct req *next;
} req;

static CURLM *multi;
static req *reqs;
static int next_id = 1;
static char origin[256];

void net_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi = curl_multi_init();
    const char *p = strstr(g_cfg.base_url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    snprintf(origin, sizeof origin, "%.*s", (int)(p ? p - g_cfg.base_url : (long)strlen(g_cfg.base_url)),
             g_cfg.base_url);
    char dir[512];
    snprintf(dir, sizeof dir, "%s/http", g_cfg.data_dir);
    mkdir(dir, 0777);
}

void net_shutdown(void) {
    curl_multi_cleanup(multi);
    curl_global_cleanup();
}

// ---------------------------------------------------------------- disk cache

static bool is_cacheable(const char *method, const char *url) {
    if (strcmp(method, "GET")) return false;
    size_t olen = strlen(origin);
    if (strncmp(url, origin, olen)) return false;
    const char *path = url + olen;
    return !strncmp(path, "/assets/", 8) || strstr(path, "?t=") || strstr(path, "&t=") || strstr(path, "?v=");
}

static void cache_path(const char *url, char *out, size_t n) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (const char *p = url; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    snprintf(out, n, "%s/http/%016llx", g_cfg.data_dir, (unsigned long long)h);
}

static char *cache_read(const char *url, size_t *len) {
    char path[600];
    cache_path(url, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(n + 1);
    if (fread(d, 1, n, f) != (size_t)n) {
        free(d);
        d = NULL;
    } else {
        d[n] = 0;
        *len = n;
    }
    fclose(f);
    return d;
}

static void cache_write(const char *url, const char *data, size_t len) {
    char path[600], tmp[620];
    cache_path(url, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) == 0 && ok) rename(tmp, path);
}

// ---------------------------------------------------------------- curl helpers

static size_t on_write(char *p, size_t size, size_t n, void *ud) {
    buf_add(ud, p, size * n);
    return size * n;
}

static struct curl_slist *default_headers(struct curl_slist *h, bool has_origin, bool has_referer) {
    char line[512];
    if (!has_origin) {
        snprintf(line, sizeof line, "Origin: %s", origin);
        h = curl_slist_append(h, line);
    }
    if (!has_referer) {
        snprintf(line, sizeof line, "Referer: %s/", origin);
        h = curl_slist_append(h, line);
    }
    return h;
}

// Browsers percent-encode spaces and other unsafe bytes in URLs; curl rejects them raw.
static void encode_url(const char *in, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if (*p <= 0x20 || *p >= 0x7F || strchr("\"<>\\^`{|}", *p)) {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = 0;
}

static CURL *make_easy(const char *raw_url) {
    char url[8192];
    encode_url(raw_url, url, sizeof url);
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 120L);
    return e;
}

char *net_get_sync(const char *url, size_t *len, long *status) {
    if (is_cacheable("GET", url)) {
        char *d = cache_read(url, len);
        if (d) {
            *status = 200;
            return d;
        }
    }
    CURL *e = make_easy(url);
    buf b = {0};
    struct curl_slist *h = default_headers(NULL, false, false);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &b);
    CURLcode rc = curl_easy_perform(e);
    *status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, status);
    else host_log("[net] GET %s failed: %s", url, curl_easy_strerror(rc));
    curl_slist_free_all(h);
    curl_easy_cleanup(e);
    if (rc != CURLE_OK) {
        free(b.data);
        return NULL;
    }
    if (!b.data) buf_add(&b, "", 0);
    if (*status == 200 && is_cacheable("GET", url)) cache_write(url, b.data, b.len);
    *len = b.len;
    return b.data;
}

// ---------------------------------------------------------------- JS API

// http(method, url, headers: [k, v, ...], body: string|ArrayBuffer|TypedArray|null, callback) -> id
// callback(status, headersText, ArrayBuffer body, error|null, finalURL)
static JSValue n_http(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *method = JS_ToCString(ctx, argv[0]);
    const char *url = JS_ToCString(ctx, argv[1]);
    if (!method || !url) {
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        return JS_EXCEPTION;
    }
    req *r = calloc(1, sizeof *r);
    r->id = next_id++;
    snprintf(r->url, sizeof r->url, "%s", url);
    r->callback = JS_DupValue(ctx, argv[4]);
    r->cacheable = is_cacheable(method, url);
    r->next = reqs;
    reqs = r;

    if (r->cacheable) {
        size_t len;
        char *d = cache_read(url, &len);
        if (d) {
            r->body.data = d;
            r->body.len = r->body.cap = len;
            r->from_cache = true;
            JS_FreeCString(ctx, method);
            JS_FreeCString(ctx, url);
            return JS_NewInt32(ctx, r->id);
        }
    }

    CURL *e = r->easy = make_easy(url);
    bool has_origin = false, has_referer = false;
    uint32_t nh = 0;
    JSValue lenv = JS_GetPropertyStr(ctx, argv[2], "length");
    JS_ToUint32(ctx, &nh, lenv);
    JS_FreeValue(ctx, lenv);
    for (uint32_t i = 0; i + 1 < nh; i += 2) {
        JSValue kv = JS_GetPropertyUint32(ctx, argv[2], i), vv = JS_GetPropertyUint32(ctx, argv[2], i + 1);
        const char *k = JS_ToCString(ctx, kv), *v = JS_ToCString(ctx, vv);
        if (k && v) {
            char line[8192];
            snprintf(line, sizeof line, "%s: %s", k, v);
            r->headers = curl_slist_append(r->headers, line);
            if (!strcasecmp(k, "origin")) has_origin = true;
            if (!strcasecmp(k, "referer")) has_referer = true;
        }
        JS_FreeCString(ctx, k);
        JS_FreeCString(ctx, v);
        JS_FreeValue(ctx, kv);
        JS_FreeValue(ctx, vv);
    }
    r->headers = default_headers(r->headers, has_origin, has_referer);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, r->headers);
    curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, method);
    if (!strcmp(method, "HEAD")) curl_easy_setopt(e, CURLOPT_NOBODY, 1L);

    if (!JS_IsNull(argv[3]) && !JS_IsUndefined(argv[3])) {
        size_t blen = 0;
        const uint8_t *bp = NULL;
        const char *str = NULL;
        size_t off, tlen, bpe;
        if (JS_IsString(argv[3])) {
            str = JS_ToCStringLen(ctx, &blen, argv[3]);
            bp = (const uint8_t *)str;
        } else if ((bp = JS_GetArrayBuffer(ctx, &blen, argv[3])) == NULL) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[3], &off, &tlen, &bpe);
            if (!JS_IsException(ab)) {
                size_t total;
                uint8_t *p = JS_GetArrayBuffer(ctx, &total, ab);
                if (p) {
                    bp = p + off;
                    blen = tlen;
                }
                JS_FreeValue(ctx, ab);
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
        }
        if (bp) {
            r->upload = malloc(blen + 1);
            memcpy(r->upload, bp, blen);
            curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)blen);
            curl_easy_setopt(e, CURLOPT_POSTFIELDS, r->upload);
        }
        JS_FreeCString(ctx, str);
    }
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &r->body);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, &r->resp_headers);
    curl_easy_setopt(e, CURLOPT_PRIVATE, r);
    curl_multi_add_handle(multi, e);
    JS_FreeCString(ctx, method);
    JS_FreeCString(ctx, url);
    return JS_NewInt32(ctx, r->id);
}

// ArrayBuffer storage callback: size 0 frees, otherwise (re)allocates.
static void *ab_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
    if (!size) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

static void deliver(JSContext *ctx, req *r, long status, const char *error, const char *final_url) {
    // Keep only the last response's headers (redirects produce several blocks).
    const char *hdrs = r->resp_headers.data ? r->resp_headers.data : "";
    const char *last = hdrs, *p = hdrs;
    while ((p = strstr(p, "\r\nHTTP/"))) last = p += 2;
    JSValue args[5];
    args[0] = JS_NewInt32(ctx, (int)status);
    args[1] = JS_NewString(ctx, last);
    if (r->body.data) {
        args[2] = JS_NewArrayBuffer(ctx, (uint8_t *)r->body.data, r->body.len, 0, ab_realloc, NULL, false);
        r->body.data = NULL;
    } else {
        args[2] = JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    }
    args[3] = error ? JS_NewString(ctx, error) : JS_NULL;
    args[4] = JS_NewString(ctx, final_url ? final_url : r->url);
    JSValue ret = JS_Call(ctx, r->callback, JS_UNDEFINED, 5, args);
    if (JS_IsException(ret)) host_report_exception(ctx);
    JS_FreeValue(ctx, ret);
    for (int i = 0; i < 5; i++) JS_FreeValue(ctx, args[i]);
}

static void req_free(JSContext *ctx, req *r) {
    for (req **pp = &reqs; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    if (r->easy) {
        curl_multi_remove_handle(multi, r->easy);
        curl_easy_cleanup(r->easy);
    }
    curl_slist_free_all(r->headers);
    free(r->body.data);
    free(r->resp_headers.data);
    free(r->upload);
    JS_FreeValue(ctx, r->callback);
    free(r);
}

void net_poll(JSContext *ctx) {
    // Cache hits first.
    for (req *r = reqs, *next; r; r = next) {
        next = r->next;
        if (r->from_cache) {
            buf_add(&r->resp_headers, "HTTP/1.1 200 OK\r\nx-runtime-cache: hit\r\n\r\n", 41);
            deliver(ctx, r, 200, NULL, NULL);
            req_free(ctx, r);
            next = reqs; // list changed; restart (cheap, few entries)
        }
    }
    int running;
    curl_multi_perform(multi, &running);
    CURLMsg *m;
    int left;
    while ((m = curl_multi_info_read(multi, &left))) {
        if (m->msg != CURLMSG_DONE) continue;
        req *r;
        curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&r);
        long status = 0;
        char *final_url = NULL;
        curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(r->easy, CURLINFO_EFFECTIVE_URL, &final_url);
        const char *err = m->data.result == CURLE_OK ? NULL : curl_easy_strerror(m->data.result);
        if (err) host_log("[net] %s failed: %s", r->url, err);
        if (!err && status == 200 && r->cacheable && r->body.data) cache_write(r->url, r->body.data, r->body.len);
        char fu[4096];
        snprintf(fu, sizeof fu, "%s", final_url ? final_url : r->url);
        deliver(ctx, r, err ? 0 : status, err, fu);
        req_free(ctx, r);
    }
}

static JSValue n_http_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    for (req *r = reqs; r; r = r->next) {
        if (r->id == id) {
            req_free(ctx, r);
            break;
        }
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry net_funcs[] = {
    JS_CFUNC_DEF("http", 5, n_http),
    JS_CFUNC_DEF("httpAbort", 1, n_http_abort),
};

void net_register(JSContext *ctx, JSValue native) {
    JS_SetPropertyFunctionList(ctx, native, net_funcs, sizeof net_funcs / sizeof *net_funcs);
}
