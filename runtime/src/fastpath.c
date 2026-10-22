// SPDX-License-Identifier: AGPL-3.0-only
// Native fast path for Phaser's per-sprite WebGL batching (MultiPipeline.batchSprite).
// JS still reads the game object's properties; this does the matrix math, tint packing and
// the 42 vertex writes in one call, replacing about 15 JS function calls per sprite.
// Mirrors Phaser 3.90: TransformMatrix.applyITRS / multiplyWithOffset / multiply / setQuad,
// Utils.getTintAppendFloatAlpha and MultiPipeline.batchQuad. Matrices are float32 like Phaser's.

#include <math.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "host.h"
#include "quickjs.h"

static inline double num(JSContext *ctx, JSValueConst v) {
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    if (JS_TAG_IS_FLOAT64(tag)) return JS_VALUE_GET_FLOAT64(v);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

// Returns a pointer to a Float32Array's data, or NULL.
static float *f32(JSContext *ctx, JSValueConst arr) {
    size_t off, len, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, arr, &off, &len, &bpe);
    if (JS_IsException(ab)) return NULL;
    size_t size;
    uint8_t *p = JS_GetArrayBuffer(ctx, &size, ab);
    JS_FreeValue(ctx, ab); // the typed array keeps the buffer alive
    return p ? (float *)(p + off) : NULL;
}

// JS's Math.round, bit for bit (QuickJS's js_math_round): floor(x + 0.5) gives +0 where it gives -0
// (-0.5 <= x < 0), and 1 for 0.49999999999999994.
static double js_round(double a) {
    union { double d; uint64_t u; } v = {a};
    unsigned e = (v.u >> 52) & 0x7ff;
    if (e < 1023) {
        if (e == 1023 - 1 && v.u != 0xbfe0000000000000ull) v.u = (v.u & (1ull << 63)) | (1023ull << 52);
        else v.u &= 1ull << 63;
    } else if (e < 1023 + 52) {
        uint64_t one = 1ull << (52 - (e - 1023));
        v.u += (one >> 1) - (v.u >> 63);
        v.u &= ~(one - 1);
    }
    return v.d;
}

static inline uint32_t tint(double rgb, double a) {
    uint32_t ua = ((uint32_t)(int32_t)(a * 255)) & 0xff;
    return (ua << 24) | (uint32_t)(int64_t)rgb;
}

typedef struct {
    double offX, offY, gx, gy;
    double rot;
    double sx, sy;
    double x, y, w, h;
    int round;
    float u0, v0, u1, v1;
    uint32_t tTL, tTR, tBL, tBR;
    float tintEffect, unit;
} sprite_quad;

// The sprite's corners (MultiPipeline.batchSprite's matrix math and calcMatrix.setQuad), as
// x0, y0, x1, y1, x2, y2, x3, y3 in float32 like Phaser's quad array.
// GetCalcMatrix's calc: camera (* parent, with the scroll offset) * the object's own matrix, from
// q's gx, gy, rot, sx, sy, offX, offY. out: a, b, c, d, e, f as float32 like Phaser's matrices.
static void calc_from(float *out, const float *cam, const float *par, const sprite_quad *q) {
    // Phaser's TransformMatrix math is done in doubles and stored in Float32Arrays: same here, so
    // that every stored value rounds exactly like Phaser's.
    // spriteMatrix.applyITRS(gx, gy, rotation, sx, sy)
    double s = sin(q->rot), c = cos(q->rot);
    float sa = (float)(c * q->sx), sb = (float)(s * q->sx), sc = (float)(-s * q->sy), sd = (float)(c * q->sy);
    float se = (float)q->gx, sf = (float)q->gy;

    // camMatrix = copy of camera matrix, optionally multiplied with parent (multiplyWithOffset)
    float m[6];
    memcpy(m, cam, sizeof m);
    if (par) {
        double a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3];
        double pse = q->offX * a0 + q->offY * c0 + m[4];
        double psf = q->offX * b0 + q->offY * d0 + m[5];
        m[0] = (float)((double)par[0] * a0 + (double)par[1] * c0);
        m[1] = (float)((double)par[0] * b0 + (double)par[1] * d0);
        m[2] = (float)((double)par[2] * a0 + (double)par[3] * c0);
        m[3] = (float)((double)par[2] * b0 + (double)par[3] * d0);
        m[4] = (float)((double)par[4] * a0 + (double)par[5] * c0 + pse);
        m[5] = (float)((double)par[4] * b0 + (double)par[5] * d0 + psf);
    } else {
        se = (float)((double)se + q->offX); // spriteMatrix.e -= camera.scrollX * scrollFactorX
        sf = (float)((double)sf + q->offY);
    }

    // calc = camMatrix * spriteMatrix (multiply)
    float a = (float)((double)sa * m[0] + (double)sb * m[2]);
    float b = (float)((double)sa * m[1] + (double)sb * m[3]);
    float cc = (float)((double)sc * m[0] + (double)sd * m[2]);
    float d = (float)((double)sc * m[1] + (double)sd * m[3]);
    float e = (float)((double)se * m[0] + (double)sf * m[2] + m[4]);
    float f = (float)((double)se * m[1] + (double)sf * m[3] + m[5]);
    out[0] = a, out[1] = b, out[2] = cc, out[3] = d, out[4] = e, out[5] = f;
}

static void sprite_corners(float *out, const float *cam, const float *par, const sprite_quad *q) {
    float k[6];
    calc_from(k, cam, par, q);
    float a = k[0], b = k[1], cc = k[2], d = k[3], e = k[4], f = k[5];

    // setQuad
    double x = q->x, y = q->y, xw = q->x + q->w, yh = q->y + q->h;
    float x0 = x * a + y * cc + e, y0 = x * b + y * d + f;
    float x1 = x * a + yh * cc + e, y1 = x * b + yh * d + f;
    float x2 = xw * a + yh * cc + e, y2 = xw * b + yh * d + f;
    float x3 = xw * a + y * cc + e, y3 = xw * b + y * d + f;
    if (q->round) {
        float rx0 = floorf(x0 + 0.5f), ry0 = floorf(y0 + 0.5f);
        float dx = rx0 - x0, dy = ry0 - y0;
        x0 = rx0; y0 = ry0;
        x1 += dx; y1 += dy;
        x2 += dx; y2 += dy;
        x3 += dx; y3 += dy;
    }
    out[0] = x0, out[1] = y0, out[2] = x1, out[3] = y1, out[4] = x2, out[5] = y2, out[6] = x3, out[7] = y3;
}

// Writes one sprite's 6 vertices at vf (MultiPipeline.batchSprite after the property reads).
static void write_sprite(float *vf, const float *cam, const float *par, const sprite_quad *q) {
    float k[8];
    sprite_corners(k, cam, par, q);
    float x0 = k[0], y0 = k[1], x1 = k[2], y1 = k[3], x2 = k[4], y2 = k[5], x3 = k[6], y3 = k[7];

    // batchQuad vertex layout: x, y, u, v, unit, tintEffect, tint(u32) x 6 vertices
    uint32_t *vu = (uint32_t *)vf;
    float unit = q->unit, tintEffect = q->tintEffect;
    float u0 = q->u0, v0 = q->v0, u1 = q->u1, v1 = q->v1;
#define VERT(i, X, Y, U, V, T) \
    vf[i * 7 + 0] = X; vf[i * 7 + 1] = Y; vf[i * 7 + 2] = U; vf[i * 7 + 3] = V; \
    vf[i * 7 + 4] = unit; vf[i * 7 + 5] = tintEffect; vu[i * 7 + 6] = T;
    VERT(0, x0, y0, u0, v0, q->tTL)
    VERT(1, x1, y1, u0, v1, q->tBL)
    VERT(2, x2, y2, u1, v1, q->tBR)
    VERT(3, x0, y0, u0, v0, q->tTL)
    VERT(4, x2, y2, u1, v1, q->tBR)
    VERT(5, x3, y3, u1, v0, q->tTR)
#undef VERT
}

// __nativeBatchSprite(vertexData: ArrayBuffer, offset: number (float index),
//   camMatrix: Float32Array, parentMatrix: Float32Array|null, offX, offY,
//   gx, gy, rotation, scaleX, scaleY, x, y, w, h, roundPixels,
//   u0, v0, u1, v1, tintTL, tintTR, tintBL, tintBR, alphaTL, alphaTR, alphaBL, alphaBR,
//   tintEffect, unit)
static JSValue js_batch_sprite(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 30) return JS_ThrowTypeError(ctx, "__nativeBatchSprite: expected 30 args");
    size_t vsize;
    uint8_t *vb = JS_GetArrayBuffer(ctx, &vsize, argv[0]);
    float *cam = f32(ctx, argv[2]);
    float *par = JS_IsNull(argv[3]) ? NULL : f32(ctx, argv[3]);
    if (!vb || !cam) return JS_ThrowTypeError(ctx, "__nativeBatchSprite: bad buffers");
    uint32_t off = (uint32_t)num(ctx, argv[1]);
    if ((off + 42) * 4 > vsize) return JS_ThrowRangeError(ctx, "__nativeBatchSprite: vertex buffer overflow");
    sprite_quad q = {
        .offX = num(ctx, argv[4]), .offY = num(ctx, argv[5]), .gx = num(ctx, argv[6]), .gy = num(ctx, argv[7]),
        .rot = num(ctx, argv[8]), .sx = num(ctx, argv[9]), .sy = num(ctx, argv[10]),
        .x = num(ctx, argv[11]), .y = num(ctx, argv[12]), .w = num(ctx, argv[13]), .h = num(ctx, argv[14]),
        .round = JS_ToBool(ctx, argv[15]),
        .u0 = num(ctx, argv[16]), .v0 = num(ctx, argv[17]), .u1 = num(ctx, argv[18]), .v1 = num(ctx, argv[19]),
        .tTL = tint(num(ctx, argv[20]), num(ctx, argv[24])), .tTR = tint(num(ctx, argv[21]), num(ctx, argv[25])),
        .tBL = tint(num(ctx, argv[22]), num(ctx, argv[26])), .tBR = tint(num(ctx, argv[23]), num(ctx, argv[27])),
        .tintEffect = num(ctx, argv[28]), .unit = num(ctx, argv[29]),
    };
    write_sprite((float *)vb + off, cam, par, &q);
    return JS_UNDEFINED;
}

// batchVerts(vertexData: ArrayBuffer, offset (float index), matrix: Float32Array(6),
//   vertices: [{vx, vy, u, v}], roundPixels, unit, tintEffect, tint) -> vertices written
// The vertex loop of Phaser 3.90's NineSliceWebGLRenderer: TransformMatrix.getXRound/getYRound
// per vertex (float64 math, Math.round: js_round), one shared tint.
// Writes n vertices {vx, vy, u, v} of `verts` at vf, transformed by m (a..f).
static void write_verts(JSContext *ctx, float *vf, JSValueConst verts, int64_t n, const float *m, bool round,
                        float unit, float effect, uint32_t t) {
    static JSAtom a_vx, a_vy, a_u, a_v;
    if (!a_vx) a_vx = JS_NewAtom(ctx, "vx"), a_vy = JS_NewAtom(ctx, "vy"), a_u = JS_NewAtom(ctx, "u"), a_v = JS_NewAtom(ctx, "v");
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    uint32_t *vu = (uint32_t *)vf;
    for (int64_t i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyInt64(ctx, verts, i);
        JSValue jx = JS_GetProperty(ctx, v, a_vx), jy = JS_GetProperty(ctx, v, a_vy);
        JSValue ju = JS_GetProperty(ctx, v, a_u), jv = JS_GetProperty(ctx, v, a_v);
        double vx = num(ctx, jx), vy = num(ctx, jy);
        double x = vx * a + vy * c + e, y = vx * b + vy * d + f;
        if (round) x = js_round(x), y = js_round(y);
        vf[0] = (float)x, vf[1] = (float)y, vf[2] = (float)num(ctx, ju), vf[3] = (float)num(ctx, jv);
        vf[4] = unit, vf[5] = effect, vu[6] = t;
        vf += 7, vu += 7;
        JS_FreeValue(ctx, jx), JS_FreeValue(ctx, jy), JS_FreeValue(ctx, ju), JS_FreeValue(ctx, jv), JS_FreeValue(ctx, v);
    }
}
static JSValue js_batch_verts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t size;
    uint8_t *vb = JS_GetArrayBuffer(ctx, &size, argv[0]);
    float *m = f32(ctx, argv[2]);
    int64_t n = 0;
    if (!vb || !m || JS_GetLength(ctx, argv[3], &n) < 0) return JS_ThrowTypeError(ctx, "batchVerts: bad arguments");
    int64_t off = (int64_t)num(ctx, argv[1]);
    if (off < 0 || (size_t)(off + n * 7) * 4 > size) return JS_ThrowRangeError(ctx, "batchVerts: vertex buffer too small");
    write_verts(ctx, (float *)vb + off, argv[3], n, m, JS_ToBool(ctx, argv[4]), (float)num(ctx, argv[5]),
                (float)num(ctx, argv[6]), (uint32_t)(int64_t)num(ctx, argv[7]));
    return JS_NewInt64(ctx, n);
}

// batchChars(vertexData: ArrayBuffer, offset (float index), maxQuads, chars: [{x, y, w, h, code,
//   glyph: {width, height, u0, v0, u1, v1}}], state: Int32Array(1) (in: first char, out: the next one),
//   matrix: Float32Array(6), originX, originY, offsetX, offsetY, roundPixels, unit, tintEffect,
//   tintTL, tintTR, tintBL, tintBR) -> quads written
// The character loop of Phaser 3.90's BitmapTextWebGLRenderer: BatchChar's corners (float64 math,
// Math.round) and MultiPipeline.batchQuad's 6 vertices, skipping spaces and empty glyphs as it does.
// Stops at the first character to draw once maxQuads are written (the caller flushes, as Phaser's
// loop does there), leaving state[0] at it.
// Writes chars[*idx..n) (spaces and empty glyphs skipped) at vf, at most max quads; leaves *idx at the
// next character to draw (n when done). m: the text's calc matrix; o: displayOrigin; d: offset (the
// drop shadow's); t: tints TL, TR, BL, BR. Returns the quads written, or -1 with an exception.
static int64_t write_chars(JSContext *ctx, float *vf, int64_t max, JSValueConst chars, int64_t n, int64_t *idx,
                           const float *m, double ox, double oy, double dx, double dy, bool round, float unit,
                           float effect, const uint32_t t[4]) {
    static JSAtom a[12];
    enum { X, Y, W, H, CODE, GLYPH, GW, GH, U0, V0, U1, V1 };
    if (!a[0]) {
        static const char *const names[12] = {"x", "y", "w", "h", "code", "glyph", "width", "height", "u0", "v0", "u1", "v1"};
        for (int i = 0; i < 12; i++) a[i] = JS_NewAtom(ctx, names[i]);
    }
    double ma = m[0], mb = m[1], mc = m[2], md = m[3], me = m[4], mf = m[5];
    int64_t i = *idx, quads = 0;
    for (; i < n; i++) {
        JSValue ch = JS_GetPropertyInt64(ctx, chars, i);
        JSValue glyph = JS_GetProperty(ctx, ch, a[GLYPH]);
        if (!JS_IsObject(glyph)) {
            JS_FreeValue(ctx, glyph), JS_FreeValue(ctx, ch);
            JS_ThrowTypeError(ctx, "bitmap text: character %lld has no glyph", (long long)i);
            return -1;
        }
        double g[6], c[5];
        for (int k = 0; k < 6; k++) {
            JSValue v = JS_GetProperty(ctx, glyph, a[GW + k]);
            g[k] = num(ctx, v);
            JS_FreeValue(ctx, v);
        }
        for (int k = 0; k < 5; k++) {
            JSValue v = JS_GetProperty(ctx, ch, a[X + k]);
            c[k] = num(ctx, v);
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, glyph), JS_FreeValue(ctx, ch);
        if (c[CODE] == 32 || g[0] == 0 || g[1] == 0) continue; // a space or an empty glyph
        if (quads == max) break;
        double x = (c[X] - ox) + dx, y = (c[Y] - oy) + dy, xw = x + c[W], yh = y + c[H];
        double tx0 = x * ma + y * mc + me, ty0 = x * mb + y * md + mf;
        double tx1 = x * ma + yh * mc + me, ty1 = x * mb + yh * md + mf;
        double tx2 = xw * ma + yh * mc + me, ty2 = xw * mb + yh * md + mf;
        double tx3 = xw * ma + y * mc + me, ty3 = xw * mb + y * md + mf;
        if (round) {
            tx0 = js_round(tx0), ty0 = js_round(ty0), tx1 = js_round(tx1), ty1 = js_round(ty1);
            tx2 = js_round(tx2), ty2 = js_round(ty2), tx3 = js_round(tx3), ty3 = js_round(ty3);
        }
        float u0 = (float)g[2], v0 = (float)g[3], u1 = (float)g[4], v1 = (float)g[5];
        const float px[6] = {(float)tx0, (float)tx1, (float)tx2, (float)tx0, (float)tx2, (float)tx3};
        const float py[6] = {(float)ty0, (float)ty1, (float)ty2, (float)ty0, (float)ty2, (float)ty3};
        const float pu[6] = {u0, u0, u1, u0, u1, u1}, pv[6] = {v0, v1, v1, v0, v1, v0};
        const uint32_t pt[6] = {t[0], t[2], t[3], t[0], t[3], t[1]};
        uint32_t *vu = (uint32_t *)vf;
        for (int k = 0; k < 6; k++, vf += 7, vu += 7) {
            vf[0] = px[k], vf[1] = py[k], vf[2] = pu[k], vf[3] = pv[k], vf[4] = unit, vf[5] = effect;
            vu[6] = pt[k];
        }
        quads++;
    }
    *idx = i;
    return quads;
}

static JSValue js_batch_chars(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    size_t size, soff, slen, sbpe;
    uint8_t *vb = JS_GetArrayBuffer(ctx, &size, argv[0]);
    float *m = f32(ctx, argv[5]);
    JSValue sab = JS_GetTypedArrayBuffer(ctx, argv[4], &soff, &slen, &sbpe);
    if (JS_IsException(sab)) return JS_EXCEPTION;
    size_t ssize;
    uint8_t *sp = JS_GetArrayBuffer(ctx, &ssize, sab);
    JS_FreeValue(ctx, sab);
    int64_t n = 0;
    if (!vb || !m || !sp || sbpe != 4 || slen < 4 || JS_GetLength(ctx, argv[3], &n) < 0)
        return JS_ThrowTypeError(ctx, "batchChars: bad arguments");
    int32_t *state = (int32_t *)(sp + soff);
    int64_t off = (int64_t)num(ctx, argv[1]), max = (int64_t)num(ctx, argv[2]), idx = state[0];
    if (off < 0 || max < 0 || (size_t)(off + max * 42) * 4 > size) return JS_ThrowRangeError(ctx, "batchChars: vertex buffer too small");
    const uint32_t t[4] = {(uint32_t)(int64_t)num(ctx, argv[13]), (uint32_t)(int64_t)num(ctx, argv[14]),
                           (uint32_t)(int64_t)num(ctx, argv[15]), (uint32_t)(int64_t)num(ctx, argv[16])};
    int64_t q = write_chars(ctx, (float *)vb + off, max, argv[3], n, &idx, m, num(ctx, argv[6]), num(ctx, argv[7]),
                            num(ctx, argv[8]), num(ctx, argv[9]), JS_ToBool(ctx, argv[10]), (float)num(ctx, argv[11]),
                            (float)num(ctx, argv[12]), t);
    if (q < 0) return JS_EXCEPTION;
    state[0] = (int32_t)idx;
    return JS_NewInt64(ctx, q);
}

// ---------------------------------------------------------------- container renderer
// Phaser 3.90's ContainerWebGLRenderer walked in C (__native.renderContainer, installed as
// Container.prototype.renderWebGL by js/fastpath.js). For each child it does Phaser's bookkeeping
// (willRender, blend mode, renderer.currentType/newType/nextTypeMatch, the camera's render list)
// and then:
//  - a plain Image/Sprite on a pipeline that batches like MultiPipeline: the whole of
//    MultiPipeline.batchSprite (texture batching, flush, vertex writes) here, calling back into JS
//    only for what isn't the common case (pipeline switch, new texture, flush, a pipeline's own
//    onBind/onBatch such as PokeRogue's FieldSpritePipeline);
//  - a nested container without a mask: the same, recursively;
//  - anything else: Phaser's generic code path, i.e. the child's own renderWebGL from JS.
// Returns -1 with a pending JS exception if a callback threw.

static struct {
    bool ready;
    JSValue image_render, sprite_render, container_render, mp_batch_sprite, mp_batch_quad;
    JSValue go_will_render, ct_will_render, wp_on_bind, wp_on_batch, wp_set_game_object, cam_add_to_render_list;
    JSValue text_render;
    // Phaser's Alpha/AlphaSingle.setAlpha, ScrollFactor.setScrollFactor, MultiPipeline.batchSprite and
    // PipelineManager.preBatch/postBatch, to recognise them on objects
    JSValue alpha_set, alpha_single_set, sf_set, mp_orig_batch_sprite, pm_pre_batch, pm_post_batch;
    // Container.setScrollFactor (x, y, updateChildren), AlphaSingle's alpha setter; WebGLPipeline's
    // own pushBatch/createBatch/setVertexBuffer/onBeforeFlush/onAfterFlush and its flush events
    JSValue ct_sf_set, alpha_single_setter, wp_push_batch, wp_create_batch, wp_set_vertex_buffer, wp_on_before_flush,
        wp_on_after_flush;
    JSAtom ev_before_flush, ev_after_flush;
    // js/fastpath.js made Text's width, height and display origin accessors that measure on first
    // read and keep the values in _pkr_<name>: a text that is already drawn has them up to date
    bool text_lazy;
    // js/fastpath.js part 18's BitmapText renderer, WebGLPipeline.shouldFlush, BitmapText.getTextBounds
    JSValue bitmap_text_render, wp_should_flush, bt_get_text_bounds;
} H;

