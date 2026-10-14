// SPDX-License-Identifier: AGPL-3.0-only
// WebGL 1 on OpenGL ES 2, as a native QuickJS class (no JS wrapper layer, to keep per-call cost low).
//
// The game renders to a 1920x1080 canvas. The default framebuffer is the real screen (960x544 on
// the Vita), so viewport/scissor calls on it are scaled and letterboxed here instead of rendering
// to a full-size offscreen buffer first. Framebuffer objects (render textures) are left unscaled.

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <GLES2/gl2.h>

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
} st = {.default_fb = true, .virt_w = 300, .virt_h = 150};

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
FN(w_blendEquation) { glBlendEquation(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_blendEquationSeparate) { glBlendEquationSeparate(gi(ctx, A(0)), gi(ctx, A(1))); return JS_UNDEFINED; }
FN(w_blendFunc) { glBlendFunc(gi(ctx, A(0)), gi(ctx, A(1))); return JS_UNDEFINED; }
FN(w_blendFuncSeparate) {
    glBlendFuncSeparate(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_clear) { glClear(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_clearColor) { glClearColor(gf(ctx, A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3))); return JS_UNDEFINED; }
FN(w_clearDepth) { glClearDepthf(gf(ctx, A(0))); return JS_UNDEFINED; }
FN(w_clearStencil) { glClearStencil(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_colorMask) {
    glColorMask(JS_ToBool(ctx, A(0)), JS_ToBool(ctx, A(1)), JS_ToBool(ctx, A(2)), JS_ToBool(ctx, A(3)));
    return JS_UNDEFINED;
}
FN(w_cullFace) { glCullFace(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthFunc) { glDepthFunc(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthMask) { glDepthMask(JS_ToBool(ctx, A(0))); return JS_UNDEFINED; }
FN(w_depthRange) { glDepthRangef(gf(ctx, A(0)), gf(ctx, A(1))); return JS_UNDEFINED; }
FN(w_disable) { glDisable(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_enable) { glEnable(gi(ctx, A(0))); return JS_UNDEFINED; }
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
    else glPixelStorei(p, v);
    return JS_UNDEFINED;
}

FN(w_viewport) {
    GLint x = gi(ctx, A(0)), y = gi(ctx, A(1));
    GLsizei w = gi(ctx, A(2)), h = gi(ctx, A(3));
    if (st.default_fb) {
        refresh_virtual_size(ctx);
        float s, ox, oy;
        fb_map(&s, &ox, &oy);
        glViewport((GLint)lroundf(ox + x * s), (GLint)lroundf(oy + y * s), (GLsizei)lroundf(w * s),
                   (GLsizei)lroundf(h * s));
    } else {
        glViewport(x, y, w, h);
    }
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
        glScissor(x, y, w, h);
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

FN(w_deleteBuffer) { GLuint id = oid(A(0)); if (id) glDeleteBuffers(1, &id); return JS_UNDEFINED; }
FN(w_deleteTexture) { GLuint id = oid(A(0)); if (id) glDeleteTextures(1, &id); return JS_UNDEFINED; }
FN(w_deleteFramebuffer) { GLuint id = oid(A(0)); if (id) glDeleteFramebuffers(1, &id); return JS_UNDEFINED; }
FN(w_deleteRenderbuffer) { GLuint id = oid(A(0)); if (id) glDeleteRenderbuffers(1, &id); return JS_UNDEFINED; }
FN(w_deleteProgram) { GLuint id = oid(A(0)); if (id) glDeleteProgram(id); return JS_UNDEFINED; }
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
    return JS_UNDEFINED;
}
FN(w_bindFramebuffer) {
    GLuint id = oid(A(1));
    glBindFramebuffer(gi(ctx, A(0)), id);
    st.default_fb = id == 0;
    track(ctx, &st.bound_framebuffer, A(1));
    return JS_UNDEFINED;
}
FN(w_bindRenderbuffer) {
    glBindRenderbuffer(gi(ctx, A(0)), oid(A(1)));
    track(ctx, &st.bound_renderbuffer, A(1));
    return JS_UNDEFINED;
}

// bufferData(target, size | data, usage)
FN(w_bufferData) {
    int target = gi(ctx, A(0)), usage = gi(ctx, A(2));
    if (JS_IsNumber(A(1))) {
        glBufferData(target, gi(ctx, A(1)), NULL, usage);
    } else {
        size_t len = 0;
        void *p = buffer_data(ctx, A(1), &len);
        glBufferData(target, len, p, usage);
    }
    return JS_UNDEFINED;
}

FN(w_bufferSubData) {
    size_t len = 0;
    void *p = buffer_data(ctx, A(2), &len);
    if (p) glBufferSubData(gi(ctx, A(0)), gi(ctx, A(1)), len, p);
    return JS_UNDEFINED;
}

FN(w_checkFramebufferStatus) { return JS_NewInt32(ctx, glCheckFramebufferStatus(gi(ctx, A(0)))); }
FN(w_framebufferTexture2D) {
    glFramebufferTexture2D(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), oid(A(3)), gi(ctx, A(4)));
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

// ---------------------------------------------------------------- shaders / programs

FN(w_shaderSource) {
    const char *src = JS_ToCString(ctx, A(1));
    if (src) glShaderSource(oid(A(0)), 1, &src, NULL);
    JS_FreeCString(ctx, src);
    return JS_UNDEFINED;
}
FN(w_compileShader) { glCompileShader(oid(A(0))); return JS_UNDEFINED; }
FN(w_attachShader) { glAttachShader(oid(A(0)), oid(A(1))); return JS_UNDEFINED; }
FN(w_detachShader) { glDetachShader(oid(A(0)), oid(A(1))); return JS_UNDEFINED; }
FN(w_linkProgram) { glLinkProgram(oid(A(0))); return JS_UNDEFINED; }
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
FN(w_getShaderSource) {
    char src[65536] = "";
    glGetShaderSource(oid(A(0)), sizeof src, NULL, src);
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
    JS_FreeCString(ctx, name);
    if (l < 0) return JS_NULL;
    JSValue v = JS_NewObjectClass(ctx, uloc_class);
    uloc *u = malloc(sizeof *u);
    u->loc = l;
    JS_SetOpaque(v, u);
    return v;
}

static inline GLint lid(JSValueConst v) {
    uloc *u = JS_GetOpaque(v, uloc_class);
    return u ? u->loc : -1;
}

// ---------------------------------------------------------------- uniforms / attributes

FN(w_uniform1f) { glUniform1f(lid(A(0)), gf(ctx, A(1))); return JS_UNDEFINED; }
FN(w_uniform2f) { glUniform2f(lid(A(0)), gf(ctx, A(1)), gf(ctx, A(2))); return JS_UNDEFINED; }
FN(w_uniform3f) { glUniform3f(lid(A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3))); return JS_UNDEFINED; }
FN(w_uniform4f) {
    glUniform4f(lid(A(0)), gf(ctx, A(1)), gf(ctx, A(2)), gf(ctx, A(3)), gf(ctx, A(4)));
    return JS_UNDEFINED;
}
FN(w_uniform1i) { glUniform1i(lid(A(0)), gi(ctx, A(1))); return JS_UNDEFINED; }
FN(w_uniform2i) { glUniform2i(lid(A(0)), gi(ctx, A(1)), gi(ctx, A(2))); return JS_UNDEFINED; }
FN(w_uniform3i) { glUniform3i(lid(A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3))); return JS_UNDEFINED; }
FN(w_uniform4i) {
    glUniform4i(lid(A(0)), gi(ctx, A(1)), gi(ctx, A(2)), gi(ctx, A(3)), gi(ctx, A(4)));
    return JS_UNDEFINED;
}

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
    if (p && count > 0) {
        if (dim == 2) glUniformMatrix2fv(l, count, GL_FALSE, p);
        else if (dim == 3) glUniformMatrix3fv(l, count, GL_FALSE, p);
        else glUniformMatrix4fv(l, count, GL_FALSE, p);
    }
    free(tmp);
    return JS_UNDEFINED;
}
FN(w_uniformMatrix2fv) { return uniform_matrix(ctx, argv, argc, 2); }
FN(w_uniformMatrix3fv) { return uniform_matrix(ctx, argv, argc, 3); }
FN(w_uniformMatrix4fv) { return uniform_matrix(ctx, argv, argc, 4); }

FN(w_vertexAttribPointer) {
    glVertexAttribPointer(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), JS_ToBool(ctx, A(3)), gi(ctx, A(4)),
                          (const void *)(intptr_t)gi(ctx, A(5)));
    return JS_UNDEFINED;
}
FN(w_enableVertexAttribArray) { glEnableVertexAttribArray(gi(ctx, A(0))); return JS_UNDEFINED; }
FN(w_disableVertexAttribArray) { glDisableVertexAttribArray(gi(ctx, A(0))); return JS_UNDEFINED; }
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

FN(w_drawArrays) { glDrawArrays(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2))); return JS_UNDEFINED; }
FN(w_drawElements) {
    glDrawElements(gi(ctx, A(0)), gi(ctx, A(1)), gi(ctx, A(2)), (const void *)(intptr_t)gi(ctx, A(3)));
    return JS_UNDEFINED;
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
FN(w_texImage2D) {
    int target = gi(ctx, A(0)), level = gi(ctx, A(1)), ifmt = gi(ctx, A(2));
    if (argc == 6) {
        int fmt = gi(ctx, A(3)), type = gi(ctx, A(4)), w, h;
        bool owned, fr;
        const uint8_t *px = pixels_from_js(ctx, A(5), &w, &h, &owned);
        if (!px) return JS_ThrowTypeError(ctx, "texImage2D: unsupported source");
        const uint8_t *up = unpack(px, w, h, owned, &fr);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(target, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, up);
        (void)ifmt, (void)fmt, (void)type;
        if (fr) free((void *)up);
        if (owned && up != px) free((void *)px);
        return JS_UNDEFINED;
    }
    int w = gi(ctx, A(3)), h = gi(ctx, A(4)), fmt = gi(ctx, A(6)), type = gi(ctx, A(7));
    size_t len = 0;
    const uint8_t *px = JS_IsNull(A(8)) || JS_IsUndefined(A(8)) ? NULL : buffer_data(ctx, A(8), &len);
    bool fr = false;
    const uint8_t *up = px;
    if (px && fmt == GL_RGBA && type == GL_UNSIGNED_BYTE && len >= (size_t)w * h * 4) up = unpack(px, w, h, false, &fr);
    glTexImage2D(target, level, ifmt, w, h, 0, fmt, type, up);
    if (fr) free((void *)up);
    return JS_UNDEFINED;
}

// texSubImage2D(target, level, x, y, format, type, source)
// texSubImage2D(target, level, x, y, width, height, format, type, pixels)
FN(w_texSubImage2D) {
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
    F(getShaderParameter, 2), F(getShaderPrecisionFormat, 2), F(getShaderSource, 1), F(getTexParameter, 2),
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
    F(vertexAttrib4f, 5), F(vertexAttribPointer, 6), F(viewport, 4), F(setCanvas, 1),
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
    JS_SetPropertyStr(ctx, native, "createWebGLPrototype", JS_NewCFunction(ctx, n_create_gl_proto, "createWebGLPrototype", 0));
}

void gl_after_frame(void) {
    // Clear the whole screen (including the letterbox bars) for the next frame, then put back every
    // piece of GL state touched, since Phaser caches its own view of the state.
    GLint fb = 0, vp[4];
    GLfloat cc[4];
    GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean mask[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);

    int w, h;
    plat_drawable_size(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glColorMask(mask[0], mask[1], mask[2], mask[3]);
    glClearColor(cc[0], cc[1], cc[2], cc[3]);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
}
