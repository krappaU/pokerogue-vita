// SPDX-License-Identifier: AGPL-3.0-only
// Images, fonts and a Canvas 2D drawing surface (on plutovg) for the JS CanvasRenderingContext2D.
//
// The JS side keeps the 2D context's state (fillStyle strings, font, textAlign, ...) and calls these
// primitives with already-parsed numbers. Phaser uses Canvas 2D mainly to draw text, which it then
// uploads as a WebGL texture, so this is not on the per-frame hot path.

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "plutovg.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

static JSClassID bitmap_class, canvas_class;

// ---------------------------------------------------------------- bitmaps (decoded images)

// A decoded image. After its pixels are uploaded to the GPU the decoded copy is dropped and only
// the compressed file is kept; the rare canvas drawImage/getImageData of an image decodes it again.
// This keeps main memory to roughly the size of the PNG files instead of all their pixels.
typedef struct {
    int w, h;
    uint8_t *encoded;           // the PNG/JPEG file
    size_t encoded_len;
    uint8_t *rgba;              // decoded pixels (straight alpha), or NULL
    plutovg_surface_t *surface; // premultiplied copy for drawImage, created on demand
} bitmap;

static size_t mem_encoded, mem_decoded;

static void bitmap_drop_pixels(bitmap *b) {
    if (b->rgba) {
        stbi_image_free(b->rgba);
        mem_decoded -= (size_t)b->w * b->h * 4;
        b->rgba = NULL;
    }
    if (b->surface) {
        plutovg_surface_destroy(b->surface);
        mem_decoded -= (size_t)b->w * b->h * 4;
        b->surface = NULL;
    }
}

static const uint8_t *bitmap_pixels(bitmap *b) {
    if (!b->rgba) {
        int w, h, comp;
        b->rgba = stbi_load_from_memory(b->encoded, (int)b->encoded_len, &w, &h, &comp, 4);
        if (b->rgba) mem_decoded += (size_t)b->w * b->h * 4;
    }
    return b->rgba;
}

static void bitmap_finalizer(JSRuntime *rt, JSValue v) {
    bitmap *b = JS_GetOpaque(v, bitmap_class);
    if (!b) return;
    bitmap_drop_pixels(b);
    mem_encoded -= b->encoded_len;
    free(b->encoded);
    free(b);
}

static plutovg_surface_t *bitmap_surface(bitmap *b) {
    if (!b->surface && bitmap_pixels(b)) {
        b->surface = plutovg_surface_create(b->w, b->h);
        if (!b->surface) return NULL; // out of memory: drawImage reports an unsupported source
        mem_decoded += (size_t)b->w * b->h * 4;
        plutovg_convert_rgba_to_argb(plutovg_surface_get_data(b->surface), b->rgba, b->w, b->h,
                                     plutovg_surface_get_stride(b->surface));
    }
    return b->surface;
}

// decodeImage(ArrayBuffer | TypedArray) -> Bitmap {width, height}
static JSValue n_decode_image(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off, tlen, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &tlen, &bpe);
        if (JS_IsException(ab)) return JS_EXCEPTION;
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p) return JS_EXCEPTION;
        p += off;
        len = tlen;
    }
    int w, h, comp;
    uint8_t *px = stbi_load_from_memory(p, (int)len, &w, &h, &comp, 4);
    if (!px) return JS_ThrowTypeError(ctx, "decodeImage: %s", stbi_failure_reason());
    bitmap *b = calloc(1, sizeof *b);
    b->w = w;
    b->h = h;
    b->rgba = px;
    b->encoded = malloc(len);
    memcpy(b->encoded, p, len);
    b->encoded_len = len;
    mem_encoded += len;
    mem_decoded += (size_t)w * h * 4;
    JSValue v = JS_NewObjectClass(ctx, bitmap_class);
    JS_SetOpaque(v, b);
    JS_DefinePropertyValueStr(ctx, v, "width", JS_NewInt32(ctx, w), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, v, "height", JS_NewInt32(ctx, h), JS_PROP_ENUMERABLE);
    return v;
}

// ---------------------------------------------------------------- decoding on a worker thread
// Image loads decode on their own thread (CPU core 1 on the Vita) so that the game's loading screens
// don't stop for every PNG; the JS side (js/dom.js) gets the bitmap on a later frame, as browsers
// deliver images asynchronously too.

typedef struct img_job {
    uint32_t id;
    uint8_t *data;
    size_t len;
    uint8_t *rgba;
    int w, h;
    const char *error;
    struct img_job *next;
} img_job;
static pthread_mutex_t img_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t img_cond = PTHREAD_COND_INITIALIZER;
static img_job *img_in, *img_in_tail, *img_out;
static uint32_t img_next_id = 1;
static bool img_thread_started;

static void *img_thread(void *arg) {
    (void)arg;
    plat_thread_setup("pkr_image", 1, 10); // below the network thread on the same core
    for (;;) {
        pthread_mutex_lock(&img_lock);
        while (!img_in) pthread_cond_wait(&img_cond, &img_lock);
        img_job *j = img_in;
        img_in = j->next;
        if (!img_in) img_in_tail = NULL;
        pthread_mutex_unlock(&img_lock);
        int comp;
        j->rgba = stbi_load_from_memory(j->data, (int)j->len, &j->w, &j->h, &comp, 4);
        if (!j->rgba) j->error = stbi_failure_reason();
        pthread_mutex_lock(&img_lock);
        j->next = img_out;
        img_out = j;
        pthread_mutex_unlock(&img_lock);
    }
    return NULL;
}

// decodeImageAsync(ArrayBuffer | TypedArray) -> job id (the bytes are copied)
static JSValue n_decode_image_async(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off, tlen, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &tlen, &bpe);
        if (JS_IsException(ab)) return JS_EXCEPTION;
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p) return JS_EXCEPTION;
        p += off;
        len = tlen;
    }
    if (!img_thread_started) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024);
        img_thread_started = pthread_create(&t, &attr, img_thread, NULL) == 0;
        if (img_thread_started) pthread_detach(t);
        pthread_attr_destroy(&attr);
        if (!img_thread_started) return JS_NewInt32(ctx, 0); // JS falls back to decodeImage
    }
    img_job *j = calloc(1, sizeof *j);
    if (!j || !(j->data = malloc(len ? len : 1))) {
        free(j);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(j->data, p, len);
    j->len = len;
    pthread_mutex_lock(&img_lock);
    j->id = img_next_id++;
    if (img_in_tail) img_in_tail->next = j;
    else img_in = j;
    img_in_tail = j;
    uint32_t id = j->id;
    pthread_cond_signal(&img_cond);
    pthread_mutex_unlock(&img_lock);
    return JS_NewUint32(ctx, id);
}