enum {
    A_list, A_length, A_localTransform, A_matrix, A_x, A_y, A__rotation, A__scaleX, A__scaleY, A_hasPostPipeline,
    A_pipelines, A_preBatch, A_postBatch, A__blendMode, A_blendMode, A_currentBlendMode, A_setBlendMode, A__alpha,
    A_scrollFactorX, A_scrollFactorY, A_willRender, A_displayList, A_active, A_renderFlags, A_cameraFilter, A_id,
    A_type, A_currentType, A_newType, A_nextTypeMatch, A_renderWebGL, A_pipeline, A_mask, A_isCropped, A_frame,
    A_customPivot, A_batchSprite, A_batchQuad, A_currentShader, A_vertexComponentCount, A_setScrollFactor,
    A_setAlpha, A_alpha, A_alphaTopLeft, A_alphaTopRight, A_alphaBottomLeft, A_alphaBottomRight, A_preRenderWebGL,
    A_postRenderWebGL, A_current, A_program, A_currentProgram, A_set, A_projectionMatrix, A_projectionWidth,
    A_projectionHeight, A_setProjectionMatrix, A_onBind, A_onBatch, A__displayOriginX, A__displayOriginY,
    A_flipX, A_flipY, A_realWidth, A_realHeight, A_vertexCount, A_vertexCapacity, A_flush, A_setGameObject,
    A_source, A_glTexture, A_currentBatch, A_forceZero, A_currentTexture, A_currentUnit, A_pushBatch,
    A_setTexture2D, A_vertexData, A_start, A_count, A_cutWidth, A_cutHeight, A_u0, A_v0, A_u1, A_v1,
    A_tintTopLeft, A_tintTopRight, A_tintBottomLeft, A_tintBottomRight, A__alphaTL, A__alphaTR, A__alphaBL,
    A__alphaBR, A_tintFill, A_matrixCam, A_scrollX, A_scrollY, A_roundPixels, A_renderRoundPixels, A_renderList,
    A_addToRenderList, A__pkrTextDirty, A_width, A_height, A_style, A_resolution, A_isRenderTexture,
    A_uniforms, A_value1, A_value2, A_value3, A_value4, A_setter, A_location, A_webGLUniformLocation, A_renderer, A_gl,
    A_setProgram, A_batch, A_activeTextures, A_texture, A_maxUnit, A_webGLTexture, A_topology, A_bytes, A_vertexSize,
    A_vertexBuffer, A_activeBuffer, A_setVertexBuffer, A_webGLBuffer, A__events, A_emit, A_onBeforeFlush, A_onAfterFlush,
    A_bufferData, A_activeTexture, A_bindTexture, A_drawArrays, A_bindBuffer, A_createBatch, A_unit, A_maxTextures,
    A_setScrollFactorC, A__pkr_width, A__pkr_height, A__pkr__displayOriginX, A__pkr__displayOriginY, A_isStroked, A_isFilled, A_isRounded, A_fillColor, A_fillAlpha, A_maskCount,
    A__text, A_charColors, A__dirty, A__bounds, A_characters, A_scaleX, A_scaleY, A_dropShadowX, A_dropShadowY,
    A_dropShadowColor, A_dropShadowAlpha, A_getTextBounds, A_shouldFlush, A_COUNT
};
static const char *atom_names[A_COUNT] = {
    "list", "length", "localTransform", "matrix", "x", "y", "_rotation", "_scaleX", "_scaleY", "hasPostPipeline",
    "pipelines", "preBatch", "postBatch", "_blendMode", "blendMode", "currentBlendMode", "setBlendMode", "_alpha",
    "scrollFactorX", "scrollFactorY", "willRender", "displayList", "active", "renderFlags", "cameraFilter", "id",
    "type", "currentType", "newType", "nextTypeMatch", "renderWebGL", "pipeline", "mask", "isCropped", "frame",
    "customPivot", "batchSprite", "batchQuad", "currentShader", "vertexComponentCount", "setScrollFactor",
    "setAlpha", "alpha", "alphaTopLeft", "alphaTopRight", "alphaBottomLeft", "alphaBottomRight", "preRenderWebGL",
    "postRenderWebGL", "current", "program", "currentProgram", "set", "projectionMatrix", "projectionWidth",
    "projectionHeight", "setProjectionMatrix", "onBind", "onBatch", "_displayOriginX", "_displayOriginY",
    "flipX", "flipY", "realWidth", "realHeight", "vertexCount", "vertexCapacity", "flush", "setGameObject",
    "source", "glTexture", "currentBatch", "forceZero", "currentTexture", "currentUnit", "pushBatch",
    "setTexture2D", "vertexData", "start", "count", "cutWidth", "cutHeight", "u0", "v0", "u1", "v1",
    "tintTopLeft", "tintTopRight", "tintBottomLeft", "tintBottomRight", "_alphaTL", "_alphaTR", "_alphaBL",
    "_alphaBR", "tintFill", "matrix", "scrollX", "scrollY", "roundPixels", "renderRoundPixels", "renderList",
    "addToRenderList", "_pkrTextDirty", "width", "height", "style", "resolution", "isRenderTexture",
    "uniforms", "value1", "value2", "value3", "value4", "setter", "location", "webGLUniformLocation", "renderer", "gl",
    "setProgram", "batch", "activeTextures", "texture", "maxUnit", "webGLTexture", "topology", "bytes", "vertexSize",
    "vertexBuffer", "activeBuffer", "setVertexBuffer", "webGLBuffer", "_events", "emit", "onBeforeFlush", "onAfterFlush",
    "bufferData", "activeTexture", "bindTexture", "drawArrays", "bindBuffer", "createBatch", "unit", "maxTextures",
    "setScrollFactor", "_pkr_width", "_pkr_height", "_pkr__displayOriginX", "_pkr__displayOriginY", "isStroked", "isFilled", "isRounded", "fillColor", "fillAlpha", "maskCount",
    "_text", "charColors", "_dirty", "_bounds", "characters", "scaleX", "scaleY", "dropShadowX", "dropShadowY",
    "dropShadowColor", "dropShadowAlpha", "getTextBounds", "shouldFlush",
};
static JSAtom AT[A_COUNT];

#define G(o, a) JS_GetProperty(ctx, (o), AT[a])

static inline bool same(JSValueConst a, JSValueConst b) {
    return JS_VALUE_GET_TAG(a) == JS_TAG_OBJECT && JS_VALUE_GET_TAG(b) == JS_TAG_OBJECT &&
           JS_VALUE_GET_PTR(a) == JS_VALUE_GET_PTR(b);
}
static double gnum(JSContext *ctx, JSValueConst o, int a) {
    JSValue v = G(o, a);
    double d = num(ctx, v);
    JS_FreeValue(ctx, v);
    return d;
}
static bool gbool(JSContext *ctx, JSValueConst o, int a) {
    JSValue v = G(o, a);
    bool b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}
