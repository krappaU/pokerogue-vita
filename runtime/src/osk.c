// SPDX-License-Identifier: AGPL-3.0-only
// Text entry for the game's login and register forms.
//
// A built-in keyboard: a key grid drawn over the game, used with the D-pad / left stick and the face
// buttons, or by touching keys. The Vita's system keyboard is not used: it never opened on the
// console (sceImeDialogInit 0x800204c4), and the system dialogs' memory serves as GPU memory instead
// (platform_vita.c). On the desktop runtime a text field is answered through the control channel
// ("ime <text>", for scripted runs) unless config.txt has osk=1.
//   cross: type the selected key    circle: delete    triangle: space    square: capitals
//   START: done                      SELECT: cancel
// On the desktop runtime, arrow keys, Enter, Backspace, Escape and typed characters work too.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define COLS 10
#define ROWS 5 // 4 rows of characters + 1 row of special keys
#define KEY_W 44
#define KEY_H 36
#define MARGIN 12
#define IMG_W (COLS * KEY_W + 2 * MARGIN)
#define TOP (MARGIN + 18 + 8 + 30 + 10) // title, gap, text field, gap
#define IMG_H (TOP + ROWS * KEY_H + 8 + 16 + MARGIN)

static const char *rows_lower[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"};
static const char *rows_upper[4] = {"!?#$%&*()=", "QWERTYUIOP", "ASDFGHJKL+", "ZXCVBNM/,:"};
// Special keys in the last row: label, first column, width in columns.
enum { SK_SHIFT, SK_SPACE, SK_DEL, SK_OK, SK_COUNT };
static const struct { const char *label; int col, span; } special[SK_COUNT] = {
    {"Shift", 0, 2}, {"Space", 2, 4}, {"Del", 6, 2}, {"OK", 8, 2}};

static enum { TI_NONE, TI_SYSTEM, TI_OSK } mode;
static bool open_, shift, dirty, mute_until_release;
static int result; // 0 open, 1 done, -1 cancelled
static int sel_row, sel_col, max_len;
static bool password;
static char title[64], text[256];
static uint8_t img[IMG_W * IMG_H * 4];
static int img_x, img_y; // where the image was drawn (for touch)

// ---------------------------------------------------------------- dispatch

bool textin_open(const char *t, const char *initial, int max, bool pw) {
    if (mode != TI_NONE) return false;
#ifndef __vita__
    if (!g_cfg.osk) {
        if (!plat_ime_open(t, initial, max, pw)) return false;
        mode = TI_SYSTEM;
        return true;
    }
#endif
    snprintf(title, sizeof title, "%s", t);
    snprintf(text, sizeof text, "%s", initial);
    max_len = max > 0 && max < (int)sizeof text ? max : (int)sizeof text - 1;
    password = pw;
    shift = false;
    sel_row = 1, sel_col = 0;
    open_ = dirty = true;
    result = 0;
    mode = TI_OSK;
    return true;
}

int textin_poll(char *out, size_t n) {
#ifndef __vita__
    if (mode == TI_SYSTEM) {
        int r = plat_ime_poll(out, n);
        if (r != 0) mode = TI_NONE;
        return r;
    }
#endif
    if (mode != TI_OSK) return -1;
    if (result == 0) return 0;
    mode = TI_NONE;
    if (result > 0) snprintf(out, n, "%s", text);
    return result;
}

bool osk_active(void) { return mode == TI_OSK && open_; }
bool osk_mutes_pad(void) { return osk_active() || mute_until_release; }

// ---------------------------------------------------------------- editing

static bool swallow_keyup; // the key that closed the keyboard: its release isn't the game's

static void close_kb(int r) {
    result = r;
    open_ = false;
    mute_until_release = true;
    swallow_keyup = true;
}

static void type_char(char c) {
    size_t l = strlen(text);
    if ((int)l < max_len) text[l] = c, text[l + 1] = 0;
    dirty = true;
}

static void delete_char(void) {
    size_t l = strlen(text);
    if (l) text[l - 1] = 0;
    dirty = true;
}

static int special_at(int col) {
    for (int i = 0; i < SK_COUNT; i++)
        if (col >= special[i].col && col < special[i].col + special[i].span) return i;
    return SK_OK;
}

static void press_selected(void) {
    if (sel_row < 4) {
        type_char((shift ? rows_upper : rows_lower)[sel_row][sel_col]);
        return;
    }
    switch (special_at(sel_col)) {
    case SK_SHIFT: shift = !shift; dirty = true; break;
    case SK_SPACE: type_char(' '); break;
    case SK_DEL: delete_char(); break;
    default: close_kb(1); break;
    }
}

static void move(int dr, int dc) {
    if (dr) sel_row = (sel_row + dr + ROWS) % ROWS;
    if (dc) {
        if (sel_row == 4) { // jump between special keys
            int k = (special_at(sel_col) + dc + SK_COUNT) % SK_COUNT;
            sel_col = special[k].col;
        } else {
            sel_col = (sel_col + dc + COLS) % COLS;
        }
    }
    dirty = true;
}

// ---------------------------------------------------------------- input

bool osk_event(const plat_event *ev) {
    if (!osk_active()) {
        if (swallow_keyup && ev->type == PEV_KEY_UP) {
            swallow_keyup = false;
            return true;
        }
        return false;
    }
    if (ev->type == PEV_POINTER_DOWN) {
        int x = (int)ev->x - img_x - MARGIN, y = (int)ev->y - img_y - TOP;
        if (x >= 0 && y >= 0 && x < COLS * KEY_W && y < ROWS * KEY_H) {
            sel_row = y / KEY_H, sel_col = x / KEY_W;
            press_selected();
            dirty = true;
        }
        return true;
    }
    if (ev->type == PEV_KEY_DOWN && ev->key) {
        const char *k = ev->key;
        if (!strcmp(k, "ArrowUp")) move(-1, 0);
        else if (!strcmp(k, "ArrowDown")) move(1, 0);
        else if (!strcmp(k, "ArrowLeft")) move(0, -1);
        else if (!strcmp(k, "ArrowRight")) move(0, 1);
        else if (!strcmp(k, "Enter")) close_kb(1);
        else if (!strcmp(k, "Escape")) close_kb(-1);
        else if (!strcmp(k, "Backspace")) delete_char();
        else if (strlen(k) == 1 && k[0] >= 32 && k[0] < 127) type_char(k[0]);
        return true;
    }
    return ev->type == PEV_KEY_UP || ev->type == PEV_POINTER_MOVE || ev->type == PEV_POINTER_UP;
}

void osk_frame(const plat_gamepad *pad) {
    // Standard gamepad indices: 0 cross, 1 circle, 2 square, 3 triangle, 8 select, 9 start, 12-15 D-pad.
    bool now[18];
    for (int i = 0; i < 18; i++) now[i] = pad && pad->buttons[i] > 0.5f;
    float ax = pad ? pad->axes[0] : 0, ay = pad ? pad->axes[1] : 0;
    bool up = now[12] || ay < -0.6f, down = now[13] || ay > 0.6f, left = now[14] || ax < -0.6f, right = now[15] || ax > 0.6f;
    if (mute_until_release && !osk_active()) {
        bool any = up || down || left || right;
        for (int i = 0; i < 18; i++) any |= now[i];
        if (!any) mute_until_release = false;
    }
    static bool prev[18], pdir[4];
    static double repeat_at;
    double t = host_now_ms();
    if (osk_active()) {
        bool dir[4] = {up, down, left, right};
        static const int dr[4] = {-1, 1, 0, 0}, dc[4] = {0, 0, -1, 1};
        for (int d = 0; d < 4; d++) {
            if (dir[d] && !pdir[d]) {
                move(dr[d], dc[d]);
                repeat_at = t + 400;
            } else if (dir[d] && t >= repeat_at) {
                move(dr[d], dc[d]);
                repeat_at = t + 90;
            }
        }
        if (now[0] && !prev[0]) press_selected();
        if (now[1] && !prev[1]) delete_char();
        if (now[3] && !prev[3]) type_char(' ');
        if (now[2] && !prev[2]) shift = !shift, dirty = true;
        if (now[9] && !prev[9]) close_kb(1);
        if (now[8] && !prev[8]) close_kb(-1);
        memcpy(pdir, dir, sizeof pdir);
    }
    memcpy(prev, now, sizeof prev);
}

// ---------------------------------------------------------------- drawing

static void fill(int x, int y, int w, int h, const uint8_t c[4]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < IMG_W && j < IMG_H) memcpy(img + ((size_t)j * IMG_W + i) * 4, c, 4);
}

