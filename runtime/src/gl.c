// SPDX-License-Identifier: AGPL-3.0-only
// WebGL 1 on OpenGL ES 2, as a native QuickJS class (no JS wrapper layer, to keep per-call cost low).
//
// The game renders to a 1920x1080 canvas. The default framebuffer is the real screen (960x544 on
// the Vita), so viewport/scissor calls on it are scaled and letterboxed here instead of rendering
// to a full-size offscreen buffer first. Framebuffer objects (render textures) are left unscaled.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gles.h"

#include "host.h"

static JSClassID ctx_class, obj_class, uloc_class;

typedef struct {
    GLuint id;
    int kind; // GLO_*
} globj;

enum { GLO_BUFFER, GLO_TEXTURE, GLO_PROGRAM, GLO_SHADER, GLO_FRAMEBUFFER, GLO_RENDERBUFFER };
static const char *glo_names[] = {"WebGLBuffer", "WebGLTexture", "WebGLProgram",
                                  "WebGLShader", "WebGLFramebuffer", "WebGLRenderbuffer"};

typedef struct {
    GLint loc;
    bool projection; // Phaser's uProjectionMatrix (see uniform_matrix)
} uloc;

// Context state that WebGL exposes but GLES does not track the same way.
static struct {
    bool flip_y, premultiply;
    bool default_fb;    // default framebuffer bound -> scale viewport/scissor
    int virt_w, virt_h; // canvas size the game thinks it renders at
    JSValue canvas;
    JSValue bound_program, bound_array_buffer, bound_element_buffer, bound_framebuffer, bound_renderbuffer;
    JSValue bound_texture[32];
    int active_unit;
    // Shadow copies of state that gl_after_frame changes and restores (no glGet round trips).
    bool scissor_on;
    GLint vp[4];
    GLfloat clear_color[4];
    GLboolean color_mask[4];
    GLuint fb;
    float fb_scale; // render-target scale of the bound framebuffer (1 or 0.5)
    // More shadow state, for drawing over the game in the middle of a frame (gl_restore_state).
    bool blend_on, depth_on, stencil_on, cull_on;
    GLenum blend_func[4], blend_eq[2];
    GLint unpack_align;
    struct { bool on, set; GLint size; GLenum type; GLboolean norm; GLsizei stride; intptr_t off; GLuint buf; } attrib[2];
} st = {.default_fb = true, .virt_w = 300, .virt_h = 150, .color_mask = {1, 1, 1, 1}, .fb_scale = 1,
        .blend_func = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO}, .blend_eq = {GL_FUNC_ADD, GL_FUNC_ADD}, .unpack_align = 4};

// Off-screen render targets the size of the whole game canvas (Phaser's post-FX frames, 1920x1080)
// are allocated at half size: the screen shows the game at about half that resolution anyway, and
// each one saves 6 MB of video memory. Drawing into them is scaled to match.
#define RT_SCALE 0.5f
static uint8_t tex_scaled[65536], fb_scaled[65536];

static void tex_forget(JSContext *ctx, GLuint id);
static void tex_use(JSContext *ctx, GLuint id);

// ---------------------------------------------------------------- helpers

static inline int gi(JSContext *ctx, JSValueConst v) {
    int32_t i = 0;
    if (JS_VALUE_GET_TAG(v) == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    JS_ToInt32(ctx, &i, v);
    return i;
}

static inline double gd(JSContext *ctx, JSValueConst v) {
    int tag = JS_VALUE_GET_TAG(v);
    if (tag == JS_TAG_INT) return JS_VALUE_GET_INT(v);
    if (JS_TAG_IS_FLOAT64(tag)) return JS_VALUE_GET_FLOAT64(v);
    double d = 0;
    JS_ToFloat64(ctx, &d, v);
    return d;
}

#define gf(ctx, v) ((GLfloat)gd(ctx, v))

static GLuint oid(JSValueConst v) {
    globj *o = JS_GetOpaque(v, obj_class);
    return o ? o->id : 0;
}

static JSValue new_obj(JSContext *ctx, GLuint id, int kind) {
    if (!id && kind != GLO_PROGRAM && kind != GLO_SHADER) return JS_NULL;
    JSValue v = JS_NewObjectClass(ctx, obj_class);
    globj *o = malloc(sizeof *o);
    o->id = id;
    o->kind = kind;
    JS_SetOpaque(v, o);
    JS_DefinePropertyValueStr(ctx, v, "__type", JS_NewString(ctx, glo_names[kind]), 0);
    return v;
}

static void obj_finalizer(JSRuntime *rt, JSValue v) { free(JS_GetOpaque(v, obj_class)); }
static void tex_account(GLuint id, size_t bytes);
static void uloc_finalizer(JSRuntime *rt, JSValue v) { free(JS_GetOpaque(v, uloc_class)); }

static void track(JSContext *ctx, JSValue *slot, JSValueConst v) {
    JS_FreeValue(ctx, *slot);
    *slot = JS_IsObject(v) ? JS_DupValue(ctx, v) : JS_NULL;
}

// Pointer + byte length of an ArrayBuffer or ArrayBufferView.
static void *buffer_data(JSContext *ctx, JSValueConst v, size_t *len) {
    size_t n;
    uint8_t *p = JS_GetArrayBuffer(ctx, &n, v);
    if (p) {
        *len = n;
        return p;
    }
    JS_FreeValue(ctx, JS_GetException(ctx));
    size_t off, tlen, bpe;
    JSValue ab = JS_GetTypedArrayBuffer(ctx, v, &off, &tlen, &bpe);
    if (JS_IsException(ab)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return NULL;
    }
    p = JS_GetArrayBuffer(ctx, &n, ab);
    JS_FreeValue(ctx, ab);
    if (!p) return NULL;
    *len = tlen;
    return p + off;
}

// Float data from a Float32Array or a plain JS array. *tmp is set if the caller must free.
static const GLfloat *float_data(JSContext *ctx, JSValueConst v, int *count, GLfloat **tmp) {
    *tmp = NULL;
    if (JS_GetTypedArrayType(v) == JS_TYPED_ARRAY_FLOAT32) {
        size_t len;
        const GLfloat *p = buffer_data(ctx, v, &len);
        *count = (int)(len / 4);
        return p;
    }
    uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    *tmp = malloc((n ? n : 1) * sizeof(GLfloat));
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        (*tmp)[i] = gf(ctx, e);
        JS_FreeValue(ctx, e);
    }
    *count = (int)n;
    return *tmp;
}

static const GLint *int_data(JSContext *ctx, JSValueConst v, int *count, GLint **tmp) {
    *tmp = NULL;
    int t = JS_GetTypedArrayType(v);
    if (t == JS_TYPED_ARRAY_INT32) {
        size_t len;
        const GLint *p = buffer_data(ctx, v, &len);
        *count = (int)(len / 4);
        return p;
    }
    uint32_t n = 0;
    JSValue lv = JS_GetPropertyStr(ctx, v, "length");
    JS_ToUint32(ctx, &n, lv);
    JS_FreeValue(ctx, lv);
    *tmp = malloc((n ? n : 1) * sizeof(GLint));
    for (uint32_t i = 0; i < n; i++) {
        JSValue e = JS_GetPropertyUint32(ctx, v, i);
        (*tmp)[i] = gi(ctx, e);
        JS_FreeValue(ctx, e);
    }
    *count = (int)n;
    return *tmp;
}

// Default-framebuffer mapping from canvas pixels to screen pixels (fit + letterbox).
static void fb_map(float *s, float *ox, float *oy) {
    int fw, fh;
    plat_drawable_size(&fw, &fh);
    float sx = (float)fw / st.virt_w, sy = (float)fh / st.virt_h;
    *s = sx < sy ? sx : sy;
    *ox = (fw - st.virt_w * *s) * 0.5f;
    *oy = (fh - st.virt_h * *s) * 0.5f;
}

// Game canvas coordinates -> screen pixels (top-left origin), as the game is drawn on screen.
void gl_game_to_screen(float gx, float gy, float *sx, float *sy, float *scale) {
    float s, ox, oy;
    fb_map(&s, &ox, &oy);
    *sx = ox + gx * s;
    *sy = oy + gy * s;
    *scale = s;
}

static void refresh_virtual_size(JSContext *ctx) {
    if (!JS_IsObject(st.canvas)) return;
    JSValue w = JS_GetPropertyStr(ctx, st.canvas, "width"), h = JS_GetPropertyStr(ctx, st.canvas, "height");
    int iw = gi(ctx, w), ih = gi(ctx, h);
    if (iw > 0 && ih > 0) {
        st.virt_w = iw;
        st.virt_h = ih;
    }
    JS_FreeValue(ctx, w);
    JS_FreeValue(ctx, h);
}

#define FN(name) static JSValue name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
#define A(i) (argc > (i) ? argv[i] : JS_UNDEFINED)

// ---------------------------------------------------------------- state

