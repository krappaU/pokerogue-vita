// SPDX-License-Identifier: AGPL-3.0-only
// HTTP for fetch/XHR and the module loader, on libcurl.
//
// Requests run on a network thread (CPU core 2 on the Vita): it reads disk-cache hits and files of
// the offline game, runs curl's multi interface (connections, TLS, decompression) and writes the disk
// cache. The main thread only creates requests and, once per frame, hands finished ones to JS. A
// request belongs to the network thread from submission until it comes back on the done queue.
// Responses for versioned static files (hashed /assets/ chunks, or URLs carrying ?t=/?v= cache
// busters from the game's manifest) are cached on disk, which is what makes later boots fast.
// The page itself, the manifest and all API calls always go to the network.

#include <math.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include <curl/curl.h>
#include "miniz.h"
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "host.h"

#ifdef __vita__
#include <psp2/kernel/rng.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
// CA bundle for HTTPS (curl's cacert.pem, shipped in the vpk). OpenSSL on the Vita can't open it by
// path, so it is read here and handed to curl from memory.
#define CA_FILE "app0:cacert.pem"
#endif

// The CA bundle is parsed once into a shared OpenSSL certificate store that every connection uses.
// (Handing curl the bundle per request makes it re-parse ~150 certificates for each new connection,
// which on the Vita is slow and fails under the game's many parallel downloads.)
static X509_STORE *ca_store;

static X509_STORE *load_ca_store(const char *data, size_t len) {
    X509_STORE *store = X509_STORE_new();
    BIO *bio = BIO_new_mem_buf(data, (int)len);
    int n = 0, bad = 0;
    for (;;) {
        X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
        if (cert) {
            if (X509_STORE_add_cert(store, cert)) n++;
            X509_free(cert);
            continue;
        }
        unsigned long e = ERR_peek_last_error();
        if (ERR_GET_LIB(e) == ERR_LIB_PEM && ERR_GET_REASON(e) == PEM_R_NO_START_LINE) break; // end of file
        // One unreadable certificate must not end the whole bundle: log it and skip to the next.
        char msg[256];
        ERR_error_string_n(e, msg, sizeof msg);
        if (bad++ < 5) host_log("[net] skipping a CA certificate: %s", msg);
        ERR_clear_error();
        if (BIO_eof(bio) || bad > 200) break;
    }
    ERR_clear_error();
    BIO_free(bio);
    host_log("[net] %d CA certificates loaded (%d skipped)", n, bad);
    return store;
}

#ifdef __vita__
// OpenSSL finds no OS entropy source on the Vita ("Insufficient randomness" on every TLS
// connection), so all of its random numbers come from the kernel's hardware RNG instead.
static int vita_rand_bytes(unsigned char *buf, int num) {
    while (num > 0) {
        int n = num > 64 ? 64 : num; // the kernel call returns at most 64 bytes
        if (sceKernelGetRandomNumber(buf, n) < 0) return 0;
        buf += n;
        num -= n;
    }
    return 1;
}
static int vita_rand_seed(const void *buf, int num) { return 1; }
static int vita_rand_add(const void *buf, int num, double randomness) { return 1; }
static int vita_rand_status(void) { return 1; }
static RAND_METHOD vita_rand = {vita_rand_seed, vita_rand_bytes, NULL, vita_rand_add, vita_rand_bytes, vita_rand_status};
#endif

// Fills buf with cryptographically secure random bytes (also used for JS crypto.getRandomValues).
bool host_random_bytes(void *buf, size_t n) {
#ifdef __vita__
    return vita_rand_bytes(buf, (int)n);
#else
    return RAND_bytes(buf, (int)n) == 1;
#endif
}

static CURLcode ssl_ctx_cb(CURL *curl, void *sslctx, void *userptr) {
    X509_STORE_up_ref(ca_store);
    SSL_CTX_set_cert_store((SSL_CTX *)sslctx, ca_store);
    return CURLE_OK;
}

// Cloudflare in front of pokerogue.net rejects requests that don't look like a browser.
#define USER_AGENT "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/140.0 Safari/537.36"

typedef struct buf {
    char *data;
    size_t len, cap;
} buf;