// decodeImagePoll() -> [[jobId, Bitmap | null, error], ...] for the decodes finished so far
static JSValue n_decode_image_poll(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    pthread_mutex_lock(&img_lock);
    img_job *done = img_out;
    img_out = NULL;
    pthread_mutex_unlock(&img_lock);
    JSValue arr = JS_NewArray(ctx);
    uint32_t n = 0;
    while (done) {
        img_job *j = done;
        done = j->next;
        JSValue e = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, e, 0, JS_NewUint32(ctx, j->id));
        if (j->rgba) {
            bitmap *b = calloc(1, sizeof *b);
            b->w = j->w, b->h = j->h, b->rgba = j->rgba;
            b->encoded = j->data, b->encoded_len = j->len;
            j->data = NULL;
            mem_encoded += b->encoded_len;
            mem_decoded += (size_t)b->w * b->h * 4;
            JSValue v = JS_NewObjectClass(ctx, bitmap_class);
            JS_SetOpaque(v, b);
            JS_DefinePropertyValueStr(ctx, v, "width", JS_NewInt32(ctx, b->w), JS_PROP_ENUMERABLE);
            JS_DefinePropertyValueStr(ctx, v, "height", JS_NewInt32(ctx, b->h), JS_PROP_ENUMERABLE);
            JS_SetPropertyUint32(ctx, e, 1, v);
            JS_SetPropertyUint32(ctx, e, 2, JS_NULL);
        } else {
            JS_SetPropertyUint32(ctx, e, 1, JS_NULL);
            JS_SetPropertyUint32(ctx, e, 2, JS_NewString(ctx, j->error ? j->error : "decode failed"));
        }
        JS_SetPropertyUint32(ctx, arr, n++, e);
        free(j->data);
        free(j);
    }
    return arr;
}

// imageMemory() -> [compressed bytes kept, decoded bytes alive]
static JSValue n_image_memory(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValue a = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, a, 0, JS_NewFloat64(ctx, (double)mem_encoded));
    JS_SetPropertyUint32(ctx, a, 1, JS_NewFloat64(ctx, (double)mem_decoded));
    return a;
}

void canvas_image_memory(size_t *encoded, size_t *decoded) {
    *encoded = mem_encoded;
    *decoded = mem_decoded;
}


// ---------------------------------------------------------------- fonts

typedef struct {
    char family[64];
    bool bold, italic;
    bool latin; // its unicode-range covers basic Latin (CJK-only faces share family names)
    uint64_t range; // hash of its unicode-range
    plutovg_font_face_t *face;
} font_entry;

static font_entry fonts[32];
static int n_fonts;

// The game registers the same font file under several family names (and several times). Faces are
// shared by content, so each distinct file is held in memory once.
static struct {
    uint64_t hash;
    size_t len;
    plutovg_font_face_t *face;
    const uint8_t *data; // the font file (owned by the face)
    uint32_t cmap;       // offset of its format 4 or 12 character map, 0 if none found
    uint16_t cmap_fmt;
} font_files[16];
static int n_font_files;
static size_t mem_fonts;

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void free_font_data(void *closure) { free(closure); }

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// Finds a Unicode character map (format 12 preferred, else 4) in a TrueType file.
static void find_cmap(const uint8_t *d, size_t n, uint32_t *off, uint16_t *fmt) {
    *off = 0, *fmt = 0;
    if (n < 12) return;
    uint16_t nt = be16(d + 4);
    uint32_t cmap = 0;
    for (uint16_t i = 0; i < nt && 12 + 16u * (i + 1) <= n; i++)
        if (!memcmp(d + 12 + 16 * i, "cmap", 4)) cmap = be32(d + 12 + 16 * i + 8);
    if (!cmap || cmap + 4 > n) return;
    uint16_t ns = be16(d + cmap + 2);
    for (uint16_t i = 0; i < ns && cmap + 4 + 8u * (i + 1) <= n; i++) {
        uint16_t pid = be16(d + cmap + 4 + 8 * i), eid = be16(d + cmap + 4 + 8 * i + 2);
        uint32_t st = cmap + be32(d + cmap + 4 + 8 * i + 4);
        if (st + 4 > n || !(pid == 0 || (pid == 3 && (eid == 1 || eid == 10)))) continue;
        uint16_t f = be16(d + st);
        if (f == 12 && st + 16 <= n) {
            *off = st, *fmt = 12;
            return;
        }
        if (f == 4 && !*fmt && st + 14 <= n) *off = st, *fmt = 4;
    }
}

// Whether the font file maps codepoint cp to a glyph.
static bool cmap_has(const uint8_t *d, size_t n, uint32_t st, uint16_t fmt, uint32_t cp) {
    if (fmt == 12) {
        uint32_t groups = be32(d + st + 12);
        for (uint32_t lo = 0, hi = groups; lo < hi;) {
            uint32_t mid = (lo + hi) / 2, g = st + 16 + 12 * mid;
            if (g + 12 > n) return false;
            if (cp < be32(d + g)) hi = mid;
            else if (cp > be32(d + g + 4)) lo = mid + 1;
            else return be32(d + g + 8) + (cp - be32(d + g)) != 0;
        }
        return false;
    }
    if (fmt != 4 || cp > 0xFFFF) return false;
    uint16_t seg = be16(d + st + 6) / 2;
    uint32_t ends = st + 14, starts = ends + 2u * seg + 2, deltas = starts + 2u * seg, ranges = deltas + 2u * seg;
    if (ranges + 2u * seg > n) return false;
    for (uint16_t i = 0; i < seg; i++) {
        if (cp > be16(d + ends + 2 * i)) continue;
        uint16_t start = be16(d + starts + 2 * i);
        if (cp < start) return false;
        uint16_t ro = be16(d + ranges + 2 * i), glyph;
        if (!ro) {
            glyph = (uint16_t)(cp + be16(d + deltas + 2 * i));
        } else {
            uint32_t ga = ranges + 2 * i + ro + 2 * (cp - start);
            if (ga + 2 > n) return false;
            glyph = be16(d + ga);
            if (glyph) glyph = (uint16_t)(glyph + be16(d + deltas + 2 * i));
        }
        return glyph != 0;
    }
    return false;
}