FN(w_activeTexture) {
    int e = gi(ctx, A(0));
    st.active_unit = e - GL_TEXTURE0;
    glActiveTexture(e);
    return JS_UNDEFINED;
}
FN(w_blendColor) { glBlendColor(gf(ctx, A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3))); return JS_UNDEFINED; }
FN(w_blendEquation) {
    st.blend_eq[0] = st.blend_eq[1] = gi(ctx, A(0));
    glBlendEquation(st.blend_eq[0]);
    return JS_UNDEFINED;
}
FN(w_blendEquationSeparate) {
    st.blend_eq[0] = gi(ctx, A(0)), st.blend_eq[1] = gi(ctx, A(1));
    glBlendEquationSeparate(st.blend_eq[0], st.blend_eq[1]);
    return JS_UNDEFINED;
}
FN(w_blendFunc) {
    st.blend_func[0] = st.blend_func[2] = gi(ctx, A(0));
    st.blend_func[1] = st.blend_func[3] = gi(ctx, A(1));
    glBlendFunc(st.blend_func[0], st.blend_func[1]);
    return JS_UNDEFINED;
}
FN(w_blendFuncSeparate) {
    for (int i = 0; i < 4; i++) st.blend_func[i] = gi(ctx, A(i));
    glBlendFuncSeparate(st.blend_func[0], st.blend_func[1], st.blend_func[2], st.blend_func[3]);
    return JS_UNDEFINED;
}
FN(w_clear) { glClear(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_clearColor) {
    for (int i = 0; i < 4; i++) st.clear_color[i] = gf(ctx, A(i));
    glClearColor(st.clear_color[0], st.clear_color[1], st.clear_color[2], st.clear_color[3]);
    return JS_UNDEFINED;
}
FN(w_clearDepth) { glClearDepthf(gf(ctx, A(0))); return JS_UNDEFINED; }
FN(w_clearStencil) { glClearStencil(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_colorMask) {
    for (int i = 0; i < 4; i++) st.color_mask[i] = JS_ToBool(ctx, A(i));
    glColorMask(st.color_mask[0], st.color_mask[1], st.color_mask[2], st.color_mask[3]);
    return JS_UNDEFINED;
}
FN(w_cullFace) { glCullFace(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthFunc) { glDepthFunc(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthMask) { glDepthMask(JS_ToBool(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthRange) { glDepthRangef(gf(ctx, A(0)), gf(ctx, A(1))); return JS_UNDEFINED; }
static void track_cap(int cap, bool on) {
    switch (cap) {
    case GL_SCISSOR_TEST: st.scissor_on = on; break;
    case GL_BLEND: st.blend_on = on; break;
    case GL_DEPTH_TEST: st.depth_on = on; break;
    case GL_STENCIL_TEST: st.stencil_on = on; break;
    case GL_CULL_FACE: st.cull_on = on; break;
    }
}
FN(w_disable) {
    int cap = gi(ctx, A(0));
    track_cap(cap, false);
    glDisable(cap);
    return JS_UNDEFINED;
}
FN(w_enable) {
    int cap = gi(ctx, A(0));
    track_cap(cap, true);
    glEnable(cap);
    return JS_UNDEFINED;
}
FN(w_finish) { glFinish(); return JS_UNDEFINED; }
FN(w_flush) { glFlush(); return JS_UNDEFINED; }
FN(w_frontFace) { glFrontFace(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_hint) { glHint(gi(ctx, A(0)), gi(ctx, A(1))); return JS_UNDEFINED; }
FN(w_isEnabled) { return JS_NewBool(ctx, glIsEnabled(gi(ctx, A(0)))); }
FN(w_lineWidth) { glLineWidth(gf(ctx, A(0))); return JS_UNDEFINED; }
FN(w_polygonOffset) { glPolygonOffset(gf(ctx, A(0)), gf(ctx, A(1))); return JS_UNDEFINED; }
FN(w_sampleCoverage) { glSampleCoverage(gf(ctx, A(0)), JS_ToBool(ctx, A(1))); return JS_UNDEFINED; }
FN(w_stencilFunc) { glStencilFunc(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2))); return JS_UNDEFINED; }
FN(w_stencilFuncSeparate) {
    glStencilFuncSeparate(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_stencilMask) { glStencilMask(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_stencilMaskSeparate) { glStencilMaskSeparate(gi(ctx, A(0)), gi(ctx, A(1))); return JS_UNDEFINED; }
FN(w_stencilOp) { glStencilOp(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2))); return JS_UNDEFINED; }
FN(w_stencilOpSeparate) {
    glStencilOpSeparate(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_getError) { return JS_NewInt32(ctx, glGetError()); }

FN(w_pixelStorei) {
    int p = gi(ctx, A(0)), v = gi(ctx, A(1));
    if (p == 0x9240) st.flip_y = v != 0;
    else if (p == 0x9241) st.premultiply = v != 0;
    else if (p == 0x9243) { /* colorspace conversion: no-op */ }
    else {
        if (p == GL_UNPACK_ALIGNMENT) st.unpack_align = v;
        gl_pixel_store(p, v);
    }
    return JS_UNDEFINED;
}

FN(w_viewport) {
    GLint x = gi(ctx, A(0)), y = gi(ctx, A(1));
    GLsizei w = gi(ctx, A(2)), h = gi(ctx, A(3));
    if (st.default_fb) {
        refresh_virtual_size(ctx);
        float s, ox, oy;
        fb_map(&s, &ox, &oy);
        st.vp[0] = (GLint)lroundf(ox + x * s);
        st.vp[1] = (GLint)lroundf(oy + y * s);
        st.vp[2] = (GLsizei)lroundf(w * s);
        st.vp[3] = (GLsizei)lroundf(h * s);
    } else {
        float k = st.fb_scale;
        st.vp[0] = (GLint)(x * k), st.vp[1] = (GLint)(y * k), st.vp[2] = (GLsizei)(w * k), st.vp[3] = (GLsizei)(h * k);
    }
    glViewport(st.vp[0], st.vp[1], st.vp[2], st.vp[3]);
    return JS_UNDEFINED;
}

FN(w_scissor) {
    GLint x = gi(ctx, A(0)), y = gi(ctx, A(1));
    GLsizei w = gi(ctx, A(2)), h = gi(ctx, A(3));
    if (st.default_fb) {
        float s, ox, oy;
        fb_map(&s, &ox, &oy);
        glScissor((GLint)floorf(ox + x * s), (GLint)floorf(oy + y * s), (GLsizei)ceilf(w * s), (GLsizei)ceilf(h * s));
    } else {
        float k = st.fb_scale;
        glScissor((GLint)floorf(x * k), (GLint)floorf(y * k), (GLsizei)ceilf(w * k), (GLsizei)ceilf(h * k));
    }
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- objects

FN(w_createBuffer) { GLuint id; glGenBuffers(1, &id); return new_obj(ctx, id, GLO_BUFFER); }
FN(w_createTexture) { GLuint id; glGenTextures(1, &id); return new_obj(ctx, id, GLO_TEXTURE); }
FN(w_createFramebuffer) { GLuint id; glGenFramebuffers(1, &id); return new_obj(ctx, id, GLO_FRAMEBUFFER); }
FN(w_createRenderbuffer) { GLuint id; glGenRenderbuffers(1, &id); return new_obj(ctx, id, GLO_RENDERBUFFER); }
FN(w_createProgram) { return new_obj(ctx, glCreateProgram(), GLO_PROGRAM); }
FN(w_createShader) { return new_obj(ctx, glCreateShader(gi(ctx, A(0))), GLO_SHADER); }

// Per-buffer state for w_bufferSubData (below). vitaGL's buffer names are pointers, so a small
// open-addressing table rather than an array indexed by name.
typedef struct { GLuint id; uint32_t size; uint16_t usage; bool orphaned, partial; } buf_info;
#define BUF_SLOTS 1024
static buf_info bufs[BUF_SLOTS];

static buf_info *buffer_info(GLuint id, bool create) {
    if (!id) return NULL;
    unsigned h = (unsigned)(id * 2654435761u) % BUF_SLOTS;
    for (int i = 0; i < BUF_SLOTS; i++, h = (h + 1) % BUF_SLOTS) {
        if (bufs[h].id == id) return &bufs[h];
        if (bufs[h].id == 0) {
            if (!create) return NULL;
            memset(&bufs[h], 0, sizeof bufs[h]);
            bufs[h].id = id;
            return &bufs[h];
        }
    }
    return NULL;
}

FN(w_deleteBuffer) {
    GLuint id = oid(A(0));
    if (id) {
        buf_info *b = buffer_info(id, false);
        if (b) { // keep the probe chain intact: mark as a dead entry with an impossible name
            memset(b, 0, sizeof *b);
            b->id = (GLuint)-1;
        }
        glDeleteBuffers(1, &id);
    }
    return JS_UNDEFINED;
}
FN(w_deleteTexture) {
    GLuint id = oid(A(0));
    if (id) {
        tex_account(id, 0);
        tex_forget(ctx, id);
        if (id < 65536) tex_scaled[id] = 0;
        glDeleteTextures(1, &id);
    }
    return JS_UNDEFINED;
}
FN(w_deleteFramebuffer) {
    GLuint id = oid(A(0));
    if (id) {
        if (id < 65536) fb_scaled[id] = 0;
        glDeleteFramebuffers(1, &id);
    }
    return JS_UNDEFINED;
}
FN(w_deleteRenderbuffer) { GLuint id = oid(A(0)); if (id) glDeleteRenderbuffers(1, &id); return JS_UNDEFINED; }
static void family_free(GLuint prog);
FN(w_deleteProgram) {
    GLuint id = oid(A(0));
    if (id) family_free(id), glDeleteProgram(id);
    return JS_UNDEFINED;
}
FN(w_deleteShader) { GLuint id = oid(A(0)); if (id) glDeleteShader(id); return JS_UNDEFINED; }

FN(w_isBuffer) { return JS_NewBool(ctx, glIsBuffer(oid(A(0)))); }
FN(w_isTexture) { return JS_NewBool(ctx, glIsTexture(oid(A(0)))); }
FN(w_isFramebuffer) { return JS_NewBool(ctx, glIsFramebuffer(oid(A(0)))); }
FN(w_isRenderbuffer) { return JS_NewBool(ctx, glIsRenderbuffer(oid(A(0)))); }
FN(w_isProgram) { return JS_NewBool(ctx, glIsProgram(oid(A(0)))); }
FN(w_isShader) { return JS_NewBool(ctx, glIsShader(oid(A(0)))); }

FN(w_bindBuffer) {
    int t = gi(ctx, A(0));
    glBindBuffer(t, oid(A(1)));
    track(ctx, t == GL_ARRAY_BUFFER ? &st.bound_array_buffer : &st.bound_element_buffer, A(1));
    return JS_UNDEFINED;
}
FN(w_bindTexture) {
    glBindTexture(gi(ctx, A(0)), oid(A(1)));
    if (st.active_unit >= 0 && st.active_unit < 32) track(ctx, &st.bound_texture[st.active_unit], A(1));
    tex_use(ctx, oid(A(1)));
    return JS_UNDEFINED;
}
FN(w_bindFramebuffer) {
    GLuint id = oid(A(1));
    glBindFramebuffer(gi(ctx, A(0)), id);
    st.default_fb = id == 0;
    st.fb = id;
    st.fb_scale = id && id < 65536 && fb_scaled[id] ? RT_SCALE : 1;
    track(ctx, &st.bound_framebuffer, A(1));
    return JS_UNDEFINED;
}
FN(w_bindRenderbuffer) {
    glBindRenderbuffer(gi(ctx, A(0)), oid(A(1)));
    track(ctx, &st.bound_renderbuffer, A(1));
    return JS_UNDEFINED;
}

// bufferData(target, size | data, usage)
// Vertex buffer updates. Phaser re-uploads its batch from offset 0 with bufferSubData each time
// it flushes, 10-20 times a frame, into one ~688 KB buffer. While the GPU may still read the old
// contents, vitaGL allocates a whole new copy of the buffer for every such update and frees the
// old ones a few frames later: tens of MB of GPU memory in flight, and when that runs out vitaGL
// sleeps a second before retrying. So an update from offset 0 is turned into bufferData with just
// the bytes given (the buffer keeps its declared size here, and goes back to it if it is ever
// updated elsewhere than at offset 0).
static GLuint bound_buffer(int target) {
    return oid(target == GL_ARRAY_BUFFER ? st.bound_array_buffer : st.bound_element_buffer);
}

FN(w_bufferData) {
    int target = gi(ctx, A(0)), usage = gi(ctx, A(2));
    size_t len = 0;
    void *p = NULL;
    if (JS_IsNumber(A(1))) len = (size_t)gi(ctx, A(1));
    else p = buffer_data(ctx, A(1), &len);
    glBufferData(target, len, p, usage);
    GLuint id = bound_buffer(target);
    buf_info *b = buffer_info(id, true);
    if (b) b->size = (uint32_t)len, b->usage = (uint16_t)usage, b->orphaned = false;
    return JS_UNDEFINED;
}

FN(w_bufferSubData) {
    int target = gi(ctx, A(0)), offset = gi(ctx, A(1));
    size_t len = 0;
    void *p = buffer_data(ctx, A(2), &len);
    if (p) gl_buffer_sub_data(target, offset, p, len);
    return JS_UNDEFINED;
}

// gl.bufferSubData(target, offset, bytes) without a JS view (WebGLPipeline.flush in fastpath.c).
void gl_buffer_sub_data(int target, int offset, const void *p, size_t len) {
    buf_info *b = buffer_info(bound_buffer(target), false);
    if (b && b->size) {
        if (offset == 0 && !b->partial && len <= b->size) {
            glBufferData(target, len, p, b->usage);
            b->orphaned = true;
            return;
        }
        if (offset != 0) b->partial = true;
        if (b->orphaned) { // back to the declared size before a partial update
            glBufferData(target, b->size, NULL, b->usage);
            b->orphaned = false;
        }
    }
    glBufferSubData(target, offset, len, p);
}

FN(w_checkFramebufferStatus) { return JS_NewInt32(ctx, glCheckFramebufferStatus(gi(ctx, A(0)))); }
FN(w_framebufferTexture2D) {
    GLuint tex = oid(A(3));
    if (st.fb && st.fb < 65536 && gi(ctx, A(1)) == GL_COLOR_ATTACHMENT0) {
        fb_scaled[st.fb] = tex < 65536 ? tex_scaled[tex] : 0;
        st.fb_scale = fb_scaled[st.fb] ? RT_SCALE : 1;
    }
    glFramebufferTexture2D(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), tex, gi(ctx, A(4)));
    return JS_UNDEFINED;
}
FN(w_framebufferRenderbuffer) {
    int att = gi(ctx, A(1));
    GLuint rb = oid(A(3));
    if (att == 0x821A) { // DEPTH_STENCIL_ATTACHMENT (WebGL only): attach to both
        glFramebufferRenderbuffer(gi(ctx, A(0)), GL_DEPTH_ATTACHMENT, gi(ctx, A(2)), rb);
        glFramebufferRenderbuffer(gi(ctx, A(0)), GL_STENCIL_ATTACHMENT, gi(ctx, A(2)), rb);
    } else {
        glFramebufferRenderbuffer(gi(ctx, A(0)), att, gi(ctx, A(2)), rb);
    }
    return JS_UNDEFINED;
}
FN(w_renderbufferStorage) {
    int fmt = gi(ctx, A(1));
    if (fmt == 0x84F9) fmt = GL_STENCIL_INDEX8; // DEPTH_STENCIL: stencil is what Phaser needs
    glRenderbufferStorage(gi(ctx, A(0)), fmt, gi(ctx, A(2)), gi(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_generateMipmap) { glGenerateMipmap(gi(ctx, A(0))); return JS_UNDEFINED; }

// ---------------------------------------------------------------- texture memory accounting

// Bytes of level-0 texture storage per GL texture id (RGBA8 = 4 bytes per pixel).
static uint32_t tex_bytes[65536];
static size_t tex_total, tex_peak;
static int tex_count;
static GLuint bound_tex_id(void) {
    return st.active_unit >= 0 && st.active_unit < 32 ? oid(st.bound_texture[st.active_unit]) : 0;
}
static void tex_account(GLuint id, size_t bytes) {
    if (!id || id >= 65536) return;
    if (getenv("PKR_TEX_LOG") && bytes >= 512 * 1024) host_log("[tex] %u: %lu KB", id, (unsigned long)(bytes / 1024));
    if (!tex_bytes[id] && bytes) tex_count++;
    if (tex_bytes[id] && !bytes) tex_count--;
    tex_total = tex_total - tex_bytes[id] + bytes;
    tex_bytes[id] = (uint32_t)bytes;
    if (tex_total > tex_peak) tex_peak = tex_total;
}

// ---------------------------------------------------------------- texture eviction
// Phaser never frees a texture it has loaded, and PokeRogue loads a lot of them: on the Vita the
// GPU memory pools fill up (then vitaGL stalls for a second per failed allocation, and uploads
// fail). Textures uploaded from images keep a reference to their decoded-image object, whose PNG
// data stays in memory; when texture memory is over budget, the ones not drawn for a while are
// shrunk to 1x1 and re-uploaded from the PNG the next time they are bound or drawn with.
#define TEX_MAX 16384 // vitaGL's texture name limit
#define TEX_BUDGET (64u * 1024 * 1024)
#define TEX_IDLE_FRAMES 600 // about 20 s at 30 fps
#define TEX_PRESSURE_IDLE_FRAMES 90 // when memory is short (see memory_pressure)
typedef struct {
    JSValue src;         // Bitmap the texture was uploaded from, or JS_UNDEFINED (not evictable)
    uint32_t last_used;  // frame number
    uint16_t w, h;
    bool has_src, flip, premul, evicted;
    uint8_t restores; // times it came back: each doubles the idle time before it can go again
    uint32_t evicted_at; // frame number
} tex_rec;
static tex_rec texrec[TEX_MAX];
static uint32_t gl_frame;
static size_t evictable_bytes;
static int n_evictions, n_restores;
static double draw_ms_frame, draw_ms_total, draw_ms_max_frame; // see w_drawArrays
static int draws_total; // draw calls since the last gl_draw_stats

static void tex_forget(JSContext *ctx, GLuint id) {
    if (!id || id >= TEX_MAX) return;
    tex_rec *t = &texrec[id];
    if (t->has_src) {
        if (!t->evicted) evictable_bytes -= (size_t)t->w * t->h * 4;
        JS_FreeValue(ctx, t->src);
    }
    memset(t, 0, sizeof *t);
    t->src = JS_UNDEFINED;
}

static void tex_remember(JSContext *ctx, GLuint id, JSValueConst src, int w, int h) {
    tex_forget(ctx, id);
    if (!id || id >= TEX_MAX) return;
    JSValue b = bitmap_ref(ctx, src);
    if (JS_IsUndefined(b)) return;
    tex_rec *t = &texrec[id];
    t->src = b;
    t->has_src = true;
    t->w = (uint16_t)w, t->h = (uint16_t)h;
    t->flip = st.flip_y, t->premul = st.premultiply;
    t->last_used = gl_frame;
    evictable_bytes += (size_t)w * h * 4;
}

static const uint8_t *unpack(const uint8_t *src, int w, int h, bool owned, bool *fr);

// Re-uploads an evicted texture; it must be bound to the active unit.
static void tex_restore(JSContext *ctx, GLuint id) {
    tex_rec *t = &texrec[id];
    double t0 = host_now_ms();
    int w, h;
    bool owned, fr;
    const uint8_t *px = pixels_from_js(ctx, t->src, &w, &h, &owned);
    if (!px) return; // out of memory: stays evicted, tried again next time
    bool sf = st.flip_y, sp = st.premultiply;
    st.flip_y = t->flip, st.premultiply = t->premul;
    const uint8_t *up = unpack(px, w, h, owned, &fr);
    st.flip_y = sf, st.premultiply = sp;
    gl_pixel_store(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
    tex_account(id, (size_t)w * h * 4);
    if (fr) free((void *)up);
    if (owned && up != px) free((void *)px);
    t->evicted = false;
    evictable_bytes += (size_t)t->w * t->h * 4;
    if (t->restores < 4) t->restores++;
    n_restores++;
    // The PNG is decoded again, in the middle of the frame that needs it: a stutter if it is large.
    double ms = host_now_ms() - t0;
    if (ms >= 30)
        host_log("[gl] texture %u (%dx%d) re-uploaded in %.0f ms, %u frames after its eviction (back %d times)", id, w, h,
                 ms, gl_frame - t->evicted_at, t->restores);
}

static void tex_use(JSContext *ctx, GLuint id) {
    if (!id || id >= TEX_MAX) return;
    texrec[id].last_used = gl_frame;
    if (texrec[id].has_src && texrec[id].evicted) tex_restore(ctx, id);
}

// Before a draw: every texture bound to a unit counts as used (Phaser skips bindTexture for a
// texture that is still bound), and evicted ones come back.
static void textures_for_draw(JSContext *ctx) {
    for (int u = 0; u < 16; u++) {
        GLuint id = oid(st.bound_texture[u]);
        if (!id || id >= TEX_MAX) continue;
        texrec[id].last_used = gl_frame;
        if (texrec[id].has_src && texrec[id].evicted) {
            glActiveTexture(GL_TEXTURE0 + u);
            tex_restore(ctx, id);
            glActiveTexture(GL_TEXTURE0 + st.active_unit);
        }
    }
}

// Once per frame: over budget -> evict the textures idle longest (oldest first, a few per frame).
static bool memory_pressure(void) {
    // Well over budget, or vitaGL's GPU pools nearly full (it then puts textures in the main heap,
    // which the game needs): evict by idle time alone, however often a texture came back.
    if (tex_total > TEX_BUDGET + TEX_BUDGET / 2) return true;
#ifdef __vita__
    if (vglMemFree(VGL_MEM_VRAM) + vglMemFree(VGL_MEM_RAM) < 16u * 1024 * 1024) return true;
#endif
    return false;
}

static void textures_evict(void) {
    static uint32_t next_scan;
    gl_frame++;
    draw_ms_total += draw_ms_frame;
    if (draw_ms_frame > draw_ms_max_frame) draw_ms_max_frame = draw_ms_frame;
    draw_ms_frame = 0;
    if (tex_total <= TEX_BUDGET || evictable_bytes == 0 || gl_frame < next_scan) return;
    bool pressure = memory_pressure();
    for (int round = 0; round < 8 && tex_total > TEX_BUDGET; round++) {
        GLuint victim = 0;
        uint32_t oldest = gl_frame;
        for (GLuint id = 1; id < TEX_MAX; id++) {
            tex_rec *t = &texrec[id];
            if (!t->has_src || t->evicted) continue;
            // Textures used every few waves (the shop, per-wave UI) would otherwise be evicted and
            // decoded again each time: 20-30 at once froze the game for half a second. Under
            // pressure too, though less (0.66: battle menus used every turn were evicted between
            // turns at ~12 fps, and each came back with a stutter when a button opened it).
            uint32_t idle = pressure ? (uint32_t)TEX_PRESSURE_IDLE_FRAMES << (t->restores < 2 ? t->restores : 2)
                                     : (uint32_t)TEX_IDLE_FRAMES << t->restores;
            if (gl_frame - t->last_used < idle) continue;
            if (t->last_used < oldest) oldest = t->last_used, victim = id;
        }
        if (!victim) {
            // Over budget, but everything was drawn recently: scanning all 16,384 slots every frame
            // found nothing; look again in a while.
            next_scan = gl_frame + 30;
            return;
        }
        static const uint8_t clear[4] = {0, 0, 0, 0};
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, victim);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear);
        tex_account(victim, 4);
        texrec[victim].evicted = true;
        texrec[victim].evicted_at = gl_frame;
        evictable_bytes -= (size_t)texrec[victim].w * texrec[victim].h * 4;
        n_evictions++;
        glBindTexture(GL_TEXTURE_2D, oid(st.bound_texture[0]));
        glActiveTexture(GL_TEXTURE0 + st.active_unit);
    }
}
// textureDump(minKB) -> "id:KB ..." for live textures of at least minKB, largest first (diagnostics)
static JSValue n_texture_dump(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    int32_t min_kb = 512;
    if (argc > 0) JS_ToInt32(ctx, &min_kb, argv[0]);
    static char out[16384];
    size_t o = 0;
    int hist[8] = {0}; // <64K, <256K, <1M, <4M, <8M, >=8M
    double sum[8] = {0};
    for (uint32_t id = 1; id < 65536; id++) {
        uint32_t b = tex_bytes[id];
        if (!b) continue;
        int k = b < 65536 ? 0 : b < 262144 ? 1 : b < 1048576 ? 2 : b < 4194304 ? 3 : b < 8388608 ? 4 : 5;
        hist[k]++;
        sum[k] += b / 1048576.0;
        if (b >= (uint32_t)min_kb * 1024 && o + 32 < sizeof out) o += snprintf(out + o, sizeof out - o, "%u:%uKB ", id, b / 1024);
    }
    char summary[256];
    snprintf(summary, sizeof summary, "by size (count/MB): <64K %d/%.0f, <256K %d/%.0f, <1M %d/%.0f, <4M %d/%.0f, <8M %d/%.0f, >=8M %d/%.0f | ",
             hist[0], sum[0], hist[1], sum[1], hist[2], sum[2], hist[3], sum[3], hist[4], sum[4], hist[5], sum[5]);
    static char all[16640];
    snprintf(all, sizeof all, "%s%s", summary, out);
    return JS_NewString(ctx, all);
}

void gl_texture_memory(size_t *bytes, size_t *peak, int *count) {
    *bytes = tex_total;
    *peak = tex_peak;
    *count = tex_count;
}

#ifdef __vita__
#include <psp2/kernel/sysmem.h>

// Where the textures' pixels are: vitaGL puts a texture in the first of its pools with room (video
// RAM, then main RAM, contiguous RAM, the dialogs' budget) or else in the heap, and the GPU reads
// main RAM more slowly than video RAM. Each texture is looked up by binding it on the active unit
// (and binding back), so this is for the occasional [mem] line, not every frame.
void gl_texture_pools(size_t bytes[GL_POOLS], size_t largest_out[3]) {
    memset(bytes, 0, GL_POOLS * sizeof *bytes);
    memset(largest_out, 0, 3 * sizeof *largest_out);
    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    for (GLuint id = 1; id < 65536; id++) {
        if (!tex_bytes[id]) continue;
        glBindTexture(GL_TEXTURE_2D, id);
        void *p = vglGetTexDataPointer(GL_TEXTURE_2D);
        SceKernelMemBlockInfo info = {.size = sizeof info};
        int pool = GL_POOL_HEAP;
        if (p && sceKernelGetMemBlockInfoByAddr(p, &info) >= 0) {
            switch (info.type) {
            case SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW: pool = GL_POOL_VIDEO; break;
            case SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE: pool = GL_POOL_RAM; break; // vitaGL's uncached pool
            case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW: pool = GL_POOL_PHYCONT; break;
            case SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_CDIALOG_NC_RW: pool = GL_POOL_DIALOG; break;
            default: pool = GL_POOL_HEAP; break; // the heap (cached USER_RW)
            }
        }
        bytes[pool] += tex_bytes[id];
        if (pool == GL_POOL_VIDEO) continue;
        size_t b = tex_bytes[id]; // the three biggest outside video RAM
        for (int i = 0; i < 3; i++) {
            if (b > largest_out[i]) {
                size_t t = largest_out[i];
                largest_out[i] = b, b = t;
            }
        }
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
}
#endif

// ---------------------------------------------------------------- shaders / programs

// Shader sources and program->shader attachments, kept so a failed link can be reported with the
// compiler's messages and the full sources saved to <data dir>/shader-fail-*.txt.
#define MAX_GL_IDS 4096
static char *shader_src[MAX_GL_IDS];
static GLuint prog_shaders[MAX_GL_IDS][2];
static int link_failures;

static void report_link_failure(GLuint prog) {
    link_failures++;
    host_log("[gl] program %u failed to link (shader compile error)", prog);
    for (int i = 0; i < 2; i++) {
        GLuint sh = prog < MAX_GL_IDS ? prog_shaders[prog][i] : 0;
        if (!sh) continue;
        char log[2048] = "";
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        GLint type = 0;
        glGetShaderiv(sh, GL_SHADER_TYPE, &type);
        const char *kind = type == GL_VERTEX_SHADER ? "vertex" : "fragment";
        host_log("[gl]   %s shader %u: %s", kind, sh, log[0] ? log : "(no compiler message)");
        if (link_failures <= 8 && sh < MAX_GL_IDS && shader_src[sh]) {
            char path[512];
            snprintf(path, sizeof path, "%s/shader-fail-%d-%s.txt", g_cfg.data_dir, link_failures, kind);
            FILE *f = fopen(path, "w");
            if (f) {
                fprintf(f, "%s\n\n/* compiler messages:\n%s\n*/\n", shader_src[sh], log);
                fclose(f);
                host_log("[gl]   source saved to %s", path);
            }
        }
    }
}

FN(w_shaderSource) {
    const char *src = JS_ToCString(ctx, A(1));
    GLuint id = oid(A(0));
    if (src) {
        glShaderSource(id, 1, &src, NULL);
        if (id < MAX_GL_IDS) {
            free(shader_src[id]);
            shader_src[id] = strdup(src);
        }
    }
    JS_FreeCString(ctx, src);
    return JS_UNDEFINED;
}
FN(w_compileShader) { glCompileShader(oid(A(0))); return JS_UNDEFINED; }
FN(w_attachShader) {
    GLuint prog = oid(A(0)), sh = oid(A(1));
    glAttachShader(prog, sh);
    if (prog < MAX_GL_IDS) prog_shaders[prog][prog_shaders[prog][0] ? 1 : 0] = sh;
    return JS_UNDEFINED;
}
FN(w_detachShader) { glDetachShader(oid(A(0)), oid(A(1))); return JS_UNDEFINED; }
// ---------------------------------------------------------------- shader variants
// The Vita's shader compiler runs all of a fragment shader's code for every pixel, the effects a
// setting switches off included (0.64's benchmark: on PokeRogue's field shader, terrain colouring
// that was off cost 6.6 of 19 ms per full screen; on its sprite shader, tera and the palette loops
// 3-4 ms each). js/webgl.js wraps such effects in /*pkr+NAME*/ ... /*pkr-NAME*/ comments. For a
// program with them (a "family"), a draw picks the effects the current uniform values switch off,
// with the same tests as the shader's own conditions, and draws with a copy of the program built
// without that code (once, on first use). The game only ever sees the original program: uniform
// writes to it are recorded, and a copy gets the changed values just before it draws.
enum { VF_TERRAIN, VF_DAYNIGHT, VF_TERA, VF_VARIANTS, VF_FUSION, VF_SHADOW, VF_COUNT };
static const char *const vf_names[VF_COUNT] = {"terrain", "daynight", "tera", "variants", "fusion", "shadow"};

typedef struct {
    char name[64];
    GLenum type;
    GLint size, comps, loc;
    bool is_int;
    uint32_t *data; // size * comps 32-bit values, as last written
    unsigned version;
} vuniform;
typedef struct {
    GLuint prog, fs;
    bool failed;
    GLint *locs;       // per uniform, in this copy
    unsigned *synced;  // per uniform: the version the copy has
} vcopy;
typedef struct {
    GLuint vs, fs;
    unsigned present; // VF_* bits marked in the source
    vuniform *u;
    int nu;
    int idx[VF_COUNT][2]; // the uniforms each feature's test reads
    vcopy copies[1 << VF_COUNT]; // by the VF_* bits left out
} vfamily;
static vfamily *families[MAX_GL_IDS];
static unsigned var_version;
static int var_built, var_draws;

static void family_free(GLuint prog) {
    vfamily *f = prog < MAX_GL_IDS ? families[prog] : NULL;
    if (!f) return;
    for (int m = 0; m < (1 << VF_COUNT); m++) {
        vcopy *c = &f->copies[m];
        if (c->prog) glDeleteProgram(c->prog);
        if (c->fs) glDeleteShader(c->fs);
        free(c->locs), free(c->synced);
    }
    for (int i = 0; i < f->nu; i++) free(f->u[i].data);
    free(f->u);
    free(f);
    families[prog] = NULL;
}

static int type_comps(GLenum t, bool *is_int) {
    *is_int = false;
    switch (t) {
    case GL_FLOAT: return 1;
    case GL_FLOAT_VEC2: return 2;
    case GL_FLOAT_VEC3: return 3;
    case GL_FLOAT_VEC4: return 4;
    case GL_FLOAT_MAT2: return 4;
    case GL_FLOAT_MAT3: return 9;
    case GL_FLOAT_MAT4: return 16;
    }
    *is_int = true;
    switch (t) {
    case GL_INT_VEC2: case GL_BOOL_VEC2: return 2;
    case GL_INT_VEC3: case GL_BOOL_VEC3: return 3;
    case GL_INT_VEC4: case GL_BOOL_VEC4: return 4;
    default: return 1; // int, bool, samplers
    }
}

static int family_uniform(vfamily *f, const char *name) {
    for (int i = 0; i < f->nu; i++)
        if (!strcmp(f->u[i].name, name)) return i;
    return -1;
}

static void precompile_queue(GLuint prog, unsigned present);

// After a successful link: a family if the fragment shader has marked effects.
static void family_setup(GLuint prog) {
    family_free(prog);
    if (prog >= MAX_GL_IDS) return;
    GLuint vs = 0, fs = 0;
    for (int i = 0; i < 2; i++) {
        GLuint sh = prog_shaders[prog][i];
        GLint type = 0;
        if (sh) glGetShaderiv(sh, GL_SHADER_TYPE, &type);
        if (type == GL_VERTEX_SHADER) vs = sh;
        else if (type == GL_FRAGMENT_SHADER) fs = sh;
    }
    const char *src = fs && fs < MAX_GL_IDS ? shader_src[fs] : NULL;
    if (!vs || !src) return;
    unsigned present = 0;
    for (int b = 0; b < VF_COUNT; b++) {
        char mark[32];
        snprintf(mark, sizeof mark, "/*pkr+%s*/", vf_names[b]);
        if (strstr(src, mark)) present |= 1u << b;
    }
    if (!present) return;
    vfamily *f = calloc(1, sizeof *f);
    f->vs = vs, f->fs = fs, f->present = present;
    GLint n = 0;
    glGetProgramiv(prog, GL_ACTIVE_UNIFORMS, &n);
    f->u = calloc(n > 0 ? n : 1, sizeof *f->u);
    for (GLint i = 0; i < n; i++) {
        vuniform *u = &f->u[f->nu];
        GLsizei len = 0;
        glGetActiveUniform(prog, i, sizeof u->name, &len, &u->size, &u->type, u->name);
        u->loc = glGetUniformLocation(prog, u->name);
        char *br = strchr(u->name, '[');
        if (br) *br = 0; // "name[0]" of an array
        if (u->loc == -1) u->loc = glGetUniformLocation(prog, u->name);
        if (u->loc == -1 || u->size < 1) continue;
        u->comps = type_comps(u->type, &u->is_int);
        u->data = calloc((size_t)u->size * u->comps, 4);
        f->nu++;
    }
    static const char *const reads[VF_COUNT][2] = {
        {"terrainColorRatio", "terrainColor"}, {"ignoreTimeTint", NULL}, {"teraColor", NULL},
        {"baseVariantColors", NULL}, {"spriteColors", NULL}, {"hasShadow", NULL}};
    for (int b = 0; b < VF_COUNT; b++) {
        for (int k = 0; k < 2; k++) f->idx[b][k] = reads[b][k] ? family_uniform(f, reads[b][k]) : -2;
        if (f->idx[b][0] == -1 || f->idx[b][1] == -1) f->present &= ~(1u << b); // can't read its setting
    }
    if (!f->present) {
        families[prog] = f; // nothing to leave out: free it the usual way
        family_free(prog);
        return;
    }
    families[prog] = f;
    precompile_queue(prog, f->present);
}

// A uniform write to the bound program, as the GL got it.
static void var_record(GLint loc, const void *data, int scalars) {
    GLuint prog = oid(st.bound_program);
    vfamily *f = prog < MAX_GL_IDS ? families[prog] : NULL;
    if (!f || loc == -1 || scalars <= 0) return;
    for (int i = 0; i < f->nu; i++) {
        vuniform *u = &f->u[i];
        if (u->loc != loc) continue;
        int max = u->size * u->comps;
        memcpy(u->data, data, (size_t)(scalars < max ? scalars : max) * 4);
        u->version = ++var_version;
        return;
    }
}

static float vf_float(vfamily *f, int idx, int k) {
    float v;
    memcpy(&v, &f->u[idx].data[k], 4);
    return f->u[idx].is_int ? (float)(int32_t)f->u[idx].data[k] : v;
}
static bool vf_bool(vfamily *f, int idx) {
    return f->u[idx].is_int ? f->u[idx].data[0] != 0 : vf_float(f, idx, 0) != 0.0f;
}

// The effects the current values switch off, with each effect's own test in the shader.
static unsigned family_off(vfamily *f) {
    unsigned off = 0;
    int (*x)[2] = f->idx;
    if (f->present & (1u << VF_TERRAIN)) { // terrainColorRatio > 0.0 && any(lessThan(vec3(0.0), terrainColor))
        bool on = vf_float(f, x[VF_TERRAIN][0], 0) > 0.0f &&
                  (0.0f < vf_float(f, x[VF_TERRAIN][1], 0) || 0.0f < vf_float(f, x[VF_TERRAIN][1], 1) ||
                   0.0f < vf_float(f, x[VF_TERRAIN][1], 2));
        if (!on) off |= 1u << VF_TERRAIN;
    }
    if ((f->present & (1u << VF_DAYNIGHT)) && vf_bool(f, x[VF_DAYNIGHT][0])) off |= 1u << VF_DAYNIGHT; // !ignoreTimeTint
    if (f->present & (1u << VF_TERA)) { // all(lessThan(vec3(0.0), teraColor))
        int t = x[VF_TERA][0];
        if (!(0.0f < vf_float(f, t, 0) && 0.0f < vf_float(f, t, 1) && 0.0f < vf_float(f, t, 2))) off |= 1u << VF_TERA;
    }
    // The palette loops stop at once when entry 0's alpha is 0.
    if ((f->present & (1u << VF_VARIANTS)) && vf_float(f, x[VF_VARIANTS][0], 3) == 0.0f) off |= 1u << VF_VARIANTS;
    if ((f->present & (1u << VF_FUSION)) && vf_float(f, x[VF_FUSION][0], 3) == 0.0f) off |= 1u << VF_FUSION;
    if ((f->present & (1u << VF_SHADOW)) && !vf_bool(f, x[VF_SHADOW][0])) off |= 1u << VF_SHADOW;
    return off;
}

static void copy_build(GLuint root, vfamily *f, unsigned off, vcopy *c) {
    double t0 = host_now_ms();
    char *src = strdup(shader_src[f->fs]);
    for (int b = 0; b < VF_COUNT; b++) {
        if (!(off & (1u << b))) continue;
        char open[32], close[32];
        snprintf(open, sizeof open, "/*pkr+%s*/", vf_names[b]);
        snprintf(close, sizeof close, "/*pkr-%s*/", vf_names[b]);
        char *a;
        while ((a = strstr(src, open))) {
            char *z = strstr(a, close);
            if (!z) break;
            z += strlen(close);
            memmove(a, z, strlen(z) + 1);
        }
    }
    c->failed = true;
    c->fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char *s = src;
    glShaderSource(c->fs, 1, &s, NULL);
    glCompileShader(c->fs);
    free(src);
    c->prog = glCreateProgram();
    glAttachShader(c->prog, f->vs);
    glAttachShader(c->prog, c->fs);
    GLint na = 0;
    glGetProgramiv(root, GL_ACTIVE_ATTRIBUTES, &na);
    for (GLint i = 0; i < na; i++) { // the attributes where the game set them up for the original
        char name[64];
        GLint size;
        GLenum type;
        glGetActiveAttrib(root, i, sizeof name, NULL, &size, &type, name);
        GLint l = glGetAttribLocation(root, name);
        if (l >= 0) glBindAttribLocation(c->prog, l, name);
    }
    glLinkProgram(c->prog);
    GLint ok = 0;
    glGetProgramiv(c->prog, GL_LINK_STATUS, &ok);
    char names[100] = "";
    for (int b = 0, o = 0; b < VF_COUNT; b++)
        if (off & (1u << b)) o += snprintf(names + o, sizeof names - o, "%s%s", o ? "/" : "", vf_names[b]);
    if (!ok) {
        host_log("[gl] shader variant without %s didn't link; the original draws instead", names);
        return;
    }
    c->failed = false;
    c->locs = malloc(f->nu * sizeof *c->locs);
    c->synced = calloc(f->nu, sizeof *c->synced);
    for (int i = 0; i < f->nu; i++) c->locs[i] = glGetUniformLocation(c->prog, f->u[i].name);
    var_built++;
    host_log("[gl] shader variant: program %u without %s (%.0f ms to build)", root, names, host_now_ms() - t0);
}

static void copy_sync(vfamily *f, vcopy *c) {
    for (int i = 0; i < f->nu; i++) {
        vuniform *u = &f->u[i];
        if (c->synced[i] == u->version) continue;
        c->synced[i] = u->version;
        GLint l = c->locs[i];
        if (l == -1) continue;
        const void *d = u->data;
        switch (u->type) {
        case GL_FLOAT_MAT2: glUniformMatrix2fv(l, u->size, GL_FALSE, d); continue;
        case GL_FLOAT_MAT3: glUniformMatrix3fv(l, u->size, GL_FALSE, d); continue;
        case GL_FLOAT_MAT4: glUniformMatrix4fv(l, u->size, GL_FALSE, d); continue;
        }
        if (u->is_int) {
            if (u->comps == 1) glUniform1iv(l, u->size, d);
            else if (u->comps == 2) glUniform2iv(l, u->size, d);
            else if (u->comps == 3) glUniform3iv(l, u->size, d);
            else glUniform4iv(l, u->size, d);
        } else {
            if (u->comps == 1) glUniform1fv(l, u->size, d);
            else if (u->comps == 2) glUniform2fv(l, u->size, d);
            else if (u->comps == 3) glUniform3fv(l, u->size, d);
            else glUniform4fv(l, u->size, d);
        }
    }
}

// Before a draw: binds the copy the current values allow and returns the original's id to bind
// back afterwards (var_end), or 0 when the original draws.
static GLuint var_begin(void) {
    GLuint prog = oid(st.bound_program);
    vfamily *f = prog < MAX_GL_IDS ? families[prog] : NULL;
    if (!f) return 0;
    unsigned off = family_off(f);
    if (!off) return 0;
    vcopy *c = &f->copies[off];
    if (!c->prog && !c->failed) copy_build(prog, f, off, c);
    if (c->failed) return 0;
    glUseProgram(c->prog);
    copy_sync(f, c);
    var_draws++;
    return prog;
}
static inline void var_end(GLuint prog) {
    if (prog) glUseProgram(prog);
}

// Precompiling. Building a copy when a draw first needs it stalls that frame: 0.8-1 s per copy on
// the console (0.65), until vitaGL's shader cache in <data>/shader-cache has it. So when a family is
// linked, the copies it is likely to need are queued and built ahead of use: during the game's
// long loading frames (main.c's busy_poll, which shows the progress on its loading screen), else
// between frames with a notice over the game. Likely: terrain, day/night and shadow each on or off,
// with at most one of tera, shiny variants and fusion palettes on (16 copies of the sprite shader).
#define PRE_MAX 256
static struct { GLuint prog; unsigned off; } pre_queue[PRE_MAX];
static int pre_n, pre_done, pre_built;
static double pre_ms;

static void precompile_queue(GLuint prog, unsigned present) {
    int n = pre_done; // a relinked program: drop what was queued for it
    for (int i = pre_done; i < pre_n; i++)
        if (pre_queue[i].prog != prog) pre_queue[n++] = pre_queue[i];
    pre_n = n;
    if (g_cfg.no_precompile) return;
    const unsigned common = present & (1u << VF_TERRAIN | 1u << VF_DAYNIGHT | 1u << VF_SHADOW);
    static const int rare[] = {-1, VF_TERA, VF_VARIANTS, VF_FUSION};
    for (int r = 0; r < 4; r++) {
        unsigned one = rare[r] < 0 ? 0 : 1u << rare[r];
        if (rare[r] >= 0 && !(present & one)) continue;
        for (unsigned s = common;; s = (s - 1) & common) { // every subset of `common` switched on
            unsigned off = present & ~(s | one);
            if (off && pre_n < PRE_MAX) pre_queue[pre_n].prog = prog, pre_queue[pre_n++].off = off;
            if (!s) break;
        }
    }
}

// Builds queued copies for about budget_ms (at least one). Returns how many are still left.
int gl_precompile(double budget_ms) {
    if (pre_done == pre_n) return 0;
    double t0 = host_now_ms();
    while (pre_done < pre_n) {
        GLuint prog = pre_queue[pre_done].prog;
        unsigned off = pre_queue[pre_done++].off;
        vfamily *f = families[prog];
        if (!f || (off & ~f->present)) continue;
        vcopy *c = &f->copies[off];
        if (c->prog || c->failed) continue; // a draw needed it first
        copy_build(prog, f, off, c);
        pre_built++;
        if (host_now_ms() - t0 >= budget_ms) break;
    }
    pre_ms += host_now_ms() - t0;
    if (pre_done == pre_n)
        host_log("[gl] shader variants precompiled: %d in %.1f s", pre_built, pre_ms / 1000);
    return pre_n - pre_done;
}

// Queued copies left; *total: queued in all (for "N/M" on the screen).
int gl_precompile_left(int *total) {
    if (total) *total = pre_n;
    return pre_n - pre_done;
}

// gl.pkrShaderVariants() -> {built, draws}: copies built and draws they made (tests/shaders).
FN(w_pkrShaderVariants) {
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "built", JS_NewInt32(ctx, var_built));
    JS_SetPropertyStr(ctx, o, "draws", JS_NewInt32(ctx, var_draws));
    return o;
}

#ifdef PKR_DEBUG
// A program's name for the GPU profile: Phaser's shaders define SHADER_NAME; PokeRogue's own
// (src/pipelines/glsl in the game) are known by a uniform only they have.
static char prog_names[MAX_GL_IDS][40];
static void name_program(GLuint prog) {
    if (prog >= MAX_GL_IDS) return;
    snprintf(prog_names[prog], sizeof prog_names[prog], "program %u", prog);
    for (int i = 0; i < 2; i++) {
        GLuint sh = prog_shaders[prog][i];
        const char *src = sh && sh < MAX_GL_IDS ? shader_src[sh] : NULL;
        if (!src || !strstr(src, "gl_FragColor")) continue;
        const char *def = strstr(src, "#define SHADER_NAME ");
        if (strstr(src, "spriteColors")) snprintf(prog_names[prog], sizeof prog_names[prog], "PokeRogue sprite");
        else if (strstr(src, "terrainColor")) snprintf(prog_names[prog], sizeof prog_names[prog], "PokeRogue field");
        else if (def) sscanf(def + 20, "%39[A-Za-z0-9_]", prog_names[prog]);
    }
}
#endif

FN(w_linkProgram) {
    GLuint prog = oid(A(0));
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) report_link_failure(prog);
    else family_setup(prog);
#ifdef PKR_DEBUG
    name_program(prog);
#endif
    return JS_UNDEFINED;
}
FN(w_validateProgram) { glValidateProgram(oid(A(0))); return JS_UNDEFINED; }
FN(w_useProgram) {
    glUseProgram(oid(A(0)));
    track(ctx, &st.bound_program, A(0));
    return JS_UNDEFINED;
}
FN(w_bindAttribLocation) {
    const char *name = JS_ToCString(ctx, A(2));
    if (name) glBindAttribLocation(oid(A(0)), gi(ctx, A(1)), name);
    JS_FreeCString(ctx, name);
    return JS_UNDEFINED;
}

FN(w_getShaderParameter) {
    GLint v = 0;
    int p = gi(ctx, A(1));
    glGetShaderiv(oid(A(0)), p, &v);
    if (p == GL_COMPILE_STATUS || p == GL_DELETE_STATUS) return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}
FN(w_getProgramParameter) {
    GLint v = 0;
    int p = gi(ctx, A(1));
    glGetProgramiv(oid(A(0)), p, &v);
    if (p == GL_LINK_STATUS || p == GL_DELETE_STATUS || p == GL_VALIDATE_STATUS) return JS_NewBool(ctx, v);
    return JS_NewInt32(ctx, v);
}
FN(w_getShaderInfoLog) {
    char log[4096] = "";
    glGetShaderInfoLog(oid(A(0)), sizeof log, NULL, log);
    return JS_NewString(ctx, log);
}
FN(w_getProgramInfoLog) {
    char log[4096] = "";
    glGetProgramInfoLog(oid(A(0)), sizeof log, NULL, log);
    return JS_NewString(ctx, log);
}
// The source as given to the GL (after js/webgl.js's changes), from our own copy: vitaGL's
// glGetShaderSource returns nothing.
FN(w_getShaderSource) {
    GLuint id = oid(A(0));
    if (id < MAX_GL_IDS && shader_src[id]) return JS_NewString(ctx, shader_src[id]);
    char src[65536] = "";
    glGetShaderSource(id, sizeof src, NULL, src);
    return JS_NewString(ctx, src);
}
FN(w_getShaderPrecisionFormat) {
    GLint range[2] = {127, 127}, prec = 23;
    glGetShaderPrecisionFormat(gi(ctx, A(0)), gi(ctx, A(1)), range, &prec);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "rangeMin", JS_NewInt32(ctx, range[0]));
    JS_SetPropertyStr(ctx, o, "rangeMax", JS_NewInt32(ctx, range[1]));
    JS_SetPropertyStr(ctx, o, "precision", JS_NewInt32(ctx, prec));
    return o;
}

static JSValue active_info(JSContext *ctx, bool uniform, GLuint prog, int idx) {
    char name[256];
    GLint size = 0;
    GLenum type = 0;
    GLsizei len = 0;
    if (uniform) glGetActiveUniform(prog, idx, sizeof name, &len, &size, &type, name);
    else glGetActiveAttrib(prog, idx, sizeof name, &len, &size, &type, name);
    if (!len) return JS_NULL;
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "name", JS_NewStringLen(ctx, name, len));
    JS_SetPropertyStr(ctx, o, "size", JS_NewInt32(ctx, size));
    JS_SetPropertyStr(ctx, o, "type", JS_NewInt32(ctx, type));
    return o;
}
FN(w_getActiveUniform) { return active_info(ctx, true, oid(A(0)), gi(ctx, A(1))); }
FN(w_getActiveAttrib) { return active_info(ctx, false, oid(A(0)), gi(ctx, A(1))); }

FN(w_getAttribLocation) {
    const char *name = JS_ToCString(ctx, A(1));
    GLint l = name ? glGetAttribLocation(oid(A(0)), name) : -1;
    JS_FreeCString(ctx, name);
    return JS_NewInt32(ctx, l);
}
FN(w_getUniformLocation) {
    const char *name = JS_ToCString(ctx, A(1));
    GLint l = name ? glGetUniformLocation(oid(A(0)), name) : -1;
    // Only -1 means "no such uniform": vitaGL's locations are negated pointers, often negative.
    if (l == -1) {
        JS_FreeCString(ctx, name);
        return JS_NULL;
    }
    JSValue v = JS_NewObjectClass(ctx, uloc_class);
    uloc *u = malloc(sizeof *u);
    u->loc = l;
    u->projection = !strcmp(name, "uProjectionMatrix");
    JS_SetOpaque(v, u);
    JS_FreeCString(ctx, name);
    return v;
}

static inline GLint lid(JSValueConst v) {
    uloc *u = JS_GetOpaque(v, uloc_class);
    return u ? u->loc : -1;
}

// ---------------------------------------------------------------- uniforms / attributes

// Each uniform write also goes to var_record (shader variants), as the values the GL got.
static JSValue uniform_f(JSContext *ctx, int argc, JSValueConst *argv, int n) {
    GLfloat v[4];
    for (int i = 0; i < n; i++) v[i] = gf(ctx, A(i + 1));
    GLint l = lid(A(0));
    if (n == 1) glUniform1f(l, v[0]);
    else if (n == 2) glUniform2f(l, v[0], v[1]);
    else if (n == 3) glUniform3f(l, v[0], v[1], v[2]);
    else glUniform4f(l, v[0], v[1], v[2], v[3]);
    var_record(l, v, n);
    return JS_UNDEFINED;
}
static JSValue uniform_i(JSContext *ctx, int argc, JSValueConst *argv, int n) {
    GLint v[4];
    for (int i = 0; i < n; i++) v[i] = gi(ctx, A(i + 1));
    GLint l = lid(A(0));
    if (n == 1) glUniform1i(l, v[0]);
    else if (n == 2) glUniform2i(l, v[0], v[1]);
    else if (n == 3) glUniform3i(l, v[0], v[1], v[2]);
    else glUniform4i(l, v[0], v[1], v[2], v[3]);
    var_record(l, v, n);
    return JS_UNDEFINED;
}
FN(w_uniform1f) { return uniform_f(ctx, argc, argv, 1); }
FN(w_uniform2f) { return uniform_f(ctx, argc, argv, 2); }
FN(w_uniform3f) { return uniform_f(ctx, argc, argv, 3); }
FN(w_uniform4f) { return uniform_f(ctx, argc, argv, 4); }
FN(w_uniform1i) { return uniform_i(ctx, argc, argv, 1); }
FN(w_uniform2i) { return uniform_i(ctx, argc, argv, 2); }
FN(w_uniform3i) { return uniform_i(ctx, argc, argv, 3); }
FN(w_uniform4i) { return uniform_i(ctx, argc, argv, 4); }

static JSValue uniform_fv(JSContext *ctx, JSValueConst *argv, int argc, int comps) {
    int n;
    GLfloat *tmp;
    const GLfloat *p = float_data(ctx, A(1), &n, &tmp);
    GLint l = lid(A(0));
    if (p && n >= comps) {
        switch (comps) {
        case 1: glUniform1fv(l, n, p); break;
        case 2: glUniform2fv(l, n / 2, p); break;
        case 3: glUniform3fv(l, n / 3, p); break;
        case 4: glUniform4fv(l, n / 4, p); break;
        }
        var_record(l, p, n / comps * comps);
    }
    free(tmp);
    return JS_UNDEFINED;
}
FN(w_uniform1fv) { return uniform_fv(ctx, argv, argc, 1); }
FN(w_uniform2fv) { return uniform_fv(ctx, argv, argc, 2); }
FN(w_uniform3fv) { return uniform_fv(ctx, argv, argc, 3); }
FN(w_uniform4fv) { return uniform_fv(ctx, argv, argc, 4); }

static JSValue uniform_iv(JSContext *ctx, JSValueConst *argv, int argc, int comps) {
    int n;
    GLint *tmp;
    const GLint *p = int_data(ctx, A(1), &n, &tmp);
    GLint l = lid(A(0));
    if (p && n >= comps) {
        switch (comps) {
        case 1: glUniform1iv(l, n, p); break;
        case 2: glUniform2iv(l, n / 2, p); break;
        case 3: glUniform3iv(l, n / 3, p); break;
        case 4: glUniform4iv(l, n / 4, p); break;
        }
        var_record(l, p, n / comps * comps);
    }
    free(tmp);
    return JS_UNDEFINED;
}
FN(w_uniform1iv) { return uniform_iv(ctx, argv, argc, 1); }
FN(w_uniform2iv) { return uniform_iv(ctx, argv, argc, 2); }
FN(w_uniform3iv) { return uniform_iv(ctx, argv, argc, 3); }
FN(w_uniform4iv) { return uniform_iv(ctx, argv, argc, 4); }

static JSValue uniform_matrix(JSContext *ctx, JSValueConst *argv, int argc, int dim) {
    int n;
    GLfloat *tmp;
    const GLfloat *p = float_data(ctx, A(2), &n, &tmp);
    GLint l = lid(A(0));
    int count = n / (dim * dim);
    // The game is drawn at half its resolution, so sprite edges often fall exactly on screen
    // pixel centres; the Vita's GPU then samples the texel just outside a sprite's atlas frame
    // (1-pixel stripes of the neighbouring frame along top/left edges). Moving the whole picture
    // by 1/16 pixel keeps pixel centres off those edges.
    uloc *u = JS_GetOpaque(A(0), uloc_class);
    GLfloat biased[16];
    if (p && dim == 4 && count == 1 && u && u->projection) {
        memcpy(biased, p, sizeof biased);
        biased[12] += 2.0f / 16 / 960;
        biased[13] -= 2.0f / 16 / 544;
        p = biased;
    }
    if (p && count > 0) {
        if (dim == 2) glUniformMatrix2fv(l, count, GL_FALSE, p);
        else if (dim == 3) glUniformMatrix3fv(l, count, GL_FALSE, p);
        else glUniformMatrix4fv(l, count, GL_FALSE, p);
        var_record(l, p, count * dim * dim);
    }
    free(tmp);
    return JS_UNDEFINED;
}
FN(w_uniformMatrix2fv) { return uniform_matrix(ctx, argv, argc, 2); }
FN(w_uniformMatrix3fv) { return uniform_matrix(ctx, argv, argc, 3); }
FN(w_uniformMatrix4fv) { return uniform_matrix(ctx, argv, argc, 4); }

FN(w_vertexAttribPointer) {
    int i = gi(ctx, A(0));
    GLint size = gi(ctx, A(1));
    GLenum type = gi(ctx, A(2));
    GLboolean norm = JS_ToBool(ctx, A(3));
    GLsizei stride = gi(ctx, A(4));
    intptr_t off = gi(ctx, A(5));
    glVertexAttribPointer(i, size, type, norm, stride, (const void *)off);
    if (i >= 0 && i < 2) {
        st.attrib[i].set = true;
        st.attrib[i].size = size, st.attrib[i].type = type, st.attrib[i].norm = norm;
        st.attrib[i].stride = stride, st.attrib[i].off = off, st.attrib[i].buf = oid(st.bound_array_buffer);
    }
    return JS_UNDEFINED;
}
FN(w_enableVertexAttribArray) {
    int i = gi(ctx, A(0));
    if (i >= 0 && i < 2) st.attrib[i].on = true;
    glEnableVertexAttribArray(i);
    return JS_UNDEFINED;
}
FN(w_disableVertexAttribArray) {
    int i = gi(ctx, A(0));
    if (i >= 0 && i < 2) st.attrib[i].on = false;
    glDisableVertexAttribArray(i);
    return JS_UNDEFINED;
}
FN(w_vertexAttrib1f) { glVertexAttrib1f(gi(ctx, A(0)), gf(ctx, A(1))); return JS_UNDEFINED; }
FN(w_vertexAttrib2f) { glVertexAttrib2f(gi(ctx, A(0)), gf(ctx, A(1)), gf(ctx, A(2))); return JS_UNDEFINED; }
FN(w_vertexAttrib3f) {
    glVertexAttrib3f(gi(ctx, A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_vertexAttrib4f) {
    glVertexAttrib4f(gi(ctx, A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3)), gf(ctx, A(4)));
    return JS_UNDEFINED;
}

#ifdef PKR_DEBUG
// GPU time per shader program (config.txt gpuprofile=1, debug builds): every 10 s one frame waits
// for the GPU after each of its draw calls (glFinish), with the GPU idle when the frame starts, and
// the log gets the time per program. Each wait also ends the GPU's scene, so every draw pays that
// overhead too (about the smallest draw's time, logged): compare programs, not absolute totals.
#define PROF_MAX 24
static bool prof_on;
static double prof_next;
typedef struct { GLuint prog; double ms, max; int draws, verts; } prof_entry;
static prof_entry prof[PROF_MAX];
static int n_prof;
static double prof_min;

static void prof_draw(int verts, double t0) {
    glFinish();
    double ms = host_now_ms() - t0;
    GLuint prog = oid(st.bound_program);
    int i = 0;
    while (i < n_prof && prof[i].prog != prog) i++;
    if (i == n_prof) {
        if (n_prof == PROF_MAX) return;
        prof[n_prof++] = (prof_entry){.prog = prog};
    }
    prof[i].ms += ms, prof[i].draws++, prof[i].verts += verts;
    if (ms > prof[i].max) prof[i].max = ms;
    if (prof_min == 0 || ms < prof_min) prof_min = ms;
}

// After the frame's swap (gl_after_frame): logs a profiled frame, or starts one.
static void prof_after_frame(void) {
    if (prof_on && !n_prof) { // nothing drawn (loading screens): try again a little later
        prof_on = false;
        prof_next = host_now_ms() + 500;
    }
    if (prof_on) {
        prof_on = false;
        char line[2048];
        int o = 0;
        double total = 0;
        for (int i = 0; i < n_prof; i++) total += prof[i].ms;
        o += snprintf(line + o, sizeof line - o, "[gpu] per shader, one frame (%.1f ms; each draw also pays ~%.1f ms "
                      "of scene overhead, the smallest draw):", total, prof_min);
        for (int k = 0; k < n_prof && o < (int)sizeof line - 120; k++) {
            int best = k; // biggest first
            for (int i = k + 1; i < n_prof; i++) if (prof[i].ms > prof[best].ms) best = i;
            prof_entry t = prof[k];
            prof[k] = prof[best], prof[best] = t;
            const char *name = prof[k].prog < MAX_GL_IDS && prog_names[prof[k].prog][0] ? prog_names[prof[k].prog] : "?";
            o += snprintf(line + o, sizeof line - o, " | %s: %.1f ms in %d draws (max %.1f, %d vertices)", name,
                          prof[k].ms, prof[k].draws, prof[k].max, prof[k].verts);
        }
        host_log("%s", line);
        return;
    }
    if (!g_cfg.gpu_profile || host_now_ms() < prof_next) return;
    prof_next = host_now_ms() + 10000;
    n_prof = 0, prof_min = 0;
    glFinish(); // the GPU is idle when the profiled frame starts
    prof_on = true;
}
#endif

// Time spent inside draw calls (a driver that waits for the GPU there shows up as JS time).
FN(w_drawArrays) {
    textures_for_draw(ctx);
    double t0 = host_now_ms();
    GLuint root = var_begin();
    glDrawArrays(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)));
    var_end(root);
    draw_ms_frame += host_now_ms() - t0;
    draws_total++;
#ifdef PKR_DEBUG
    if (prof_on) prof_draw(gi(ctx, A(2)), t0);
#endif
    return JS_UNDEFINED;
}
FN(w_drawElements) {
    textures_for_draw(ctx);
    double t0 = host_now_ms();
    GLuint root = var_begin();
    glDrawElements(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), (const void *)(intptr_t)gi(ctx, A(3)));
    var_end(root);
    draw_ms_frame += host_now_ms() - t0;
    draws_total++;
#ifdef PKR_DEBUG
    if (prof_on) prof_draw(gi(ctx, A(1)), t0);
#endif
    return JS_UNDEFINED;
}

// For the [perf] line: ms spent in the game's draw calls (total and the worst frame) and textures
// re-uploaded after eviction since the last call.
void gl_draw_stats(double *total_ms, double *max_frame_ms, int *restores, int *draws) {
    static int last_restores;
    *total_ms = draw_ms_total, *max_frame_ms = draw_ms_max_frame, *restores = n_restores - last_restores;
    *draws = draws_total;
    last_restores = n_restores;
    draw_ms_total = draw_ms_max_frame = 0;
    draws_total = 0;
}

// ---------------------------------------------------------------- textures

static void premultiply(uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++, p += 4) {
        unsigned a = p[3];
        if (a == 255) continue;
        p[0] = p[0] * a / 255;
        p[1] = p[1] * a / 255;
        p[2] = p[2] * a / 255;
    }
}

static void flip_rows(uint8_t *p, int w, int h, int bpp) {
    size_t row = (size_t)w * bpp;
    uint8_t *tmp = malloc(row);
    for (int y = 0; y < h / 2; y++) {
        memcpy(tmp, p + y * row, row);
        memcpy(p + y * row, p + (h - 1 - y) * row, row);
        memcpy(p + (h - 1 - y) * row, tmp, row);
    }
    free(tmp);
}

// Applies UNPACK_FLIP_Y / UNPACK_PREMULTIPLY to RGBA8 pixels. Returns a buffer to upload; *fr is set
// when it must be freed.
static const uint8_t *unpack(const uint8_t *src, int w, int h, bool owned, bool *fr) {
    *fr = owned;
    if (!st.flip_y && !st.premultiply) return src;
    uint8_t *p = owned ? (uint8_t *)src : malloc((size_t)w * h * 4);
    if (!owned) memcpy(p, src, (size_t)w * h * 4);
    if (st.premultiply) premultiply(p, (size_t)w * h);
    if (st.flip_y) flip_rows(p, w, h, 4);
    *fr = true;
    return p;
}

// texImage2D(target, level, internalformat, format, type, source)
// texImage2D(target, level, internalformat, width, height, border, format, type, pixels)
static JSValue tex_image_2d(JSContext *ctx, int argc, JSValueConst *argv, int *ow, int *oh);

// Uploads are timed: with the GPU pools nearly full, vitaGL can stall on an allocation (log71: a
// 320 ms native call while a stat-change animation was created, cause not yet known).
FN(w_texImage2D) {
    double t0 = host_now_ms();
    int w = 0, h = 0;
    JSValue r = tex_image_2d(ctx, argc, argv, &w, &h);
    double ms = host_now_ms() - t0;
    if (ms >= 30)
        host_log("[gl] texImage2D %dx%d took %.0f ms (%s)", w, h, ms, argc == 6 ? "from an image or canvas" : "from data, or empty");
    return r;
}

static JSValue tex_image_2d(JSContext *ctx, int argc, JSValueConst *argv, int *ow, int *oh) {
    int target = gi(ctx, A(0)), level = gi(ctx, A(1)), ifmt = gi(ctx, A(2));
    if (argc == 6) {
        int fmt = gi(ctx, A(3)), type = gi(ctx, A(4)), w, h;
        bool owned, fr;
#ifdef __vita__
        // A canvas never drawn on is all transparent, and vitaGL zero-fills a texture made without
        // data: no pixels to allocate, clear, convert and copy (log75: Phaser's TileSprite uploads
        // its blank 936x1896 canvas for each stat-change animation, 200 ms each).
        if (canvas_is_blank(ctx, A(5), &w, &h)) {
            *ow = w, *oh = h;
            glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            if (level == 0) tex_account(bound_tex_id(), (size_t)w * h * 4), tex_forget(ctx, bound_tex_id());
            return JS_UNDEFINED;
        }
#endif
        const uint8_t *px = pixels_from_js(ctx, A(5), &w, &h, &owned);
        if (!px) {
            // No pixels (usually out of memory). Throwing here would abort the game's render loop
            // for good, so upload a transparent pixel instead.
            static int warned;
            if (warned++ < 5) host_log("[gl] texImage2D: no pixels from the source (out of memory?)");
            static const uint8_t clear[4] = {0, 0, 0, 0};
            glTexImage2D(target, level, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, clear);
            if (level == 0) tex_account(bound_tex_id(), 4), tex_forget(ctx, bound_tex_id());
            return JS_UNDEFINED;
        }
        *ow = w, *oh = h;
        const uint8_t *up = unpack(px, w, h, owned, &fr);
        gl_pixel_store(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
        if (level == 0) {
            tex_account(bound_tex_id(), (size_t)w * h * 4);
            if (target == GL_TEXTURE_2D) tex_remember(ctx, bound_tex_id(), A(5), w, h);
        }
        (void)ifmt, (void)fmt, (void)type;
        if (fr) free((void *)up);
        if (owned && up != px) free((void *)px);
        return JS_UNDEFINED;
    }
    int w = gi(ctx, A(3)), h = gi(ctx, A(4)), fmt = gi(ctx, A(6)), type = gi(ctx, A(7));
    size_t len = 0;
    const uint8_t *px = JS_IsNull(A(8)) || JS_IsUndefined(A(8)) ? NULL : buffer_data(ctx, A(8), &len);
    GLuint tid = bound_tex_id();
    if (tid && tid < 65536) tex_scaled[tid] = 0;
    if (level == 0) tex_forget(ctx, tid);
    if (!px && level == 0 && tid && tid < 65536 && w == st.virt_w && h == st.virt_h && w >= 1280) {
        w = (int)(w * RT_SCALE);
        h = (int)(h * RT_SCALE);
        tex_scaled[tid] = 1;
    }
    bool fr = false;
    const uint8_t *up = px;
    if (px && fmt == GL_RGBA && type == GL_UNSIGNED_BYTE && len >= (size_t)w * h * 4) up = unpack(px, w, h, false, &fr);
    *ow = w, *oh = h;
    glTexImage2D(target, level, ifmt, w, h, 0, fmt, type, up);
    if (level == 0) tex_account(bound_tex_id(), (size_t)w * h * 4);
    if (fr) free((void *)up);
    return JS_UNDEFINED;
}

// texSubImage2D(target, level, x, y, format, type, source)
// texSubImage2D(target, level, x, y, width, height, format, type, pixels)
FN(w_texSubImage2D) {
    if (gi(ctx, A(1)) == 0) tex_forget(ctx, bound_tex_id()); // contents no longer just the image
    int target = gi(ctx, A(0)), level = gi(ctx, A(1)), x = gi(ctx, A(2)), y = gi(ctx, A(3));
    if (argc == 7) {
        int w, h;
        bool owned, fr;
        const uint8_t *px = pixels_from_js(ctx, A(6), &w, &h, &owned);
        if (!px) return JS_ThrowTypeError(ctx, "texSubImage2D: unsupported source");
        const uint8_t *up = unpack(px, w, h, owned, &fr);
        glTexSubImage2D(target, level, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, up);
        if (fr) free((void *)up);
        if (owned && up != px) free((void *)px);
        return JS_UNDEFINED;
    }
    int w = gi(ctx, A(4)), h = gi(ctx, A(5)), fmt = gi(ctx, A(6)), type = gi(ctx, A(7));
    size_t len = 0;
    const uint8_t *px = buffer_data(ctx, A(8), &len);
    bool fr = false;
    const uint8_t *up = px;
    if (px && fmt == GL_RGBA && type == GL_UNSIGNED_BYTE) up = unpack(px, w, h, false, &fr);
    if (px) glTexSubImage2D(target, level, x, y, w, h, fmt, type, up);
    if (fr) free((void *)up);
    return JS_UNDEFINED;
}

FN(w_texParameteri) { glTexParameteri(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2))); return JS_UNDEFINED; }
FN(w_texParameterf) { glTexParameterf(gi(ctx, A(0)), gi(ctx, A(1)), gf(ctx, A(2))); return JS_UNDEFINED; }
FN(w_getTexParameter) {
    GLint v = 0;
    glGetTexParameteriv(gi(ctx, A(0)), gi(ctx, A(1)), &v);
    return JS_NewInt32(ctx, v);
}
FN(w_copyTexImage2D) {
    glCopyTexImage2D(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)), gi(ctx, A(4)), gi(ctx, A(5)),
                     gi(ctx, A(6)), gi(ctx, A(7)));
    return JS_UNDEFINED;
}
FN(w_copyTexSubImage2D) {
    glCopyTexSubImage2D(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)), gi(ctx, A(4)), gi(ctx, A(5)),
                        gi(ctx, A(6)), gi(ctx, A(7)));
    return JS_UNDEFINED;
}

