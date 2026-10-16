// SPDX-License-Identifier: AGPL-3.0-only
// Full-screen status messages ("Connecting...", "Loading game code...", boot errors) and the
// optional debug line, drawn with an 8x8 bitmap font so they need no font files. Status messages
// are only used before the game takes over the screen: once Phaser creates its WebGL context, the
// overlay switches itself off for good. The debug line is drawn over the game's frame.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gles.h"
#include "host.h"

#include "font8x8_basic.h"

#define MAX_COLS 56

// A block of text rendered into a texture: glyphs in `fg`, the rest filled with `bg` (RGBA).
typedef struct {
    GLuint tex;
    int w, h;
    char text[2048];
    bool dirty;
} textbox;

static char message[2048];
static bool disabled;
static textbox msg_box, footer_box, hud_box, busy_box;
static GLuint prog, vbo;
static GLint a_pos, a_uv, u_tex;

void overlay_set(const char *msg) {
    if (disabled) return;
    snprintf(message, sizeof message, "%s", msg ? msg : "");
    msg_box.dirty = true;
}

void overlay_disable(void) {
    if (!disabled) host_log("[overlay] off: the game's renderer has started");
    disabled = true;
    message[0] = 0;
}

bool overlay_active(void) { return !disabled && message[0]; }

// Word-wrap text into lines of at most MAX_COLS characters.
static int layout(const char *text, char lines[][MAX_COLS + 1], int max_lines) {
    int n = 0;
    const char *p = text;
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

static void upload(GLuint *tex, const uint8_t *px, int w, int h);

// Draws text into an RGBA buffer of size W x H (8x8 glyphs times `scale`; no clipping beyond the buffer).
void overlay_blit_text(uint8_t *px, int W, int H, int x0, int y0, const char *text, int scale, const uint8_t rgba[4]) {
    for (int ci = 0; text[ci]; ci++) {
        unsigned char ch = (unsigned char)text[ci];
        const char *glyph = font8x8_basic[ch < 128 ? ch : '?'];
        for (int gy = 0; gy < 8; gy++)
            for (int gx = 0; gx < 8; gx++) {
                if (!(glyph[gy] & (1 << gx))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int x = x0 + ci * 8 * scale + gx * scale + sx, y = y0 + gy * scale + sy;
                        if (x >= 0 && y >= 0 && x < W && y < H) memcpy(px + ((size_t)y * W + x) * 4, rgba, 4);
                    }
            }
    }
}

static void build_texture(textbox *tb, const char *text, int scale, bool center, const uint8_t fg[4],
                          const uint8_t bg[4], int pad) {
    char lines[30][MAX_COLS + 1];
    int n = layout(text, lines, 30), cols = 1, cell = 8 * scale, line_h = cell + 2 * scale;
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(lines[i]);
        if (l > cols) cols = l;
    }
    tb->w = cols * cell + 2 * pad;
    tb->h = (n ? n : 1) * line_h - 2 * scale + 2 * pad;
    uint8_t *px = malloc((size_t)tb->w * tb->h * 4);
    for (size_t i = 0; i < (size_t)tb->w * tb->h; i++) memcpy(px + i * 4, bg, 4);
    for (int li = 0; li < n; li++) {
        int x0 = pad + (center ? (cols - (int)strlen(lines[li])) * cell / 2 : 0);
        overlay_blit_text(px, tb->w, tb->h, x0, pad + li * line_h, lines[li], scale, fg);
    }
    upload(&tb->tex, px, tb->w, tb->h);
    free(px);
    tb->dirty = false;
}

