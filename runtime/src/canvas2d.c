// SPDX-License-Identifier: AGPL-3.0-only
// Images, fonts and a Canvas 2D drawing surface (on plutovg) for the JS CanvasRenderingContext2D.
//
// The JS side keeps the 2D context's state (fillStyle strings, font, textAlign, ...) and calls these
// primitives with already-parsed numbers. Phaser uses Canvas 2D mainly to draw text, which it then
// uploads as a WebGL texture, so this is not on the per-frame hot path.

#include <ctype.h>
#include <math.h>
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

typedef struct {
    int w, h;
    uint8_t *rgba;              // straight alpha
    plutovg_surface_t *surface; // premultiplied copy for drawImage, created on demand
} bitmap;

static void bitmap_finalizer(JSRuntime *rt, JSValue v) {
    bitmap *b = JS_GetOpaque(v, bitmap_class);
    if (!b) return;
    stbi_image_free(b->rgba);
    if (b->surface) plutovg_surface_destroy(b->surface);
    free(b);
}

static plutovg_surface_t *bitmap_surface(bitmap *b) {
    if (!b->surface) {
        b->surface = plutovg_surface_create(b->w, b->h);
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
    JSValue v = JS_NewObjectClass(ctx, bitmap_class);
    JS_SetOpaque(v, b);
    JS_DefinePropertyValueStr(ctx, v, "width", JS_NewInt32(ctx, w), JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx, v, "height", JS_NewInt32(ctx, h), JS_PROP_ENUMERABLE);
    return v;
}

// ---------------------------------------------------------------- fonts

typedef struct {
    char family[64];
    bool bold, italic;
    bool latin; // its unicode-range covers basic Latin (CJK-only faces share family names)
    plutovg_font_face_t *face;
} font_entry;

static font_entry fonts[32];
static int n_fonts;

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void free_font_data(void *closure) { free(closure); }

// registerFont(family, ArrayBuffer, bold, italic, coversLatin) -> bool
static JSValue n_register_font(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    const char *fam = JS_ToCString(ctx, argv[0]);
    size_t len = 0;
    uint8_t *p = JS_GetArrayBuffer(ctx, &len, argv[1]);
    bool ok = false;
    if (fam && p && n_fonts < 32) {
        void *copy = malloc(len);
        memcpy(copy, p, len);
        plutovg_font_face_t *face = plutovg_font_face_load_from_data(copy, (unsigned)len, 0, free_font_data, copy);
        if (face) {
            font_entry *f = &fonts[n_fonts++];
            snprintf(f->family, sizeof f->family, "%s", fam);
            lower(f->family);
            f->bold = argc > 2 && JS_ToBool(ctx, argv[2]);
            f->italic = argc > 3 && JS_ToBool(ctx, argv[3]);
            f->latin = argc <= 4 || JS_ToBool(ctx, argv[4]);
            f->face = face;
            ok = true;
        } else {
            free(copy);
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
    float font_size;
} c2d;

static void c2d_alloc(c2d *c, int w, int h) {
    if (c->canvas) plutovg_canvas_destroy(c->canvas);
    if (c->surface) plutovg_surface_destroy(c->surface);
    c->w = w > 0 ? w : 1;
    c->h = h > 0 ? h : 1;
    c->surface = plutovg_surface_create(c->w, c->h);
    c->canvas = plutovg_canvas_create(c->surface);
    if (c->face) plutovg_canvas_set_font(c->canvas, c->face, c->font_size);
}

static void canvas_finalizer(JSRuntime *rt, JSValue v) {
    c2d *c = JS_GetOpaque(v, canvas_class);
    if (!c) return;
    plutovg_canvas_destroy(c->canvas);
    plutovg_surface_destroy(c->surface);
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
    if (!c) return JS_ThrowTypeError(ctx, "not a Canvas2D");
#define F(i) ((float)nd(ctx, argv[i]))

CFN(c_resize) {
    SELF;
    c2d_alloc(c, (int)nd(ctx, argv[0]), (int)nd(ctx, argv[1]));
    return JS_UNDEFINED;
}
CFN(c_width) { SELF; return JS_NewInt32(ctx, c->w); }
CFN(c_height) { SELF; return JS_NewInt32(ctx, c->h); }
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
    SELF;
    for (int i = 0; i < 4; i++) c->fill[i] = F(i);
    return JS_UNDEFINED;
}
CFN(c_set_stroke) {
    SELF;
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
    SELF;
    const char *fam = JS_ToCString(ctx, argv[0]);
    bool bold = JS_ToBool(ctx, argv[2]), italic = JS_ToBool(ctx, argv[3]);
    plutovg_font_face_t *face = fam ? find_font(fam, bold, italic) : NULL;
    JS_FreeCString(ctx, fam);
    bool found = face != NULL;
    if (!face && n_fonts) face = fonts[0].face;
    c->face = face;
    c->font_size = F(1);
    if (face) plutovg_canvas_set_font(c->canvas, face, c->font_size);
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

static JSValue text_op(JSContext *ctx, JSValueConst this_val, JSValueConst *argv, bool stroke) {
    SELF;
    if (!c->face) return JS_UNDEFINED;
    size_t len;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    if (stroke) {
        use_stroke(c);
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
    SELF;
    JSValue a = JS_NewArray(ctx);
    float width = 0, ascent = 0, descent = 0, gap = 0;
    plutovg_rect_t ext = {0};
    if (c->face) {
        size_t len;
        const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
        width = plutovg_font_face_text_extents(c->face, c->font_size, s, (int)len, PLUTOVG_TEXT_ENCODING_UTF8, &ext);
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
    plutovg_surface_t *src = NULL;
    bitmap *b = JS_GetOpaque(argv[0], bitmap_class);
    c2d *sc = b ? NULL : JS_GetOpaque(argv[0], canvas_class);
    if (b) src = bitmap_surface(b);
    else if (sc) src = sc->surface;
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
CFN(c_get_image_data) {
    SELF;
    int x = (int)F(0), y = (int)F(1), w = (int)F(2), h = (int)F(3);
    if (w <= 0 || h <= 0) return JS_NewArrayBufferCopy(ctx, (const uint8_t *)"", 0);
    uint8_t *out = calloc((size_t)w * h, 4);
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
    JSValue ab = JS_NewArrayBufferCopy(ctx, out, (size_t)w * h * 4);
    free(out);
    return ab;
}

// putImageData(ArrayBuffer|TypedArray rgba, w, h, dx, dy)
CFN(c_put_image_data) {
    SELF;
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

static JSValue n_create_canvas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    c2d *c = calloc(1, sizeof *c);
    c->fill[3] = c->stroke[3] = 1;
    c2d_alloc(c, (int)nd(ctx, argv[0]), (int)nd(ctx, argv[1]));
    JSValue v = JS_NewObjectProtoClass(ctx, canvas_proto, canvas_class);
    JS_SetOpaque(v, c);
    return v;
}

// ---------------------------------------------------------------- pixel sources for WebGL

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
        *w = b->w;
        *h = b->h;
        return b->rgba;
    }
    if (c) {
        *w = c->w;
        *h = c->h;
        uint8_t *out = malloc((size_t)c->w * c->h * 4);
        plutovg_convert_argb_to_rgba(out, plutovg_surface_get_data(c->surface), c->w, c->h,
                                     plutovg_surface_get_stride(c->surface));
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
    JS_SetPropertyStr(ctx, native, "registerFont", JS_NewCFunction(ctx, n_register_font, "registerFont", 5));
    JS_SetPropertyStr(ctx, native, "createCanvas2D", JS_NewCFunction(ctx, n_create_canvas, "createCanvas2D", 2));
}