// Appends to a growing buffer. Returns false (buffer unchanged) if memory runs out.
static bool buf_add(buf *b, const void *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        // Grow by 1.5x rather than 2x: big responses are sized up front from Content-Length anyway.
        size_t cap = b->len + n + 1 + (b->len + n) / 2;
        char *d = realloc(b->data, cap);
        if (!d) return false;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
    return true;
}

// Size a response buffer exactly once the server announces Content-Length (avoids repeated
// reallocations of multi-megabyte downloads, e.g. the game's 5 MB font).
static void buf_reserve(buf *b, size_t total) {
    if (total + 1 <= b->cap) return;
    char *d = realloc(b->data, total + 1);
    if (d) {
        b->data = d;
        b->cap = total + 1;
    }
}

typedef struct req {
    char errbuf[CURL_ERROR_SIZE];
    int id;
    CURL *easy;
    struct curl_slist *headers;
    buf body, resp_headers;
    char *upload;
    char url[4096];
    bool cacheable;
    JSValue callback;
    // Served from disk cache: delivered on the next poll without touching the network.
    int local_status; // local mode: 200 or 404 (delivered like a cache hit)
    bool from_cache;
    bool local;            // offline game file
    // Set by the network thread before the request comes back:
    long status;
    const char *error;     // curl's static error string, or NULL
    char *final_url;       // after redirects (malloc), or NULL
    bool aborted;          // set by the main thread (under qlock): no callback
    struct req *next;      // main thread's list of live requests
    struct req *qnext;     // submit / in-flight / done queues
} req;

static CURLM *multi;
static void net_thread_start(void);
static req *reqs;
// Queues between the main thread and the network thread.
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static req *submit_q, *submit_tail, *done_q, *done_tail;
static int next_id = 1;
static int n_done, n_cache_hits;
static unsigned long long n_bytes;
static char origin[256];

// The page's origin ("https://pokerogue.net"): sent as Origin/Referer like a browser would (the
// API's firewall blocks requests without them) and used to decide what goes in the disk cache.
void net_set_origin(const char *page_url) {
    const char *p = strstr(page_url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    snprintf(origin, sizeof origin, "%.*s", (int)(p ? p - page_url : (long)strlen(page_url)), page_url);
}

void net_init(void) {
#ifdef __vita__
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    static char net_mem[1024 * 1024];
    SceNetInitParam np = {.memory = net_mem, .size = sizeof net_mem, .flags = 0};
    int rc = sceNetInit(&np);
    if (rc < 0) host_log("[net] sceNetInit: 0x%08x", rc);
    rc = sceNetCtlInit();
    if (rc < 0) host_log("[net] sceNetCtlInit: 0x%08x", rc);
#endif
#ifdef __vita__
    RAND_set_rand_method(&vita_rand);
#endif
    curl_global_init(CURL_GLOBAL_DEFAULT);
    multi = curl_multi_init();
    net_set_origin(g_cfg.base_url);
    // PKR_CA_FILE (Linux) tests the same in-memory path the Vita uses.
    const char *ca = getenv("PKR_CA_FILE");
#ifdef CA_FILE
    if (!ca) ca = CA_FILE;
#endif
    if (ca) {
        FILE *f = fopen(ca, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *data = malloc(n);
            size_t got = fread(data, 1, n, f);
            fclose(f);
            ca_store = load_ca_store(data, got);
            free(data);
            time_t now = time(NULL);
            host_log("[net] CA bundle %s: %lu KB; system clock (UTC): %s", ca, (unsigned long)(got / 1024),
                     asctime(gmtime(&now)));
        } else {
            host_log("[net] cannot open CA bundle %s; HTTPS will fail", ca);
        }
    }
    // Like browsers: at most 6 connections per host; extra requests wait and reuse them.
    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, 6L);
    curl_multi_setopt(multi, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8L);
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    char dir[512];
    snprintf(dir, sizeof dir, "%s/http", g_cfg.data_dir);
    mkdir(dir, 0777);
    net_thread_start();
}

void net_shutdown(void) {
    // The network thread still uses the multi handle; the process is about to end anyway.
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
    buf *b = ud;
    if (!buf_add(b, p, size * n)) {
        host_log("[net] out of memory while downloading (%lu KB so far)", (unsigned long)(b->len / 1024));
        return 0; // aborts this transfer with CURLE_WRITE_ERROR
    }
    return size * n;
}