// Whether face has a glyph for cp (faces whose file isn't known count as having everything).
static bool face_has(plutovg_font_face_t *face, uint32_t cp) {
    for (int i = 0; i < n_font_files; i++)
        if (font_files[i].face == face)
            return !font_files[i].cmap ||
                   cmap_has(font_files[i].data, font_files[i].len, font_files[i].cmap, font_files[i].cmap_fmt, cp);
    return true;
}

// registerFont(family, ArrayBuffer, bold, italic, coversLatin, unicodeRange) -> bool
static JSValue n_register_font(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *fam = JS_ToCString(ctx, argv[0]);
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[1]);
    bool ok = false;
    if (fam && p) {
        uint64_t h = fnv1a(p, len);
        plutovg_font_face_t *face = NULL;
        for (int i = 0; i < n_font_files; i++)
            if (font_files[i].hash == h && font_files[i].len == len) face = plutovg_font_face_reference(font_files[i].face);
        if (!face) {
            void *copy = malloc(len);
            if (copy) {
                memcpy(copy, p, len);
                face = plutovg_font_face_load_from_data(copy, (unsigned)len, 0, free_font_data, copy);
                if (!face) free(copy);
            }
            if (face && n_font_files < 16) {
                font_files[n_font_files].hash = h;
                font_files[n_font_files].len = len;
                font_files[n_font_files].data = copy;
                find_cmap(copy, len, &font_files[n_font_files].cmap, &font_files[n_font_files].cmap_fmt);
                font_files[n_font_files++].face = plutovg_font_face_reference(face);
                mem_fonts += len;
            }
        }
        if (face) {
            // A face registered again for the same family, style and range replaces the earlier one, as in
            // browsers, where the face added last wins: the game adds a newer version of its font
            // file (with the "Lv" glyph at U+F000) after the page's @font-face loaded the old one.
            font_entry e = {.face = face};
            snprintf(e.family, sizeof e.family, "%s", fam);
            lower(e.family);
            e.bold = argc > 2 && JS_ToBool(ctx, argv[2]);
            e.italic = argc > 3 && JS_ToBool(ctx, argv[3]);
            e.latin = argc <= 4 || JS_ToBool(ctx, argv[4]);
            const char *range = argc > 5 ? JS_ToCString(ctx, argv[5]) : NULL;
            e.range = range ? fnv1a((const uint8_t *)range, strlen(range)) : 0;
            JS_FreeCString(ctx, range);
            font_entry *f = NULL;
            for (int i = 0; i < n_fonts && !f; i++)
                if (!strcmp(fonts[i].family, e.family) && fonts[i].bold == e.bold && fonts[i].italic == e.italic &&
                    fonts[i].range == e.range)
                    f = &fonts[i];
            // (the replaced face stays alive: canvases may still be set to it)
            if (!f && n_fonts < 32) f = &fonts[n_fonts++];
            if (f) {
                *f = e;
                ok = true;
            } else {
                plutovg_font_face_destroy(face);
            }
        }
    }
    if (!ok) JS_FreeValue(ctx, JS_GetException(ctx));
    JS_FreeCString(ctx, fam);
    return JS_NewBool(ctx, ok);
}

// Best face for a family: one covering Latin text with the right style, then any Latin face, then
// anything with that name. Per-glyph fallback for mixed CJK text is not done yet.
static plutovg_font_face_t *find_font(const char *family, bool bold, bool italic) {
    char fam[64];
    snprintf(fam, sizeof fam, "%s", family);
    lower(fam);
    font_entry *best = NULL;
    int best_score = -1;
    for (int i = 0; i < n_fonts; i++) {
        if (strcmp(fonts[i].family, fam)) continue;
        int score = (fonts[i].latin ? 2 : 0) + (fonts[i].bold == bold && fonts[i].italic == italic ? 1 : 0);
        if (score > best_score) {
            best = &fonts[i];
            best_score = score;
        }
    }
    return best ? best->face : NULL;
}

// ---------------------------------------------------------------- canvas

typedef struct {
    plutovg_surface_t *surface;
    plutovg_canvas_t *canvas;
    int w, h;
    float fill[4], stroke[4];
    plutovg_font_face_t *face;
    char family[64]; // lower case, for glyph fallback within the family first
    float font_size;
    // Pixel memory is freed once the canvas has been uploaded to the GPU, unless the canvas is one
    // the game reads back or draws images into ("keep"). Phaser redraws text canvases from scratch
    // whenever their text changes, so a released canvas is recreated blank on its next use.
    bool keep, released;
} c2d;

static size_t mem_canvas;
static int n_canvas;
static bool release_canvases = true;
static int n_released, n_keep_getimage, n_keep_draw, n_keep_put, n_keep_source, n_uploads;

// Allocates the canvas's pixels. On failure (out of memory) the canvas stays without pixels
// ("released") and drawing calls on it do nothing.
static bool c2d_alloc(c2d *c, int w, int h) {
    c->released = false;
    if (c->canvas) plutovg_canvas_destroy(c->canvas);
    if (c->surface) {
        mem_canvas -= (size_t)c->w * c->h * 4;
        plutovg_surface_destroy(c->surface);
    } else if (!c->w) {
        n_canvas++; // first allocation (not a re-allocation after release)
    }
    c->canvas = NULL;
    c->w = w > 0 ? w : 1;
    c->h = h > 0 ? h : 1;
    c->surface = plutovg_surface_create(c->w, c->h);
    if (!c->surface) {
        static int warned;
        if (warned++ < 5) host_log("[canvas] out of memory for a %dx%d canvas", c->w, c->h);
        c->released = true;
        return false;
    }
    mem_canvas += (size_t)c->w * c->h * 4;
    c->canvas = plutovg_canvas_create(c->surface);
    if (c->face) plutovg_canvas_set_font(c->canvas, c->face, c->font_size);
    return true;
}

static void canvas_finalizer(JSRuntime *rt, JSValue v) {
    c2d *c = JS_GetOpaque(v, canvas_class);
    if (!c) return;
    if (c->surface) mem_canvas -= (size_t)c->w * c->h * 4;
    n_canvas--;
    if (c->canvas) plutovg_canvas_destroy(c->canvas);
    if (c->surface) plutovg_surface_destroy(c->surface);
    free(c);
}