FN(w_readPixels) {
    size_t len = 0;
    void *p = buffer_data(ctx, A(6), &len);
    if (p) glReadPixels(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)), gi(ctx, A(4)), gi(ctx, A(5)), p);
    return JS_UNDEFINED;
}

// ---------------------------------------------------------------- queries

FN(w_getBufferParameter) {
    GLint v = 0;
    glGetBufferParameteriv(gi(ctx, A(0)), gi(ctx, A(1)), &v);
    return JS_NewInt32(ctx, v);
}
FN(w_getRenderbufferParameter) {
    GLint v = 0;
    glGetRenderbufferParameteriv(gi(ctx, A(0)), gi(ctx, A(1)), &v);
    return JS_NewInt32(ctx, v);
}
FN(w_getFramebufferAttachmentParameter) {
    GLint v = 0;
    glGetFramebufferAttachmentParameteriv(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), &v);
    return JS_NewInt32(ctx, v);
}

static JSValue int_array(JSContext *ctx, const GLint *v, int n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v, n * 4);
    JSValue arr = JS_NewTypedArray(ctx, 1, &ab, JS_TYPED_ARRAY_INT32);
    JS_FreeValue(ctx, ab);
    return arr;
}
static JSValue float_array(JSContext *ctx, const GLfloat *v, int n) {
    JSValue ab = JS_NewArrayBufferCopy(ctx, (const uint8_t *)v, n * 4);
    JSValue arr = JS_NewTypedArray(ctx, 1, &ab, JS_TYPED_ARRAY_FLOAT32);
    JS_FreeValue(ctx, ab);
    return arr;
}