// Body writer for async requests: reserves the full size on the first chunk when it's known.
static size_t on_write_body(char *p, size_t size, size_t n, void *ud) {
    struct req *r = ud;
    if (r->body.len == 0) {
        curl_off_t total = -1;
        curl_easy_getinfo(r->easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total);
        if (total > 0) buf_reserve(&r->body, (size_t)total);
    }
    return on_write(p, size, n, &r->body);
}

static bool same_origin(const char *url) {
    size_t olen = strlen(origin);
    return !strncmp(url, origin, olen) && (url[olen] == '/' || url[olen] == '?' || !url[olen]);
}

// Browsers send Origin with cross-origin and non-GET requests only. Same-origin GETs must not carry
// it: Cloudflare keeps a separate cached copy of pokerogue.net for requests with an Origin header,
// and that copy was a months-old build of the game (whose saves the server then rejects as older
// than the account's last save).
static struct curl_slist *default_headers(struct curl_slist *h, const char *method, const char *url,
                                          bool has_origin, bool has_referer) {
    char line[512];
    bool simple_get = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    if (!has_origin && !(simple_get && same_origin(url))) {
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

// nettrace=1 in config.txt: log the headers of requests to the game's API (not assets), with
// cookie and authorization values shortened.
static int trace_cb(CURL *h, curl_infotype type, char *data, size_t size, void *ud) {
    (void)h, (void)ud;
    if (type != CURLINFO_HEADER_OUT) return 0;
    static char block[4096];
    size_t n = size < sizeof block - 1 ? size : sizeof block - 1;
    memcpy(block, data, n);
    block[n] = 0;
    if (strstr(block, "/assets/") || strstr(block, "/images/") || strstr(block, "/audio/")) return 0;
    char *save = NULL;
    for (char *l = strtok_r(block, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
        char *colon = strchr(l, ':');
        if (colon && (!strncasecmp(l, "cookie", 6) || !strncasecmp(l, "authorization", 13)) && strlen(colon) > 12)
            strcpy(colon + 10, "...");
        host_log("[nettrace] %s", l);
    }
    return 0;
}

static void apply_common_opts(CURL *e) {
    if (g_cfg.net_trace) {
        curl_easy_setopt(e, CURLOPT_DEBUGFUNCTION, trace_cb);
        curl_easy_setopt(e, CURLOPT_VERBOSE, 1L);
    }
    curl_easy_setopt(e, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    // gzip only: zstd (which Cloudflare prefers) needs megabytes of buffer per download, too much for
    // the Vita with many parallel requests.
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 120L);
    if (ca_store) {
        curl_easy_setopt(e, CURLOPT_CAINFO, NULL);
        curl_easy_setopt(e, CURLOPT_CAPATH, NULL);
        curl_easy_setopt(e, CURLOPT_SSL_CTX_FUNCTION, ssl_ctx_cb);
    }
}

static CURL *make_easy(const char *raw_url) {
    char url[8192];
    encode_url(raw_url, url, sizeof url);
    CURL *e = curl_easy_init();
    curl_easy_setopt(e, CURLOPT_URL, url);
    apply_common_opts(e);
    return e;
}

// Module downloads reuse one easy handle, so they share a single connection (a new TLS handshake
// per chunk is slow on the Vita).
static CURL *sync_easy;

// ---------------------------------------------------------------- file downloads
// Downloads url to path (resuming a partial file left by an earlier attempt). progress() is called
// regularly; returning false cancels. Returns true when the file is complete.

typedef struct {
    FILE *f;
    bool (*progress)(long long done, long long total, void *ud);
    void *ud;
    long long base;
    bool write_error;
} dl_state;

static size_t dl_write(char *p, size_t size, size_t n, void *ud) {
    dl_state *d = ud;
    size_t len = size * n;
    if (fwrite(p, 1, len, d->f) != len) {
        d->write_error = true;
        return 0;
    }
    return len;
}

static int dl_progress(void *ud, curl_off_t total, curl_off_t now, curl_off_t ut, curl_off_t un) {
    (void)ut, (void)un;
    dl_state *d = ud;
    return d->progress(d->base + now, total ? d->base + total : 0, d->ud) ? 0 : 1;
}

bool net_download_file(const char *url, const char *path, bool (*progress)(long long, long long, void *), void *ud,
                       char *err, size_t errn) {
    struct stat st;
    long long have = stat(path, &st) == 0 ? (long long)st.st_size : 0;
    dl_state d = {.f = fopen(path, have ? "ab" : "wb"), .progress = progress, .ud = ud, .base = have};
    if (!d.f) {
        snprintf(err, errn, "can't write %s", path);
        return false;
    }
    CURL *e = curl_easy_init();
    char errbuf[CURL_ERROR_SIZE] = "";
    curl_easy_setopt(e, CURLOPT_URL, url);
    apply_common_opts(e);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 0L);           // a big file takes a while
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);   // but give up if it stalls for a minute
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, dl_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(e, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(e, CURLOPT_XFERINFOFUNCTION, dl_progress);
    curl_easy_setopt(e, CURLOPT_XFERINFODATA, &d);
    if (have) curl_easy_setopt(e, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
    CURLcode rc = curl_easy_perform(e);
    long status = 0;
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(e);
    bool closed_ok = fclose(d.f) == 0;
    if (rc == CURLE_OK && (status == 200 || status == 206) && closed_ok && !d.write_error) return true;
    if (status == 416) return true; // range not satisfiable: the file was already complete
    if (d.write_error || !closed_ok) snprintf(err, errn, "writing to the memory card failed (full?)");
    else if (rc == CURLE_ABORTED_BY_CALLBACK) snprintf(err, errn, "cancelled");
    else if (rc != CURLE_OK) snprintf(err, errn, "%s", errbuf[0] ? errbuf : curl_easy_strerror(rc));
    else snprintf(err, errn, "HTTP %ld", status);
    return false;
}

// ---------------------------------------------------------------- local mode
// local=<dir> in config.txt: the game is served from a folder (a downloaded offline build) under
// the made-up origin https://offline.invalid instead of from the network.

bool net_is_local(const char *url) {
    size_t n = strlen(LOCAL_ORIGIN);
    return g_cfg.local_dir && !strncmp(url, LOCAL_ORIGIN, n) && (url[n] == 0 || url[n] == '/');
}

// The offline game can also be a zip (as downloaded): files are read straight out of it. Unpacking
// its 34,000 files took hours on the Vita's memory card.
static mz_zip_archive local_zip;
static int local_zip_state; // 0 not opened, 1 open, -1 failed

static bool local_is_zip(void) {
    size_t n = strlen(g_cfg.local_dir);
    return n > 4 && !strcmp(g_cfg.local_dir + n - 4, ".zip");
}

static pthread_mutex_t zip_lock = PTHREAD_MUTEX_INITIALIZER; // network thread + module loader
static char *zip_read_locked(const char *name, size_t *len);
static char *zip_read(const char *name, size_t *len) {
    pthread_mutex_lock(&zip_lock);
    char *d = zip_read_locked(name, len);
    pthread_mutex_unlock(&zip_lock);
    return d;
}
static char *zip_read_locked(const char *name, size_t *len) {
    if (local_zip_state == 0) {
        memset(&local_zip, 0, sizeof local_zip);
        local_zip_state = mz_zip_reader_init_file(&local_zip, g_cfg.local_dir, 0) ? 1 : -1;
        if (local_zip_state < 0) host_log("[local] can't open %s as a zip", g_cfg.local_dir);
        else host_log("[local] %s: %u files", g_cfg.local_dir, (unsigned)mz_zip_reader_get_num_files(&local_zip));
    }
    if (local_zip_state < 0) return NULL;
    int idx = mz_zip_reader_locate_file(&local_zip, name, NULL, 0);
    if (idx < 0) return NULL;
    size_t size = 0;
    char *d = mz_zip_reader_extract_to_heap(&local_zip, (mz_uint)idx, &size, 0);
    if (!d) {
        host_log("[local] can't read %s from the zip (out of memory?)", name);
        return NULL;
    }
    char *t = realloc(d, size + 1); // callers get a NUL-terminated buffer
    if (!t) {
        free(d);
        return NULL;
    }
    t[size] = 0;
    *len = size;
    return t;
}

static char *local_read(const char *url, size_t *len) {
    const char *p = url + strlen(LOCAL_ORIGIN);
    while (*p == '/') p++;
    char rel[1024];
    size_t o = 0;
    for (; *p && *p != '?' && *p != '#' && o < sizeof rel - 1; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = {p[1], p[2], 0};
            rel[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            rel[o++] = *p;
        }
    }
    rel[o] = 0;
    if (strstr(rel, "..")) return NULL;
    if (local_is_zip()) return zip_read(rel[0] ? rel : "index.html", len);
    char path[1600];
    snprintf(path, sizeof path, "%s/%s", g_cfg.local_dir, rel[0] ? rel : "index.html");
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc(sz > 0 ? (size_t)sz + 1 : 1);
    if (d && sz > 0 && fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d);
        d = NULL;
    }
    fclose(f);
    if (d) d[sz > 0 ? sz : 0] = 0, *len = sz > 0 ? (size_t)sz : 0;
    return d;
}

// A plain GET with its own connection, safe to call from another thread (net_get_sync shares one
// handle with the main thread). No cache, no page Origin. Returns the body (NUL-terminated) or NULL.
char *net_get_threadsafe(const char *url, size_t *len, long *status) {
    CURL *e = make_easy(url);
    if (!e) return NULL;
    buf b = {0};
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(e);
    *status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, status);
    curl_easy_cleanup(e);
    if (rc != CURLE_OK || !buf_add(&b, "", 1)) {
        free(b.data);
        return NULL;
    }
    *len = b.len - 1;
    return b.data;
}

char *net_get_sync(const char *url, size_t *len, long *status) {
    if (net_is_local(url)) {
        char *d = local_read(url, len);
        *status = d ? 200 : 404;
        return d;
    }
    if (is_cacheable("GET", url)) {
        char *d = cache_read(url, len);
        if (d) {
            *status = 200;
            return d;
        }
    }
    if (!sync_easy) sync_easy = curl_easy_init();
    else curl_easy_reset(sync_easy);
    CURL *e = sync_easy;
    char enc[8192];
    encode_url(url, enc, sizeof enc);
    curl_easy_setopt(e, CURLOPT_URL, enc);
    apply_common_opts(e);
    buf b = {0};
    static char errbuf[CURL_ERROR_SIZE];
    errbuf[0] = 0;
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, errbuf);
    struct curl_slist *h = default_headers(NULL, "GET", url, false, false);
    curl_easy_setopt(e, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &b);
    // Retry transient failures (DNS hiccups, dropped Wi-Fi): game code is essential.
    CURLcode rc = CURLE_OK;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            host_log("[net] retrying %s", url);
            b.len = 0;
        }
        rc = curl_easy_perform(e);
        if (rc == CURLE_OK || rc == CURLE_WRITE_ERROR) break;
    }
    *status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, status);
    else host_log("[net] GET %s failed: %s (%s)", url, curl_easy_strerror(rc), errbuf);
    curl_slist_free_all(h);
    if (rc != CURLE_OK) {
        free(b.data);
        return NULL;
    }
    if (!b.data && !buf_add(&b, "", 0)) return NULL;
    if (*status == 200 && is_cacheable("GET", url)) cache_write(url, b.data, b.len);
    *len = b.len;
    return b.data;
}