static void upload(GLuint *tex, const uint8_t *px, int w, int h) {
    if (!*tex) glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_pixel_store(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
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
                            "void main() { gl_FragColor = texture2D(uTex, vUV); }\n";
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

// Draws a text box with its top-left corner at (x, y) in screen pixels.
static void draw_box(const textbox *tb, int x, int y, int sw, int sh) {
    float x0 = 2.0f * x / sw - 1, x1 = 2.0f * (x + tb->w) / sw - 1;
    float y0 = 1 - 2.0f * y / sh, y1 = 1 - 2.0f * (y + tb->h) / sh;
    float quad[] = {x0, y1, 0, 1, x1, y1, 1, 1, x0, y0, 0, 0, x1, y0, 1, 0};
    glBindTexture(GL_TEXTURE_2D, tb->tex);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void begin_draw(int *w, int *h) {
    init_gl();
    plat_drawable_size(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, *w, *h);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(a_pos);
    glEnableVertexAttribArray(a_uv);
}

static void end_draw(void) {
    glDisableVertexAttribArray(a_pos);
    glDisableVertexAttribArray(a_uv);
    glDisable(GL_BLEND);
}

static const uint8_t TEXT_FG[4] = {245, 235, 250, 255}, CLEAR[4] = {0, 0, 0, 0};
static const uint8_t SMALL_FG[4] = {255, 255, 255, 255}, SMALL_BG[4] = {0, 0, 0, 150};

// Draws the current message over a plain background, with the build version in the bottom-right
// corner. Returns false if there is nothing to show.
bool overlay_draw(void) {
    if (!overlay_active()) return false;
    int w, h;
    begin_draw(&w, &h);
    if (msg_box.dirty || !msg_box.tex) build_texture(&msg_box, message, 2, true, TEXT_FG, CLEAR, 0);
    if (!footer_box.tex) {
        snprintf(footer_box.text, sizeof footer_box.text, "%s", host_version_string());
        build_texture(&footer_box, footer_box.text, 1, false, SMALL_FG, SMALL_BG, 3);
    }
    glClearColor(0.20f, 0.13f, 0.24f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_box(&msg_box, (w - msg_box.w) / 2, (h - msg_box.h) / 2, w, h);
    draw_box(&footer_box, w - footer_box.w - 4, h - footer_box.h - 4, w, h);
    end_draw();
    return true;
}

// Full-screen "busy" message drawn while one of the game's frames runs for seconds (see busy_poll in
// main.c). Unlike overlay_draw it works after the game's renderer has taken over; the caller swaps
// and then restores the game's GL state (gl_restore_state).
void overlay_busy_draw(const char *text) {
    int w, h;
    begin_draw(&w, &h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glColorMask(1, 1, 1, 1);
    glBlendEquation(GL_FUNC_ADD);
    if (!busy_box.tex || strcmp(busy_box.text, text)) {
        snprintf(busy_box.text, sizeof busy_box.text, "%s", text);
        build_texture(&busy_box, busy_box.text, 2, true, TEXT_FG, CLEAR, 0);
    }
    if (!footer_box.tex) {
        snprintf(footer_box.text, sizeof footer_box.text, "%s", host_version_string());
        build_texture(&footer_box, footer_box.text, 1, false, SMALL_FG, SMALL_BG, 3);
    }
    glClearColor(0.20f, 0.13f, 0.24f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    draw_box(&busy_box, (w - busy_box.w) / 2, (h - busy_box.h) / 2, w, h);
    draw_box(&footer_box, w - footer_box.w - 4, h - footer_box.h - 4, w, h);
    end_draw();
}

// Debug line drawn over the game in the top-right corner. The caller lets Phaser know that GL
// state changed behind its back (see onExternalGL in js/bootstrap.js).
void overlay_hud_draw(const char *text) {
    int w, h;
    begin_draw(&w, &h);
    if (!hud_box.tex || strcmp(hud_box.text, text)) {
        snprintf(hud_box.text, sizeof hud_box.text, "%s", text);
        build_texture(&hud_box, hud_box.text, 1, false, SMALL_FG, SMALL_BG, 3);
    }
    draw_box(&hud_box, w - hud_box.w - 4, 4, w, h);
    end_draw();
}

// Show a message right now (used while the main thread is blocked, e.g. compiling game code).
void overlay_present(const char *msg) {
    if (disabled) return;
    overlay_set(msg);
    if (overlay_draw()) plat_swap();
}

// Draws an RGBA image centred on the screen over a dimmed game frame (the built-in keyboard).
// `changed` re-uploads the pixels. Returns the image's top-left corner in screen pixels.
void overlay_image_draw(const uint8_t *rgba, int iw, int ih, bool changed, int *ox, int *oy) {
    static GLuint tex, dim_tex;
    int w, h;
    begin_draw(&w, &h);
    if (!dim_tex) {
        static const uint8_t dim[4] = {0, 0, 0, 140};
        upload(&dim_tex, dim, 1, 1);
    }
    if (changed || !tex) upload(&tex, rgba, iw, ih);
    textbox full = {.tex = dim_tex, .w = w, .h = h}, img = {.tex = tex, .w = iw, .h = ih};
    draw_box(&full, 0, 0, w, h);
    *ox = (w - iw) / 2, *oy = (h - ih) / 2;
    draw_box(&img, *ox, *oy, w, h);
    end_draw();
}

// Draws an RGBA image stretched over a screen rectangle (text of the game's input fields).
// Each `slot` keeps its own texture; `changed` re-uploads the pixels.
void overlay_rgba_at(int slot, const uint8_t *rgba, int iw, int ih, bool changed, float x, float y, float w, float h) {
    static GLuint tex[8];
    if (slot < 0 || slot >= 8) return;
    int sw, sh;
    begin_draw(&sw, &sh);
    if (changed || !tex[slot]) upload(&tex[slot], rgba, iw, ih);
    float x0 = 2.0f * x / sw - 1, x1 = 2.0f * (x + w) / sw - 1;
    float y0 = 1 - 2.0f * y / sh, y1 = 1 - 2.0f * (y + h) / sh;
    float quad[] = {x0, y1, 0, 1, x1, y1, 1, 1, x0, y0, 0, 0, x1, y0, 1, 0};
    glBindTexture(GL_TEXTURE_2D, tex[slot]);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
    glVertexAttribPointer(a_pos, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    end_draw();
}