FN(w_getParameter) {
    int p = gi(ctx, A(0));
    GLint iv[4] = {0};
    GLfloat fv[4] = {0};
    GLboolean bv = 0;
    switch (p) {
    case GL_VERSION: return JS_NewString(ctx, "WebGL 1.0 (PokeRogue runtime, OpenGL ES 2.0)");
    case GL_SHADING_LANGUAGE_VERSION: return JS_NewString(ctx, "WebGL GLSL ES 1.0");
    case GL_VENDOR: return JS_NewString(ctx, "PokeRogue runtime");
    case GL_RENDERER: return JS_NewString(ctx, (const char *)glGetString(GL_RENDERER));
    case GL_VIEWPORT:
    case GL_SCISSOR_BOX:
    case GL_MAX_VIEWPORT_DIMS:
        glGetIntegerv(p, iv);
        return int_array(ctx, iv, p == GL_MAX_VIEWPORT_DIMS ? 2 : 4);
    case GL_COLOR_CLEAR_VALUE:
    case GL_BLEND_COLOR:
        glGetFloatv(p, fv);
        return float_array(ctx, fv, 4);
    case GL_ALIASED_LINE_WIDTH_RANGE:
    case GL_ALIASED_POINT_SIZE_RANGE:
    case GL_DEPTH_RANGE:
        glGetFloatv(p, fv);
        return float_array(ctx, fv, 2);
    case GL_BLEND:
    case GL_CULL_FACE:
    case GL_DEPTH_TEST:
    case GL_DITHER:
    case GL_POLYGON_OFFSET_FILL:
    case GL_SAMPLE_COVERAGE_INVERT:
    case GL_SCISSOR_TEST:
    case GL_STENCIL_TEST:
    case GL_DEPTH_WRITEMASK:
        glGetBooleanv(p, &bv);
        return JS_NewBool(ctx, bv);
    case GL_LINE_WIDTH:
    case GL_DEPTH_CLEAR_VALUE:
    case GL_POLYGON_OFFSET_FACTOR:
    case GL_POLYGON_OFFSET_UNITS:
    case GL_SAMPLE_COVERAGE_VALUE:
        glGetFloatv(p, fv);
        return JS_NewFloat64(ctx, fv[0]);
    case GL_CURRENT_PROGRAM: return JS_DupValue(ctx, st.bound_program);
    case GL_ARRAY_BUFFER_BINDING: return JS_DupValue(ctx, st.bound_array_buffer);
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: return JS_DupValue(ctx, st.bound_element_buffer);
    case GL_FRAMEBUFFER_BINDING: return JS_DupValue(ctx, st.bound_framebuffer);
    case GL_RENDERBUFFER_BINDING: return JS_DupValue(ctx, st.bound_renderbuffer);
    case GL_TEXTURE_BINDING_2D:
        return JS_DupValue(ctx, st.active_unit >= 0 && st.active_unit < 32 ? st.bound_texture[st.active_unit] : JS_NULL);
    case GL_MAX_TEXTURE_IMAGE_UNITS: // Phaser builds its multi-texture shader for this many (an if per unit)
        glGetIntegerv(p, iv);
        if (g_cfg.max_textures > 0 && g_cfg.max_textures < iv[0]) iv[0] = g_cfg.max_textures;
        return JS_NewInt32(ctx, iv[0]);
    case 0x9240: return JS_NewBool(ctx, st.flip_y);
    case 0x9241: return JS_NewBool(ctx, st.premultiply);
    case 0x9243: return JS_NewInt32(ctx, 0x9244);
    default:
        glGetIntegerv(p, iv);
        return JS_NewInt32(ctx, iv[0]);
    }
}