// ---------------------------------------------------------------- network thread

static void submit(req *r) {
    pthread_mutex_lock(&qlock);
    r->qnext = NULL;
    if (submit_tail) submit_tail->qnext = r;
    else submit_q = r;
    submit_tail = r;
    pthread_mutex_unlock(&qlock);
    curl_multi_wakeup(multi);
}

static void finish(req *r) { // network thread: hand back to the main thread
    if (r->easy) {
        curl_multi_remove_handle(multi, r->easy);
        curl_easy_cleanup(r->easy);
        r->easy = NULL;
    }
    pthread_mutex_lock(&qlock);
    r->qnext = NULL;
    if (done_tail) done_tail->qnext = r;
    else done_q = r;
    done_tail = r;
    pthread_mutex_unlock(&qlock);
}

static bool is_aborted(req *r) {
    pthread_mutex_lock(&qlock);
    bool a = r->aborted;
    pthread_mutex_unlock(&qlock);
    return a;
}

static void *net_thread(void *arg) {
    (void)arg;
    plat_thread_setup("pkr_net", 1, -10); // core 1: the audio decoder works on core 2
    req *inflight = NULL; // transfers in the multi handle
    for (;;) {
        pthread_mutex_lock(&qlock);
        req *jobs = submit_q;
        submit_q = submit_tail = NULL;
        pthread_mutex_unlock(&qlock);
        for (req *r = jobs, *next; r; r = next) {
            next = r->qnext;
            if (is_aborted(r)) {
                finish(r);
            } else if (r->local) {
                size_t len = 0;
                char *d = local_read(r->url, &len);
                r->local_status = d ? 200 : 404;
                r->body.data = d;
                r->body.len = r->body.cap = d ? len : 0;
                r->from_cache = true;
                finish(r);
            } else {
                size_t len = 0;
                char *d = r->cacheable ? cache_read(r->url, &len) : NULL;
                if (d) {
                    r->body.data = d;
                    r->body.len = r->body.cap = len;
                    r->from_cache = true;
                    curl_easy_cleanup(r->easy); // never added to the multi handle
                    r->easy = NULL;
                    finish(r);
                } else {
                    curl_multi_add_handle(multi, r->easy);
                    r->qnext = inflight;
                    inflight = r;
                }
            }
        }
        int running = 0;
        curl_multi_perform(multi, &running);
        CURLMsg *m;
        int left;
        while ((m = curl_multi_info_read(multi, &left))) {
            if (m->msg != CURLMSG_DONE) continue;
            req *r;
            curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, (char **)&r);
            char *fu = NULL;
            r->status = 0;
            curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &r->status);
            curl_easy_getinfo(r->easy, CURLINFO_EFFECTIVE_URL, &fu);
            r->error = m->data.result == CURLE_OK ? NULL : curl_easy_strerror(m->data.result);
            if (r->error) {
                host_log("[net] %s failed: %s (%s)", r->url, r->error, r->errbuf);
                r->status = 0;
            }
            if (fu) r->final_url = strdup(fu);
            if (!r->error && r->status == 200 && r->cacheable && r->body.data && !is_aborted(r))
                cache_write(r->url, r->body.data, r->body.len);
            for (req **pp = &inflight; *pp; pp = &(*pp)->qnext)
                if (*pp == r) {
                    *pp = r->qnext;
                    break;
                }
            finish(r);
        }
        // Aborted transfers stop now.
        for (req **pp = &inflight; *pp;) {
            req *r = *pp;
            if (is_aborted(r)) {
                *pp = r->qnext;
                finish(r);
            } else {
                pp = &r->qnext;
            }
        }
        // Sleep until a socket is ready, a request is submitted (curl_multi_wakeup) or a timer runs.
        curl_multi_poll(multi, NULL, 0, inflight ? 100 : 1000, NULL);
    }
    return NULL;
}