static inline double nd(JSContext *ctx, JSValueConst v) {
    double d = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    JS_ToFloat64(ctx, &d, v);
    return d;
}

#define CFN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
#define SELF                                            \
    c2d *c = JS_GetOpaque(this_val, canvas_class);      \
    if (!c) return JS_ThrowTypeError(ctx, "not a Canvas2D"); \
    if (c->released && !c2d_alloc(c, c->w, c->h)) return JS_UNDEFINED;
// For calls that don't touch pixels: no (re-)allocation of a released canvas.
#define SELF_NOALLOC                                    \
    c2d *c = JS_GetOpaque(this_val, canvas_class);      \
    if (!c) return JS_ThrowTypeError(ctx, "not a Canvas2D");
#define F(i) ((float)nd(ctx, argv[i]))

CFN(c_resize) {
    SELF_NOALLOC;
    // Like browsers, setting width/height wipes the canvas, so earlier read-backs no longer pin its
    // pixels (Phaser's canvas pool hands measured canvases on to text objects). The pixels are
    // allocated on the next drawing call: text objects get resized while only being measured.
    c->keep = false;
    int w = (int)nd(ctx, argv[0]), h = (int)nd(ctx, argv[1]);
    if (c->surface) {
        mem_canvas -= (size_t)c->w * c->h * 4;
        plutovg_canvas_destroy(c->canvas);
        plutovg_surface_destroy(c->surface);
        c->canvas = NULL;
        c->surface = NULL;
    }
    c->w = w > 0 ? w : 1;
    c->h = h > 0 ? h : 1;
    c->released = true;
    return JS_UNDEFINED;
}
CFN(c_width) { SELF_NOALLOC; return JS_NewInt32(ctx, c->w); }
CFN(c_height) { SELF_NOALLOC; return JS_NewInt32(ctx, c->h); }
CFN(c_save) { SELF; plutovg_canvas_save(c->canvas); return JS_UNDEFINED; }
CFN(c_restore) { SELF; plutovg_canvas_restore(c->canvas); return JS_UNDEFINED; }

CFN(c_set_transform) {
    SELF;
    plutovg_matrix_t m = {F(0), F(1), F(2), F(3), F(4), F(5)};
    plutovg_canvas_set_matrix(c->canvas, &m);
    return JS_UNDEFINED;
}
CFN(c_transform) {
    SELF;
    plutovg_matrix_t m = {F(0), F(1), F(2), F(3), F(4), F(5)};
    plutovg_canvas_transform(c->canvas, &m);
    return JS_UNDEFINED;
}
CFN(c_translate) { SELF; plutovg_canvas_translate(c->canvas, F(0), F(1)); return JS_UNDEFINED; }
CFN(c_scale) { SELF; plutovg_canvas_scale(c->canvas, F(0), F(1)); return JS_UNDEFINED; }
CFN(c_rotate) { SELF; plutovg_canvas_rotate(c->canvas, F(0)); return JS_UNDEFINED; }
CFN(c_get_transform) {
    SELF;
    plutovg_matrix_t m;
    plutovg_canvas_get_matrix(c->canvas, &m);
    JSValue a = JS_NewArray(ctx);
    float v[6] = {m.a, m.b, m.c, m.d, m.e, m.f};
    for (int i = 0; i < 6; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewFloat64(ctx, v[i]));
    return a;
}

CFN(c_set_fill) {
    SELF_NOALLOC;
    for (int i = 0; i < 4; i++) c->fill[i] = F(i);
    return JS_UNDEFINED;
}
CFN(c_set_stroke) {
    SELF_NOALLOC;
    for (int i = 0; i < 4; i++) c->stroke[i] = F(i);
    return JS_UNDEFINED;
}
CFN(c_set_alpha) { SELF; plutovg_canvas_set_opacity(c->canvas, F(0)); return JS_UNDEFINED; }
CFN(c_set_composite) { SELF; plutovg_canvas_set_operator(c->canvas, (plutovg_operator_t)(int)nd(ctx, argv[0])); return JS_UNDEFINED; }
CFN(c_set_line_width) { SELF; plutovg_canvas_set_line_width(c->canvas, F(0)); return JS_UNDEFINED; }
CFN(c_set_line_cap) { SELF; plutovg_canvas_set_line_cap(c->canvas, (plutovg_line_cap_t)(int)nd(ctx, argv[0])); return JS_UNDEFINED; }
CFN(c_set_line_join) { SELF; plutovg_canvas_set_line_join(c->canvas, (plutovg_line_join_t)(int)nd(ctx, argv[0])); return JS_UNDEFINED; }
CFN(c_set_miter_limit) { SELF; plutovg_canvas_set_miter_limit(c->canvas, F(0)); return JS_UNDEFINED; }

// setFont(family, sizePx, bold, italic) -> bool (false: family not registered, a fallback is used)
CFN(c_set_font) {
    SELF_NOALLOC;
    const char *fam = JS_ToCString(ctx, argv[0]);
    bool bold = JS_ToBool(ctx, argv[2]), italic = JS_ToBool(ctx, argv[3]);
    plutovg_font_face_t *face = fam ? find_font(fam, bold, italic) : NULL;
    snprintf(c->family, sizeof c->family, "%s", fam ? fam : "");
    lower(c->family);
    JS_FreeCString(ctx, fam);
    bool found = face != NULL;
    if (!face && n_fonts) face = fonts[0].face;
    c->face = face;
    c->font_size = F(1);
    if (face && c->canvas) plutovg_canvas_set_font(c->canvas, face, c->font_size);
    return JS_NewBool(ctx, found);
}

static inline void use_fill(c2d *c) { plutovg_canvas_set_rgba(c->canvas, c->fill[0], c->fill[1], c->fill[2], c->fill[3]); }
static inline void use_stroke(c2d *c) { plutovg_canvas_set_rgba(c->canvas, c->stroke[0], c->stroke[1], c->stroke[2], c->stroke[3]); }

CFN(c_clear_rect) {
    SELF;
    plutovg_canvas_save(c->canvas);
    plutovg_canvas_set_operator(c->canvas, PLUTOVG_OPERATOR_CLEAR);
    plutovg_canvas_set_opacity(c->canvas, 1);
    plutovg_canvas_fill_rect(c->canvas, F(0), F(1), F(2), F(3));
    plutovg_canvas_restore(c->canvas);
    return JS_UNDEFINED;
}
CFN(c_fill_rect) { SELF; use_fill(c); plutovg_canvas_fill_rect(c->canvas, F(0), F(1), F(2), F(3)); return JS_UNDEFINED; }
CFN(c_stroke_rect) { SELF; use_stroke(c); plutovg_canvas_stroke_rect(c->canvas, F(0), F(1), F(2), F(3)); return JS_UNDEFINED; }