static void frame_rect(int x, int y, int w, int h, const uint8_t c[4]) {
    fill(x, y, w, 2, c), fill(x, y + h - 2, w, 2, c), fill(x, y, 2, h, c), fill(x + w - 2, y, 2, h, c);
}

static void render(void) {
    static const uint8_t bg[4] = {40, 30, 48, 245}, border[4] = {200, 60, 50, 255}, key[4] = {64, 52, 74, 255},
                         sel[4] = {232, 168, 56, 255}, fg[4] = {245, 235, 250, 255}, dark[4] = {30, 22, 36, 255},
                         dim_fg[4] = {170, 160, 180, 255}, field[4] = {24, 18, 30, 255};
    fill(0, 0, IMG_W, IMG_H, bg);
    frame_rect(0, 0, IMG_W, IMG_H, border);
    overlay_blit_text(img, IMG_W, IMG_H, MARGIN, MARGIN, title, 2, fg);
    // Text field: the end of the text if it's long; passwords as asterisks.
    int fy = MARGIN + 18 + 8;
    fill(MARGIN, fy, IMG_W - 2 * MARGIN, 30, field);
    char shown[64];
    size_t l = strlen(text), cap = (IMG_W - 2 * MARGIN - 16) / 16 - 1;
    const char *src = l > cap ? text + l - cap : text;
    size_t sl = strlen(src);
    for (size_t i = 0; i < sl && i < sizeof shown - 2; i++) shown[i] = password ? '*' : src[i];
    shown[sl < sizeof shown - 2 ? sl : sizeof shown - 2] = '_';
    shown[(sl < sizeof shown - 2 ? sl : sizeof shown - 2) + 1] = 0;
    overlay_blit_text(img, IMG_W, IMG_H, MARGIN + 8, fy + 7, shown, 2, fg);
    // Keys.
    const char **rows = shift ? rows_upper : rows_lower;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < COLS; c++) {
            int x = MARGIN + c * KEY_W, y = TOP + r * KEY_H;
            bool s = r == sel_row && c == sel_col;
            fill(x + 2, y + 2, KEY_W - 4, KEY_H - 4, s ? sel : key);
            char ch[2] = {rows[r][c], 0};
            overlay_blit_text(img, IMG_W, IMG_H, x + (KEY_W - 16) / 2, y + (KEY_H - 16) / 2, ch, 2, s ? dark : fg);
        }
    for (int k = 0; k < SK_COUNT; k++) {
        int x = MARGIN + special[k].col * KEY_W, y = TOP + 4 * KEY_H, w = special[k].span * KEY_W;
        bool s = sel_row == 4 && special_at(sel_col) == k;
        bool on = k == SK_SHIFT && shift;
        fill(x + 2, y + 2, w - 4, KEY_H - 4, s ? sel : on ? border : key);
        int tw = (int)strlen(special[k].label) * 16;
        overlay_blit_text(img, IMG_W, IMG_H, x + (w - tw) / 2, y + (KEY_H - 16) / 2, special[k].label, 2, s ? dark : fg);
    }
    overlay_blit_text(img, IMG_W, IMG_H, MARGIN, TOP + ROWS * KEY_H + 8,
                      "X type  O del  /\\ space  [] caps  START ok  SELECT back", 1, dim_fg);
}