static void net_thread_start(void) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024); // TLS, inflate, zip reads
    if (pthread_create(&t, &attr, net_thread, NULL) == 0) pthread_detach(t);
    else host_log("[net] could not start the network thread");
    pthread_attr_destroy(&attr);
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

    if (net_is_local(url)) { // read by the network thread
        r->local = true;
        r->cacheable = false;
        submit(r);
        JS_FreeCString(ctx, method);
        JS_FreeCString(ctx, url);
        return JS_NewInt32(ctx, r->id);
    }

    // The network thread tries the disk cache first (cacheable GETs) and only then the network.
    CURL *e = r->easy = make_easy(url);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, r->errbuf);
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
    r->headers = default_headers(r->headers, method, url, has_origin, has_referer);
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
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, on_write_body);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, on_write);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, &r->resp_headers);
    curl_easy_setopt(e, CURLOPT_PRIVATE, r);
    submit(r);
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

// ---------------------------------------------------------------- server time
// The console's clock can be hours off (wrong time zone setting). The game stamps saves with
// Date.now() and prefers a local save copy whose stamp is newer than the server's, so a clock that
// runs ahead could let an old local copy win over newer progress. The first response from a server
// tells the real time (its Date header); js/core.js corrects Date by the difference.
static double clock_offset_ms;
static bool clock_known;