// ---------------------------------------------------------------- class setup

FN(w_setCanvas) {
    // A second argument means this is the game's real renderer (Phaser's feature probe passes no
    // context attributes): status messages must not draw from now on, so GL state stays the game's.
    if (argc > 1 && JS_ToBool(ctx, A(1))) overlay_disable();
    track(ctx, &st.canvas, A(0));
    refresh_virtual_size(ctx);
    return JS_UNDEFINED;
}

#define F(name, n) JS_CFUNC_DEF(#name, n, w_##name)
static const JSCFunctionListEntry gl_funcs[] = {
    F(activeTexture, 1), F(attachShader, 2), F(bindAttribLocation, 3), F(bindBuffer, 2), F(bindFramebuffer, 2),
    F(bindRenderbuffer, 2), F(bindTexture, 2), F(blendColor, 4), F(blendEquation, 1), F(blendEquationSeparate, 2),
    F(blendFunc, 2), F(blendFuncSeparate, 4), F(bufferData, 3), F(bufferSubData, 3), F(checkFramebufferStatus, 1),
    F(clear, 1), F(clearColor, 4), F(clearDepth, 1), F(clearStencil, 1), F(colorMask, 4), F(compileShader, 1),
    F(copyTexImage2D, 8), F(copyTexSubImage2D, 8), F(createBuffer, 0), F(createFramebuffer, 0),
    F(createProgram, 0), F(createRenderbuffer, 0), F(createShader, 1), F(createTexture, 0), F(cullFace, 1),
    F(deleteBuffer, 1), F(deleteFramebuffer, 1), F(deleteProgram, 1), F(deleteRenderbuffer, 1),
    F(deleteShader, 1), F(deleteTexture, 1), F(depthFunc, 1), F(depthMask, 1), F(depthRange, 2),
    F(detachShader, 2), F(disable, 1), F(disableVertexAttribArray, 1), F(drawArrays, 3), F(drawElements, 4),
    F(enable, 1), F(enableVertexAttribArray, 1), F(finish, 0), F(flush, 0), F(framebufferRenderbuffer, 4),
    F(framebufferTexture2D, 5), F(frontFace, 1), F(generateMipmap, 1), F(getActiveAttrib, 2),
    F(getActiveUniform, 2), F(getAttribLocation, 2), F(getBufferParameter, 2), F(getError, 0),
    F(getFramebufferAttachmentParameter, 3), F(getParameter, 1), F(getProgramInfoLog, 1),
    F(getProgramParameter, 2), F(getRenderbufferParameter, 2), F(getShaderInfoLog, 1),
    F(getShaderParameter, 2), F(pkrShaderVariants, 0), F(getShaderPrecisionFormat, 2), F(getShaderSource, 1), F(getTexParameter, 2),
    F(getUniformLocation, 2), F(hint, 2), F(isBuffer, 1), F(isEnabled, 1), F(isFramebuffer, 1),
    F(isProgram, 1), F(isRenderbuffer, 1), F(isShader, 1), F(isTexture, 1), F(lineWidth, 1), F(linkProgram, 1),
    F(pixelStorei, 2), F(polygonOffset, 2), F(readPixels, 7), F(renderbufferStorage, 4), F(sampleCoverage, 2),
    F(scissor, 4), F(shaderSource, 2), F(stencilFunc, 3), F(stencilFuncSeparate, 4), F(stencilMask, 1),
    F(stencilMaskSeparate, 2), F(stencilOp, 3), F(stencilOpSeparate, 4), F(texImage2D, 9),
    F(texParameterf, 3), F(texParameteri, 3), F(texSubImage2D, 9), F(uniform1f, 2), F(uniform1fv, 2),
    F(uniform1i, 2), F(uniform1iv, 2), F(uniform2f, 3), F(uniform2fv, 2), F(uniform2i, 3), F(uniform2iv, 2),
    F(uniform3f, 4), F(uniform3fv, 2), F(uniform3i, 4), F(uniform3iv, 2), F(uniform4f, 5), F(uniform4fv, 2),
    F(uniform4i, 5), F(uniform4iv, 2), F(uniformMatrix2fv, 3), F(uniformMatrix3fv, 3), F(uniformMatrix4fv, 3),
    F(useProgram, 1), F(validateProgram, 1), F(vertexAttrib1f, 2), F(vertexAttrib2f, 3), F(vertexAttrib3f, 4),
    F(vertexAttrib4f, 5), F(vertexAttribPointer, 6), F(viewport, 4), F(setCanvas, 2),
#define GLC(name, value) JS_PROP_INT32_DEF(#name, value, JS_PROP_ENUMERABLE),
#include "gl_consts.h"
#undef GLC
};

