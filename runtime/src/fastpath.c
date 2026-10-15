// SPDX-License-Identifier: AGPL-3.0-only
// Native fast path for Phaser's per-sprite WebGL batching (MultiPipeline.batchSprite).
// JS still reads the game object's properties; this does the matrix math, tint packing and
// the 42 vertex writes in one call, replacing about 15 JS function calls per sprite.
// Mirrors Phaser 3.90: TransformMatrix.applyITRS / multiplyWithOffset / multiply / setQuad,
// Utils.getTintAppendFloatAlpha and MultiPipeline.batchQuad. Matrices are float32 like Phaser's.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

static inline uint32_t tint(double rgb, double a) {
    uint32_t ua = ((uint32_t)(int32_t)(a * 255)) & 0xff;
    return (ua << 24) | (uint32_t)(int64_t)rgb;
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

    float offX = num(ctx, argv[4]), offY = num(ctx, argv[5]);
    float gx = num(ctx, argv[6]), gy = num(ctx, argv[7]);
    double rot = num(ctx, argv[8]);
    float sx = num(ctx, argv[9]), sy = num(ctx, argv[10]);
    double x = num(ctx, argv[11]), y = num(ctx, argv[12]);
    double xw = x + num(ctx, argv[13]), yh = y + num(ctx, argv[14]);
    int round = JS_ToBool(ctx, argv[15]);
    float u0 = num(ctx, argv[16]), v0 = num(ctx, argv[17]), u1 = num(ctx, argv[18]), v1 = num(ctx, argv[19]);
    uint32_t tTL = tint(num(ctx, argv[20]), num(ctx, argv[24]));
    uint32_t tTR = tint(num(ctx, argv[21]), num(ctx, argv[25]));
    uint32_t tBL = tint(num(ctx, argv[22]), num(ctx, argv[26]));
    uint32_t tBR = tint(num(ctx, argv[23]), num(ctx, argv[27]));
    float tintEffect = num(ctx, argv[28]);
    float unit = num(ctx, argv[29]);

    // spriteMatrix.applyITRS(gx, gy, rotation, sx, sy)
    float s = sinf(rot), c = cosf(rot);
    float sa = c * sx, sb = s * sx, sc = -s * sy, sd = c * sy, se = gx, sf = gy;

    // camMatrix = copy of camera matrix, optionally multiplied with parent (with scroll offset)
    float m[6];
    memcpy(m, cam, sizeof m);
    if (par) {
        float pse = offX * m[0] + offY * m[2] + m[4];
        float psf = offX * m[1] + offY * m[3] + m[5];
        float a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3];
        m[0] = par[0] * a0 + par[1] * c0;
        m[1] = par[0] * b0 + par[1] * d0;
        m[2] = par[2] * a0 + par[3] * c0;
        m[3] = par[2] * b0 + par[3] * d0;
        m[4] = par[4] * a0 + par[5] * c0 + pse;
        m[5] = par[4] * b0 + par[5] * d0 + psf;
    } else {
        se += offX;
        sf += offY;
    }

    // calc = camMatrix * spriteMatrix
    float a = sa * m[0] + sb * m[2];
    float b = sa * m[1] + sb * m[3];
    float cc = sc * m[0] + sd * m[2];
    float d = sc * m[1] + sd * m[3];
    float e = se * m[0] + sf * m[2] + m[4];
    float f = se * m[1] + sf * m[3] + m[5];

    // setQuad
    float x0 = x * a + y * cc + e, y0 = x * b + y * d + f;
    float x1 = x * a + yh * cc + e, y1 = x * b + yh * d + f;
    float x2 = xw * a + yh * cc + e, y2 = xw * b + yh * d + f;
    float x3 = xw * a + y * cc + e, y3 = xw * b + y * d + f;
    if (round) {
        float rx0 = floorf(x0 + 0.5f), ry0 = floorf(y0 + 0.5f);
        float dx = rx0 - x0, dy = ry0 - y0;
        x0 = rx0; y0 = ry0;
        x1 += dx; y1 += dy;
        x2 += dx; y2 += dy;
        x3 += dx; y3 += dy;
    }

    // batchQuad vertex layout: x, y, u, v, unit, tintEffect, tint(u32) x 6 vertices
    float *vf = (float *)vb + off;
    uint32_t *vu = (uint32_t *)vb + off;