CFN(c_begin_path) { SELF; plutovg_canvas_new_path(c->canvas); return JS_UNDEFINED; }
CFN(c_close_path) { SELF; plutovg_canvas_close_path(c->canvas); return JS_UNDEFINED; }
CFN(c_move_to) { SELF; plutovg_canvas_move_to(c->canvas, F(0), F(1)); return JS_UNDEFINED; }
CFN(c_line_to) { SELF; plutovg_canvas_line_to(c->canvas, F(0), F(1)); return JS_UNDEFINED; }
CFN(c_quad_to) { SELF; plutovg_canvas_quad_to(c->canvas, F(0), F(1), F(2), F(3)); return JS_UNDEFINED; }
CFN(c_cubic_to) { SELF; plutovg_canvas_cubic_to(c->canvas, F(0), F(1), F(2), F(3), F(4), F(5)); return JS_UNDEFINED; }
CFN(c_rect) { SELF; plutovg_canvas_rect(c->canvas, F(0), F(1), F(2), F(3)); return JS_UNDEFINED; }
CFN(c_arc) {
    SELF;
    plutovg_canvas_arc(c->canvas, F(0), F(1), F(2), F(3), F(4), argc > 5 && JS_ToBool(ctx, argv[5]));
    return JS_UNDEFINED;
}
CFN(c_ellipse) { SELF; plutovg_canvas_ellipse(c->canvas, F(0), F(1), F(2), F(3)); return JS_UNDEFINED; }
CFN(c_fill) { SELF; use_fill(c); plutovg_canvas_fill_preserve(c->canvas); return JS_UNDEFINED; }
CFN(c_stroke) { SELF; use_stroke(c); plutovg_canvas_stroke_preserve(c->canvas); return JS_UNDEFINED; }
CFN(c_clip) { SELF; plutovg_canvas_clip_preserve(c->canvas); return JS_UNDEFINED; }

// Browsers draw a space for no-break and typographic spaces the font has no glyph for, and nothing
// for zero-width characters; the game's pixel fonts lack them (the shop puts U+00A0 between an
// item's name and its price), and plutovg would draw the missing-glyph box. Rewrites them in place
// (the result is never longer) and returns the new length.
static size_t fix_spaces(char *s, size_t len) {
    if (!memchr(s, 0xC2, len) && !memchr(s, 0xE2, len) && !memchr(s, 0xEF, len)) return len;
    size_t o = 0;
    for (size_t i = 0; i < len;) {
        unsigned char a = s[i], b = i + 1 < len ? s[i + 1] : 0, c = i + 2 < len ? s[i + 2] : 0;
        if (a == 0xC2 && b == 0xA0) { s[o++] = ' '; i += 2; continue; }         // U+00A0
        if (a == 0xC2 && b == 0xAD) { i += 2; continue; }                       // U+00AD soft hyphen
        if (a == 0xE2 && b == 0x80) {
            if (c >= 0x80 && c <= 0x8A) { s[o++] = ' '; i += 3; continue; }      // U+2000-200A
            if (c >= 0x8B && c <= 0x8D) { i += 3; continue; }                    // U+200B-200D
            if (c == 0xAF) { s[o++] = ' '; i += 3; continue; }                   // U+202F
        }
        if (a == 0xE2 && b == 0x81 && c == 0xA0) { i += 3; continue; }           // U+2060
        if (a == 0xEF && b == 0xBB && c == 0xBF) { i += 3; continue; }           // U+FEFF
        s[o++] = s[i++];
    }
    return o;
}

// fontInfo(codepoint) -> "family[b/i/latin] file#k has=0/1; ..." (diagnostics)
static JSValue n_font_info(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    uint32_t cp = 0;
    if (argc) JS_ToUint32(ctx, &cp, argv[0]);
    char out[2048];
    size_t o = 0;
    o += snprintf(out + o, sizeof out - o, "%d files, %d faces:", n_font_files, n_fonts);
    for (int i = 0; i < n_fonts && o < sizeof out - 96; i++) {
        int k = -1;
        for (int j = 0; j < n_font_files; j++)
            if (font_files[j].face == fonts[i].face) k = j;
        o += snprintf(out + o, sizeof out - o, " %s[%d%d%d] file%d(fmt%d) has=%d;", fonts[i].family, fonts[i].bold, fonts[i].italic,
                      fonts[i].latin, k, k >= 0 ? font_files[k].cmap_fmt : -1, face_has(fonts[i].face, cp));
    }
    return JS_NewString(ctx, out);
}

// Per-character font fallback, as browsers do: a character the canvas's face has no glyph for is
// drawn with another registered face that has one, from the same family first (the game's
// unicode-range faces for CJK, Thai, Devanagari), then any (PokeRogue's text now uses U+F000 for
// "Lv", which only the newer copy of its font has).
static plutovg_font_face_t *face_for(c2d *c, uint32_t cp) {
    if (cp < 0x80 || face_has(c->face, cp)) return c->face; // (ASCII: the game's fonts all have it)
    for (int pass = 0; pass < 2; pass++)
        for (int i = n_fonts - 1; i >= 0; i--)
            if ((pass || !strcmp(fonts[i].family, c->family)) && fonts[i].face != c->face && face_has(fonts[i].face, cp))
                return fonts[i].face;
    return c->face;
}

static uint32_t utf8_next(const char *s, size_t len, size_t *i) {
    const uint8_t *p = (const uint8_t *)s;
    uint32_t c = p[*i];
    int n = c < 0x80 ? 0 : c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
    if (n) c &= 0x3F >> n;
    (*i)++;
    for (; n && *i < len; n--, (*i)++) c = c << 6 | (p[*i] & 0x3F);
    return c;
}