// createWebGLPrototype() -> object with all WebGL methods and constants. The JS shim uses it as
// WebGLRenderingContext.prototype, so instances are plain JS objects (with .canvas etc.).
FN(n_create_gl_proto) {
    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, gl_funcs, sizeof gl_funcs / sizeof *gl_funcs);
    return proto;
}

void gl_register(JSContext *ctx, JSValue native) {
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &ctx_class);
    JS_NewClassID(rt, &obj_class);
    JS_NewClassID(rt, &uloc_class);
    JSClassDef od = {.class_name = "WebGLObject", .finalizer = obj_finalizer};
    JSClassDef ud = {.class_name = "WebGLUniformLocation", .finalizer = uloc_finalizer};
    JS_NewClass(rt, obj_class, &od);
    JS_NewClass(rt, uloc_class, &ud);
    st.canvas = st.bound_program = st.bound_array_buffer = st.bound_element_buffer = JS_NULL;
    st.bound_framebuffer = st.bound_renderbuffer = JS_NULL;
    for (int i = 0; i < 32; i++) st.bound_texture[i] = JS_NULL;
    for (int i = 0; i < TEX_MAX; i++) texrec[i].src = JS_UNDEFINED;
    JS_SetPropertyStr(ctx, native, "createWebGLPrototype", JS_NewCFunction(ctx, n_create_gl_proto, "createWebGLPrototype", 0));
    JS_SetPropertyStr(ctx, native, "textureDump", JS_NewCFunction(ctx, n_texture_dump, "textureDump", 1));
}