static bool gsame(JSContext *ctx, JSValueConst o, int a, JSValueConst ref) {
    JSValue v = G(o, a);
    bool r = same(v, ref);
    JS_FreeValue(ctx, v);
    return r;
}
static void sset(JSContext *ctx, JSValueConst o, int a, JSValue v) { JS_SetProperty(ctx, o, AT[a], v); }
// Calls o[method](args...); returns the result (caller frees) or JS_EXCEPTION.
static JSValue call(JSContext *ctx, JSValueConst o, int method, int argc, JSValueConst *argv) {
    JSValue f = G(o, method);
    JSValue r = JS_Call(ctx, f, o, argc, argv);
    JS_FreeValue(ctx, f);
    return r;
}
static int call0(JSContext *ctx, JSValueConst o, int method, int argc, JSValueConst *argv) {
    JSValue r = call(ctx, o, method, argc, argv);
    if (JS_IsException(r)) return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

// Math.max(0, Math.min(1, v)) (Phaser's Clamp), NaN and -0 included
static double clamp01(double v) {
    if (v != v) return v;
    if (v <= 0) return 0;
    return v > 1 ? 1 : v;
}

// Whether o's "alpha" accessor is AlphaSingle's (looked up along its prototypes; remembered per
// prototype object, which only ever gains or keeps that accessor in practice).
static bool stock_alpha_single(JSContext *ctx, JSValueConst o) {
    static void *protos[16];
    static bool answers[16];
    static int next;
    if (JS_GetOwnProperty(ctx, NULL, o, AT[A_alpha]) != 0) return false; // own property or error
    JSValue proto = JS_GetPrototype(ctx, o);
    void *key = JS_IsObject(proto) ? JS_VALUE_GET_PTR(proto) : NULL;
    for (int i = 0; i < 16; i++)
        if (protos[i] == key && key) {
            JS_FreeValue(ctx, proto);
            return answers[i];
        }
    bool ans = false;
    for (int depth = 0; depth < 32 && JS_IsObject(proto); depth++) {
        JSPropertyDescriptor d;
        int has = JS_GetOwnProperty(ctx, &d, proto, AT[A_alpha]);
        if (has < 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            break;
        }
        if (has) {
            ans = (d.flags & JS_PROP_GETSET) && same(d.setter, H.alpha_single_setter);
            JS_FreeValue(ctx, d.value), JS_FreeValue(ctx, d.getter), JS_FreeValue(ctx, d.setter);
            break;
        }
        JSValue up = JS_GetPrototype(ctx, proto);
        JS_FreeValue(ctx, proto);
        proto = up;
    }
    JS_FreeValue(ctx, proto);
    if (key) protos[next] = key, answers[next] = ans, next = (next + 1) % 16;
    return ans;
}

// o.setAlpha(tl, tr, bl, br) and o.setScrollFactor(x, y), with Phaser's own versions done here.
static int set_alpha4(JSContext *ctx, JSValueConst o, double tl, double tr, double bl, double br) {
    JSValue f = G(o, A_setAlpha);
    int rc = 0;
    if (same(f, H.alpha_set)) { // Alpha: four corners, clamped
        sset(ctx, o, A__alphaTL, JS_NewFloat64(ctx, clamp01(tl)));
        sset(ctx, o, A__alphaTR, JS_NewFloat64(ctx, clamp01(tr)));
        sset(ctx, o, A__alphaBL, JS_NewFloat64(ctx, clamp01(bl)));
        sset(ctx, o, A__alphaBR, JS_NewFloat64(ctx, clamp01(br)));
    } else if (same(f, H.alpha_single_set)) { // AlphaSingle: this.alpha = tl
        if (stock_alpha_single(ctx, o)) {
            // the setter: _alpha = Clamp(v, 0, 1), render flag 2 off at 0 and on otherwise
            double v = clamp01(tl);
            sset(ctx, o, A__alpha, JS_NewFloat64(ctx, v));
            int32_t fl = (int32_t)gnum(ctx, o, A_renderFlags);
            sset(ctx, o, A_renderFlags, JS_NewInt32(ctx, v == 0 ? fl & ~2 : fl | 2));
        } else if (JS_SetProperty(ctx, o, AT[A_alpha], JS_NewFloat64(ctx, tl)) < 0) {
            rc = -1;
        }
    } else {
        JSValueConst al[4] = {JS_NewFloat64(ctx, tl), JS_NewFloat64(ctx, tr), JS_NewFloat64(ctx, bl), JS_NewFloat64(ctx, br)};
        JSValue r = JS_Call(ctx, f, o, 4, al);
        if (JS_IsException(r)) rc = -1;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, f);
    return rc;
}
static int set_scroll_factor(JSContext *ctx, JSValueConst o, double x, double y) {
    JSValue f = G(o, A_setScrollFactor);
    int rc = 0;
    if (same(f, H.sf_set) || same(f, H.ct_sf_set)) { // (the container's only does more with updateChildren)
        sset(ctx, o, A_scrollFactorX, JS_NewFloat64(ctx, x));
        sset(ctx, o, A_scrollFactorY, JS_NewFloat64(ctx, y));
    } else {
        JSValueConst sf[2] = {JS_NewFloat64(ctx, x), JS_NewFloat64(ctx, y)};
        JSValue r = JS_Call(ctx, f, o, 2, sf);
        if (JS_IsException(r)) rc = -1;
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, f);
    return rc;
}

typedef struct {
    JSValueConst renderer, camera, mgr;
    JSValue render_list; // camera.renderList, or undefined if addToRenderList is overridden
    float cam_m[6];
    double scroll_x, scroll_y, alpha, id;
    bool round_pixels, render_round;
} rctx;

static int add_to_render_list(JSContext *ctx, rctx *r, JSValueConst obj) {
    if (JS_IsUndefined(r->render_list)) return call0(ctx, r->camera, A_addToRenderList, 1, &obj);
    int64_t n;
    if (JS_GetLength(ctx, r->render_list, &n) < 0) return -1;
    return JS_SetPropertyInt64(ctx, r->render_list, n, JS_DupValue(ctx, obj)) < 0 ? -1 : 0;
}

// GameObject / Container willRender; 1 yes, 0 no, -1 exception.
static int will_render(JSContext *ctx, rctx *r, JSValueConst child) {
    JSValue wr = G(child, A_willRender);
    int res;
    if (same(wr, H.go_will_render) || same(wr, H.ct_will_render)) {
        bool is_ct = same(wr, H.ct_will_render);
        res = 1;
        if (!is_ct) {
            JSValue dl = G(child, A_displayList);
            if (JS_IsObject(dl) && gbool(ctx, dl, A_active)) {
                JSValue v = call(ctx, dl, A_willRender, 1, &r->camera);
                if (JS_IsException(v)) res = -1;
                else res = JS_ToBool(ctx, v) > 0;
                JS_FreeValue(ctx, v);
            }
            JS_FreeValue(ctx, dl);
        }
        if (res == 1 && gnum(ctx, child, A_renderFlags) != 15) res = 0;
        if (res == 1 && is_ct) {
            JSValue l = G(child, A_list);
            if (gnum(ctx, l, A_length) == 0) res = 0;
            JS_FreeValue(ctx, l);
        }
        if (res == 1) {
            int32_t f = (int32_t)gnum(ctx, child, A_cameraFilter);
            if (f != 0 && (f & (int32_t)r->id)) res = 0;
        }
    } else {
        JSValue v = JS_Call(ctx, wr, child, 1, &r->camera);
        if (JS_IsException(v)) res = -1;
        else res = JS_ToBool(ctx, v) > 0;
        JS_FreeValue(ctx, v);
    }
    JS_FreeValue(ctx, wr);
    return res;
}

static JSValue push_batch(JSContext *ctx, JSValueConst p, JSValueConst tex);

// Whether `pipeline` batches sprites exactly like MultiPipeline (so batch_sprite can do it).
static bool fast_pipeline(JSContext *ctx, JSValueConst pipeline) {
    if (!JS_IsObject(pipeline)) return false;
    if (!gsame(ctx, pipeline, A_batchSprite, H.mp_batch_sprite) || !gsame(ctx, pipeline, A_batchQuad, H.mp_batch_quad) ||
        !gsame(ctx, pipeline, A_setGameObject, H.wp_set_game_object))
        return false;
    JSValue sh = G(pipeline, A_currentShader);
    bool ok = JS_IsObject(sh) && gnum(ctx, sh, A_vertexComponentCount) == 7;
    JS_FreeValue(ctx, sh);
    return ok;
}

// MultiPipeline.batchSprite for a plain Image/Sprite (see fastBatchSprite in js/fastpath.js), or
// with `text`, TextWebGLRenderer + MultiPipeline.batchTexture for a plain Text (no flip, no crop).
// alpha_mul, sfx, sfy: the parent container's alpha and scroll factors; par: its matrix.
// renderer.pipelines.set(pipeline, go): isCurrent -> updateProjectionMatrix + onBind, else rebind.
// 0, or -1 with an exception.
static int pipe_set(JSContext *ctx, rctx *r, JSValueConst pipeline, JSValueConst go) {
    JSValue cur = G(r->mgr, A_current);
    bool current = same(cur, pipeline);
    JS_FreeValue(ctx, cur);
    if (current) {
        JSValue sh = G(pipeline, A_currentShader), prog = G(sh, A_program), rp = G(r->renderer, A_currentProgram);
        current = JS_IsStrictEqual(ctx, prog, rp);
        JS_FreeValue(ctx, sh), JS_FreeValue(ctx, prog), JS_FreeValue(ctx, rp);
    }
    if (!current || !gsame(ctx, pipeline, A_onBind, H.wp_on_bind)) {
        JSValueConst args[2] = {pipeline, go};
        return call0(ctx, r->mgr, A_set, 2, args);
    }
    if (gbool(ctx, pipeline, A_projectionMatrix)) {
        double pw = gnum(ctx, r->renderer, A_projectionWidth), ph = gnum(ctx, r->renderer, A_projectionHeight);
        if (gnum(ctx, pipeline, A_projectionWidth) != pw || gnum(ctx, pipeline, A_projectionHeight) != ph) {
            JSValueConst args[2] = {JS_NewFloat64(ctx, pw), JS_NewFloat64(ctx, ph)};
            return call0(ctx, pipeline, A_setProjectionMatrix, 2, args);
        }
    }
    return 0;
}

static int batch_sprite(JSContext *ctx, rctx *r, JSValueConst pipeline, JSValueConst go, JSValueConst frame,
                        const float *par, double alpha_mul, double sfx, double sfy, bool text) {
    int rc = -1;
    JSValue tex = JS_UNDEFINED, cb = JS_UNDEFINED, unitv = JS_UNDEFINED;
    if (pipe_set(ctx, r, pipeline, go)) goto out;

    // a text here passed fast_text (drawn, so measured): its stored origin, without the accessor call
    bool stored = text && H.text_lazy;
    double dox = gnum(ctx, go, stored ? A__pkr__displayOriginX : A__displayOriginX);
    double doy = gnum(ctx, go, stored ? A__pkr__displayOriginY : A__displayOriginY);
    double x, y, w, h, u0, v0, u1, v1, fx = 1, fy = 1;
    if (text) {
        // batchTexture(src, tex, fw, fh, x, y, fw / res, fh / res, ..., frameX 0, frameY 0, fw, fh)
        JSValue style = G(go, A_style);
        double res = gnum(ctx, style, A_resolution), fw = gnum(ctx, frame, A_width), fh = gnum(ctx, frame, A_height);
        JS_FreeValue(ctx, style);
        x = -dox, y = -doy, w = fw / res, h = fh / res;
        u0 = 0 / fw, v0 = 0 / fh, u1 = fw / fw, v1 = fh / fh;
    } else {
        x = -dox + gnum(ctx, frame, A_x), y = -doy + gnum(ctx, frame, A_y);
        if (gbool(ctx, go, A_flipX)) x += -gnum(ctx, frame, A_realWidth) + dox * 2, fx = -1;
        if (gbool(ctx, go, A_flipY)) y += -gnum(ctx, frame, A_realHeight) + doy * 2, fy = -1;
        w = gnum(ctx, frame, A_cutWidth), h = gnum(ctx, frame, A_cutHeight);
        u0 = gnum(ctx, frame, A_u0), v0 = gnum(ctx, frame, A_v0), u1 = gnum(ctx, frame, A_u1), v1 = gnum(ctx, frame, A_v1);
    }
    double gx = gnum(ctx, go, A_x), gy = gnum(ctx, go, A_y);
    if (r->round_pixels) gx = floor(gx), gy = floor(gy);

    // batchSprite checks for room before taking the texture; batchTexture (texts) only after.
    if (!text && gnum(ctx, pipeline, A_vertexCount) + 6 > gnum(ctx, pipeline, A_vertexCapacity) &&
        call0(ctx, pipeline, A_flush, 0, NULL))
        goto out;
    // setGameObject(go, frame) = pushBatch(frame.source.glTexture)
    {
        JSValue src = G(frame, A_source);
        tex = G(src, A_glTexture);
        JS_FreeValue(ctx, src);
    }
    cb = G(pipeline, A_currentBatch);
    JSValue curtex = G(pipeline, A_currentTexture);
    bool same_tex = JS_IsStrictEqual(ctx, tex, curtex);
    JS_FreeValue(ctx, curtex);
    if (JS_ToBool(ctx, cb) > 0 && same_tex) {
        unitv = G(pipeline, A_currentUnit);
    } else {
        unitv = gsame(ctx, pipeline, A_pushBatch, H.wp_push_batch) ? push_batch(ctx, pipeline, tex)
                                                                   : call(ctx, pipeline, A_pushBatch, 1, &tex);
        if (JS_IsException(unitv)) goto out;
    }
    if (gnum(ctx, pipeline, A_vertexCount) + 6 > gnum(ctx, pipeline, A_vertexCapacity) &&
        call0(ctx, pipeline, A_flush, 0, NULL))
        goto out;
    JS_FreeValue(ctx, cb);
    cb = G(pipeline, A_currentBatch);
    if (JS_ToBool(ctx, cb) <= 0) {
        JS_FreeValue(ctx, unitv);
        unitv = call(ctx, pipeline, A_setTexture2D, 1, &tex);
        if (JS_IsException(unitv)) goto out;
        JS_FreeValue(ctx, cb);
        cb = G(pipeline, A_currentBatch);
    }

    // Products in Phaser's order: the container first sets the child's alpha and scroll factor to
    // (own value * container's), the batcher then multiplies by the camera's.
    double a = r->alpha;
    sprite_quad q = {
        .offX = -r->scroll_x * (gnum(ctx, go, A_scrollFactorX) * sfx),
        .offY = -r->scroll_y * (gnum(ctx, go, A_scrollFactorY) * sfy),
        .gx = gx, .gy = gy, .rot = gnum(ctx, go, A__rotation),
        .sx = gnum(ctx, go, A__scaleX) * fx, .sy = gnum(ctx, go, A__scaleY) * fy,
        .x = x, .y = y, .w = w, .h = h,
        .round = r->render_round,
        .u0 = u0, .v0 = v0, .u1 = u1, .v1 = v1,
        .tTL = tint(gnum(ctx, go, A_tintTopLeft), a * (gnum(ctx, go, A__alphaTL) * alpha_mul)),
        .tTR = tint(gnum(ctx, go, A_tintTopRight), a * (gnum(ctx, go, A__alphaTR) * alpha_mul)),
        .tBL = tint(gnum(ctx, go, A_tintBottomLeft), a * (gnum(ctx, go, A__alphaBL) * alpha_mul)),
        .tBR = tint(gnum(ctx, go, A_tintBottomRight), a * (gnum(ctx, go, A__alphaBR) * alpha_mul)),
        .tintEffect = gbool(ctx, go, A_tintFill) ? 1 : 0,
        .unit = num(ctx, unitv),
    };
    {
        JSValue vd = G(pipeline, A_vertexData);
        size_t vsize;
        uint8_t *vb = JS_GetArrayBuffer(ctx, &vsize, vd);
        JS_FreeValue(ctx, vd);
        double vc = gnum(ctx, pipeline, A_vertexCount);
        if (!vb || (vc * 7 + 42) * 4 > vsize) {
            JS_ThrowRangeError(ctx, "renderContainer: vertex buffer overflow");
            goto out;
        }
        write_sprite((float *)vb + (size_t)vc * 7, r->cam_m, par, &q);
        vc += 6;
        sset(ctx, pipeline, A_vertexCount, JS_NewInt32(ctx, (int32_t)vc));
        sset(ctx, cb, A_count, JS_NewInt32(ctx, (int32_t)(vc - gnum(ctx, cb, A_start))));
    }
    if (!gsame(ctx, pipeline, A_onBatch, H.wp_on_batch) && call0(ctx, pipeline, A_onBatch, 1, &go)) goto out;
    rc = 0;
out:
    JS_FreeValue(ctx, tex), JS_FreeValue(ctx, cb), JS_FreeValue(ctx, unitv);
    return rc;
}

// pipeline.pushBatch(tex) -> unit (the native one when it's WebGLPipeline's), or -1 with an exception.
static int push_unit(JSContext *ctx, JSValueConst pipeline, JSValueConst tex, double *unit) {
    JSValue u = gsame(ctx, pipeline, A_pushBatch, H.wp_push_batch) ? push_batch(ctx, pipeline, tex)
                                                                     : call(ctx, pipeline, A_pushBatch, 1, &tex);
    if (JS_IsException(u)) return -1;
    *unit = num(ctx, u);
    JS_FreeValue(ctx, u);
    return 0;
}

// Adds q quads written at the vertex count to vertexCount and the current batch's count.
static void chars_written(JSContext *ctx, JSValueConst pipeline, int64_t q) {
    double vc = gnum(ctx, pipeline, A_vertexCount) + 6 * (double)q;
    sset(ctx, pipeline, A_vertexCount, JS_NewInt32(ctx, (int32_t)vc));
    JSValue cb = G(pipeline, A_currentBatch);
    sset(ctx, cb, A_count, JS_NewInt32(ctx, (int32_t)(vc - gnum(ctx, cb, A_start))));
    JS_FreeValue(ctx, cb);
}

// One pass of BitmapTextWebGLRenderer's character loop: all of chars at *unit, flushing where
// Phaser does (a character to draw and no room for its 6 vertices). After a flush the main pass
// takes setGameObject's unit for the rest; the drop shadow pass (batchQuad flushing by itself) takes
// setTexture2D's for that character only. 0, or -1 with an exception.
static int chars_pass(JSContext *ctx, JSValueConst pipeline, JSValueConst tex, JSValueConst chars, int64_t n,
                      const float *m, double ox, double oy, double dx, double dy, bool round, double *unit, float effect,
                      const uint32_t t[4], bool shadow) {
    int64_t i = 0;
    for (bool first = true;; first = false) {
        double vc = gnum(ctx, pipeline, A_vertexCount), cap = gnum(ctx, pipeline, A_vertexCapacity);
        int64_t room = cap > vc ? (int64_t)floor((cap - vc) / 6) : 0;
        JSValue vd = G(pipeline, A_vertexData);
        size_t vsize;
        uint8_t *vb = JS_GetArrayBuffer(ctx, &vsize, vd);
        JS_FreeValue(ctx, vd);
        if (!vb || (vc * 7 + (double)room * 42) * 4 > vsize) {
            JS_ThrowRangeError(ctx, "bitmap text: vertex buffer overflow");
            return -1;
        }
        int64_t q = write_chars(ctx, (float *)vb + (size_t)vc * 7, room, chars, n, &i, m, ox, oy, dx, dy, round,
                                (float)*unit, effect, t);
        if (q < 0) return -1;
        if (q) chars_written(ctx, pipeline, q);
        if (i >= n) return 0;
        if (!first && !q) { // no room even in an empty batch
            JS_ThrowRangeError(ctx, "bitmap text: no room after a flush");
            return -1;
        }
        if (call0(ctx, pipeline, A_flush, 0, NULL)) return -1;
        if (shadow) {
            JSValue u = call(ctx, pipeline, A_setTexture2D, 1, &tex);
            if (JS_IsException(u)) return -1;
            double u1 = num(ctx, u);
            JS_FreeValue(ctx, u);
            vd = G(pipeline, A_vertexData);
            vb = JS_GetArrayBuffer(ctx, &vsize, vd);
            JS_FreeValue(ctx, vd);
            vc = gnum(ctx, pipeline, A_vertexCount);
            if (!vb || (vc * 7 + 42) * 4 > vsize) {
                JS_ThrowRangeError(ctx, "bitmap text: vertex buffer overflow");
                return -1;
            }
            q = write_chars(ctx, (float *)vb + (size_t)vc * 7, 1, chars, n, &i, m, ox, oy, dx, dy, round, (float)u1, effect, t);
            if (q < 0) return -1;
            if (q) chars_written(ctx, pipeline, q);
            if (i >= n) return 0;
        } else if (push_unit(ctx, pipeline, tex, unit)) {
            return -1;
        }
    }
}

// Phaser 3.90's BitmapTextWebGLRenderer for a BitmapText child of a container (js/fastpath.js part
// 18 does the same from JS, with only the character loop here): the container's alpha and scroll
// factor multiplied in as batch_sprite does, GetCalcMatrix (calc_from), the tints, setGameObject,
// getTextBounds (its stored bounds when nothing changed) and the character passes.
static int batch_bitmap_text(JSContext *ctx, rctx *r, JSValueConst pipeline, JSValueConst go, const float *par,
                             double alpha_mul, double sfx, double sfy) {
    int rc = -1;
    JSValue frame = JS_UNDEFINED, src = JS_UNDEFINED, tex = JS_UNDEFINED, bounds = JS_UNDEFINED, chars = JS_UNDEFINED;
    if (pipe_set(ctx, r, pipeline, go)) goto out;
    sprite_quad q = {
        .offX = -r->scroll_x * (gnum(ctx, go, A_scrollFactorX) * sfx),
        .offY = -r->scroll_y * (gnum(ctx, go, A_scrollFactorY) * sfy),
        .gx = gnum(ctx, go, A_x), .gy = gnum(ctx, go, A_y), .rot = gnum(ctx, go, A__rotation),
        .sx = gnum(ctx, go, A__scaleX), .sy = gnum(ctx, go, A__scaleY),
    };
    float calc[6];
    calc_from(calc, r->cam_m, par, &q);
    double ca = r->alpha;
    double atl = gnum(ctx, go, A__alphaTL) * alpha_mul, atr = gnum(ctx, go, A__alphaTR) * alpha_mul;
    double abl = gnum(ctx, go, A__alphaBL) * alpha_mul, abr = gnum(ctx, go, A__alphaBR) * alpha_mul;
    const uint32_t t[4] = {tint(gnum(ctx, go, A_tintTopLeft), ca * atl), tint(gnum(ctx, go, A_tintTopRight), ca * atr),
                           tint(gnum(ctx, go, A_tintBottomLeft), ca * abl), tint(gnum(ctx, go, A_tintBottomRight), ca * abr)};
    frame = G(go, A_frame), src = G(frame, A_source), tex = G(src, A_glTexture);
    double unit;
    if (push_unit(ctx, pipeline, tex, &unit)) goto out;
    // getTextBounds(false): GetBitmapTextSize only when dirty or rescaled
    bounds = G(go, A__bounds);
    if (!gsame(ctx, go, A_getTextBounds, H.bt_get_text_bounds) || gbool(ctx, go, A__dirty) ||
        gnum(ctx, go, A__scaleX) != gnum(ctx, bounds, A_scaleX) || gnum(ctx, go, A__scaleY) != gnum(ctx, bounds, A_scaleY)) {
        JS_FreeValue(ctx, bounds);
        JSValueConst no = JS_FALSE;
        bounds = call(ctx, go, A_getTextBounds, 1, &no);
        if (JS_IsException(bounds)) goto out;
    }
    chars = G(bounds, A_characters);
    int64_t n = 0;
    if (JS_GetLength(ctx, chars, &n) < 0) goto out;
    double ox = gnum(ctx, go, A__displayOriginX), oy = gnum(ctx, go, A__displayOriginY);
    double dsx = gnum(ctx, go, A_dropShadowX), dsy = gnum(ctx, go, A_dropShadowY);
    if (dsx != 0 || dsy != 0) {
        double col = gnum(ctx, go, A_dropShadowColor), sa = gnum(ctx, go, A_dropShadowAlpha);
        const uint32_t st[4] = {tint(col, ca * sa * atl), tint(col, ca * sa * atr), tint(col, ca * sa * abl), tint(col, ca * sa * abr)};
        double su = unit;
        if (chars_pass(ctx, pipeline, tex, chars, n, calc, ox, oy, dsx, dsy, r->round_pixels, &su, 1, st, true)) goto out;
    }
    if (chars_pass(ctx, pipeline, tex, chars, n, calc, ox, oy, 0, 0, r->round_pixels, &unit,
                   gbool(ctx, go, A_tintFill) ? 1 : 0, t, false))
        goto out;
    rc = 0;
out:
    JS_FreeValue(ctx, frame), JS_FreeValue(ctx, src), JS_FreeValue(ctx, tex), JS_FreeValue(ctx, bounds), JS_FreeValue(ctx, chars);
    return rc;
}

// A BitmapText batch_bitmap_text can draw: js/fastpath.js's renderer, text not empty, no per-character
// colours or post pipeline, on a plain pipeline whose onBatch and shouldFlush are WebGLPipeline's.
static bool fast_bitmap_text(JSContext *ctx, JSValueConst t, JSValueConst rw, JSValue *pipeline) {
    if (!JS_IsObject(H.bitmap_text_render) || !same(rw, H.bitmap_text_render) || gbool(ctx, t, A_hasPostPipeline)) return false;
    JSValue text = G(t, A__text), cc = G(t, A_charColors);
    int64_t tl = 0, cl = 1;
    bool ok = JS_IsString(text) && JS_GetLength(ctx, text, &tl) == 0 && tl > 0 && JS_GetLength(ctx, cc, &cl) == 0 && cl == 0;
    JS_FreeValue(ctx, text), JS_FreeValue(ctx, cc);
    if (!ok) return false;
    *pipeline = G(t, A_pipeline);
    return fast_pipeline(ctx, *pipeline) && gsame(ctx, *pipeline, A_onBatch, H.wp_on_batch) &&
           gsame(ctx, *pipeline, A_shouldFlush, H.wp_should_flush);
}

// Loads the container's local transform into its localTransform matrix, like Phaser.
static void container_matrix(JSContext *ctx, JSValueConst ct, float *m, const float *par) {
    double x = gnum(ctx, ct, A_x), y = gnum(ctx, ct, A_y), rot = gnum(ctx, ct, A__rotation);
    double sx = gnum(ctx, ct, A__scaleX), sy = gnum(ctx, ct, A__scaleY);
    if (par) {
        // loadIdentity().multiply(parent).translate(x, y).rotate(rot).scale(sx, sy), float32 storage
        for (int i = 0; i < 6; i++) m[i] = par[i];
        m[4] = (double)m[0] * x + (double)m[2] * y + m[4];
        m[5] = (double)m[1] * x + (double)m[3] * y + m[5];
        double sn = sin(rot), cs = cos(rot), a = m[0], b = m[1], c = m[2], d = m[3];
        m[0] = a * cs + c * sn;
        m[1] = b * cs + d * sn;
        m[2] = a * -sn + c * cs;
        m[3] = b * -sn + d * cs;
        m[0] = m[0] * sx, m[1] = m[1] * sx, m[2] = m[2] * sy, m[3] = m[3] * sy;
    } else {
        // applyITRS
        double sn = sin(rot), cs = cos(rot);
        m[4] = x, m[5] = y, m[0] = cs * sx, m[1] = sn * sx, m[2] = -sn * sy, m[3] = cs * sy;
    }
}

static int render_container(JSContext *ctx, rctx *r, JSValueConst ct, JSValueConst parent_tm);

// A Text that batch_sprite can draw: js/fastpath.js's text renderer, text already drawn (not
// waiting for its lazy update), not empty, no flip/crop/post pipeline, on a plain pipeline whose
// onBind does nothing (batchTexture would bind it a second time). Sets *pipeline and *frame.
static bool fast_text(JSContext *ctx, JSValueConst t, JSValueConst rw, JSValue *pipeline, JSValue *frame) {
    if (!same(rw, H.text_render) || gbool(ctx, t, A__pkrTextDirty) ||
        gnum(ctx, t, H.text_lazy ? A__pkr_width : A_width) == 0 ||
        gnum(ctx, t, H.text_lazy ? A__pkr_height : A_height) == 0 || gbool(ctx, t, A_flipX) || gbool(ctx, t, A_flipY) || gbool(ctx, t, A_isCropped) ||
        gbool(ctx, t, A_hasPostPipeline))
        return false;
    *pipeline = G(t, A_pipeline);
    if (!fast_pipeline(ctx, *pipeline) || !gsame(ctx, *pipeline, A_onBind, H.wp_on_bind)) return false;
    *frame = G(t, A_frame);
    JSValue src = G(*frame, A_source), tex = G(src, A_glTexture);
    bool rt = gbool(ctx, tex, A_isRenderTexture);
    JS_FreeValue(ctx, tex), JS_FreeValue(ctx, src);
    return !rt && gnum(ctx, *frame, A_width) != 0;
}

static int set_type(JSContext *ctx, rctx *r, JSValueConst list, JSValueConst child, int64_t i, int64_t n) {
    JSValue type = G(child, A_type), cur = G(r->renderer, A_currentType);
    if (!JS_IsStrictEqual(ctx, type, cur)) {
        sset(ctx, r->renderer, A_newType, JS_TRUE);
        sset(ctx, r->renderer, A_currentType, JS_DupValue(ctx, type));
        JS_FreeValue(ctx, cur);
        cur = JS_DupValue(ctx, type);
    }
    bool match = false;
    if (i < n - 1) {
        JSValue next = JS_GetPropertyInt64(ctx, list, i + 1), nt = G(next, A_type);
        match = JS_IsStrictEqual(ctx, nt, cur);
        JS_FreeValue(ctx, nt), JS_FreeValue(ctx, next);
    }
    sset(ctx, r->renderer, A_nextTypeMatch, JS_NewBool(ctx, match));
    JS_FreeValue(ctx, type), JS_FreeValue(ctx, cur);
    return 0;
}

// A Sprite/Image on a pipeline with Phaser's MultiPipeline.batchSprite that batch_sprite can't take
// (PokeRogue's SpritePipeline has its own batchQuad): generic_batch_sprite does batchSprite's part.
static bool generic_sprite_ok(JSContext *ctx, JSValueConst pipeline, JSValueConst bs, JSValueConst go) {
    if (!JS_IsObject(pipeline) || !(same(bs, H.mp_batch_sprite) || same(bs, H.mp_orig_batch_sprite))) return false;
    if (gbool(ctx, go, A_isCropped)) return false;
    JSValue frame = G(go, A_frame);
    bool ok = JS_IsObject(frame) && !gbool(ctx, frame, A_customPivot);
    JS_FreeValue(ctx, frame);
    return ok;
}

// MultiPipeline.batchSprite(go, camera, parentMatrix) for an uncropped sprite without a custom
// pivot: the matrix and tint math here, everything else (manager.set and the pipeline's onBind,
// flush, setGameObject, batchQuad and its onBatch) called as Phaser calls it.
static int generic_batch_sprite(JSContext *ctx, rctx *r, JSValueConst pipeline, JSValueConst go, JSValueConst tm) {
    int rc = -1;
    JSValue frame = JS_UNDEFINED, src = JS_UNDEFINED, tex = JS_UNDEFINED, unitv = JS_UNDEFINED, tmm = JS_UNDEFINED, res = JS_UNDEFINED;
    {
        JSValueConst args[2] = {pipeline, go};
        if (call0(ctx, r->mgr, A_set, 2, args)) goto out;
    }
    frame = G(go, A_frame);
    src = G(frame, A_source);
    tex = G(src, A_glTexture);
    double dox = gnum(ctx, go, A__displayOriginX), doy = gnum(ctx, go, A__displayOriginY);
    double x = -dox + gnum(ctx, frame, A_x), y = -doy + gnum(ctx, frame, A_y), fx = 1, fy = 1;
    if (gbool(ctx, go, A_flipX)) x += -gnum(ctx, frame, A_realWidth) + dox * 2, fx = -1;
    if (gbool(ctx, go, A_flipY)) y += -gnum(ctx, frame, A_realHeight) + doy * 2, fy = -1;
    double gx = gnum(ctx, go, A_x), gy = gnum(ctx, go, A_y);
    if (r->round_pixels) gx = floor(gx), gy = floor(gy);
    float *par = NULL;
    if (JS_IsObject(tm)) {
        tmm = G(tm, A_matrix);
        par = f32(ctx, tmm);
        if (!par) {
            JS_ThrowTypeError(ctx, "batchSprite: no parent matrix");
            goto out;
        }
    }
    double a = r->alpha;
    sprite_quad q = {
        .offX = -r->scroll_x * gnum(ctx, go, A_scrollFactorX), .offY = -r->scroll_y * gnum(ctx, go, A_scrollFactorY),
        .gx = gx, .gy = gy, .rot = gnum(ctx, go, A__rotation),
        .sx = gnum(ctx, go, A__scaleX) * fx, .sy = gnum(ctx, go, A__scaleY) * fy,
        .x = x, .y = y, .w = gnum(ctx, frame, A_cutWidth), .h = gnum(ctx, frame, A_cutHeight),
        .round = r->render_round,
    };
    float k[8];
    sprite_corners(k, r->cam_m, par, &q);
    uint32_t tTL = tint(gnum(ctx, go, A_tintTopLeft), a * gnum(ctx, go, A__alphaTL));
    uint32_t tTR = tint(gnum(ctx, go, A_tintTopRight), a * gnum(ctx, go, A__alphaTR));
    uint32_t tBL = tint(gnum(ctx, go, A_tintBottomLeft), a * gnum(ctx, go, A__alphaBL));
    uint32_t tBR = tint(gnum(ctx, go, A_tintBottomRight), a * gnum(ctx, go, A__alphaBR));
    if (gnum(ctx, pipeline, A_vertexCount) + 6 > gnum(ctx, pipeline, A_vertexCapacity) && call0(ctx, pipeline, A_flush, 0, NULL))
        goto out;
    {
        JSValueConst args[2] = {go, frame};
        unitv = call(ctx, pipeline, A_setGameObject, 2, args);
        if (JS_IsException(unitv)) goto out;
    }
    bool post = gbool(ctx, go, A_hasPostPipeline);
    if ((post || !gsame(ctx, r->mgr, A_preBatch, H.pm_pre_batch)) && call0(ctx, r->mgr, A_preBatch, 1, &go)) goto out;
    {
        JSValue args[20];
        args[0] = go;
        for (int i = 0; i < 8; i++) args[1 + i] = JS_NewFloat64(ctx, k[i]);
        args[9] = G(frame, A_u0), args[10] = G(frame, A_v0), args[11] = G(frame, A_u1), args[12] = G(frame, A_v1);
        args[13] = JS_NewUint32(ctx, tTL), args[14] = JS_NewUint32(ctx, tTR), args[15] = JS_NewUint32(ctx, tBL);
        args[16] = JS_NewUint32(ctx, tBR), args[17] = G(go, A_tintFill), args[18] = tex, args[19] = unitv;
        res = call(ctx, pipeline, A_batchQuad, 20, (JSValueConst *)args);
        for (int i = 9; i < 13; i++) JS_FreeValue(ctx, args[i]);
        JS_FreeValue(ctx, args[17]);
        if (JS_IsException(res)) goto out;
    }
    if ((post || !gsame(ctx, r->mgr, A_postBatch, H.pm_post_batch)) && call0(ctx, r->mgr, A_postBatch, 1, &go)) goto out;
    rc = 0;
out:
    JS_FreeValue(ctx, frame), JS_FreeValue(ctx, src), JS_FreeValue(ctx, tex), JS_FreeValue(ctx, unitv);
    JS_FreeValue(ctx, tmm), JS_FreeValue(ctx, res);
    return rc;
}

// Phaser's generic per-child path (ContainerWebGLRenderer's loop body after willRender).
static int render_generic(JSContext *ctx, rctx *r, JSValueConst ct, JSValueConst tm, JSValueConst list, JSValueConst child,
                          int64_t i, int64_t n, double alpha, double sfx, double sfy) {
    int rc = -1;
    double tl, tr, bl, br;
    JSValue atl = G(child, A_alphaTopLeft);
    if (!JS_IsUndefined(atl)) {
        tl = num(ctx, atl);
        tr = gnum(ctx, child, A_alphaTopRight), bl = gnum(ctx, child, A_alphaBottomLeft), br = gnum(ctx, child, A_alphaBottomRight);
    } else {
        tl = tr = bl = br = gnum(ctx, child, A_alpha);
    }
    JS_FreeValue(ctx, atl);
    double csx = gnum(ctx, child, A_scrollFactorX), csy = gnum(ctx, child, A_scrollFactorY);
    JSValue mask = G(child, A_mask);
    bool has_mask = JS_ToBool(ctx, mask) > 0;
    if (has_mask) {
        JSValueConst args[3] = {r->renderer, child, r->camera};
        if (call0(ctx, mask, A_preRenderWebGL, 3, args)) goto out;
    }
    set_type(ctx, r, list, child, i, n);
    if (set_scroll_factor(ctx, child, csx * sfx, csy * sfy) || set_alpha4(ctx, child, tl * alpha, tr * alpha, bl * alpha, br * alpha))
        goto out;
    {
        JSValue rw = G(child, A_renderWebGL);
        int e;
        if (same(rw, H.image_render) || same(rw, H.sprite_render)) {
            // SpriteWebGLRenderer / ImageWebGLRenderer: camera.addToRenderList(src), then
            // src.pipeline.batchSprite(src, camera, parentMatrix)
            e = add_to_render_list(ctx, r, child);
            JSValue pipeline = e ? JS_UNDEFINED : G(child, A_pipeline);
            if (!e) {
                JSValue bs = G(pipeline, A_batchSprite);
                if (generic_sprite_ok(ctx, pipeline, bs, child)) {
                    e = generic_batch_sprite(ctx, r, pipeline, child, tm);
                } else {
                    JSValueConst args[3] = {child, r->camera, tm};
                    JSValue v = JS_Call(ctx, bs, pipeline, 3, args);
                    e = JS_IsException(v) ? -1 : 0;
                    JS_FreeValue(ctx, v);
                }
                JS_FreeValue(ctx, bs);
            }
            JS_FreeValue(ctx, pipeline);
        } else {
            JSValueConst args[5] = {r->renderer, child, r->camera, tm, ct};
            JSValue v = JS_Call(ctx, rw, child, 5, args);
            e = JS_IsException(v) ? -1 : 0;
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, rw);
        if (e) goto out;
    }
    if (set_alpha4(ctx, child, tl, tr, bl, br) || set_scroll_factor(ctx, child, csx, csy)) goto out;
    if (has_mask) {
        JSValueConst args[2] = {r->renderer, r->camera};
        if (call0(ctx, mask, A_postRenderWebGL, 2, args)) goto out;
    }
    sset(ctx, r->renderer, A_newType, JS_FALSE);
    rc = 0;
out:
    JS_FreeValue(ctx, mask);
    return rc;
}

static int render_container(JSContext *ctx, rctx *r, JSValueConst ct, JSValueConst parent_tm) {
    if (add_to_render_list(ctx, r, ct)) return -1;
    int rc = -1;
    JSValue list = G(ct, A_list), tm = G(ct, A_localTransform), tmm = JS_UNDEFINED, ptmm = JS_UNDEFINED;
    int64_t n = 0;
    if (JS_GetLength(ctx, list, &n) < 0) goto out;
    if (n == 0) {
        rc = 0;
        goto out;
    }
    tmm = G(tm, A_matrix);
    float *m = f32(ctx, tmm);
    float *par = NULL;
    if (JS_IsObject(parent_tm)) {
        ptmm = G(parent_tm, A_matrix);
        par = f32(ctx, ptmm);
    }
    if (!m) {
        JS_ThrowTypeError(ctx, "renderContainer: no localTransform");
        goto out;
    }
    container_matrix(ctx, ct, m, par);
    bool post = gbool(ctx, ct, A_hasPostPipeline);
    if (post && call0(ctx, r->mgr, A_preBatch, 1, &ct)) goto out;
    bool has_blend = gnum(ctx, ct, A__blendMode) != -1;
    if (!has_blend && gnum(ctx, r->renderer, A_currentBlendMode) != 0) {
        JSValueConst zero = JS_NewInt32(ctx, 0);
        if (call0(ctx, r->renderer, A_setBlendMode, 1, &zero)) goto out;
    }
    double alpha = gnum(ctx, ct, A__alpha), sfx = gnum(ctx, ct, A_scrollFactorX), sfy = gnum(ctx, ct, A_scrollFactorY);
    for (int64_t i = 0; i < n; i++) {
        JSValue child = JS_GetPropertyInt64(ctx, list, i);
        int w = will_render(ctx, r, child);
        if (w <= 0) {
            JS_FreeValue(ctx, child);
            if (w < 0) goto out;
            continue;
        }
        if (!has_blend) {
            JSValue bm = G(child, A__blendMode);
            if (JS_IsUndefined(bm)) bm = G(child, A_blendMode);
            if (gnum(ctx, r->renderer, A_currentBlendMode) != num(ctx, bm)) {
                int e = call0(ctx, r->renderer, A_setBlendMode, 1, (JSValueConst *)&bm);
                if (e) {
                    JS_FreeValue(ctx, bm), JS_FreeValue(ctx, child);
                    goto out;
                }
            }
            JS_FreeValue(ctx, bm);
        }
        JSValue rw = G(child, A_renderWebGL), mask = G(child, A_mask), pipeline = JS_UNDEFINED, frame = JS_UNDEFINED;
        bool no_mask = JS_ToBool(ctx, mask) <= 0;
        JS_FreeValue(ctx, mask);
        int e = 0;
        if (no_mask && (same(rw, H.image_render) || same(rw, H.sprite_render)) && !gbool(ctx, child, A_hasPostPipeline) &&
            !gbool(ctx, child, A_isCropped) && fast_pipeline(ctx, (pipeline = G(child, A_pipeline))) &&
            !gbool(ctx, (frame = G(child, A_frame)), A_customPivot)) {
            set_type(ctx, r, list, child, i, n);
            e = add_to_render_list(ctx, r, child);
            if (!e) e = batch_sprite(ctx, r, pipeline, child, frame, m, alpha, sfx, sfy, false);
            sset(ctx, r->renderer, A_newType, JS_FALSE);
        } else if (no_mask && fast_text(ctx, child, rw, &pipeline, &frame)) {
            set_type(ctx, r, list, child, i, n);
            e = add_to_render_list(ctx, r, child);
            if (!e) e = batch_sprite(ctx, r, pipeline, child, frame, m, alpha, sfx, sfy, true);
            sset(ctx, r->renderer, A_newType, JS_FALSE);
        } else if (no_mask && fast_bitmap_text(ctx, child, rw, &pipeline)) {
            set_type(ctx, r, list, child, i, n);
            e = add_to_render_list(ctx, r, child);
            if (!e) e = batch_bitmap_text(ctx, r, pipeline, child, m, alpha, sfx, sfy);
            sset(ctx, r->renderer, A_newType, JS_FALSE);
        } else if (no_mask && same(rw, H.container_render)) {
            // Phaser sets the child's scroll factor and alpha to the combined values while it renders.
            set_type(ctx, r, list, child, i, n);
            double csx = gnum(ctx, child, A_scrollFactorX), csy = gnum(ctx, child, A_scrollFactorY);
            double ca = gnum(ctx, child, A__alpha);
            e = set_scroll_factor(ctx, child, csx * sfx, csy * sfy) || set_alpha4(ctx, child, ca * alpha, ca * alpha, ca * alpha, ca * alpha) ||
                render_container(ctx, r, child, tm);
            if (!e) e = set_alpha4(ctx, child, ca, ca, ca, ca) || set_scroll_factor(ctx, child, csx, csy);
            sset(ctx, r->renderer, A_newType, JS_FALSE);
        } else {
            e = render_generic(ctx, r, ct, tm, list, child, i, n, alpha, sfx, sfy);
        }
        JS_FreeValue(ctx, rw), JS_FreeValue(ctx, pipeline), JS_FreeValue(ctx, frame), JS_FreeValue(ctx, child);
        if (e) goto out;
    }
    if (post && call0(ctx, r->mgr, A_postBatch, 1, &ct)) goto out;
    rc = 0;
out:
    JS_FreeValue(ctx, list), JS_FreeValue(ctx, tm), JS_FreeValue(ctx, tmm), JS_FreeValue(ctx, ptmm);
    return rc;
}

// renderContainer(renderer, container, camera, parentMatrix): Container.prototype.renderWebGL.
static JSValue js_render_container(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!H.ready || argc < 3) return JS_ThrowTypeError(ctx, "renderContainer: not set up");
    rctx r = {.renderer = argv[0], .camera = argv[2], .render_list = JS_UNDEFINED};
    JSValue mgr = G(argv[0], A_pipelines), cm = G(argv[2], A_matrixCam), cmm = G(cm, A_matrix);
    JSValue add = G(argv[2], A_addToRenderList);
    float *cam = f32(ctx, cmm);
    JSValue ret = JS_UNDEFINED;
    if (!cam) {
        ret = JS_ThrowTypeError(ctx, "renderContainer: no camera matrix");
        goto out;
    }
    memcpy(r.cam_m, cam, sizeof r.cam_m);
    r.mgr = mgr;
    if (same(add, H.cam_add_to_render_list)) r.render_list = G(argv[2], A_renderList);
    r.scroll_x = gnum(ctx, argv[2], A_scrollX), r.scroll_y = gnum(ctx, argv[2], A_scrollY);
    r.alpha = gnum(ctx, argv[2], A_alpha), r.id = gnum(ctx, argv[2], A_id);
    r.round_pixels = gbool(ctx, argv[2], A_roundPixels), r.render_round = gbool(ctx, argv[2], A_renderRoundPixels);
    if (render_container(ctx, &r, argv[1], argc > 3 ? argv[3] : JS_UNDEFINED)) ret = JS_EXCEPTION;
out:
    JS_FreeValue(ctx, mgr), JS_FreeValue(ctx, cm), JS_FreeValue(ctx, cmm), JS_FreeValue(ctx, add), JS_FreeValue(ctx, r.render_list);
    return ret;
}