bool osk_draw(void) {
    if (!osk_active()) return false;
    bool changed = dirty;
    if (dirty) render(), dirty = false;
    overlay_image_draw(img, IMG_W, IMG_H, changed, &img_x, &img_y);
    return true;
}

// ---------------------------------------------------------------- the game's input fields
// The game's text fields are HTML <input> elements over the canvas, which this runtime doesn't
// draw. js/input.js renders each visible field's text with the game's font and hands the pixels
// and position (game canvas coordinates) here, to be drawn over the game.

typedef struct {
    uint8_t *rgba;
    int w, h;
    bool changed;
    float x, y, dw, dh;
} field_overlay;

#define MAX_FIELDS 8
static field_overlay fields[MAX_FIELDS];
static int n_fields;

void fields_set(int n) { n_fields = n < MAX_FIELDS ? n : MAX_FIELDS; }

void field_update(int i, const uint8_t *rgba, int w, int h, float x, float y, float dw, float dh) {
    if (i < 0 || i >= MAX_FIELDS) return;
    field_overlay *f = &fields[i];
    if (rgba && w > 0 && h > 0) {
        uint8_t *copy = realloc(f->rgba, (size_t)w * h * 4);
        if (copy) {
            memcpy(copy, rgba, (size_t)w * h * 4);
            f->rgba = copy, f->w = w, f->h = h, f->changed = true;
        }
    }
    f->x = x, f->y = y, f->dw = dw, f->dh = dh;
}

bool fields_draw_pending(void) { return n_fields > 0; }

bool fields_draw(void) {
    for (int i = 0; i < n_fields; i++) {
        field_overlay *f = &fields[i];
        if (!f->rgba) continue;
        float sx, sy, s;
        gl_game_to_screen(f->x, f->y, &sx, &sy, &s);
        overlay_rgba_at(i, f->rgba, f->w, f->h, f->changed, sx, sy, f->dw * s, f->dh * s);
        f->changed = false;
    }
    return n_fields > 0;
}