void gl_after_frame(void) {
    textures_evict();
    // Clear the whole screen (including the letterbox bars) for the next frame, then put back the
    // GL state touched, from our own shadow copies, since Phaser caches its own view of the state.
    int w, h;
    plat_drawable_size(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glColorMask(st.color_mask[0], st.color_mask[1], st.color_mask[2], st.color_mask[3]);
    glClearColor(st.clear_color[0], st.clear_color[1], st.clear_color[2], st.clear_color[3]);
    glViewport(st.vp[0], st.vp[1], st.vp[2], st.vp[3]);
    if (st.scissor_on) glEnable(GL_SCISSOR_TEST);
    if (st.fb) glBindFramebuffer(GL_FRAMEBUFFER, st.fb);
#ifdef PKR_DEBUG
    prof_after_frame();
#endif
}

// Puts back the GL state the overlay's drawing changes (overlay.c: framebuffer, viewport, caps,
// blending, program, texture unit 0, array buffer, attributes 0-1), from the shadow copies. Used
// when something is drawn in the middle of one of the game's frames, where Phaser expects its
// own state to be untouched.
void gl_restore_state(void) {
    glBindFramebuffer(GL_FRAMEBUFFER, st.fb);
    glViewport(st.vp[0], st.vp[1], st.vp[2], st.vp[3]);
    if (st.scissor_on) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st.blend_on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st.depth_on) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st.stencil_on) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (st.cull_on) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glBlendFuncSeparate(st.blend_func[0], st.blend_func[1], st.blend_func[2], st.blend_func[3]);
    glBlendEquationSeparate(st.blend_eq[0], st.blend_eq[1]);
    glColorMask(st.color_mask[0], st.color_mask[1], st.color_mask[2], st.color_mask[3]);
    glClearColor(st.clear_color[0], st.clear_color[1], st.clear_color[2], st.clear_color[3]);
    gl_pixel_store(GL_UNPACK_ALIGNMENT, st.unpack_align);
    glUseProgram(oid(st.bound_program));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, oid(st.bound_texture[0]));
    glActiveTexture(GL_TEXTURE0 + st.active_unit);
    for (int i = 0; i < 2; i++) {
        if (st.attrib[i].set) {
            glBindBuffer(GL_ARRAY_BUFFER, st.attrib[i].buf);
            glVertexAttribPointer(i, st.attrib[i].size, st.attrib[i].type, st.attrib[i].norm, st.attrib[i].stride,
                                  (const void *)st.attrib[i].off);
        }
        if (st.attrib[i].on) glEnableVertexAttribArray(i); else glDisableVertexAttribArray(i);
    }
    glBindBuffer(GL_ARRAY_BUFFER, oid(st.bound_array_buffer));
}