// renderSetup({imageRender, spriteRender, containerRender, mpBatchSprite, mpBatchQuad, goWillRender,
//   containerWillRender, wpOnBind, wpOnBatch, wpSetGameObject, camAddToRenderList}): the functions
// whose identity tells renderContainer that an object or pipeline is the plain Phaser one.
static JSValue js_render_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    JSValueConst o = argv[0];
#define HOOK(field, name) JS_FreeValue(ctx, H.field), H.field = JS_GetPropertyStr(ctx, o, name);
    HOOK(image_render, "imageRender") HOOK(sprite_render, "spriteRender") HOOK(container_render, "containerRender")
    HOOK(mp_batch_sprite, "mpBatchSprite") HOOK(mp_batch_quad, "mpBatchQuad") HOOK(go_will_render, "goWillRender")
    HOOK(ct_will_render, "containerWillRender") HOOK(wp_on_bind, "wpOnBind") HOOK(wp_on_batch, "wpOnBatch")
    HOOK(wp_set_game_object, "wpSetGameObject") HOOK(cam_add_to_render_list, "camAddToRenderList")
    HOOK(text_render, "textRender") HOOK(alpha_set, "alphaSetAlpha") HOOK(alpha_single_set, "alphaSingleSetAlpha")
    HOOK(sf_set, "setScrollFactor") HOOK(mp_orig_batch_sprite, "mpOrigBatchSprite") HOOK(pm_pre_batch, "pmPreBatch")
    HOOK(pm_post_batch, "pmPostBatch") HOOK(ct_sf_set, "containerSetScrollFactor")
    HOOK(alpha_single_setter, "alphaSingleSetter") HOOK(wp_push_batch, "wpPushBatch") HOOK(wp_create_batch, "wpCreateBatch")
    HOOK(wp_set_vertex_buffer, "wpSetVertexBuffer") HOOK(wp_on_before_flush, "wpOnBeforeFlush")
    HOOK(wp_on_after_flush, "wpOnAfterFlush")
#undef HOOK
    JSValue tl = JS_GetPropertyStr(ctx, o, "textLazySize");
    H.text_lazy = JS_ToBool(ctx, tl) > 0;
    JS_FreeValue(ctx, tl);
    JSValue eb = JS_GetPropertyStr(ctx, o, "evBeforeFlush"), ea = JS_GetPropertyStr(ctx, o, "evAfterFlush");
    H.ev_before_flush = JS_ValueToAtom(ctx, eb);
    H.ev_after_flush = JS_ValueToAtom(ctx, ea);
    JS_FreeValue(ctx, eb), JS_FreeValue(ctx, ea);
    H.ready = true;
    return JS_UNDEFINED;
}

// bitmapTextSetup(render, wpShouldFlush, btGetTextBounds): js/fastpath.js part 18's renderer and
// the Phaser functions batch_bitmap_text relies on, so containers draw such texts here.
static JSValue js_bitmap_text_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 3) return JS_ThrowTypeError(ctx, "bitmapTextSetup: 3 arguments");
    JS_FreeValue(ctx, H.bitmap_text_render), JS_FreeValue(ctx, H.wp_should_flush), JS_FreeValue(ctx, H.bt_get_text_bounds);
    H.bitmap_text_render = JS_DupValue(ctx, argv[0]);
    H.wp_should_flush = JS_DupValue(ctx, argv[1]);
    H.bt_get_text_bounds = JS_DupValue(ctx, argv[2]);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- WebGLPipeline batching
// pushBatch / flush / MultiPipeline.batchQuad: Phaser's code in C. PokeRogue's field and sprite
// pipelines flush after every sprite, so these run for nearly every object drawn in a battle.

// WebGLPipeline.createBatch(texture)
static JSValue create_batch(JSContext *ctx, JSValueConst p, JSValueConst tex) {
    if (!gsame(ctx, p, A_createBatch, H.wp_create_batch)) return call(ctx, p, A_createBatch, 1, &tex);
    JSValue b = JS_NewObject(ctx), arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_DupValue(ctx, tex));
    sset(ctx, b, A_start, JS_NewFloat64(ctx, gnum(ctx, p, A_vertexCount)));
    sset(ctx, b, A_count, JS_NewInt32(ctx, 0));
    sset(ctx, b, A_texture, arr);
    sset(ctx, b, A_unit, JS_NewInt32(ctx, 0));
    sset(ctx, b, A_maxUnit, JS_NewInt32(ctx, 0));
    sset(ctx, p, A_currentBatch, JS_DupValue(ctx, b));
    sset(ctx, p, A_currentUnit, JS_NewInt32(ctx, 0));
    sset(ctx, p, A_currentTexture, JS_DupValue(ctx, tex));
    JSValue list = G(p, A_batch);
    int64_t n = 0;
    int e = JS_GetLength(ctx, list, &n);
    if (!e) e = JS_SetPropertyInt64(ctx, list, n, b) < 0 ? -1 : 0;
    else JS_FreeValue(ctx, b);
    JS_FreeValue(ctx, list);
    return e ? JS_EXCEPTION : JS_NewInt32(ctx, 0);
}

// WebGLPipeline.pushBatch(texture) -> unit
static JSValue push_batch(JSContext *ctx, JSValueConst p, JSValueConst tex) {
    JSValue cb = G(p, A_currentBatch), ct = G(p, A_currentTexture), ret;
    if (JS_ToBool(ctx, cb) <= 0 || (gbool(ctx, p, A_forceZero) && !JS_IsStrictEqual(ctx, tex, ct))) {
        ret = create_batch(ctx, p, tex);
    } else if (JS_IsStrictEqual(ctx, tex, ct)) {
        ret = G(p, A_currentUnit);
    } else {
        JSValue texs = G(cb, A_texture);
        int64_t n = 0, idx = -1;
        if (JS_GetLength(ctx, texs, &n) < 0) {
            ret = JS_EXCEPTION;
        } else {
            for (int64_t i = 0; i < n && idx < 0; i++) { // indexOf
                JSValue t = JS_GetPropertyInt64(ctx, texs, i);
                if (JS_IsStrictEqual(ctx, t, tex)) idx = i;
                JS_FreeValue(ctx, t);
            }
            if (idx >= 0) {
                sset(ctx, p, A_currentUnit, JS_NewInt64(ctx, idx));
                sset(ctx, p, A_currentTexture, JS_DupValue(ctx, tex));
                ret = JS_NewInt64(ctx, idx);
            } else {
                JSValue rend = G(p, A_renderer);
                double maxt = gnum(ctx, rend, A_maxTextures);
                JS_FreeValue(ctx, rend);
                if ((double)n == maxt) {
                    ret = create_batch(ctx, p, tex);
                } else {
                    double unit = gnum(ctx, cb, A_unit) + 1;
                    sset(ctx, cb, A_unit, JS_NewFloat64(ctx, unit));
                    sset(ctx, cb, A_maxUnit, JS_NewFloat64(ctx, gnum(ctx, cb, A_maxUnit) + 1));
                    JS_SetPropertyInt64(ctx, texs, n, JS_DupValue(ctx, tex));
                    sset(ctx, p, A_currentUnit, JS_NewFloat64(ctx, unit));
                    sset(ctx, p, A_currentTexture, JS_DupValue(ctx, tex));
                    ret = JS_NewFloat64(ctx, unit);
                }
            }
        }
        JS_FreeValue(ctx, texs);
    }
    JS_FreeValue(ctx, cb), JS_FreeValue(ctx, ct);
    return ret;
}
static JSValue js_push_batch(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return push_batch(ctx, this_val, argc ? argv[0] : JS_UNDEFINED);
}

// this.emit(event, this, isPostFlush), skipped when nothing listens (eventemitter3's _events)
static int emit_flush_event(JSContext *ctx, JSValueConst p, JSAtom ev, JSValueConst post) {
    JSValue evs = G(p, A__events), l = JS_IsObject(evs) ? JS_GetProperty(ctx, evs, ev) : JS_UNDEFINED;
    bool listened = !JS_IsUndefined(l);
    JS_FreeValue(ctx, evs), JS_FreeValue(ctx, l);
    if (!listened) return 0;
    JSValueConst args[3] = {JS_AtomToValue(ctx, ev), p, post};
    int e = call0(ctx, p, A_emit, 3, args);
    JS_FreeValue(ctx, (JSValue)args[0]);
    return e;
}

static int gl_call(JSContext *ctx, JSValueConst gl, int method, int argc, JSValueConst *argv) { return call0(ctx, gl, method, argc, argv); }

// WebGLPipeline.flush(isPostFlush)
static JSValue js_flush(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst p = this_val, post = argc > 0 && !JS_IsUndefined(argv[0]) ? argv[0] : JS_FALSE;
    double n = gnum(ctx, p, A_vertexCount);
    if (!(n > 0)) return JS_DupValue(ctx, p);
    JSValue gl = JS_UNDEFINED, batches = JS_UNDEFINED, act = JS_UNDEFINED;
    if (emit_flush_event(ctx, p, H.ev_before_flush, post)) goto fail;
    if (!gsame(ctx, p, A_onBeforeFlush, H.wp_on_before_flush) && call0(ctx, p, A_onBeforeFlush, 1, &post)) goto fail;
    gl = G(p, A_gl);
    JSValue sh = G(p, A_currentShader);
    double vsize = gnum(ctx, sh, A_vertexSize);
    JS_FreeValue(ctx, sh);
    double topo = gnum(ctx, p, A_topology);
    if (gbool(ctx, p, A_active)) {
        // setVertexBuffer()
        if (gsame(ctx, p, A_setVertexBuffer, H.wp_set_vertex_buffer)) {
            JSValue vb = G(p, A_vertexBuffer), ab = G(p, A_activeBuffer);
            int e = 0;
            if (!JS_IsStrictEqual(ctx, vb, ab)) {
                JSValue wb = G(vb, A_webGLBuffer);
                JSValueConst args[2] = {JS_NewInt32(ctx, 0x8892), wb};
                e = gl_call(ctx, gl, A_bindBuffer, 2, args);
                JS_FreeValue(ctx, wb);
                if (!e) sset(ctx, p, A_activeBuffer, JS_DupValue(ctx, vb));
            }
            JS_FreeValue(ctx, vb), JS_FreeValue(ctx, ab);
            if (e) goto fail;
        } else if (call0(ctx, p, A_setVertexBuffer, 0, NULL)) {
            goto fail;
        }
        if (n == gnum(ctx, p, A_vertexCapacity)) {
            JSValue vd = G(p, A_vertexData);
            JSValueConst args[3] = {JS_NewInt32(ctx, 0x8892), vd, JS_NewInt32(ctx, 0x88E8)};
            int e = gl_call(ctx, gl, A_bufferData, 3, args);
            JS_FreeValue(ctx, vd);
            if (e) goto fail;
        } else {
            JSValue bytes = G(p, A_bytes);
            size_t off = 0, len = 0, bpe = 0, total = 0;
            JSValue ab = JS_GetTypedArrayBuffer(ctx, bytes, &off, &len, &bpe);
            JS_FreeValue(ctx, bytes);
            if (JS_IsException(ab)) goto fail;
            uint8_t *base = JS_GetArrayBuffer(ctx, &total, ab);
            JS_FreeValue(ctx, ab);
            if (!base) goto fail;
            size_t want = (size_t)(n * vsize);
            if (want > len) want = len; // subarray clamps
            gl_buffer_sub_data(0x8892, 0, base + off, want);
        }
        batches = G(p, A_batch);
        act = G(p, A_activeTextures);
        int64_t nb = 0;
        if (JS_GetLength(ctx, batches, &nb) < 0) goto fail;
        bool fz = gbool(ctx, p, A_forceZero);
        if (fz) {
            JSValue a0 = JS_GetPropertyInt64(ctx, act, 0);
            bool has = JS_ToBool(ctx, a0) > 0;
            JS_FreeValue(ctx, a0);
            JSValueConst t0 = JS_NewInt32(ctx, 0x84C0);
            if (!has && gl_call(ctx, gl, A_activeTexture, 1, &t0)) goto fail;
        }
        for (int64_t bi = 0; bi < nb; bi++) {
            JSValue b = JS_GetPropertyInt64(ctx, batches, bi), texs = G(b, A_texture);
            int maxu = fz ? 0 : (int)gnum(ctx, b, A_maxUnit);
            int e = 0;
            for (int d = 0; d <= maxu && !e; d++) {
                JSValue c = JS_GetPropertyInt64(ctx, texs, d), cur = JS_GetPropertyInt64(ctx, act, d);
                if (!JS_IsStrictEqual(ctx, cur, c)) {
                    if (!fz) {
                        JSValueConst u = JS_NewInt32(ctx, 0x84C0 + d);
                        e = gl_call(ctx, gl, A_activeTexture, 1, &u);
                    }
                    if (!e) {
                        JSValue wt = G(c, A_webGLTexture);
                        JSValueConst args[2] = {JS_NewInt32(ctx, 0x0DE1), wt};
                        e = gl_call(ctx, gl, A_bindTexture, 2, args);
                        JS_FreeValue(ctx, wt);
                    }
                    if (!e) e = JS_SetPropertyInt64(ctx, act, d, JS_DupValue(ctx, c)) < 0;
                }
                JS_FreeValue(ctx, c), JS_FreeValue(ctx, cur);
            }
            if (!e) {
                JSValueConst args[3] = {JS_NewFloat64(ctx, topo), G(b, A_start), G(b, A_count)};
                e = gl_call(ctx, gl, A_drawArrays, 3, args);
                JS_FreeValue(ctx, (JSValue)args[1]), JS_FreeValue(ctx, (JSValue)args[2]);
            }
            JS_FreeValue(ctx, texs), JS_FreeValue(ctx, b);
            if (e) goto fail;
        }
    }
    sset(ctx, p, A_vertexCount, JS_NewInt32(ctx, 0));
    {
        JSValue list = JS_IsUndefined(batches) ? G(p, A_batch) : JS_DupValue(ctx, batches);
        int e = JS_SetProperty(ctx, list, AT[A_length], JS_NewInt32(ctx, 0));
        JS_FreeValue(ctx, list);
        if (e < 0) goto fail;
    }
    sset(ctx, p, A_currentBatch, JS_NULL);
    sset(ctx, p, A_currentTexture, JS_NULL);
    sset(ctx, p, A_currentUnit, JS_NewInt32(ctx, 0));
    if (emit_flush_event(ctx, p, H.ev_after_flush, post)) goto fail;
    if (!gsame(ctx, p, A_onAfterFlush, H.wp_on_after_flush) && call0(ctx, p, A_onAfterFlush, 1, &post)) goto fail;
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, batches), JS_FreeValue(ctx, act);
    return JS_DupValue(ctx, p);
fail:
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, batches), JS_FreeValue(ctx, act);
    return JS_EXCEPTION;
}

