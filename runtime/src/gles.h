// SPDX-License-Identifier: AGPL-3.0-only
// OpenGL ES 2 headers: Mesa/desktop GLES on Linux, vitaGL on the Vita.
#pragma once
#ifdef __vita__
#include <vitaGL.h>

// GL ES 2 pieces vitaGL does not provide. None of them matter for PokeRogue's rendering;
// they exist so the WebGL binding stays complete.
#ifndef GL_BLEND_COLOR
#define GL_BLEND_COLOR 0x8005
#endif
#ifndef GL_ALIASED_POINT_SIZE_RANGE
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D
#endif
#ifndef GL_ALIASED_LINE_WIDTH_RANGE
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#endif
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_SAMPLE_COVERAGE_VALUE
#define GL_SAMPLE_COVERAGE_VALUE 0x80AA
#endif
#ifndef GL_SAMPLE_COVERAGE_INVERT
#define GL_SAMPLE_COVERAGE_INVERT 0x80AB
#endif
#ifndef GL_LINE_WIDTH
#define GL_LINE_WIDTH 0x0B21
#endif

static inline void glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { (void)r, (void)g, (void)b, (void)a; }
static inline void glSampleCoverage(GLfloat v, GLboolean inv) { (void)v, (void)inv; }
static inline GLboolean glIsBuffer(GLuint b) { return b != 0; }
static inline GLboolean glIsShader(GLuint s) { return s != 0; }
static inline void glDetachShader(GLuint p, GLuint s) { (void)p, (void)s; }
static inline void glValidateProgram(GLuint p) { (void)p; }
static inline void glGetShaderPrecisionFormat(GLenum st, GLenum pt, GLint *range, GLint *prec) {
    (void)st, (void)pt;
    range[0] = range[1] = 127;
    *prec = 23;
}
static inline void glGetTexParameteriv(GLenum t, GLenum p, GLint *v) { (void)t, (void)p, *v = 0; }
static inline void glGetRenderbufferParameteriv(GLenum t, GLenum p, GLint *v) { (void)t, (void)p, *v = 0; }
#else
#include <GLES2/gl2.h>
#endif