// Splits s into runs of one face: calls fn(face, start, end, closure) for each; returns false (no
// call) when the canvas's own face covers it all. Plain ASCII never needs another face.
typedef void (*run_fn)(c2d *c, plutovg_font_face_t *face, const char *s, size_t n, void *closure);
static bool for_each_run(c2d *c, const char *s, size_t len, run_fn fn, void *closure) {
    size_t i = 0;
    while (i < len && (uint8_t)s[i] < 0x80) i++;
    if (i == len) return false;
    bool other = false;
    for (size_t j = 0; j < len;) {
        size_t k = j;
        if (face_for(c, utf8_next(s, len, &k)) != c->face) {
            other = true;
            break;
        }
        j = k;
    }
    if (!other) return false;
    size_t start = 0;
    plutovg_font_face_t *cur = NULL;
    for (size_t j = 0; j < len;) {
        size_t k = j;
        plutovg_font_face_t *f = face_for(c, utf8_next(s, len, &k));
        if (cur && f != cur) {
            fn(c, cur, s + start, j - start, closure);
            start = j;
        }
        cur = f;
        j = k;
    }
    if (cur) fn(c, cur, s + start, len - start, closure);
    return true;
}

typedef struct {
    float x, y;
    bool stroke;
} draw_run_state;
static void draw_run(c2d *c, plutovg_font_face_t *face, const char *s, size_t n, void *closure) {
    draw_run_state *st = closure;
    plutovg_canvas_set_font(c->canvas, face, c->font_size);
    st->x += st->stroke ? plutovg_canvas_stroke_text(c->canvas, s, (int)n, PLUTOVG_TEXT_ENCODING_UTF8, st->x, st->y)
                        : plutovg_canvas_fill_text(c->canvas, s, (int)n, PLUTOVG_TEXT_ENCODING_UTF8, st->x, st->y);
}

typedef struct {
    float width, minx, maxx;
    bool any;
} measure_run_state;
static void measure_run(c2d *c, plutovg_font_face_t *face, const char *s, size_t n, void *closure) {
    measure_run_state *st = closure;
    plutovg_rect_t e = {0};
    float w = plutovg_font_face_text_extents(face, c->font_size, s, (int)n, PLUTOVG_TEXT_ENCODING_UTF8, &e);
    if (e.w > 0 || e.h > 0) {
        float a = st->width + e.x, b = st->width + e.x + e.w;
        if (!st->any || a < st->minx) st->minx = a;
        if (!st->any || b > st->maxx) st->maxx = b;
        st->any = true;
    }
    st->width += w;
}

static JSValue text_op(JSContext *ctx, JSValueConst this_val, JSValueConst *argv, bool stroke) {
    SELF;
    if (!c->face) return JS_UNDEFINED;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    len = fix_spaces((char *)s, len);
    draw_run_state st = {F(1), F(2), stroke};
    if (stroke) use_stroke(c);
    else use_fill(c);
    if (for_each_run(c, s, len, draw_run, &st)) {
        plutovg_canvas_set_font(c->canvas, c->face, c->font_size);
    } else if (stroke) {
        plutovg_canvas_stroke_text(c->canvas, s, (int)len, PLUTOVG_TEXT_ENCODING_UTF8, F(1), F(2));
    } else {
        use_fill(c);
        plutovg_canvas_fill_text(c->canvas, s, (int)len, PLUTOVG_TEXT_ENCODING_UTF8, F(1), F(2));
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}
CFN(c_fill_text) { return text_op(ctx, this_val, argv, false); }
CFN(c_stroke_text) { return text_op(ctx, this_val, argv, true); }

// measureText(str) -> [width, ascent, descent, left, right]
CFN(c_measure_text) {
    SELF_NOALLOC;
    JSValue a = JS_NewArray(ctx);
    float width = 0, ascent = 0, descent = 0, gap = 0;
    plutovg_rect_t ext = {0};
    if (c->face) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        if (!s) {
            JS_FreeValue(ctx, a);
            return JS_EXCEPTION;
        }
        len = fix_spaces((char *)s, len);
        measure_run_state ms = {0};
        if (for_each_run(c, s, len, measure_run, &ms)) {
            width = ms.width;
            ext.x = ms.any ? ms.minx : 0, ext.w = ms.any ? ms.maxx - ms.minx : 0;
        } else {
            width = plutovg_font_face_text_extents(c->face, c->font_size, s, (int)len, PLUTOVG_TEXT_ENCODING_UTF8, &ext);
        }
        plutovg_font_face_get_metrics(c->face, c->font_size, &ascent, &descent, &gap, NULL);
        JS_FreeCString(ctx, s);
    }
    float v[5] = {width, ascent, -descent, -ext.x, ext.x + ext.w};
    for (int i = 0; i < 5; i++) JS_SetPropertyUint32(ctx, a, i, JS_NewFloat64(ctx, v[i]));
    return a;
}

// drawImage(src: Bitmap | Canvas2D, sx, sy, sw, sh, dx, dy, dw, dh, smoothing)
CFN(c_draw_image) {
    SELF;
    if (!c->keep) n_keep_draw++;
    c->keep = true;
    plutovg_surface_t *src = NULL;
    bitmap *b = JS_GetOpaque(argv[0], bitmap_class);
    c2d *sc = b ? NULL : JS_GetOpaque(argv[0], canvas_class);
    if (b) src = bitmap_surface(b);
    else if (sc) {
        if (!sc->keep) n_keep_source++;
        sc->keep = true; // read back as an image source: keep its pixels from now on
        if (sc->released && !c2d_alloc(sc, sc->w, sc->h)) return JS_UNDEFINED;
        src = sc->surface;
    }
    if (!src) return JS_ThrowTypeError(ctx, "drawImage: unsupported source");
    float sx = F(1), sy = F(2), sw = F(3), sh = F(4), dx = F(5), dy = F(6), dw = F(7), dh = F(8);
    if (sw == 0 || sh == 0) return JS_UNDEFINED;
    float ax = dw / sw, ay = dh / sh;
    plutovg_matrix_t m = {ax, 0, 0, ay, dx - sx * ax, dy - sy * ay};
    plutovg_canvas_save(c->canvas);
    plutovg_canvas_set_texture(c->canvas, src, PLUTOVG_TEXTURE_TYPE_PLAIN, 1.0f, &m);
    plutovg_canvas_fill_rect(c->canvas, dx, dy, dw, dh);
    plutovg_canvas_restore(c->canvas);
    return JS_UNDEFINED;
}

// getImageData(x, y, w, h) -> ArrayBuffer (RGBA, straight alpha)
// ArrayBuffer storage callback for getImageData's buffers: size 0 frees, otherwise reallocates.
static void *image_data_realloc(JSRuntime *rt, void *opaque, void *ptr, size_t size) {
    (void)rt, (void)opaque;
    if (!size) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, size);
}