// MultiPipeline.batchQuad(go, x0, y0, x1, y1, x2, y2, x3, y3, u0, v0, u1, v1, tintTL, tintTR,
//   tintBL, tintBR, tintEffect, texture, unit) -> hasFlushed. Pipelines with another vertex layout
//   use Phaser's (rt.origBatchQuad, passed as the function's data).
static JSValue js_batch_quad(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValue *data) {
    JSValueConst p = this_val;
    JSValueConst a[20];
    for (int i = 0; i < 20; i++) a[i] = i < argc ? argv[i] : JS_UNDEFINED;
    JSValue sh = G(p, A_currentShader);
    double vcc = JS_IsObject(sh) ? gnum(ctx, sh, A_vertexComponentCount) : 0;
    JS_FreeValue(ctx, sh);
    if (vcc != 7) return JS_Call(ctx, data[0], p, argc, argv);
    JSValue unitv = JS_IsUndefined(a[19]) ? G(p, A_currentUnit) : JS_DupValue(ctx, a[19]);
    bool flushed = false;
    if (gnum(ctx, p, A_vertexCount) + 6 > gnum(ctx, p, A_vertexCapacity)) {
        if (call0(ctx, p, A_flush, 0, NULL)) goto fail;
        flushed = true;
    }
    JSValue cb = G(p, A_currentBatch);
    bool has_cb = JS_ToBool(ctx, cb) > 0;
    JS_FreeValue(ctx, cb);
    if (!has_cb) { // unit = this.setTexture2D(texture)
        JS_FreeValue(ctx, unitv);
        unitv = call(ctx, p, A_setTexture2D, 1, &a[18]);
        if (JS_IsException(unitv)) goto fail;
    }
    {
        double v[18];
        for (int i = 1; i <= 17; i++) v[i] = num(ctx, a[i]);
        float unit = (float)num(ctx, unitv), te = (float)v[17];
        uint32_t t[4];
        for (int i = 0; i < 4; i++) {
            uint32_t u = 0;
            JS_ToUint32(ctx, &u, a[13 + i]);
            t[i] = u;
        }
        JSValue vd = G(p, A_vertexData);
        size_t vsize;
        uint8_t *vb = JS_GetArrayBuffer(ctx, &vsize, vd);
        JS_FreeValue(ctx, vd);
        double vc = gnum(ctx, p, A_vertexCount);
        if (!vb || (vc * 7 + 42) * 4 > vsize) {
            JS_ThrowRangeError(ctx, "batchQuad: vertex buffer overflow");
            goto fail;
        }
        float *vf = (float *)vb + (size_t)vc * 7;
        uint32_t *vu = (uint32_t *)vf;
        // x0 y0 u0 v0 | x1 y1 u0 v1 | x2 y2 u1 v1 | x0 y0 u0 v0 | x2 y2 u1 v1 | x3 y3 u1 v0
        const int xi[6] = {1, 3, 5, 1, 5, 7}, ui[6] = {9, 9, 11, 9, 11, 11}, vi[6] = {10, 12, 12, 10, 12, 10}, ti[6] = {0, 2, 3, 0, 3, 1};
        for (int k = 0; k < 6; k++) {
            vf[k * 7 + 0] = (float)v[xi[k]], vf[k * 7 + 1] = (float)v[xi[k] + 1];
            vf[k * 7 + 2] = (float)v[ui[k]], vf[k * 7 + 3] = (float)v[vi[k]];
            vf[k * 7 + 4] = unit, vf[k * 7 + 5] = te, vu[k * 7 + 6] = t[ti[k]];
        }
        vc += 6;
        sset(ctx, p, A_vertexCount, JS_NewInt32(ctx, (int32_t)vc));
        JSValue cb2 = G(p, A_currentBatch);
        sset(ctx, cb2, A_count, JS_NewFloat64(ctx, vc - gnum(ctx, cb2, A_start)));
        JS_FreeValue(ctx, cb2);
    }
    JS_FreeValue(ctx, unitv);
    if (!gsame(ctx, p, A_onBatch, H.wp_on_batch) && call0(ctx, p, A_onBatch, 1, &a[0])) return JS_EXCEPTION;
    return JS_NewBool(ctx, flushed);
fail:
    JS_FreeValue(ctx, unitv);
    return JS_EXCEPTION;
}

// makeBatchQuad(phaserBatchQuad) -> the C batchQuad (falls back to Phaser's for other layouts)
static JSValue js_make_batch_quad(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_NewCFunctionData(ctx, js_batch_quad, 20, 0, 1, argv);
}

// ---------------------------------------------------------------- uniforms
// ---------------------------------------------------------------- pipeline machinery
// Phaser's pipeline switching and uniform wrappers in C (docs/native-renderer.md step 2):
// PipelineManager.set / isCurrent, WebGLPipeline.bind / updateProjectionMatrix and its set*
// uniform wrappers, WebGLShader.bind / setAttribPointers / set*, WebGLRenderer.setProgram /
// setBlendMode. PokeRogue's sprite pipelines switch pipeline around nearly every sprite and set
// about a dozen uniforms each time, so these run thousands of times per battle frame.
// Each function calls the others directly when an object still has the native version (checked
// by identity, so a game that overrides one still gets its own), and through JS otherwise.

// The uniform setters: WebGLShader.<name>(name, ...) -> this.setUniformN(gl.<gl>, name, ...).
enum { U_PLAIN, U_BOOL, U_ARRAY, U_MATRIX };
static const struct { const char *name, *gl; int n, kind; } USET[] = {
    {"setBoolean", "uniform1i", 1, U_BOOL},
    {"set1f", "uniform1f", 1, U_PLAIN}, {"set2f", "uniform2f", 2, U_PLAIN},
    {"set3f", "uniform3f", 3, U_PLAIN}, {"set4f", "uniform4f", 4, U_PLAIN},
    {"set1fv", "uniform1fv", 1, U_ARRAY}, {"set2fv", "uniform2fv", 1, U_ARRAY},
    {"set3fv", "uniform3fv", 1, U_ARRAY}, {"set4fv", "uniform4fv", 1, U_ARRAY},
    {"set1iv", "uniform1iv", 1, U_ARRAY}, {"set2iv", "uniform2iv", 1, U_ARRAY},
    {"set3iv", "uniform3iv", 1, U_ARRAY}, {"set4iv", "uniform4iv", 1, U_ARRAY},
    {"set1i", "uniform1i", 1, U_PLAIN}, {"set2i", "uniform2i", 2, U_PLAIN},
    {"set3i", "uniform3i", 3, U_PLAIN}, {"set4i", "uniform4i", 4, U_PLAIN},
    {"setMatrix2fv", "uniformMatrix2fv", 2, U_MATRIX}, {"setMatrix3fv", "uniformMatrix3fv", 2, U_MATRIX},
    {"setMatrix4fv", "uniformMatrix4fv", 2, U_MATRIX},
};
#define N_USET (int)(sizeof USET / sizeof USET[0])
// arguments of the shader method after the uniform name
static inline int uset_args(int k) { return USET[k].kind == U_PLAIN ? USET[k].n : USET[k].kind == U_MATRIX ? 2 : 1; }

static struct {
    bool ready;
    JSAtom name[N_USET], gl[N_USET], set_uniform[5], ev_bind;
    // the native functions (to recognise them on objects)
    JSValue shader_set[N_USET], pipeline_set[N_USET], set_uniform_fn[5], set_program, set_blend_mode, pm_set,
        pm_is_current, wp_bind, wp_update_projection, ws_bind, ws_set_attrib_pointers;
    // Phaser's own versions of what stays in JS, to recognise them: WebGLRenderer.flush,
    // PipelineManager.flush, WebGLPipeline.unbind / onActive, WebGLShader.setAttribPointers
    JSValue renderer_flush, pm_flush, wp_unbind, wp_on_active, ws_orig_set_attrib_pointers;
} PM;

static JSAtom pa(JSContext *ctx, const char *s) { return JS_NewAtom(ctx, s); }
static JSAtom PA_isPostFX, PA_glReset, PA_rebind, PA_getParameter, PA_unbind, PA_onActive, PA_isCurrent,
    PA_updateProjectionMatrix, PA_bind, PA_currentRenderTarget, PA_blendModes, PA_equation, PA_func, PA_enable,
    PA_blendEquation, PA_blendFunc, PA_blendFuncSeparate, PA_useProgram, PA_webGLProgram, PA_pipelines,
    PA_attributes, PA_size, PA_offset, PA_enabled, PA_normalized, PA_webGLAttribLocation, PA_vertexAttribPointer,
    PA_disableVertexAttribArray, PA_setAttribPointers;
static void pm_atoms(JSContext *ctx) {
#define X(n) PA_##n = pa(ctx, #n);
    X(isPostFX) X(glReset) X(rebind) X(getParameter) X(unbind) X(onActive) X(isCurrent) X(updateProjectionMatrix)
    X(bind) X(currentRenderTarget) X(blendModes) X(equation) X(func) X(enable) X(blendEquation) X(blendFunc)
    X(blendFuncSeparate) X(useProgram) X(webGLProgram) X(pipelines) X(attributes) X(size) X(offset) X(enabled)
    X(normalized) X(webGLAttribLocation) X(vertexAttribPointer) X(disableVertexAttribArray) X(setAttribPointers)
#undef X
    for (int k = 0; k < N_USET; k++) PM.name[k] = pa(ctx, USET[k].name), PM.gl[k] = pa(ctx, USET[k].gl);
    for (int n = 1; n <= 4; n++) {
        char s[16];
        snprintf(s, sizeof s, "setUniform%d", n);
        PM.set_uniform[n] = pa(ctx, s);
    }
}
static bool psame(JSContext *ctx, JSValueConst o, JSAtom a, JSValueConst ref) {
    JSValue v = JS_GetProperty(ctx, o, a);
    bool r = same(v, ref);
    JS_FreeValue(ctx, v);
    return r;
}
static int pcall0(JSContext *ctx, JSValueConst o, JSAtom a, int argc, JSValueConst *argv) {
    JSValue f = JS_GetProperty(ctx, o, a);
    JSValue r = JS_Call(ctx, f, o, argc, argv);
    JS_FreeValue(ctx, f);
    if (JS_IsException(r)) return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

// WebGLRenderer.flush(): this.pipelines.flush() -> if (current) current.flush()
static int renderer_flush(JSContext *ctx, JSValueConst renderer) {
    if (!psame(ctx, renderer, AT[A_flush], PM.renderer_flush)) return call0(ctx, renderer, A_flush, 0, NULL);
    JSValue mgr = JS_GetProperty(ctx, renderer, PA_pipelines);
    int e;
    if (!JS_IsObject(mgr) || !psame(ctx, mgr, AT[A_flush], PM.pm_flush)) {
        e = call0(ctx, mgr, A_flush, 0, NULL);
    } else {
        JSValue cur = G(mgr, A_current);
        e = JS_ToBool(ctx, cur) > 0 ? call0(ctx, cur, A_flush, 0, NULL) : 0;
        JS_FreeValue(ctx, cur);
    }
    JS_FreeValue(ctx, mgr);
    return e;
}

// WebGLRenderer.setProgram(program) -> whether it changed
static int set_program(JSContext *ctx, JSValueConst renderer, JSValueConst program) {
    JSValue cp = G(renderer, A_currentProgram);
    bool eq = JS_IsStrictEqual(ctx, program, cp);
    JS_FreeValue(ctx, cp);
    if (eq) return 0;
    if (renderer_flush(ctx, renderer)) return -1;
    JSValue gl = G(renderer, A_gl), wp = JS_GetProperty(ctx, program, PA_webGLProgram);
    int e = JS_IsException(wp) ? -1 : pcall0(ctx, gl, PA_useProgram, 1, (JSValueConst *)&wp);
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, wp);
    if (e) return -1;
    sset(ctx, renderer, A_currentProgram, JS_DupValue(ctx, program));
    return 1;
}
static int renderer_set_program(JSContext *ctx, JSValueConst renderer, JSValueConst program) {
    if (psame(ctx, renderer, AT[A_setProgram], PM.set_program)) return set_program(ctx, renderer, program) < 0 ? -1 : 0;
    return call0(ctx, renderer, A_setProgram, 1, &program);
}
static JSValue js_set_program(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int r = set_program(ctx, this_val, argv[0]);
    return r < 0 ? JS_EXCEPTION : JS_NewBool(ctx, r);
}

// WebGLRenderer.setBlendMode(blendModeId, force) -> whether it changed
static JSValue js_set_blend_mode(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst id = argv[0];
    bool force = JS_ToBool(ctx, argv[1]) > 0; // undefined -> false
    JSValue modes = JS_GetProperty(ctx, this_val, PA_blendModes);
    JSValue bm = JS_IsException(modes) ? JS_EXCEPTION : JS_GetPropertyValue(ctx, modes, JS_DupValue(ctx, id));
    JS_FreeValue(ctx, modes);
    if (JS_IsException(bm)) return JS_EXCEPTION;
    if (!force) {
        bool skip = JS_IsStrictEqual(ctx, id, JS_NewInt32(ctx, -1)); // BlendModes.SKIP_CHECK
        JSValue cur = G(this_val, A_currentBlendMode);
        bool eq = JS_IsStrictEqual(ctx, cur, id);
        JS_FreeValue(ctx, cur);
        if (skip || eq) {
            JS_FreeValue(ctx, bm);
            return JS_FALSE;
        }
    }
    JSValue gl = JS_UNDEFINED, eqn = JS_UNDEFINED, func = JS_UNDEFINED, f[4] = {JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
    JSValue ret = JS_EXCEPTION;
    if (renderer_flush(ctx, this_val)) goto out;
    gl = G(this_val, A_gl);
    JSValueConst blend = JS_NewInt32(ctx, 0x0BE2); // gl.BLEND
    if (pcall0(ctx, gl, PA_enable, 1, &blend)) goto out;
    eqn = JS_GetProperty(ctx, bm, PA_equation);
    if (JS_IsException(eqn) || pcall0(ctx, gl, PA_blendEquation, 1, (JSValueConst *)&eqn)) goto out;
    func = JS_GetProperty(ctx, bm, PA_func);
    int64_t n = 0;
    if (JS_IsException(func) || JS_GetLength(ctx, func, &n) < 0) goto out;
    for (int i = 0; i < 4; i++) f[i] = JS_GetPropertyInt64(ctx, func, i); // beyond the length: undefined
    if (n > 2 ? pcall0(ctx, gl, PA_blendFuncSeparate, 4, (JSValueConst *)f) : pcall0(ctx, gl, PA_blendFunc, 2, (JSValueConst *)f))
        goto out;
    sset(ctx, this_val, A_currentBlendMode, JS_DupValue(ctx, id));
    ret = JS_TRUE;
out:
    for (int i = 0; i < 4; i++) JS_FreeValue(ctx, f[i]);
    JS_FreeValue(ctx, bm), JS_FreeValue(ctx, gl), JS_FreeValue(ctx, eqn), JS_FreeValue(ctx, func);
    return ret;
}

// WebGLShader.setAttribPointers(reset): the reset case (context restore, pipeline rebind) stays Phaser's.
static int set_attrib_pointers(JSContext *ctx, JSValueConst sh) {
    JSValue gl = G(sh, A_gl), vsize = G(sh, A_vertexSize), attrs = JS_GetProperty(ctx, sh, PA_attributes);
    int64_t n = 0;
    int e = JS_IsException(attrs) || JS_GetLength(ctx, attrs, &n) < 0 ? -1 : 0;
    for (int64_t i = 0; i < n && !e; i++) {
        JSValue el = JS_GetPropertyInt64(ctx, attrs, i);
        JSValue size = JS_GetProperty(ctx, el, PA_size), type = G(el, A_type), off = JS_GetProperty(ctx, el, PA_offset);
        JSValue loc = G(el, A_location);
        JSValue ev = JS_GetProperty(ctx, el, PA_enabled), nv = JS_GetProperty(ctx, el, PA_normalized);
        bool enabled = JS_ToBool(ctx, ev) > 0, norm = JS_ToBool(ctx, nv) > 0;
        JS_FreeValue(ctx, ev), JS_FreeValue(ctx, nv);
        if (enabled) {
            JSValue wl = JS_GetProperty(ctx, loc, PA_webGLAttribLocation);
            JSValueConst args[6] = {wl, size, type, JS_NewBool(ctx, norm), vsize, off};
            e = JS_IsException(wl) ? -1 : pcall0(ctx, gl, PA_vertexAttribPointer, 6, args);
            JS_FreeValue(ctx, wl);
        } else if (!JS_IsStrictEqual(ctx, loc, JS_NewInt32(ctx, -1))) {
            JSValue wl = JS_GetProperty(ctx, loc, PA_webGLAttribLocation);
            if (JS_IsException(wl)) {
                e = -1;
            } else {
                double d = 0;
                bool gt = JS_IsNumber(wl) && !JS_ToFloat64(ctx, &d, wl) && d > -1;
                if (gt && !(e = pcall0(ctx, gl, PA_disableVertexAttribArray, 1, (JSValueConst *)&wl)))
                    sset(ctx, el, A_location, JS_NewInt32(ctx, -1));
            }
            JS_FreeValue(ctx, wl);
        }
        JS_FreeValue(ctx, el), JS_FreeValue(ctx, size), JS_FreeValue(ctx, type), JS_FreeValue(ctx, off), JS_FreeValue(ctx, loc);
    }
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, vsize), JS_FreeValue(ctx, attrs);
    return e;
}
static JSValue js_set_attrib_pointers(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (JS_ToBool(ctx, argv[0]) > 0) return JS_Call(ctx, PM.ws_orig_set_attrib_pointers, this_val, argc, argv);
    return set_attrib_pointers(ctx, this_val) ? JS_EXCEPTION : JS_DupValue(ctx, this_val);
}

// WebGLShader.bind(setAttributes, flush)
static int shader_bind(JSContext *ctx, JSValueConst sh, JSValueConst set_attributes, JSValueConst flush) {
    if (JS_ToBool(ctx, flush) > 0) {
        JSValue p = G(sh, A_pipeline);
        int e = call0(ctx, p, A_flush, 0, NULL);
        JS_FreeValue(ctx, p);
        if (e) return -1;
    }
    JSValue renderer = G(sh, A_renderer), program = G(sh, A_program);
    int e = renderer_set_program(ctx, renderer, program);
    JS_FreeValue(ctx, renderer), JS_FreeValue(ctx, program);
    if (e) return -1;
    if (JS_ToBool(ctx, set_attributes) > 0) {
        if (psame(ctx, sh, PA_setAttribPointers, PM.ws_set_attrib_pointers)) return set_attrib_pointers(ctx, sh);
        return pcall0(ctx, sh, PA_setAttribPointers, 0, NULL);
    }
    return 0;
}
static JSValue js_shader_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return shader_bind(ctx, this_val, argv[0], argv[1]) ? JS_EXCEPTION : JS_DupValue(ctx, this_val);
}

// WebGLPipeline.bind(currentShader)
static int pipeline_bind(JSContext *ctx, JSValueConst p, JSValueConst shader_arg) {
    JSValue sh = JS_IsUndefined(shader_arg) ? G(p, A_currentShader) : JS_DupValue(ctx, shader_arg);
    JSValue gl = JS_UNDEFINED, vb = JS_UNDEFINED, act = JS_UNDEFINED;
    int e = -1;
    JSValue reset = JS_GetProperty(ctx, p, PA_glReset);
    bool r = JS_ToBool(ctx, reset) > 0;
    JS_FreeValue(ctx, reset);
    if (r) {
        e = pcall0(ctx, p, PA_rebind, 1, (JSValueConst *)&sh);
        goto out;
    }
    gl = G(p, A_gl);
    vb = G(p, A_vertexBuffer);
    JSValueConst q = JS_NewInt32(ctx, 0x8894); // gl.ARRAY_BUFFER_BINDING
    JSValue f = JS_GetProperty(ctx, gl, PA_getParameter);
    JSValue bound = JS_Call(ctx, f, gl, 1, &q);
    JS_FreeValue(ctx, f);
    if (JS_IsException(bound)) goto out;
    bool was_bound = !JS_IsStrictEqual(ctx, bound, vb);
    JS_FreeValue(ctx, bound);
    if (was_bound) {
        JSValue wb = G(vb, A_webGLBuffer);
        JSValueConst args[2] = {JS_NewInt32(ctx, 0x8892), wb};
        int be = JS_IsException(wb) ? -1 : call0(ctx, gl, A_bindBuffer, 2, args);
        JS_FreeValue(ctx, wb);
        if (be) goto out;
        sset(ctx, p, A_activeBuffer, JS_DupValue(ctx, vb));
    }
    if (psame(ctx, sh, PA_bind, PM.ws_bind)) {
        if (shader_bind(ctx, sh, JS_NewBool(ctx, was_bound), JS_UNDEFINED)) goto out;
    } else {
        JSValueConst a = JS_NewBool(ctx, was_bound);
        if (pcall0(ctx, sh, PA_bind, 1, &a)) goto out;
    }
    sset(ctx, p, A_currentShader, JS_DupValue(ctx, sh));
    act = G(p, A_activeTextures);
    if (JS_SetProperty(ctx, act, AT[A_length], JS_NewInt32(ctx, 0)) < 0) goto out;
    if (emit_flush_event(ctx, p, PM.ev_bind, sh)) goto out; // emit(Events.BIND, this, currentShader)
    if (!psame(ctx, p, PA_onActive, PM.wp_on_active) && pcall0(ctx, p, PA_onActive, 1, (JSValueConst *)&sh)) goto out;
    e = 0;
out:
    JS_FreeValue(ctx, sh), JS_FreeValue(ctx, gl), JS_FreeValue(ctx, vb), JS_FreeValue(ctx, act);
    return e;
}
static JSValue js_pipeline_bind(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return pipeline_bind(ctx, this_val, argv[0]) ? JS_EXCEPTION : JS_DupValue(ctx, this_val);
}

// WebGLPipeline.updateProjectionMatrix()
static int update_projection(JSContext *ctx, JSValueConst p) {
    if (!gbool(ctx, p, A_projectionMatrix)) return 0;
    JSValue renderer = G(p, A_renderer);
    JSValue gw = G(renderer, A_projectionWidth), gh = G(renderer, A_projectionHeight);
    JSValue pw = G(p, A_projectionWidth), ph = G(p, A_projectionHeight);
    int e = 0;
    if (!JS_IsStrictEqual(ctx, pw, gw) || !JS_IsStrictEqual(ctx, ph, gh)) {
        JSValueConst args[2] = {gw, gh};
        e = call0(ctx, p, A_setProjectionMatrix, 2, args);
    }
    JS_FreeValue(ctx, renderer), JS_FreeValue(ctx, gw), JS_FreeValue(ctx, gh), JS_FreeValue(ctx, pw), JS_FreeValue(ctx, ph);
    return e;
}
static JSValue js_update_projection(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return update_projection(ctx, this_val) ? JS_EXCEPTION : JS_UNDEFINED;
}