#define VERT(i, X, Y, U, V, T) \
    vf[i * 7 + 0] = X; vf[i * 7 + 1] = Y; vf[i * 7 + 2] = U; vf[i * 7 + 3] = V; \
    vf[i * 7 + 4] = unit; vf[i * 7 + 5] = tintEffect; vu[i * 7 + 6] = T;
    VERT(0, x0, y0, u0, v0, tTL)
    VERT(1, x1, y1, u0, v1, tBL)
    VERT(2, x2, y2, u1, v1, tBR)
    VERT(3, x0, y0, u0, v0, tTL)
    VERT(4, x2, y2, u1, v1, tBR)
    VERT(5, x3, y3, u1, v0, tTR)
#undef VERT
    return JS_UNDEFINED;
}

// batchVerts(vertexData: ArrayBuffer, offset (float index), matrix: Float32Array(6),
//   vertices: [{vx, vy, u, v}], roundPixels, unit, tintEffect, tint) -> vertices written
// The vertex loop of Phaser 3.90's NineSliceWebGLRenderer: TransformMatrix.getXRound/getYRound
// per vertex (float64 math, Math.round), one shared tint.
static JSValue js_batch_verts(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static JSAtom a_vx, a_vy, a_u, a_v;
    if (!a_vx) a_vx = JS_NewAtom(ctx, "vx"), a_vy = JS_NewAtom(ctx, "vy"), a_u = JS_NewAtom(ctx, "u"), a_v = JS_NewAtom(ctx, "v");
    size_t size;
    uint8_t *vb = JS_GetArrayBuffer(ctx, &size, argv[0]);
    float *m = f32(ctx, argv[2]);
    int64_t n = 0;
    if (!vb || !m || JS_GetLength(ctx, argv[3], &n) < 0) return JS_ThrowTypeError(ctx, "batchVerts: bad arguments");
    int64_t off = (int64_t)num(ctx, argv[1]);
    if (off < 0 || (size_t)(off + n * 7) * 4 > size) return JS_ThrowRangeError(ctx, "batchVerts: vertex buffer too small");
    bool round = JS_ToBool(ctx, argv[4]);
    float unit = (float)num(ctx, argv[5]), effect = (float)num(ctx, argv[6]);
    uint32_t t = (uint32_t)(int64_t)num(ctx, argv[7]);
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5];
    float *vf = (float *)vb + off;
    uint32_t *vu = (uint32_t *)vb + off;
    for (int64_t i = 0; i < n; i++) {
        JSValue v = JS_GetPropertyInt64(ctx, argv[3], i);
        JSValue jx = JS_GetProperty(ctx, v, a_vx), jy = JS_GetProperty(ctx, v, a_vy);
        JSValue ju = JS_GetProperty(ctx, v, a_u), jv = JS_GetProperty(ctx, v, a_v);
        double vx = num(ctx, jx), vy = num(ctx, jy);
        double x = vx * a + vy * c + e, y = vx * b + vy * d + f;
        if (round) x = floor(x + 0.5), y = floor(y + 0.5);
        vf[0] = (float)x, vf[1] = (float)y, vf[2] = (float)num(ctx, ju), vf[3] = (float)num(ctx, jv);
        vf[4] = unit, vf[5] = effect, vu[6] = t;
        vf += 7, vu += 7;
        JS_FreeValue(ctx, jx), JS_FreeValue(ctx, jy), JS_FreeValue(ctx, ju), JS_FreeValue(ctx, jv), JS_FreeValue(ctx, v);
    }
    return JS_NewInt64(ctx, n);
}

void fastpath_register(JSContext *ctx, JSValue native) {
    JS_SetPropertyStr(ctx, native, "batchSprite", JS_NewCFunction(ctx, js_batch_sprite, "batchSprite", 30));
    JS_SetPropertyStr(ctx, native, "batchVerts", JS_NewCFunction(ctx, js_batch_verts, "batchVerts", 8));
}