static void note_server_date(const char *hdrs) {
    if (clock_known) return;
    const char *p = NULL;
    for (const char *q = hdrs; *q; q++)
        if (q[0] == '\n' && (q[1] == 'd' || q[1] == 'D') && !strncasecmp(q + 1, "date:", 5)) {
            p = q + 6;
            break;
        }
    if (!p) return;
    while (*p == ' ') p++;
    int d, y, H, M, S;
    char mon[4] = "";
    if (sscanf(p, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6) return; // "Thu, 15 Oct 2026 07:21:40 GMT"
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *f = strstr(months, mon);
    if (!f || strlen(mon) != 3) return;
    int m = (int)(f - months) / 3 + 1;
    // Days since 1970-01-01 (H. Hinnant's days_from_civil).
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400, doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy, days = era * 146097 + doe - 719468;
    double server_ms = ((double)days * 86400 + H * 3600 + M * 60 + S) * 1000.0 + 500;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    double off = server_ms - (tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0);
    clock_known = true;
    if (fabs(off) < 30000) return; // within the header's precision: the clock is fine
    clock_offset_ms = off;
    host_log("[clock] the console's clock is off by %.2f hours; the game uses the server's time", off / 3600000.0);
}

double net_clock_offset_ms(void) { return clock_offset_ms; }

static void deliver(JSContext *ctx, req *r, long status, const char *error, const char *final_url) {
    n_done++;
    n_bytes += r->body.len;
    if (r->body.len > 1024 * 1024)
        host_log("[net] large response: %s (%lu KB)", r->url, (unsigned long)(r->body.len / 1024));
    // Keep only the last response's headers (redirects produce several blocks).
    const char *hdrs = r->resp_headers.data ? r->resp_headers.data : "";
    if (!r->from_cache && status > 0) note_server_date(hdrs);
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

// Main thread, for requests that came back from the network thread (or were never submitted).
static void req_free(JSContext *ctx, req *r) {
    for (req **pp = &reqs; *pp; pp = &(*pp)->next) {
        if (*pp == r) {
            *pp = r->next;
            break;
        }
    }
    if (r->easy) curl_easy_cleanup(r->easy);
    free(r->final_url);
    curl_slist_free_all(r->headers);
    free(r->body.data);
    free(r->resp_headers.data);
    free(r->upload);
    JS_FreeValue(ctx, r->callback);
    free(r);
}

// Transfers run on the network thread now; kept for the JS interrupt handler's call.
void net_pump(void) {}

// Hands finished requests to JS. Cache hits and offline-game files at most ~8 ms worth per frame:
// the game handles each response right away (decoding images among other things), and hundreds at
// once would freeze the screen. Network responses are always delivered.
void net_poll(JSContext *ctx) {
    pthread_mutex_lock(&qlock);
    req *list = done_q;
    done_q = done_tail = NULL;
    pthread_mutex_unlock(&qlock);
    double t0 = host_now_ms();
    req *keep = NULL, *keep_tail = NULL;
    for (req *r = list, *next; r; r = next) {
        next = r->qnext;
        bool aborted;
        pthread_mutex_lock(&qlock);
        aborted = r->aborted;
        pthread_mutex_unlock(&qlock);
        if (aborted) {
            req_free(ctx, r);
            continue;
        }
        if (r->from_cache && host_now_ms() - t0 > 8) { // later frame
            r->qnext = NULL;
            if (keep_tail) keep_tail->qnext = r;
            else keep = r;
            keep_tail = r;
            continue;
        }
        if (r->from_cache) {
            if (r->local_status == 404) {
                buf_add(&r->resp_headers, "HTTP/1.1 404 Not Found\r\n\r\n", 28);
                deliver(ctx, r, 404, NULL, NULL);
            } else {
                buf_add(&r->resp_headers, "HTTP/1.1 200 OK\r\nx-runtime-cache: hit\r\n\r\n", 41);
                n_cache_hits++;
                deliver(ctx, r, 200, NULL, NULL);
            }
        } else {
            deliver(ctx, r, r->status, r->error, r->final_url);
        }
        req_free(ctx, r);
    }
    if (keep) { // back to the front of the queue, in order
        pthread_mutex_lock(&qlock);
        keep_tail->qnext = done_q;
        done_q = keep;
        if (!done_tail) done_tail = keep_tail;
        pthread_mutex_unlock(&qlock);
    }
}

void net_stats(char *out, size_t n) {
    int pending = 0;
    for (req *r = reqs; r; r = r->next) pending++;
    snprintf(out, n, "%d requests done (%d from cache, %.1f MB), %d pending", n_done, n_cache_hits,
             n_bytes / 1048576.0, pending);
}

// The request is freed when the network thread hands it back; no callback is made.
static JSValue n_http_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    for (req *r = reqs; r; r = r->next) {
        if (r->id == id) {
            pthread_mutex_lock(&qlock);
            r->aborted = true;
            pthread_mutex_unlock(&qlock);
            curl_multi_wakeup(multi);
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