CFN(c_get_image_data) {
    SELF;
    if (!c->keep) n_keep_getimage++;
    c->keep = true;
    int x = (int)F(0), y = (int)F(1), w = (int)F(2), h = (int)F(3);
    if (w <= 0 || h <= 0) return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    uint8_t *out = calloc((size_t)w * h, 4);
    if (!out) return JS_ThrowOutOfMemory(ctx);
    int stride = plutovg_surface_get_stride(c->surface);
    const uint8_t *data = plutovg_surface_get_data(c->surface);
    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= c->h) continue;
        int x0 = x < 0 ? 0 : x, x1 = x + w > c->w ? c->w : x + w;
        if (x1 <= x0) continue;
        plutovg_convert_argb_to_rgba(out + ((size_t)row * w + (x0 - x)) * 4, data + (size_t)sy * stride + x0 * 4,
                                     x1 - x0, 1, (x1 - x0) * 4);
    }
    // Handed over without a copy: these can be several MB, and memory is tight on the Vita.
    return JS_NewArrayBuffer(ctx, out, (size_t)w * h * 4, 0, image_data_realloc, NULL, false);
}

// putImageData(ArrayBuffer|TypedArray rgba, w, h, dx, dy)
CFN(c_put_image_data) {
    SELF;
    if (!c->keep) n_keep_put++;
    c->keep = true;
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[0]);
    if (!p) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        size_t off, tlen, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &tlen, &bpe);
        if (JS_IsException(ab)) return JS_EXCEPTION;
        p = JS_GetArrayBuffer(ctx, &total, ab);
        JS_FreeValue(ctx, ab);
        if (!p) return JS_EXCEPTION;
        p += off;
        len = tlen;
    }
    int w = (int)F(1), h = (int)F(2), dx = (int)F(3), dy = (int)F(4);
    if (len < (size_t)w * h * 4) return JS_ThrowRangeError(ctx, "putImageData: data too short");
    int stride = plutovg_surface_get_stride(c->surface);
    uint8_t *data = plutovg_surface_get_data(c->surface);
    for (int row = 0; row < h; row++) {
        int ty = dy + row;
        if (ty < 0 || ty >= c->h) continue;
        int x0 = dx < 0 ? 0 : dx, x1 = dx + w > c->w ? c->w : dx + w;
        if (x1 <= x0) continue;
        plutovg_convert_rgba_to_argb(data + (size_t)ty * stride + x0 * 4, p + ((size_t)row * w + (x0 - dx)) * 4,
                                     x1 - x0, 1, (x1 - x0) * 4);
    }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry canvas_funcs[] = {
    JS_CFUNC_DEF("resize", 2, c_resize), JS_CFUNC_DEF("width", 0, c_width), JS_CFUNC_DEF("height", 0, c_height),
    JS_CFUNC_DEF("save", 0, c_save), JS_CFUNC_DEF("restore", 0, c_restore),
    JS_CFUNC_DEF("setTransform", 6, c_set_transform), JS_CFUNC_DEF("transform", 6, c_transform),
    JS_CFUNC_DEF("getTransform", 0, c_get_transform), JS_CFUNC_DEF("translate", 2, c_translate),
    JS_CFUNC_DEF("scale", 2, c_scale), JS_CFUNC_DEF("rotate", 1, c_rotate),
    JS_CFUNC_DEF("setFill", 4, c_set_fill), JS_CFUNC_DEF("setStroke", 4, c_set_stroke),
    JS_CFUNC_DEF("setAlpha", 1, c_set_alpha), JS_CFUNC_DEF("setComposite", 1, c_set_composite),
    JS_CFUNC_DEF("setLineWidth", 1, c_set_line_width), JS_CFUNC_DEF("setLineCap", 1, c_set_line_cap),
    JS_CFUNC_DEF("setLineJoin", 1, c_set_line_join), JS_CFUNC_DEF("setMiterLimit", 1, c_set_miter_limit),
    JS_CFUNC_DEF("setFont", 4, c_set_font), JS_CFUNC_DEF("clearRect", 4, c_clear_rect),
    JS_CFUNC_DEF("fillRect", 4, c_fill_rect), JS_CFUNC_DEF("strokeRect", 4, c_stroke_rect),
    JS_CFUNC_DEF("beginPath", 0, c_begin_path), JS_CFUNC_DEF("closePath", 0, c_close_path),
    JS_CFUNC_DEF("moveTo", 2, c_move_to), JS_CFUNC_DEF("lineTo", 2, c_line_to),
    JS_CFUNC_DEF("quadraticCurveTo", 4, c_quad_to), JS_CFUNC_DEF("bezierCurveTo", 6, c_cubic_to),
    JS_CFUNC_DEF("rect", 4, c_rect), JS_CFUNC_DEF("arc", 6, c_arc), JS_CFUNC_DEF("ellipse", 4, c_ellipse),
    JS_CFUNC_DEF("fill", 0, c_fill), JS_CFUNC_DEF("stroke", 0, c_stroke), JS_CFUNC_DEF("clip", 0, c_clip),
    JS_CFUNC_DEF("fillText", 3, c_fill_text), JS_CFUNC_DEF("strokeText", 3, c_stroke_text),
    JS_CFUNC_DEF("measureText", 1, c_measure_text), JS_CFUNC_DEF("drawImage", 9, c_draw_image),
    JS_CFUNC_DEF("getImageData", 4, c_get_image_data), JS_CFUNC_DEF("putImageData", 5, c_put_image_data),
};

static JSValue canvas_proto;

// canvasStats() -> diagnostics about canvas pixel releases
static JSValue n_canvas_stats(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    char buf[256];
    snprintf(buf, sizeof buf, "canvases %d, uploads %d, released %d, kept for: getImageData %d, drawImage into %d, putImageData %d, used as source %d",
             n_canvas, n_uploads, n_released, n_keep_getimage, n_keep_draw, n_keep_put, n_keep_source);
    return JS_NewString(ctx, buf);
}

// setCanvasRelease(bool): free canvas pixels after GPU upload (default on)
static JSValue n_set_canvas_release(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    release_canvases = JS_ToBool(ctx, argv[0]);
    return JS_UNDEFINED;
}