// PipelineManager.isCurrent(pipeline, currentShader)
static int pm_is_current(JSContext *ctx, JSValueConst mgr, JSValueConst pipeline, JSValueConst shader_arg) {
    JSValue cur = G(mgr, A_current);
    int r = 0;
    if (JS_IsStrictEqual(ctx, cur, pipeline)) { // else !(true || ...): false without touching the shader
        JSValue sh = JS_ToBool(ctx, cur) > 0 && JS_ToBool(ctx, shader_arg) <= 0 ? G(cur, A_currentShader) : JS_DupValue(ctx, shader_arg);
        JSValue prog = G(sh, A_program), renderer = G(mgr, A_renderer);
        JSValue cp = JS_IsException(prog) ? JS_UNDEFINED : G(renderer, A_currentProgram);
        r = JS_IsException(prog) || JS_IsException(cp) ? -1 : JS_IsStrictEqual(ctx, prog, cp);
        JS_FreeValue(ctx, sh), JS_FreeValue(ctx, prog), JS_FreeValue(ctx, renderer), JS_FreeValue(ctx, cp);
    }
    JS_FreeValue(ctx, cur);
    return r;
}
static JSValue js_pm_is_current(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int r = pm_is_current(ctx, this_val, argv[0], argv[1]);
    return r < 0 ? JS_EXCEPTION : JS_NewBool(ctx, r);
}

// PipelineManager.set(pipeline, gameObject, currentShader)
static JSValue js_pm_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst mgr = this_val, p = argv[0], go = argv[1], shader = argv[2];
    JSValue v = JS_GetProperty(ctx, p, PA_isPostFX);
    if (JS_IsException(v)) return JS_EXCEPTION;
    bool post = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    if (post) return JS_UNDEFINED;
    int cur;
    if (psame(ctx, mgr, PA_isCurrent, PM.pm_is_current)) {
        cur = pm_is_current(ctx, mgr, p, shader);
    } else {
        JSValueConst args[2] = {p, shader};
        JSValue f = JS_GetProperty(ctx, mgr, PA_isCurrent);
        JSValue r = JS_Call(ctx, f, mgr, 2, args);
        JS_FreeValue(ctx, f);
        cur = JS_IsException(r) ? -1 : JS_ToBool(ctx, r) > 0;
        JS_FreeValue(ctx, r);
    }
    if (cur < 0) return JS_EXCEPTION;
    if (!cur) {
        // this.flush(); if (this.current) this.current.unbind(); this.current = pipeline; pipeline.bind(currentShader)
        if (psame(ctx, mgr, AT[A_flush], PM.pm_flush)) {
            JSValue c = G(mgr, A_current);
            int e = JS_ToBool(ctx, c) > 0 ? call0(ctx, c, A_flush, 0, NULL) : 0;
            JS_FreeValue(ctx, c);
            if (e) return JS_EXCEPTION;
        } else if (call0(ctx, mgr, A_flush, 0, NULL)) {
            return JS_EXCEPTION;
        }
        JSValue c = G(mgr, A_current);
        int e = 0;
        if (JS_ToBool(ctx, c) > 0) {
            if (psame(ctx, c, PA_unbind, PM.wp_unbind)) { // if (this.currentRenderTarget) this.currentRenderTarget.unbind()
                JSValue rt = JS_GetProperty(ctx, c, PA_currentRenderTarget);
                e = JS_IsException(rt) ? -1 : JS_ToBool(ctx, rt) > 0 ? pcall0(ctx, rt, PA_unbind, 0, NULL) : 0;
                JS_FreeValue(ctx, rt);
            } else {
                e = pcall0(ctx, c, PA_unbind, 0, NULL);
            }
        }
        JS_FreeValue(ctx, c);
        if (e) return JS_EXCEPTION;
        sset(ctx, mgr, A_current, JS_DupValue(ctx, p));
        if (psame(ctx, p, PA_bind, PM.wp_bind) ? pipeline_bind(ctx, p, shader) : pcall0(ctx, p, PA_bind, 1, &shader))
            return JS_EXCEPTION;
    }
    if (psame(ctx, p, PA_updateProjectionMatrix, PM.wp_update_projection) ? update_projection(ctx, p)
                                                                           : pcall0(ctx, p, PA_updateProjectionMatrix, 0, NULL))
        return JS_EXCEPTION;
    if (!gsame(ctx, p, A_onBind, H.wp_on_bind) && call0(ctx, p, A_onBind, 1, &go)) return JS_EXCEPTION;
    return JS_DupValue(ctx, p);
}

// WebGLShader.<USET[k].name>(name, ...)
static JSValue js_set_uniform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n);
static JSValue shader_set(JSContext *ctx, JSValueConst sh, int k, JSValueConst *a) {
    int kind = USET[k].kind, un = kind == U_PLAIN ? USET[k].n : kind == U_MATRIX ? 2 : 1;
    JSValue gl = G(sh, A_gl);
    JSValue setter = JS_IsException(gl) ? JS_EXCEPTION : JS_GetProperty(ctx, gl, PM.gl[k]);
    JS_FreeValue(ctx, gl);
    if (JS_IsException(setter)) return JS_EXCEPTION;
    JSValueConst args[7];
    int argc = 0;
    args[argc++] = setter;
    args[argc++] = a[0];
    if (kind == U_BOOL) { // Number(value)
        double d;
        if (JS_ToFloat64(ctx, &d, a[1])) {
            JS_FreeValue(ctx, setter);
            return JS_EXCEPTION;
        }
        args[argc++] = JS_NewFloat64(ctx, d);
    } else if (kind == U_PLAIN) {
        for (int i = 0; i < un; i++) args[argc++] = a[1 + i];
    } else { // arrays and matrices: skipCheck
        for (int i = 0; i < un; i++) args[argc++] = a[1 + i];
        args[argc++] = JS_TRUE;
    }
    JSValue r;
    if (psame(ctx, sh, PM.set_uniform[un], PM.set_uniform_fn[un])) {
        r = js_set_uniform(ctx, sh, argc, (JSValueConst *)args, un);
    } else {
        JSValue f = JS_GetProperty(ctx, sh, PM.set_uniform[un]);
        r = JS_Call(ctx, f, sh, argc, args);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, setter);
    return r;
}
static JSValue js_shader_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int k) {
    return shader_set(ctx, this_val, k, argv);
}
// WebGLPipeline.<USET[k].name>(name, ..., shader = this.currentShader): shader.<name>(name, ...); return this
static JSValue js_pipeline_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int k) {
    int na = 1 + uset_args(k);
    JSValue sh = JS_IsUndefined(argv[na]) ? G(this_val, A_currentShader) : JS_DupValue(ctx, argv[na]);
    JSValue r;
    if (psame(ctx, sh, PM.name[k], PM.shader_set[k])) {
        r = shader_set(ctx, sh, k, argv);
    } else {
        JSValue f = JS_GetProperty(ctx, sh, PM.name[k]);
        r = JS_Call(ctx, f, sh, na, argv);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, sh);
    if (JS_IsException(r)) return JS_EXCEPTION;
    JS_FreeValue(ctx, r);
    return JS_DupValue(ctx, this_val);
}

static JSValue keep(JSContext *ctx, JSValue *slot, JSValue f) {
    JS_FreeValue(ctx, *slot);
    *slot = JS_DupValue(ctx, f);
    return f;
}
// pipelineSetup({rendererFlush, pmFlush, wpUnbind, wpOnActive, wsSetAttribPointers, evBind}) -> the
// native functions: {setProgram, setBlendMode, pmSet, pmIsCurrent, wpBind, wpUpdateProjectionMatrix,
// wsBind, wsSetAttribPointers, shader: {set1f...}, pipeline: {set1f...}}, to install in js/fastpath.js
static JSValue js_pipeline_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    if (!PM.ready) pm_atoms(ctx);
    JSValueConst o = argv[0];
#define HOOK(field, name) JS_FreeValue(ctx, PM.field), PM.field = JS_GetPropertyStr(ctx, o, name);
    HOOK(renderer_flush, "rendererFlush") HOOK(pm_flush, "pmFlush") HOOK(wp_unbind, "wpUnbind")
    HOOK(wp_on_active, "wpOnActive") HOOK(ws_orig_set_attrib_pointers, "wsSetAttribPointers")
#undef HOOK
    JSValue ev = JS_GetPropertyStr(ctx, o, "evBind");
    PM.ev_bind = JS_ValueToAtom(ctx, ev);
    JS_FreeValue(ctx, ev);
    JSValue out = JS_NewObject(ctx), sh = JS_NewObject(ctx), pl = JS_NewObject(ctx);
#define FN(field, name, fn, len) JS_SetPropertyStr(ctx, out, name, keep(ctx, &PM.field, JS_NewCFunction(ctx, fn, name, len)));
    FN(set_program, "setProgram", js_set_program, 1) FN(set_blend_mode, "setBlendMode", js_set_blend_mode, 2)
    FN(pm_set, "pmSet", js_pm_set, 3) FN(pm_is_current, "pmIsCurrent", js_pm_is_current, 2)
    FN(wp_bind, "wpBind", js_pipeline_bind, 1) FN(wp_update_projection, "wpUpdateProjectionMatrix", js_update_projection, 0)
    FN(ws_bind, "wsBind", js_shader_bind, 2) FN(ws_set_attrib_pointers, "wsSetAttribPointers", js_set_attrib_pointers, 1)
#undef FN
    for (int k = 0; k < N_USET; k++) {
        int na = 1 + uset_args(k);
        JS_SetPropertyStr(ctx, sh, USET[k].name, keep(ctx, &PM.shader_set[k],
            JS_NewCFunctionMagic(ctx, js_shader_set, USET[k].name, na, JS_CFUNC_generic_magic, k)));
        JS_SetPropertyStr(ctx, pl, USET[k].name, keep(ctx, &PM.pipeline_set[k],
            JS_NewCFunctionMagic(ctx, js_pipeline_set, USET[k].name, na + 1, JS_CFUNC_generic_magic, k)));
    }
    JS_SetPropertyStr(ctx, out, "shader", sh);
    JS_SetPropertyStr(ctx, out, "pipeline", pl);
    PM.ready = true;
    return out;
}

// WebGLShader.setUniform1..4(setter, name, v1[, v2, v3, v4], skipCheck): Phaser's code, run for
// every uniform PokeRogue's sprite pipelines set on every sprite (thousands of calls per frame).
//   var u = this.uniforms[name];
//   if (u && (skipCheck || u.value1 !== v1 || ...)) { u.setter ||= setter; u.value1 = v1 ...;
//     this.renderer.setProgram(this.program); setter.call(this.gl, u.location.webGLUniformLocation, v1 ...);
//     this.pipeline.currentShader = this; }
//   return this;
static JSValue js_set_uniform(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int n) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    JSValueConst setter = argv[0];
    JSValue uniforms = G(this_val, A_uniforms);
    JSAtom name = JS_ValueToAtom(ctx, argv[1]);
    JSValue u = JS_EXCEPTION;
    if (name != JS_ATOM_NULL) {
        u = JS_IsObject(uniforms) ? JS_GetProperty(ctx, uniforms, name) : JS_ThrowTypeError(ctx, "setUniform: no uniforms");
        JS_FreeAtom(ctx, name);
    }
    JS_FreeValue(ctx, uniforms);
    JSValue ret = JS_DupValue(ctx, this_val);
    if (JS_IsException(u)) {
        JS_FreeValue(ctx, ret);
        return JS_EXCEPTION;
    }
    if (JS_ToBool(ctx, u) <= 0) goto done;
    static const int vat[4] = {A_value1, A_value2, A_value3, A_value4};
    bool change = argc > 2 + n && JS_ToBool(ctx, argv[2 + n]) > 0;
    for (int i = 0; i < n && !change; i++) {
        JSValue cur = G(u, vat[i]);
        change = !JS_IsStrictEqual(ctx, cur, argc > 2 + i ? argv[2 + i] : JS_UNDEFINED);
        JS_FreeValue(ctx, cur);
    }
    if (!change) goto done;
    JSValue cs = G(u, A_setter);
    bool has = JS_ToBool(ctx, cs) > 0;
    JS_FreeValue(ctx, cs);
    if (!has) sset(ctx, u, A_setter, JS_DupValue(ctx, setter));
    for (int i = 0; i < n; i++) sset(ctx, u, vat[i], JS_DupValue(ctx, argc > 2 + i ? argv[2 + i] : JS_UNDEFINED));
    JSValue renderer = G(this_val, A_renderer), program = G(this_val, A_program);
    int pe = renderer_set_program(ctx, renderer, program);
    JS_FreeValue(ctx, renderer), JS_FreeValue(ctx, program);
    if (pe) goto fail;
    JSValue r, gl = G(this_val, A_gl), loc = G(u, A_location), wl = G(loc, A_webGLUniformLocation);
    JSValueConst args[5] = {wl};
    for (int i = 0; i < n; i++) args[1 + i] = argc > 2 + i ? argv[2 + i] : JS_UNDEFINED;
    r = JS_Call(ctx, setter, gl, n + 1, args);
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, loc), JS_FreeValue(ctx, wl);
    if (JS_IsException(r)) goto fail;
    JS_FreeValue(ctx, r);
    JSValue pipeline = G(this_val, A_pipeline);
    if (JS_IsObject(pipeline)) sset(ctx, pipeline, A_currentShader, JS_DupValue(ctx, this_val));
    else JS_ThrowTypeError(ctx, "setUniform: shader has no pipeline");
    bool bad = !JS_IsObject(pipeline);
    JS_FreeValue(ctx, pipeline);
    if (bad) goto fail;
done:
    JS_FreeValue(ctx, u);
    return ret;
fail:
    JS_FreeValue(ctx, u);
    JS_FreeValue(ctx, ret);
    return JS_EXCEPTION;
}


// ---------------------------------------------------------------- Graphics
// Phaser 3.90's GraphicsWebGLRenderer for command buffers made only of fill/line styles and
// fillRect (begin/closePath allowed): what PokeRogue's masks and bars use. Anything else (paths,
// arcs, transforms) runs Phaser's renderer. The commands' values are Phaser 3.90's Commands,
// checked by js/fastpath.js before this is installed.
enum { GC_BEGIN_PATH = 1, GC_CLOSE_PATH = 2, GC_FILL_RECT = 3, GC_LINE_STYLE = 6, GC_FILL_STYLE = 7,
       GC_GRADIENT_FILL_STYLE = 21, GC_GRADIENT_LINE_STYLE = 22 };
static struct {
    bool ready;
    JSValue orig_render, mp_batch_fill_rect;
    JSAtom commandBuffer, rotation, scaleX, scaleY, fillTint, strokeTint, TL, TR, BL, BR, calcMatrix, quad,
        batchFillRect, vertices, tint, applyStencil, geometryMask, colorMask, stencilFunc, stencilOp, level,
        invertAlpha, maskStack, clear, currentCameraMask, currentMask, camera, push, pop, disable;
    JSValue ns_orig_render, rect_orig_render, gm_apply_stencil;
} GR;

static void gr_init(JSContext *ctx) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    if (!PM.ready) pm_atoms(ctx), PM.ready = true;
    if (GR.ready) return;
#define X(n) GR.n = JS_NewAtom(ctx, #n);
    X(commandBuffer) X(rotation) X(scaleX) X(scaleY) X(fillTint) X(strokeTint) X(TL) X(TR) X(BL) X(BR)
    X(calcMatrix) X(quad) X(batchFillRect) X(vertices) X(tint) X(applyStencil) X(geometryMask) X(colorMask)
    X(stencilFunc) X(stencilOp) X(level) X(invertAlpha) X(maskStack) X(clear) X(currentCameraMask) X(currentMask)
    X(camera) X(push) X(pop) X(disable)
#undef X
    GR.ready = true;
}

// GetCalcMatrix(src, camera, parentMatrix).calc
static int calc_matrix(JSContext *ctx, JSValueConst src, JSValueConst camera, JSValueConst parent, float *out) {
    JSValue cm = G(camera, A_matrix), cmm = G(cm, A_matrix);
    float *cam = f32(ctx, cmm);
    JS_FreeValue(ctx, cm), JS_FreeValue(ctx, cmm);
    float *par = NULL;
    JSValue pm = JS_UNDEFINED;
    if (JS_ToBool(ctx, parent) > 0) {
        pm = G(parent, A_matrix);
        par = f32(ctx, pm);
    }
    JS_FreeValue(ctx, pm); // the TransformMatrix keeps its array alive
    if (!cam || (JS_ToBool(ctx, parent) > 0 && !par)) return -1;
    double sx = gnum(ctx, camera, A_scrollX) * gnum(ctx, src, A_scrollFactorX);
    double sy = gnum(ctx, camera, A_scrollY) * gnum(ctx, src, A_scrollFactorY);
    double v;
    JSValue t;
#define NUMA(a) (t = JS_GetProperty(ctx, src, a), v = num(ctx, t), JS_FreeValue(ctx, t), v)
    sprite_quad q = {.offX = -sx, .offY = -sy, .gx = gnum(ctx, src, A_x), .gy = gnum(ctx, src, A_y),
                     .rot = NUMA(GR.rotation), .sx = NUMA(GR.scaleX), .sy = NUMA(GR.scaleY)};
#undef NUMA
    calc_from(out, cam, par, &q);
    return 0;
}

static int set_tint4(JSContext *ctx, JSValueConst o, const uint32_t *t) {
    JSAtom k[4] = {GR.TL, GR.TR, GR.BL, GR.BR};
    for (int i = 0; i < 4; i++)
        if (JS_SetProperty(ctx, o, k[i], JS_NewUint32(ctx, t[i])) < 0) return -1;
    return 0;
}

// MultiPipeline.batchFillRect(x, y, w, h, currentMatrix, parentMatrix) on pipeline p (Phaser's, checked by
// the caller): pipelines.set(this); with `calc`, calcMatrix = calc * identity (Graphics), else the
// calcMatrix as it is (Rectangle); quad = calcMatrix.setQuad(...); batchQuad(null, quad, 0, 0, 1, 1,
// fillTint.TL/TR/BL/BR, 2)
static int fill_rect(JSContext *ctx, JSValueConst mgr, JSValueConst p, JSValueConst fill, double x, double y,
                     double w, double h, const float *calc) {
    if (call0(ctx, mgr, A_set, 1, &p)) return -1;
    JSValue cmo = JS_GetProperty(ctx, p, GR.calcMatrix), cmm = G(cmo, A_matrix), quad = JS_GetProperty(ctx, cmo, GR.quad);
    float *m = f32(ctx, cmm), *qd = f32(ctx, quad);
    JS_FreeValue(ctx, cmo), JS_FreeValue(ctx, cmm), JS_FreeValue(ctx, quad); // the pipeline keeps them
    if (!m || !qd) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "batchFillRect: bad calcMatrix");
        return -1;
    }
    if (calc) {
        static const float id[6] = {1, 0, 0, 1, 0, 0};
        m[0] = (double)id[0] * calc[0] + (double)id[1] * calc[2];
        m[1] = (double)id[0] * calc[1] + (double)id[1] * calc[3];
        m[2] = (double)id[2] * calc[0] + (double)id[3] * calc[2];
        m[3] = (double)id[2] * calc[1] + (double)id[3] * calc[3];
        m[4] = (double)id[4] * calc[0] + (double)id[5] * calc[2] + calc[4];
        m[5] = (double)id[4] * calc[1] + (double)id[5] * calc[3] + calc[5];
    }
    double xw = x + w, yh = y + h, a = m[0], b = m[1], cc = m[2], d = m[3], e = m[4], f = m[5];
    qd[0] = x * a + y * cc + e, qd[1] = x * b + y * d + f;
    qd[2] = x * a + yh * cc + e, qd[3] = x * b + yh * d + f;
    qd[4] = xw * a + yh * cc + e, qd[5] = xw * b + yh * d + f;
    qd[6] = xw * a + y * cc + e, qd[7] = xw * b + y * d + f;
    JSValueConst args[18];
    args[0] = JS_NULL;
    for (int k = 0; k < 8; k++) args[1 + k] = JS_NewFloat64(ctx, qd[k]);
    args[9] = JS_NewInt32(ctx, 0), args[10] = JS_NewInt32(ctx, 0), args[11] = JS_NewInt32(ctx, 1), args[12] = JS_NewInt32(ctx, 1);
    JSAtom tk[4] = {GR.TL, GR.TR, GR.BL, GR.BR};
    for (int k = 0; k < 4; k++) args[13 + k] = JS_GetProperty(ctx, fill, tk[k]);
    args[17] = JS_NewInt32(ctx, 2);
    int e2 = call0(ctx, p, A_batchQuad, 18, args);
    for (int k = 0; k < 4; k++) JS_FreeValue(ctx, (JSValue)args[13 + k]);
    return e2;
}

