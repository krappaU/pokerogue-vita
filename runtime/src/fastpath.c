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

typedef struct {
    float offX, offY, gx, gy;
    double rot;
    float sx, sy;
    double x, y, w, h;
    int round;
    float u0, v0, u1, v1;
    uint32_t tTL, tTR, tBL, tBR;
    float tintEffect, unit;
} sprite_quad;

// Writes one sprite's 6 vertices at vf (MultiPipeline.batchSprite after the property reads).
static void write_sprite(float *vf, const float *cam, const float *par, const sprite_quad *q) {
    // spriteMatrix.applyITRS(gx, gy, rotation, sx, sy)
    float s = sinf(q->rot), c = cosf(q->rot);
    float sa = c * q->sx, sb = s * q->sx, sc = -s * q->sy, sd = c * q->sy, se = q->gx, sf = q->gy;

    // camMatrix = copy of camera matrix, optionally multiplied with parent (with scroll offset)
    float m[6];
    memcpy(m, cam, sizeof m);
    if (par) {
        float pse = q->offX * m[0] + q->offY * m[2] + m[4];
        float psf = q->offX * m[1] + q->offY * m[3] + m[5];
        float a0 = m[0], b0 = m[1], c0 = m[2], d0 = m[3];
        m[0] = par[0] * a0 + par[1] * c0;
        m[1] = par[0] * b0 + par[1] * d0;
        m[2] = par[2] * a0 + par[3] * c0;
        m[3] = par[2] * b0 + par[3] * d0;
        m[4] = par[4] * a0 + par[5] * c0 + pse;
        m[5] = par[4] * b0 + par[5] * d0 + psf;
    } else {
        se += q->offX;
        sf += q->offY;
    }

    // calc = camMatrix * spriteMatrix
    float a = sa * m[0] + sb * m[2];
    float b = sa * m[1] + sb * m[3];
    float cc = sc * m[0] + sd * m[2];
    float d = sc * m[1] + sd * m[3];
    float e = se * m[0] + sf * m[2] + m[4];
    float f = se * m[1] + sf * m[3] + m[5];

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
    A_addToRenderList, A__pkrTextDirty, A_width, A_height, A_style, A_resolution, A_isRenderTexture, A_COUNT
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
static int batch_sprite(JSContext *ctx, rctx *r, JSValueConst pipeline, JSValueConst go, JSValueConst frame,
                        const float *par, double alpha_mul, double sfx, double sfy, bool text) {
    int rc = -1;
    JSValue cur = G(r->mgr, A_current), tex = JS_UNDEFINED, cb = JS_UNDEFINED, unitv = JS_UNDEFINED;
    // pipelines.set(pipeline, go): isCurrent -> updateProjectionMatrix + onBind, else rebind.
    bool current = same(cur, pipeline);
    if (current) {
        JSValue sh = G(pipeline, A_currentShader), prog = G(sh, A_program), rp = G(r->renderer, A_currentProgram);
        current = JS_IsStrictEqual(ctx, prog, rp);
        JS_FreeValue(ctx, sh), JS_FreeValue(ctx, prog), JS_FreeValue(ctx, rp);
    }
    if (!current || !gsame(ctx, pipeline, A_onBind, H.wp_on_bind)) {
        JSValueConst args[2] = {pipeline, go};
        if (call0(ctx, r->mgr, A_set, 2, args)) goto out;
    } else if (gbool(ctx, pipeline, A_projectionMatrix)) {
        double pw = gnum(ctx, r->renderer, A_projectionWidth), ph = gnum(ctx, r->renderer, A_projectionHeight);
        if (gnum(ctx, pipeline, A_projectionWidth) != pw || gnum(ctx, pipeline, A_projectionHeight) != ph) {
            JSValueConst args[2] = {JS_NewFloat64(ctx, pw), JS_NewFloat64(ctx, ph)};
            if (call0(ctx, pipeline, A_setProjectionMatrix, 2, args)) goto out;
        }
    }

    double dox = gnum(ctx, go, A__displayOriginX), doy = gnum(ctx, go, A__displayOriginY);
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
        unitv = call(ctx, pipeline, A_pushBatch, 1, &tex);
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
    JS_FreeValue(ctx, cur), JS_FreeValue(ctx, tex), JS_FreeValue(ctx, cb), JS_FreeValue(ctx, unitv);
    return rc;
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
    if (!same(rw, H.text_render) || gbool(ctx, t, A__pkrTextDirty) || gnum(ctx, t, A_width) == 0 ||
        gnum(ctx, t, A_height) == 0 || gbool(ctx, t, A_flipX) || gbool(ctx, t, A_flipY) || gbool(ctx, t, A_isCropped) ||
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
    {
        JSValueConst sf[2] = {JS_NewFloat64(ctx, csx * sfx), JS_NewFloat64(ctx, csy * sfy)};
        JSValueConst al[4] = {JS_NewFloat64(ctx, tl * alpha), JS_NewFloat64(ctx, tr * alpha), JS_NewFloat64(ctx, bl * alpha),
                              JS_NewFloat64(ctx, br * alpha)};
        if (call0(ctx, child, A_setScrollFactor, 2, sf) || call0(ctx, child, A_setAlpha, 4, al)) goto out;
        JSValueConst args[5] = {r->renderer, child, r->camera, tm, ct};
        if (call0(ctx, child, A_renderWebGL, 5, args)) goto out;
        JSValueConst al2[4] = {JS_NewFloat64(ctx, tl), JS_NewFloat64(ctx, tr), JS_NewFloat64(ctx, bl), JS_NewFloat64(ctx, br)};
        JSValueConst sf2[2] = {JS_NewFloat64(ctx, csx), JS_NewFloat64(ctx, csy)};
        if (call0(ctx, child, A_setAlpha, 4, al2) || call0(ctx, child, A_setScrollFactor, 2, sf2)) goto out;
    }
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
        } else if (no_mask && same(rw, H.container_render)) {
            // Phaser sets the child's scroll factor and alpha to the combined values while it renders.
            set_type(ctx, r, list, child, i, n);
            double csx = gnum(ctx, child, A_scrollFactorX), csy = gnum(ctx, child, A_scrollFactorY);
            double ca = gnum(ctx, child, A__alpha);
            JSValueConst sf[2] = {JS_NewFloat64(ctx, csx * sfx), JS_NewFloat64(ctx, csy * sfy)};
            JSValueConst al[4] = {JS_NewFloat64(ctx, ca * alpha), JS_NewFloat64(ctx, ca * alpha), JS_NewFloat64(ctx, ca * alpha),
                                  JS_NewFloat64(ctx, ca * alpha)};
            e = call0(ctx, child, A_setScrollFactor, 2, sf) || call0(ctx, child, A_setAlpha, 4, al) ||
                render_container(ctx, r, child, tm);
            if (!e) {
                JSValueConst al2[4] = {JS_NewFloat64(ctx, ca), JS_NewFloat64(ctx, ca), JS_NewFloat64(ctx, ca), JS_NewFloat64(ctx, ca)};
                JSValueConst sf2[2] = {JS_NewFloat64(ctx, csx), JS_NewFloat64(ctx, csy)};
                e = call0(ctx, child, A_setAlpha, 4, al2) || call0(ctx, child, A_setScrollFactor, 2, sf2);
            }
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
    HOOK(text_render, "textRender")
#undef HOOK
    H.ready = true;
    return JS_UNDEFINED;
}

void fastpath_register(JSContext *ctx, JSValue native) {
    JS_SetPropertyStr(ctx, native, "batchSprite", JS_NewCFunction(ctx, js_batch_sprite, "batchSprite", 30));
    JS_SetPropertyStr(ctx, native, "batchVerts", JS_NewCFunction(ctx, js_batch_verts, "batchVerts", 8));
    JS_SetPropertyStr(ctx, native, "renderContainer", JS_NewCFunction(ctx, js_render_container, "renderContainer", 4));
    JS_SetPropertyStr(ctx, native, "renderSetup", JS_NewCFunction(ctx, js_render_setup, "renderSetup", 1));
}