static JSValue n_create_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    c2d *c = calloc(1, sizeof *c);
    c->fill[3] = c->stroke[3] = 1;
    c2d_alloc(c, (int)nd(ctx, argv[0]), (int)nd(ctx, argv[1]));
    JSValue v = JS_NewObjectProtoClass(ctx, canvas_proto, canvas_class);
    JS_SetOpaque(v, c);
    return v;
}

// ---------------------------------------------------------------- pixel sources for WebGL

// The decoded-image (Bitmap) object behind a texture source (Bitmap or <img>), with a new
// reference, or JS_UNDEFINED. gl.c keeps it to re-upload evicted textures from the image data.
JSValue bitmap_ref(JSContext *ctx, JSValueConst obj) {
    if (JS_GetOpaque(obj, bitmap_class)) return JS_DupValue(ctx, obj);
    if (!JS_IsObject(obj) || JS_GetOpaque(obj, canvas_class)) return JS_UNDEFINED;
    JSValue inner = JS_GetPropertyStr(ctx, obj, "_native");
    if (JS_GetOpaque(inner, bitmap_class)) return inner;
    JS_FreeValue(ctx, inner);
    return JS_UNDEFINED;
}

const uint8_t *pixels_from_js(JSContext *ctx, JSValueConst obj, int *w, int *h, bool *owned) {
    *owned = false;
    bitmap *b = JS_GetOpaque(obj, bitmap_class);
    c2d *c = b ? NULL : JS_GetOpaque(obj, canvas_class);
    JSValue inner = JS_UNDEFINED;
    if (!b && !c && JS_IsObject(obj)) {
        // Elements keep their native backing object in `_native`.
        inner = JS_GetPropertyStr(ctx, obj, "_native");
        b = JS_GetOpaque(inner, bitmap_class);
        if (!b) c = JS_GetOpaque(inner, canvas_class);
    }
    JS_FreeValue(ctx, inner); // the element keeps it alive
    if (b) {
        // The GPU keeps its own copy, so hand the decoded pixels over and let the caller free them.
        const uint8_t *px = bitmap_pixels(b);
        if (!px) return NULL;
        *w = b->w;
        *h = b->h;
        b->rgba = NULL;
        mem_decoded -= (size_t)b->w * b->h * 4;
        *owned = true;
        return px;
    }
    if (c) {
        *w = c->w;
        *h = c->h;
        uint8_t *out = malloc((size_t)c->w * c->h * 4);
        if (!out) return NULL;
        if (c->released) {
            memset(out, 0, (size_t)c->w * c->h * 4);
        } else {
            plutovg_convert_argb_to_rgba(out, plutovg_surface_get_data(c->surface), c->w, c->h,
                                         plutovg_surface_get_stride(c->surface));
            // This is a GPU upload: the GPU now has the pixels. Free ours unless the game needs them.
            n_uploads++;
            if (!c->keep && release_canvases) {
                n_released++;
                plutovg_canvas_destroy(c->canvas);
                plutovg_surface_destroy(c->surface);
                mem_canvas -= (size_t)c->w * c->h * 4;
                c->canvas = NULL;
                c->surface = NULL;
                c->released = true;
            }
        }
        *owned = true;
        return out;
    }
    // ImageData-like {width, height, data}
    if (!JS_IsObject(obj)) return NULL;
    JSValue dv = JS_GetPropertyStr(ctx, obj, "data");
    JSValue wv = JS_GetPropertyStr(ctx, obj, "width"), hv = JS_GetPropertyStr(ctx, obj, "height");
    const uint8_t *px = NULL;
    if (JS_IsObject(dv)) {
        size_t off, len, bpe, total;
        JSValue ab = JS_GetTypedArrayBuffer(ctx, dv, &off, &len, &bpe);
        if (!JS_IsException(ab)) {
            uint8_t *p = JS_GetArrayBuffer(ctx, &total, ab);
            int32_t iw = 0, ih = 0;
            JS_ToInt32(ctx, &iw, wv);
            JS_ToInt32(ctx, &ih, hv);
            if (p && len >= (size_t)iw * ih * 4) {
                *w = iw;
                *h = ih;
                px = p + off;
            }
            JS_FreeValue(ctx, ab);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    JS_FreeValue(ctx, dv);
    JS_FreeValue(ctx, wv);
    JS_FreeValue(ctx, hv);
    return px;
}

void canvas_register(JSContext *ctx, JSValue native) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &bitmap_class);
    JS_NewClassID(rt, &canvas_class);
    JSClassDef bd = {.class_name = "Bitmap", .finalizer = bitmap_finalizer};
    JSClassDef cd = {.class_name = "Canvas2D", .finalizer = canvas_finalizer};
    JS_NewClass(rt, bitmap_class, &bd);
    JS_NewClass(rt, canvas_class, &cd);
    canvas_proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, canvas_proto, canvas_funcs, sizeof canvas_funcs / sizeof *canvas_funcs);
    JS_SetPropertyStr(ctx, native, "decodeImage", JS_NewCFunction(ctx, n_decode_image, "decodeImage", 1));
    JS_SetPropertyStr(ctx, native, "fontInfo", JS_NewCFunction(ctx, n_font_info, "fontInfo", 1));
    JS_SetPropertyStr(ctx, native, "decodeImageAsync", JS_NewCFunction(ctx, n_decode_image_async, "decodeImageAsync", 1));
    JS_SetPropertyStr(ctx, native, "decodeImagePoll", JS_NewCFunction(ctx, n_decode_image_poll, "decodeImagePoll", 0));
    JS_SetPropertyStr(ctx, native, "imageMemory", JS_NewCFunction(ctx, n_image_memory, "imageMemory", 0));
    JS_SetPropertyStr(ctx, native, "registerFont", JS_NewCFunction(ctx, n_register_font, "registerFont", 5));
    JS_SetPropertyStr(ctx, native, "createCanvas2D", JS_NewCFunction(ctx, n_create_canvas, "createCanvas2D", 2));
    JS_SetPropertyStr(ctx, native, "setCanvasRelease", JS_NewCFunction(ctx, n_set_canvas_release, "setCanvasRelease", 1));
    JS_SetPropertyStr(ctx, native, "canvasStats", JS_NewCFunction(ctx, n_canvas_stats, "canvasStats", 0));
}

void canvas_memory(size_t *canvas_bytes, int *canvas_count, size_t *font_bytes) {
    *canvas_bytes = mem_canvas;
    *canvas_count = n_canvas;
    *font_bytes = mem_fonts;
}