// renderWebGL(renderer, src, camera, parentMatrix)
static JSValue js_graphics_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst renderer = argv[0], src = argv[1], camera = argv[2], parent = argv[3];
    JSValue cmds = JS_GetProperty(ctx, src, GR.commandBuffer);
    int64_t n = 0;
    if (JS_IsException(cmds) || JS_GetLength(ctx, cmds, &n) < 0) {
        JS_FreeValue(ctx, cmds);
        return JS_EXCEPTION;
    }
    if (n == 0) {
        JS_FreeValue(ctx, cmds);
        return JS_UNDEFINED;
    }
    // Command words and their arguments, as doubles; anything unusual goes to Phaser's renderer.
    double stack_buf[64], *c = n <= 64 ? stack_buf : malloc(n * sizeof(double));
    bool simple = c != NULL;
    for (int64_t i = 0; simple && i < n; i++) {
        JSValue e = JS_GetPropertyInt64(ctx, cmds, i);
        simple = JS_IsNumber(e);
        if (simple) c[i] = num(ctx, e);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, cmds);
    for (int64_t i = 0; simple && i < n; i++) {
        int args;
        switch ((int)c[i] == c[i] ? (int)c[i] : -1) {
        case GC_BEGIN_PATH: case GC_CLOSE_PATH: args = 0; break;
        case GC_FILL_RECT: args = 4; break;
        case GC_LINE_STYLE: args = 3; break;
        case GC_FILL_STYLE: args = 2; break;
        case GC_GRADIENT_FILL_STYLE: args = 8; break;
        case GC_GRADIENT_LINE_STYLE: args = 6; break;
        default: args = -1;
        }
        if (args < 0 || i + args >= n) simple = false;
        i += args > 0 ? args : 0;
    }
    // a pipeline with its own batchFillRect (or a post-FX one): Phaser's renderer
    if (simple) {
        JSValue sp = G(src, A_pipeline);
        simple = JS_IsObject(sp) && psame(ctx, sp, GR.batchFillRect, GR.mp_batch_fill_rect);
        JS_FreeValue(ctx, sp);
    }
    if (!simple) {
        if (c != stack_buf) free(c);
        return JS_Call(ctx, GR.orig_render, this_val, argc, argv);
    }
    JSValue mgr = JS_UNDEFINED, p = JS_UNDEFINED, fill = JS_UNDEFINED, stroke = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    if (call0(ctx, camera, A_addToRenderList, 1, &src)) goto out;
    mgr = JS_GetProperty(ctx, renderer, PA_pipelines);
    {
        JSValue sp = G(src, A_pipeline);
        JSValueConst args[2] = {sp, src};
        p = call(ctx, mgr, A_set, 2, args);
        JS_FreeValue(ctx, sp);
        if (JS_IsException(p)) goto out;
    }
    if (call0(ctx, mgr, A_preBatch, 1, &src)) goto out;
    float calc[6];
    if (calc_matrix(ctx, src, camera, parent, calc)) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Graphics: bad matrices");
        goto out;
    }
    double alpha = gnum(ctx, camera, A_alpha) * gnum(ctx, src, A_alpha);
    fill = JS_GetProperty(ctx, p, GR.fillTint);
    stroke = JS_GetProperty(ctx, p, GR.strokeTint);
    if (JS_IsException(fill) || JS_IsException(stroke)) goto out;
    for (int64_t i = 0; i < n; i++) {
        uint32_t t[4];
        switch ((int)c[i]) {
        case GC_LINE_STYLE:
            t[0] = t[1] = t[2] = t[3] = tint(c[i + 2], c[i + 3] * alpha);
            if (set_tint4(ctx, stroke, t)) goto out;
            i += 3;
            break;
        case GC_FILL_STYLE:
            t[0] = t[1] = t[2] = t[3] = tint(c[i + 1], c[i + 2] * alpha);
            if (set_tint4(ctx, fill, t)) goto out;
            i += 2;
            break;
        case GC_GRADIENT_FILL_STYLE:
            for (int k = 0; k < 4; k++) t[k] = tint(c[i + 5 + k], c[i + 1 + k] * alpha);
            if (set_tint4(ctx, fill, t)) goto out;
            i += 8;
            break;
        case GC_GRADIENT_LINE_STYLE:
            for (int k = 0; k < 4; k++) t[k] = tint(c[i + 3 + k], c[i + 2] * alpha);
            if (set_tint4(ctx, stroke, t)) goto out;
            i += 6;
            break;
        case GC_FILL_RECT: {
            double x = c[i + 1], y = c[i + 2], w = c[i + 3], h = c[i + 4];
            i += 4;
            if (fill_rect(ctx, mgr, p, fill, x, y, w, h, calc)) goto out;
            break;
        }
        default: // begin/closePath: nothing to do without path commands
            break;
        }
    }
    if (call0(ctx, mgr, A_postBatch, 1, &src)) goto out;
    ret = JS_UNDEFINED;
out:
    if (c != stack_buf) free(c);
    JS_FreeValue(ctx, mgr), JS_FreeValue(ctx, p), JS_FreeValue(ctx, fill), JS_FreeValue(ctx, stroke);
    return ret;
}

// graphicsSetup({render: Phaser's renderWebGL, batchFillRect: MultiPipeline's}) -> the native renderWebGL
static JSValue js_graphics_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    gr_init(ctx);
    JS_FreeValue(ctx, GR.orig_render), JS_FreeValue(ctx, GR.mp_batch_fill_rect);
    GR.orig_render = JS_GetPropertyStr(ctx, argv[0], "render");
    GR.mp_batch_fill_rect = JS_GetPropertyStr(ctx, argv[0], "batchFillRect");
    return JS_NewCFunction(ctx, js_graphics_render, "renderWebGL", 4);
}

// ---------------------------------------------------------------- NineSlice
// NineSlice.renderWebGL (the game's window frames, 54 vertices each): Phaser 3.90's
// NineSliceWebGLRenderer, except that a batch without room for all vertices is flushed first
// instead of mid-way (as js/fastpath.js part 5 did). Pipelines with another vertex layout or
// too small a buffer use Phaser's.
static JSValue js_nineslice_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst renderer = argv[0], src = argv[1], camera = argv[2], parent = argv[3];
    JSValue verts = JS_GetProperty(ctx, src, GR.vertices);
    int64_t n = 0;
    if (JS_IsException(verts) || JS_GetLength(ctx, verts, &n) < 0) {
        JS_FreeValue(ctx, verts);
        return JS_EXCEPTION;
    }
    if (n == 0) {
        JS_FreeValue(ctx, verts);
        return JS_UNDEFINED;
    }
    JSValue sp = G(src, A_pipeline), mgr = JS_UNDEFINED, p = JS_UNDEFINED, ret = JS_EXCEPTION;
    bool fast = false;
    if (JS_IsObject(sp)) {
        JSValue sh = G(sp, A_currentShader);
        fast = JS_IsObject(sh) && gnum(ctx, sh, A_vertexComponentCount) == 7 && gnum(ctx, sp, A_vertexCapacity) >= n;
        JS_FreeValue(ctx, sh);
    }
    if (!fast) {
        JS_FreeValue(ctx, verts), JS_FreeValue(ctx, sp);
        return JS_Call(ctx, GR.ns_orig_render, this_val, argc, argv);
    }
    if (call0(ctx, camera, A_addToRenderList, 1, &src)) goto out;
    mgr = JS_GetProperty(ctx, renderer, PA_pipelines);
    {
        JSValueConst args[2] = {sp, src};
        p = call(ctx, mgr, A_set, 2, args);
        if (JS_IsException(p)) goto out;
    }
    float calc[6];
    if (calc_matrix(ctx, src, camera, parent, calc)) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "NineSlice: bad matrices");
        goto out;
    }
    if (call0(ctx, mgr, A_preBatch, 1, &src)) goto out;
    if (gnum(ctx, p, A_vertexCapacity) - gnum(ctx, p, A_vertexCount) < n && call0(ctx, p, A_flush, 0, NULL)) goto out;
    JSValue unitv;
    if (gsame(ctx, p, A_setGameObject, H.wp_set_game_object)) { // pushBatch(src.frame.source.glTexture)
        JSValue fr = G(src, A_frame), so = G(fr, A_source), tex = G(so, A_glTexture);
        unitv = JS_IsException(tex) ? JS_EXCEPTION : push_batch(ctx, p, tex);
        JS_FreeValue(ctx, fr), JS_FreeValue(ctx, so), JS_FreeValue(ctx, tex);
    } else {
        unitv = call(ctx, p, A_setGameObject, 1, &src);
    }
    if (JS_IsException(unitv)) goto out;
    double unit = num(ctx, unitv);
    JS_FreeValue(ctx, unitv);
    JSValue tv = JS_GetProperty(ctx, src, GR.tint);
    double tintv = num(ctx, tv);
    JS_FreeValue(ctx, tv);
    uint32_t t = tint(tintv, gnum(ctx, camera, A_alpha) * gnum(ctx, src, A_alpha));
    JSValue vd = G(p, A_vertexData), cam_round = G(camera, A_roundPixels), effect = G(src, A_tintFill);
    size_t size = 0;
    uint8_t *vb = JS_GetArrayBuffer(ctx, &size, vd);
    double count = gnum(ctx, p, A_vertexCount);
    bool ok = vb && (size_t)((count + n) * 7 * 4) <= size;
    if (ok)
        write_verts(ctx, (float *)vb + (int64_t)count * 7, verts, n, calc, JS_ToBool(ctx, cam_round) > 0, (float)unit,
                    (float)num(ctx, effect), t);
    JS_FreeValue(ctx, vd), JS_FreeValue(ctx, cam_round), JS_FreeValue(ctx, effect);
    if (!ok) {
        JS_ThrowRangeError(ctx, "NineSlice: vertex buffer too small");
        goto out;
    }
    count += n;
    sset(ctx, p, A_vertexCount, JS_NewInt64(ctx, (int64_t)count));
    JSValue cb = G(p, A_currentBatch);
    if (JS_IsObject(cb)) sset(ctx, cb, A_count, JS_NewInt64(ctx, (int64_t)(count - gnum(ctx, cb, A_start))));
    JS_FreeValue(ctx, cb);
    if (call0(ctx, mgr, A_postBatch, 1, &src)) goto out;
    ret = JS_UNDEFINED;
out:
    JS_FreeValue(ctx, verts), JS_FreeValue(ctx, sp), JS_FreeValue(ctx, mgr), JS_FreeValue(ctx, p);
    return ret;
}
// nineSliceSetup(Phaser's renderWebGL) -> the native renderWebGL
static JSValue js_nineslice_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    gr_init(ctx);
    JS_FreeValue(ctx, GR.ns_orig_render);
    GR.ns_orig_render = JS_DupValue(ctx, argv[0]);
    return JS_NewCFunction(ctx, js_nineslice_render, "renderWebGL", 4);
}

// ---------------------------------------------------------------- Rectangle, GeometryMask
// Rectangle.renderWebGL for a filled or empty rectangle without stroke or rounded corners (Phaser
// 3.90's RectangleWebGLRenderer), Phaser's renderer otherwise.
static JSValue js_rect_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst renderer = argv[0], src = argv[1], camera = argv[2], parent = argv[3];
    bool stroked = gbool(ctx, src, A_isStroked), filled = gbool(ctx, src, A_isFilled);
    bool rounded = filled && gbool(ctx, src, A_isRounded);
    JSValue sp = G(src, A_pipeline);
    bool fast = !stroked && !rounded && JS_IsObject(sp) && psame(ctx, sp, GR.batchFillRect, GR.mp_batch_fill_rect);
    JS_FreeValue(ctx, sp);
    if (!fast) return JS_Call(ctx, GR.rect_orig_render, this_val, argc, argv);
    JSValue mgr = JS_UNDEFINED, p = JS_UNDEFINED, fill = JS_UNDEFINED, ret = JS_EXCEPTION;
    if (call0(ctx, camera, A_addToRenderList, 1, &src)) goto out;
    mgr = JS_GetProperty(ctx, renderer, PA_pipelines);
    sp = G(src, A_pipeline);
    p = call(ctx, mgr, A_set, 1, (JSValueConst *)&sp); // renderer.pipelines.set(src.pipeline)
    JS_FreeValue(ctx, sp);
    if (JS_IsException(p)) goto out;
    float calc[6];
    if (calc_matrix(ctx, src, camera, parent, calc)) {
        if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Rectangle: bad matrices");
        goto out;
    }
    { // pipeline.calcMatrix.copyFrom(calc)
        JSValue cmo = JS_GetProperty(ctx, p, GR.calcMatrix), cmm = G(cmo, A_matrix);
        float *m = f32(ctx, cmm);
        JS_FreeValue(ctx, cmo), JS_FreeValue(ctx, cmm);
        if (!m) {
            if (!JS_HasException(ctx)) JS_ThrowTypeError(ctx, "Rectangle: bad calcMatrix");
            goto out;
        }
        memcpy(m, calc, sizeof calc);
    }
    double dx = gnum(ctx, src, A__displayOriginX), dy = gnum(ctx, src, A__displayOriginY);
    double alpha = gnum(ctx, camera, A_alpha) * gnum(ctx, src, A_alpha);
    if (call0(ctx, mgr, A_preBatch, 1, &src)) goto out;
    if (filled) {
        fill = JS_GetProperty(ctx, p, GR.fillTint);
        if (JS_IsException(fill)) goto out;
        uint32_t t[4];
        t[0] = t[1] = t[2] = t[3] = tint(gnum(ctx, src, A_fillColor), gnum(ctx, src, A_fillAlpha) * alpha);
        if (set_tint4(ctx, fill, t)) goto out;
        if (fill_rect(ctx, mgr, p, fill, -dx, -dy, gnum(ctx, src, A_width), gnum(ctx, src, A_height), NULL)) goto out;
    }
    if (call0(ctx, mgr, A_postBatch, 1, &src)) goto out;
    ret = JS_UNDEFINED;
out:
    JS_FreeValue(ctx, mgr), JS_FreeValue(ctx, p), JS_FreeValue(ctx, fill);
    return ret;
}

// GeometryMask.applyStencil(renderer, camera, inc)
static int apply_stencil(JSContext *ctx, JSValueConst mask, JSValueConst renderer, JSValueConst camera, bool inc) {
    if (!psame(ctx, mask, GR.applyStencil, GR.gm_apply_stencil)) {
        JSValueConst args[3] = {renderer, camera, JS_NewBool(ctx, inc)};
        return pcall0(ctx, mask, GR.applyStencil, 3, args);
    }
    JSValue gl = G(renderer, A_gl), gm = JS_GetProperty(ctx, mask, GR.geometryMask);
    double level = gnum(ctx, renderer, A_maskCount);
    int e = -1;
    JSValueConst f4[4] = {JS_FALSE, JS_FALSE, JS_FALSE, JS_FALSE}, t4[4] = {JS_TRUE, JS_TRUE, JS_TRUE, JS_TRUE};
    if (pcall0(ctx, gl, GR.colorMask, 4, f4)) goto out;
    {
        JSValueConst fa[3] = {JS_NewInt32(ctx, 0x0202), JS_NewFloat64(ctx, inc ? level : level + 1), JS_NewInt32(ctx, 0xff)};
        JSValueConst oa[3] = {JS_NewInt32(ctx, 0x1E00), JS_NewInt32(ctx, 0x1E00), JS_NewInt32(ctx, inc ? 0x1E02 : 0x1E03)};
        if (pcall0(ctx, gl, GR.stencilFunc, 3, fa) || pcall0(ctx, gl, GR.stencilOp, 3, oa)) goto out;
    }
    if (inc) level++;
    if (JS_SetProperty(ctx, mask, GR.level, JS_NewFloat64(ctx, level)) < 0) goto out;
    {
        JSValueConst args[3] = {renderer, gm, camera};
        if (call0(ctx, gm, A_renderWebGL, 3, args)) goto out;
    }
    if (renderer_flush(ctx, renderer)) goto out;
    if (pcall0(ctx, gl, GR.colorMask, 4, t4)) goto out;
    {
        JSValueConst oa[3] = {JS_NewInt32(ctx, 0x1E00), JS_NewInt32(ctx, 0x1E00), JS_NewInt32(ctx, 0x1E00)};
        if (pcall0(ctx, gl, GR.stencilOp, 3, oa)) goto out;
        JSValue inv = JS_GetProperty(ctx, mask, GR.invertAlpha);
        bool invert = JS_ToBool(ctx, inv) > 0;
        JS_FreeValue(ctx, inv);
        JSValueConst fa[3] = {JS_NewInt32(ctx, invert ? 0x0205 : 0x0202), JS_NewFloat64(ctx, level), JS_NewInt32(ctx, 0xff)};
        if (pcall0(ctx, gl, GR.stencilFunc, 3, fa)) goto out;
    }
    e = 0;
out:
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, gm);
    return e;
}
static JSValue js_gm_apply_stencil(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return apply_stencil(ctx, this_val, argv[0], argv[1], JS_ToBool(ctx, argv[2]) > 0) ? JS_EXCEPTION : JS_UNDEFINED;
}
static int add_count(JSContext *ctx, JSValueConst renderer, int d) {
    return JS_SetProperty(ctx, renderer, AT[A_maskCount], JS_NewFloat64(ctx, gnum(ctx, renderer, A_maskCount) + d)) < 0 ? -1 : 0;
}
// GeometryMask.preRenderWebGL(renderer, child, camera)
static JSValue js_gm_pre_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst renderer = argv[0], camera = argv[2];
    JSValue gl = G(renderer, A_gl), stack = JS_UNDEFINED, ccm = JS_UNDEFINED, cm = JS_UNDEFINED, ccmm = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    int64_t n = 0;
    if (renderer_flush(ctx, renderer)) goto out;
    stack = JS_GetProperty(ctx, renderer, GR.maskStack);
    if (JS_IsException(stack) || JS_GetLength(ctx, stack, &n) < 0) goto out;
    if (n == 0) {
        JSValueConst st = JS_NewInt32(ctx, 0x0B90), bit = JS_NewInt32(ctx, 0x400);
        if (pcall0(ctx, gl, PA_enable, 1, &st) || pcall0(ctx, gl, GR.clear, 1, &bit)) goto out;
        if (JS_SetProperty(ctx, renderer, AT[A_maskCount], JS_NewInt32(ctx, 0)) < 0) goto out;
    }
    ccm = JS_GetProperty(ctx, renderer, GR.currentCameraMask);
    ccmm = JS_IsException(ccm) ? JS_EXCEPTION : G(ccm, A_mask);
    if (JS_IsException(ccmm)) goto out;
    if (!JS_IsStrictEqual(ctx, ccmm, this_val)) {
        cm = JS_GetProperty(ctx, renderer, GR.currentMask);
        if (JS_IsException(cm) || JS_SetProperty(ctx, cm, AT[A_mask], JS_DupValue(ctx, this_val)) < 0) goto out;
    }
    {
        JSValue entry = JS_NewObject(ctx);
        JS_SetProperty(ctx, entry, AT[A_mask], JS_DupValue(ctx, this_val));
        JS_SetProperty(ctx, entry, GR.camera, JS_DupValue(ctx, camera));
        int e = pcall0(ctx, stack, GR.push, 1, (JSValueConst *)&entry);
        JS_FreeValue(ctx, entry);
        if (e) goto out;
    }
    if (apply_stencil(ctx, this_val, renderer, camera, true) || add_count(ctx, renderer, 1)) goto out;
    ret = JS_UNDEFINED;
out:
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, stack), JS_FreeValue(ctx, ccm), JS_FreeValue(ctx, ccmm), JS_FreeValue(ctx, cm);
    return ret;
}
// GeometryMask.postRenderWebGL(renderer)
static JSValue js_gm_post_render(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst renderer = argv[0];
    JSValue gl = G(renderer, A_gl), stack = JS_UNDEFINED, cur = JS_UNDEFINED, prev = JS_UNDEFINED, pm = JS_UNDEFINED,
            pc = JS_UNDEFINED, ccm = JS_UNDEFINED, ccmm = JS_UNDEFINED;
    JSValue ret = JS_EXCEPTION;
    int64_t n = 0;
    stack = JS_GetProperty(ctx, renderer, GR.maskStack);
    if (JS_IsException(stack) || pcall0(ctx, stack, GR.pop, 0, NULL)) goto out;
    if (add_count(ctx, renderer, -1) || renderer_flush(ctx, renderer)) goto out;
    cur = JS_GetProperty(ctx, renderer, GR.currentMask);
    if (JS_IsException(cur) || JS_GetLength(ctx, stack, &n) < 0) goto out;
    if (n == 0) {
        if (JS_SetProperty(ctx, cur, AT[A_mask], JS_NULL) < 0) goto out;
        JSValueConst st = JS_NewInt32(ctx, 0x0B90);
        if (pcall0(ctx, gl, GR.disable, 1, &st)) goto out;
    } else {
        prev = JS_GetPropertyInt64(ctx, stack, n - 1);
        pm = G(prev, A_mask);
        pc = JS_GetProperty(ctx, prev, GR.camera);
        if (JS_IsException(pm) || JS_IsException(pc)) goto out;
        if (apply_stencil(ctx, pm, renderer, pc, false)) goto out;
        ccm = JS_GetProperty(ctx, renderer, GR.currentCameraMask);
        ccmm = JS_IsException(ccm) ? JS_EXCEPTION : G(ccm, A_mask);
        if (JS_IsException(ccmm)) goto out;
        if (!JS_IsStrictEqual(ctx, ccmm, pm)) {
            if (JS_SetProperty(ctx, cur, AT[A_mask], JS_DupValue(ctx, pm)) < 0 ||
                JS_SetProperty(ctx, cur, GR.camera, JS_DupValue(ctx, pc)) < 0)
                goto out;
        } else if (JS_SetProperty(ctx, cur, AT[A_mask], JS_NULL) < 0) {
            goto out;
        }
    }
    ret = JS_UNDEFINED;
out:
    JS_FreeValue(ctx, gl), JS_FreeValue(ctx, stack), JS_FreeValue(ctx, cur), JS_FreeValue(ctx, prev);
    JS_FreeValue(ctx, pm), JS_FreeValue(ctx, pc), JS_FreeValue(ctx, ccm), JS_FreeValue(ctx, ccmm);
    return ret;
}
// shapesSetup({rectRender}) -> {rectRender, gmPreRender, gmPostRender, gmApplyStencil}
static JSValue js_shapes_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    gr_init(ctx);
    JS_FreeValue(ctx, GR.rect_orig_render);
    GR.rect_orig_render = JS_GetPropertyStr(ctx, argv[0], "rectRender");
    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "rectRender", JS_NewCFunction(ctx, js_rect_render, "renderWebGL", 4));
    JS_SetPropertyStr(ctx, out, "gmPreRender", JS_NewCFunction(ctx, js_gm_pre_render, "preRenderWebGL", 3));
    JS_SetPropertyStr(ctx, out, "gmPostRender", JS_NewCFunction(ctx, js_gm_post_render, "postRenderWebGL", 1));
    JS_SetPropertyStr(ctx, out, "gmApplyStencil", keep(ctx, &GR.gm_apply_stencil, JS_NewCFunction(ctx, js_gm_apply_stencil, "applyStencil", 3)));
    return out;
}

// ---------------------------------------------------------------- EventEmitter.emit
// eventemitter3's emit (Phaser.Events.EventEmitter), which Phaser runs dozens of times per frame
// (game step, scene and renderer events), in C. The same calls in the same order, except that
// listeners known to be empty functions are skipped: every pipeline subscribes its
// onPreRender/onRender/onPostRender to the renderer, and most are WebGLPipeline's empty ones
// (26 of the 30 calls per battle frame). js/fastpath.js checks that emit is eventemitter3's and
// that events are stored under plain names before installing this.
#define EE_MAX_NOOP 8
static struct {
    JSValue noop[EE_MAX_NOOP];
    int n_noop;
    JSAtom fn, context, once, removeListener;
} EE;

static bool ee_noop(JSValueConst f) {
    for (int i = 0; i < EE.n_noop; i++)
        if (same(f, EE.noop[i])) return true;
    return false;
}

