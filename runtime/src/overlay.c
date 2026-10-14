// SPDX-License-Identifier: AGPL-3.0-only
// Full-screen status messages ("Connecting...", "Loading game code...", boot errors), drawn with
// an 8x8 bitmap font so they need no font files. Only used before the game takes over the screen:
// once Phaser creates its WebGL context, the overlay switches itself off for good, so it never
// touches GL state the game relies on.

#include <stdlib.h>
#include <string.h>

#include "gles.h"
#include "host.h"

#include "font8x8_basic.h"

#define SCALE 2
#define CELL (8 * SCALE)
#define MAX_COLS 56

static char message[2048];
static bool dirty, disabled;
static GLuint prog, tex, vbo;
static GLint a_pos, a_uv, u_tex;
static int tex_w, tex_h;

void overlay_set(const char *msg) {
    if (disabled) return;
    snprintf(message, sizeof message, "%s", msg ? msg : "");
    dirty = true;
}

void overlay_disable(void) {
    if (!disabled) host_log("[overlay] off: the game's renderer has started");
    disabled = true;
    message[0] = 0;
}

bool overlay_active(void) { return !disabled && message[0]; }

// Word-wrap the message into lines of at most MAX_COLS characters.
static int layout(char lines[][MAX_COLS + 1], int max_lines) {
    int n = 0;
    const char *p = message;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p), start = 0;
        do {
            size_t take = len - start;
            if (take > MAX_COLS) {
                size_t sp = MAX_COLS;
                while (sp > 0 && p[start + sp] != ' ') sp--;
                take = sp ? sp : MAX_COLS;
            }
            if (n >= max_lines) return n;
            memcpy(lines[n], p + start, take);
            lines[n++][take] = 0;
            start += take;
            while (start < len && p[start] == ' ') start++;
        } while (start < len);
        if (!nl) break;
        p = nl + 1;
    }
    return n;
}

static void build_texture(void) {
    char lines[30][MAX_COLS + 1];
    int n = layout(lines, 30), cols = 1;
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(lines[i]);
        if (l > cols) cols = l;
    }
    tex_w = cols * CELL;
    tex_h = (n ? n : 1) * (CELL + 4);
    uint8_t *px = calloc((size_t)tex_w * tex_h, 4);
    for (int li = 0; li < n; li++) {
        int x0 = (cols - (int)strlen(lines[li])) * CELL / 2; // centered lines
        for (int ci = 0; lines[li][ci]; ci++) {
            unsigned char ch = (unsigned char)lines[li][ci];
            const char *glyph = font8x8_basic[ch < 128 ? ch : '?'];
            for (int gy = 0; gy < 8; gy++)
                for (int gx = 0; gx < 8; gx++) {
                    if (!(glyph[gy] & (1 << gx))) continue;
                    for (int sy = 0; sy < SCALE; sy++)
                        for (int sx = 0; sx < SCALE; sx++) {
                            int x = x0 + ci * CELL + gx * SCALE + sx, y = li * (CELL + 4) + gy * SCALE + sy;
                            uint8_t *d = px + ((size_t)y * tex_w + x) * 4;
                            d[0] = d[1] = d[2] = d[3] = 255;
                        }
                }
        }
    }
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex_w, tex_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    free(px);
    dirty = false;
}

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        host_log("[overlay] shader: %s", log);
    }
    return s;
}

static bool init_gl(void) {
    if (prog) return true;
    static const char *vs = "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV;\n"
                            "void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char *fs = "precision mediump float; varying vec2 vUV; uniform sampler2D uTex;\n"
                            "void main() { vec4 c = texture2D(uTex, vUV); gl_FragColor = vec4(0.96, 0.92, 0.98, c.a); }\n";
    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aUV");
    glLinkProgram(prog);
    a_pos = 0;
    a_uv = 1;
    u_tex = glGetUniformLocation(prog, "uTex");
    glGenBuffers(1, &vbo);
    return true;
}

// Draws the current message over a plain background. Returns false if there is nothing to show.
bool overlay_draw(void) {
    if (!overlay_active()) return false;
    init_gl();
    if (dirty) build_texture();
    int w, h;
    plat_drawable_size(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.20f, 0.13f, 0.24f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float x1 = (float)tex_w / w, y1 = (float)tex_h / h; // half extents in clip space are tex/screen
    float quad[] = {-x1, -y1, 0, 1, x1, -y1, 1, 1, -x1, y1, 0, 0, x1, y1, 1, 0};
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(a_pos);
    glEnableVertexAttribArray(a_uv);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(a_pos);
    glDisableVertexAttribArray(a_uv);
    glDisable(GL_BLEND);
    return true;
}

// Show a message right now (used while the main thread is blocked, e.g. compiling game code).
void overlay_present(const char *msg) {
    if (disabled) return;
    overlay_set(msg);
    if (overlay_draw()) plat_swap();
}