// calls listener l (an eventemitter3 EE object) with argv[1..argc-1]; removes it first if once
static int ee_call(JSContext *ctx, JSValueConst self, JSValueConst l, JSValueConst event, int argc, JSValueConst *argv) {
    JSValue once = JS_GetProperty(ctx, l, EE.once);
    if (JS_IsException(once)) return -1;
    int e = 0;
    if (JS_ToBool(ctx, once) > 0) {
        JSValue f = JS_GetProperty(ctx, l, EE.fn);
        JSValueConst args[4] = {event, f, JS_UNDEFINED, JS_TRUE};
        e = JS_IsException(f) ? -1 : pcall0(ctx, self, EE.removeListener, 4, args);
        JS_FreeValue(ctx, f);
    }
    JS_FreeValue(ctx, once);
    if (e) return -1;
    JSValue f = JS_GetProperty(ctx, l, EE.fn);
    if (JS_IsException(f)) return -1;
    if (ee_noop(f)) {
        JS_FreeValue(ctx, f);
        return 0;
    }
    JSValue c = JS_GetProperty(ctx, l, EE.context);
    JSValue r = JS_IsException(c) ? JS_EXCEPTION : JS_Call(ctx, f, c, argc - 1, argv + 1);
    JS_FreeValue(ctx, f), JS_FreeValue(ctx, c);
    if (JS_IsException(r)) return -1;
    JS_FreeValue(ctx, r);
    return 0;
}

static JSValue js_ee_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst event = argc > 0 ? argv[0] : JS_UNDEFINED;
    JSValue evs = G(this_val, A__events);
    if (JS_IsException(evs)) return JS_EXCEPTION;
    JSValue ls = JS_GetPropertyValue(ctx, evs, JS_DupValue(ctx, event));
    JS_FreeValue(ctx, evs);
    if (JS_IsException(ls)) return JS_EXCEPTION;
    if (JS_ToBool(ctx, ls) <= 0) {
        JS_FreeValue(ctx, ls);
        return JS_FALSE;
    }
    JSValue fn = JS_GetProperty(ctx, ls, EE.fn);
    bool single = JS_ToBool(ctx, fn) > 0;
    JS_FreeValue(ctx, fn);
    int e = 0;
    if (single) {
        e = ee_call(ctx, this_val, ls, event, argc ? argc : 1, argc ? argv : &event);
    } else {
        int64_t n = 0;
        e = JS_GetLength(ctx, ls, &n);
        for (int64_t i = 0; i < n && !e; i++) {
            JSValue l = JS_GetPropertyInt64(ctx, ls, i);
            e = JS_IsException(l) ? -1 : ee_call(ctx, this_val, l, event, argc ? argc : 1, argc ? argv : &event);
            JS_FreeValue(ctx, l);
        }
    }
    JS_FreeValue(ctx, ls);
    return e ? JS_EXCEPTION : JS_TRUE;
}

// emitSetup([empty functions to skip]) -> the native emit
static JSValue js_ee_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (!AT[0])
        for (int i = 0; i < A_COUNT; i++) AT[i] = JS_NewAtom(ctx, atom_names[i]);
    if (!EE.fn) {
        EE.fn = JS_NewAtom(ctx, "fn"), EE.context = JS_NewAtom(ctx, "context"), EE.once = JS_NewAtom(ctx, "once");
        EE.removeListener = JS_NewAtom(ctx, "removeListener");
    }
    for (int i = 0; i < EE.n_noop; i++) JS_FreeValue(ctx, EE.noop[i]);
    EE.n_noop = 0;
    int64_t n = 0;
    if (argc > 0 && JS_GetLength(ctx, argv[0], &n) == 0)
        for (int64_t i = 0; i < n && EE.n_noop < EE_MAX_NOOP; i++) EE.noop[EE.n_noop++] = JS_GetPropertyInt64(ctx, argv[0], i);
    return JS_NewCFunction(ctx, js_ee_emit, "emit", 6);
}

// ---------------------------------------------------------------- UUID
// Phaser names every Text's canvas texture with Utils.String.UUID():
//   'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => random hex digit)
// 32 JS callbacks per text (~40 us on desktop, ~0.5 ms on the Vita; 1,500+ texts per screen
// rebuild). replaceWrap(originalReplace) returns a String.prototype.replace that makes that one
// call directly (after checking the callback answers like Phaser's) and passes every other call
// to the original.
static JSValue orig_replace; // set by replaceWrap before js_replace can run
static bool have_orig_replace;
static const char uuid_template[] = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";

static uint32_t uuid_rand(void) {
    static uint64_t x;
    if (!x) x = 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&x ^ (uint64_t)time(NULL);
    x ^= x << 13, x ^= x >> 7, x ^= x << 17;
    return (uint32_t)(x >> 32);
}

static JSValue js_replace(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc == 2 && JS_IsString(this_val) && JS_IsObject(argv[0]) && JS_IsFunction(ctx, argv[1])) {
        JSValue lenv = JS_GetPropertyStr(ctx, this_val, "length");
        bool cand = JS_VALUE_GET_TAG(lenv) == JS_TAG_INT && JS_VALUE_GET_INT(lenv) == 36;
        JS_FreeValue(ctx, lenv);
        if (cand) {
            const char *str = JS_ToCString(ctx, this_val);
            cand = str && !strcmp(str, uuid_template);
            JS_FreeCString(ctx, str);
        }
        if (cand) {
            JSValue src = JS_GetPropertyStr(ctx, argv[0], "source"), fl = JS_GetPropertyStr(ctx, argv[0], "flags");
            const char *ss = JS_ToCString(ctx, src), *fs = JS_ToCString(ctx, fl);
            cand = ss && fs && !strcmp(ss, "[xy]") && !strcmp(fs, "g");
            JS_FreeCString(ctx, ss), JS_FreeCString(ctx, fs), JS_FreeValue(ctx, src), JS_FreeValue(ctx, fl);
        }
        // The callback must behave like Phaser's: 'x' -> one hex digit, 'y' -> one of 8 9 a b.
        for (int k = 0; cand && k < 2; k++) {
            JSValue arg = JS_NewString(ctx, k ? "y" : "x");
            JSValue r = JS_Call(ctx, argv[1], JS_UNDEFINED, 1, (JSValueConst *)&arg);
            JS_FreeValue(ctx, arg);
            if (JS_IsException(r)) return r;
            const char *rs = JS_ToCString(ctx, r);
            cand = rs && strlen(rs) == 1 && (k ? strchr("89ab", rs[0]) != NULL : strchr("0123456789abcdef", rs[0]) != NULL);
            JS_FreeCString(ctx, rs);
            JS_FreeValue(ctx, r);
        }
        if (cand) {
            static const char hex[] = "0123456789abcdef";
            char out[37];
            for (int i = 0; i < 36; i++) {
                char c = uuid_template[i];
                int t = (int)(uuid_rand() & 15);
                out[i] = c == 'x' ? hex[t] : c == 'y' ? hex[(t & 3) | 8] : c;
            }
            return JS_NewStringLen(ctx, out, 36);
        }
    }
    return JS_Call(ctx, orig_replace, this_val, argc, argv);
}

static JSValue js_replace_wrap(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_ThrowTypeError(ctx, "replaceWrap: need a function");
    if (have_orig_replace) JS_FreeValue(ctx, orig_replace);
    orig_replace = JS_DupValue(ctx, argv[0]);
    have_orig_replace = true;
    return JS_NewCFunction(ctx, js_replace, "replace", 2);
}

// ---------------------------------------------------------------- tweens
// Phaser 3.90's Tween.update and TweenData.update in C for the common case, with Phaser's own code
// for everything else (js/fastpath.js part 19 installs __native.tweenUpdate as Tween.prototype.update
// and passes the originals). A tween that is active, not paused and started steps its data here; a
// datum playing forward or backward that doesn't finish in this step is advanced here (the ease, the
// value, the target's property, the onUpdate dispatch when anyone listens), every other datum by its
// own update(). Linear, Sine and Cubic eases are computed here exactly as Phaser's JS does (same
// operations in the same order, the same libm); other eases are called as JS functions.
enum { TA_state, TA_paused, TA_hasStarted, TA_timeScale, TA_parent, TA_data, TA_totalData, TA_elapsed, TA_duration,
       TA_progress, TA_totalElapsed, TA_totalDuration, TA_totalProgress, TA_nextState, TA_persist, TA_setFinishedState,
       TA_isCountdown, TA_interpolation, TA_tween, TA_targets, TA_targetIndex, TA_key, TA_previous, TA_current, TA_ease,
       TA_start, TA_end, TA_isSeeking, TA__events, TA_callbacks, TA_onUpdate, TA_dispatchEvent, TA_update, TA_COUNT };
static const char *const ta_names[TA_COUNT] = {
    "state", "paused", "hasStarted", "timeScale", "parent", "data", "totalData", "elapsed", "duration",
    "progress", "totalElapsed", "totalDuration", "totalProgress", "nextState", "persist", "setFinishedState",
    "isCountdown", "interpolation", "tween", "targets", "targetIndex", "key", "previous", "current", "ease",
    "start", "end", "isSeeking", "_events", "callbacks", "onUpdate", "dispatchEvent", "update"};
static JSAtom TA[TA_COUNT];
enum { EASE_LINEAR, EASE_SINE_IN, EASE_SINE_OUT, EASE_SINE_INOUT, EASE_CUBIC_IN, EASE_CUBIC_OUT, EASE_CUBIC_INOUT, EASE_COUNT };
static struct {
    bool ready;
    JSValue orig_update, td_update, eases[EASE_COUNT]; // Tween's and TweenData's update
    JSAtom ev_update; // Events.TWEEN_UPDATE
} TW;

#define TG(o, a) JS_GetProperty(ctx, (o), TA[a])
static double tnum(JSContext *ctx, JSValueConst o, int a) {
    JSValue v = TG(o, a);
    double d = num(ctx, v);
    JS_FreeValue(ctx, v);
    return d;
}
static bool tbool(JSContext *ctx, JSValueConst o, int a) {
    JSValue v = TG(o, a);
    bool b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}
static void tset(JSContext *ctx, JSValueConst o, int a, double d) { JS_SetProperty(ctx, o, TA[a], JS_NewFloat64(ctx, d)); }
static inline bool is_num(JSValueConst v) { int t = JS_VALUE_GET_TAG(v); return t == JS_TAG_INT || JS_TAG_IS_FLOAT64(t); }
static double js_min(double a, double b) { return a != a || b != b ? NAN : a < b ? a : b; } // Math.min for these

static double ease_native(int e, double v) {
    switch (e) {
    case EASE_LINEAR: return v;
    case EASE_SINE_IN: return v == 0 ? 0 : v == 1 ? 1 : 1 - cos(v * M_PI / 2);
    case EASE_SINE_OUT: return v == 0 ? 0 : v == 1 ? 1 : sin(v * M_PI / 2);
    case EASE_SINE_INOUT: return v == 0 ? 0 : v == 1 ? 1 : 0.5 * (1 - cos(M_PI * v));
    case EASE_CUBIC_IN: return v * v * v;
    case EASE_CUBIC_OUT: --v; return v * v * v + 1;
    case EASE_CUBIC_INOUT:
        if ((v *= 2) < 1) return 0.5 * v * v * v;
        v -= 2;
        return 0.5 * (v * v * v + 2);
    }
    return v;
}

// TweenData.update(delta) for a datum playing forward or backward that doesn't finish in this step:
// 1 (still playing) or 0, -1 with an exception; 2: not the common case (the caller runs Phaser's).
static int tween_data_fast(JSContext *ctx, JSValueConst d, JSValueConst tween, JSValueConst targets, double delta) {
    double state = tnum(ctx, d, TA_state);
    bool forward = state == 5, backward = state == 6; // PLAYING_FORWARD, PLAYING_BACKWARD
    if (!forward && !backward) return 2;
    JSValue interp = TG(d, TA_interpolation), ease = TG(d, TA_ease), start = TG(d, TA_start), end = TG(d, TA_end);
    int rc = 2;
    JSValue target = JS_UNDEFINED, key = JS_UNDEFINED;
    JSValue upd = TG(d, TA_update);
    bool plain = same(upd, TW.td_update); // not TweenFrameData (texture frames) or a subclass
    JS_FreeValue(ctx, upd);
    if (!plain || JS_ToBool(ctx, interp) > 0 || tbool(ctx, d, TA_isCountdown) || !is_num(start) || !is_num(end) ||
        !JS_IsFunction(ctx, ease))
        goto out;
    double elapsed = tnum(ctx, d, TA_elapsed) + delta, duration = tnum(ctx, d, TA_duration);
    if (!(elapsed < duration) || elapsed < 0) goto out; // finishing (or rewinding past 0): Phaser's code
    target = JS_GetPropertyInt64(ctx, targets, (int64_t)tnum(ctx, d, TA_targetIndex));
    if (JS_ToBool(ctx, target) <= 0) goto out;
    key = TG(d, TA_key);
    if (!JS_IsString(key)) goto out;
    double progress = clamp01(elapsed / duration);
    tset(ctx, d, TA_elapsed, elapsed);
    tset(ctx, d, TA_progress, progress);
    JSValue prev = TG(d, TA_current);
    JS_SetProperty(ctx, d, TA[TA_previous], prev); // takes prev
    if (!forward) progress = 1 - progress;
    double v;
    int e = 0;
    while (e < EASE_COUNT && !same(ease, TW.eases[e])) e++;
    if (e < EASE_COUNT) {
        v = ease_native(e, progress);
    } else {
        JSValue arg = JS_NewFloat64(ctx, progress), r = JS_Call(ctx, ease, d, 1, &arg);
        if (JS_IsException(r)) { rc = -1; goto out; }
        if (JS_ToFloat64(ctx, &v, r)) { JS_FreeValue(ctx, r); rc = -1; goto out; }
        JS_FreeValue(ctx, r);
    }
    double s = num(ctx, start), current = s + ((num(ctx, end) - s) * v);
    tset(ctx, d, TA_current, current);
    JSAtom ka = JS_ValueToAtom(ctx, key);
    int w = JS_SetProperty(ctx, target, ka, JS_NewFloat64(ctx, current));
    JS_FreeAtom(ctx, ka);
    if (w < 0) { rc = -1; goto out; }
    // dispatchEvent(TWEEN_UPDATE, 'onUpdate'): skipped when seeking or nobody listens
    if (!tbool(ctx, tween, TA_isSeeking)) {
        JSValue evs = TG(tween, TA__events), cbs = TG(tween, TA_callbacks);
        JSValue l = JS_IsObject(evs) ? JS_GetProperty(ctx, evs, TW.ev_update) : JS_UNDEFINED;
        JSValue h = JS_IsObject(cbs) ? TG(cbs, TA_onUpdate) : JS_UNDEFINED;
        bool listen = !JS_IsUndefined(l) || JS_ToBool(ctx, h) > 0 || !JS_IsObject(evs);
        JS_FreeValue(ctx, evs), JS_FreeValue(ctx, cbs), JS_FreeValue(ctx, l), JS_FreeValue(ctx, h);
        if (listen) {
            JSValueConst args[2] = {JS_AtomToString(ctx, TW.ev_update), JS_NewAtomString(ctx, "onUpdate")};
            JSValue r = JS_Invoke(ctx, d, TA[TA_dispatchEvent], 2, args);
            JS_FreeValue(ctx, (JSValue)args[0]), JS_FreeValue(ctx, (JSValue)args[1]);
            if (JS_IsException(r)) { rc = -1; goto out; }
            JS_FreeValue(ctx, r);
        }
    }
    rc = tnum(ctx, d, TA_state) != 9; // !isComplete()
out:
    JS_FreeValue(ctx, interp), JS_FreeValue(ctx, ease), JS_FreeValue(ctx, start), JS_FreeValue(ctx, end);
    JS_FreeValue(ctx, target), JS_FreeValue(ctx, key);
    return rc;
}

// Tween.prototype.update(delta)
static JSValue js_tween_update(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    JSValueConst t = this_val;
    if (!TW.ready || argc < 1 || !JS_IsObject(t) || tnum(ctx, t, TA_state) != 21 /* ACTIVE */ || tbool(ctx, t, TA_paused) ||
        !tbool(ctx, t, TA_hasStarted))
        return JS_Call(ctx, TW.orig_update, t, argc, argv);
    JSValue parent = TG(t, TA_parent);
    double delta = num(ctx, argv[0]);
    delta *= tnum(ctx, t, TA_timeScale) * tnum(ctx, parent, TA_timeScale);
    JS_FreeValue(ctx, parent);
    JSValue data = TG(t, TA_data), targets = TG(t, TA_targets);
    double total = tnum(ctx, t, TA_totalData);
    bool running = false;
    JSValue ret = JS_EXCEPTION;
    for (int64_t i = 0; i < (int64_t)total; i++) {
        JSValue d = JS_GetPropertyInt64(ctx, data, i);
        int r = JS_IsObject(d) ? tween_data_fast(ctx, d, t, targets, delta) : 2;
        if (r == 2) {
            JSValue dv = JS_NewFloat64(ctx, delta), v = JS_Invoke(ctx, d, TA[TA_update], 1, (JSValueConst *)&dv);
            r = JS_IsException(v) ? -1 : JS_ToBool(ctx, v) > 0;
            JS_FreeValue(ctx, v);
        }
        JS_FreeValue(ctx, d);
        if (r < 0) goto out;
        if (r) running = true;
    }
    double el = tnum(ctx, t, TA_elapsed) + delta;
    tset(ctx, t, TA_elapsed, el);
    tset(ctx, t, TA_progress, js_min(el / tnum(ctx, t, TA_duration), 1));
    double tel = tnum(ctx, t, TA_totalElapsed) + delta;
    tset(ctx, t, TA_totalElapsed, tel);
    tset(ctx, t, TA_totalProgress, js_min(tel / tnum(ctx, t, TA_totalDuration), 1));
    if (!running) {
        JSValue v = JS_Invoke(ctx, t, TA[TA_nextState], 0, NULL);
        if (JS_IsException(v)) goto out;
        JS_FreeValue(ctx, v);
    }
    bool remove = tnum(ctx, t, TA_state) == 25; // isPendingRemove()
    if (remove && tbool(ctx, t, TA_persist)) {
        JSValue v = JS_Invoke(ctx, t, TA[TA_setFinishedState], 0, NULL);
        if (JS_IsException(v)) goto out;
        JS_FreeValue(ctx, v);
        remove = false;
    }
    ret = JS_NewBool(ctx, remove);
out:
    JS_FreeValue(ctx, data), JS_FreeValue(ctx, targets);
    return ret;
}

// tweenSetup(origUpdate, eases: [Linear, Sine.In, Sine.Out, Sine.InOut, Cubic.In, Cubic.Out,
//   Cubic.InOut], updateEvent, TweenData.prototype.update) -> the native Tween.prototype.update
static JSValue js_tween_setup(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    if (argc < 4 || !JS_IsFunction(ctx, argv[0]) || !JS_IsFunction(ctx, argv[3])) return JS_ThrowTypeError(ctx, "tweenSetup: bad arguments");
    JS_FreeValue(ctx, TW.td_update);
    TW.td_update = JS_DupValue(ctx, argv[3]);
    if (!TA[0])
        for (int i = 0; i < TA_COUNT; i++) TA[i] = JS_NewAtom(ctx, ta_names[i]);
    JS_FreeValue(ctx, TW.orig_update);
    TW.orig_update = JS_DupValue(ctx, argv[0]);
    for (int e = 0; e < EASE_COUNT; e++) {
        JS_FreeValue(ctx, TW.eases[e]);
        TW.eases[e] = JS_GetPropertyUint32(ctx, argv[1], e);
    }
    TW.ev_update = JS_ValueToAtom(ctx, argv[2]);
    TW.ready = true;
    return JS_NewCFunction(ctx, js_tween_update, "update", 1);
}

void fastpath_register(JSContext *ctx, JSValue native) {
    JS_SetPropertyStr(ctx, native, "pipelinePushBatch", JS_NewCFunction(ctx, js_push_batch, "pushBatch", 1));
    JS_SetPropertyStr(ctx, native, "pipelineFlush", JS_NewCFunction(ctx, js_flush, "flush", 1));
    JS_SetPropertyStr(ctx, native, "makeBatchQuad", JS_NewCFunction(ctx, js_make_batch_quad, "makeBatchQuad", 1));
    for (int n = 1; n <= 4; n++) {
        char name[16];
        snprintf(name, sizeof name, "setUniform%d", n);
        JS_SetPropertyStr(ctx, native, name, keep(ctx, &PM.set_uniform_fn[n],
            JS_NewCFunctionMagic(ctx, js_set_uniform, name, 3 + n, JS_CFUNC_generic_magic, n)));
    }
    JS_SetPropertyStr(ctx, native, "emitSetup", JS_NewCFunction(ctx, js_ee_setup, "emitSetup", 1));
    JS_SetPropertyStr(ctx, native, "shapesSetup", JS_NewCFunction(ctx, js_shapes_setup, "shapesSetup", 1));
    JS_SetPropertyStr(ctx, native, "nineSliceSetup", JS_NewCFunction(ctx, js_nineslice_setup, "nineSliceSetup", 1));
    JS_SetPropertyStr(ctx, native, "graphicsSetup", JS_NewCFunction(ctx, js_graphics_setup, "graphicsSetup", 1));
    JS_SetPropertyStr(ctx, native, "pipelineSetup", JS_NewCFunction(ctx, js_pipeline_setup, "pipelineSetup", 1));
    JS_SetPropertyStr(ctx, native, "batchSprite", JS_NewCFunction(ctx, js_batch_sprite, "batchSprite", 30));
    JS_SetPropertyStr(ctx, native, "batchVerts", JS_NewCFunction(ctx, js_batch_verts, "batchVerts", 8));
    JS_SetPropertyStr(ctx, native, "batchChars", JS_NewCFunction(ctx, js_batch_chars, "batchChars", 17));
    JS_SetPropertyStr(ctx, native, "bitmapTextSetup", JS_NewCFunction(ctx, js_bitmap_text_setup, "bitmapTextSetup", 3));
    JS_SetPropertyStr(ctx, native, "tweenSetup", JS_NewCFunction(ctx, js_tween_setup, "tweenSetup", 4));
    JS_SetPropertyStr(ctx, native, "renderContainer", JS_NewCFunction(ctx, js_render_container, "renderContainer", 4));
    JS_SetPropertyStr(ctx, native, "renderSetup", JS_NewCFunction(ctx, js_render_setup, "renderSetup", 1));
    JS_SetPropertyStr(ctx, native, "replaceWrap", JS_NewCFunction(ctx, js_replace_wrap, "replaceWrap", 1));
}
