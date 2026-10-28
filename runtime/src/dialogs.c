// SPDX-License-Identifier: AGPL-3.0-only
// Small dialogs over the game for what a browser would hand to the operating system:
//  - links the game opens (window.open: the Wiki, Discord, GitHub... menu entries) as a QR code
//    to scan with a phone, with the address below;
//  - notices (where an exported save was written);
//  - a file picker for the game's "import data" (<input type=file>), listing a folder of the
//    memory card;
//  - a choice between a few options (the network's "connection lost: retry / back to the title"),
//    which, unlike the picker, can't be cancelled.
// While one is open the game gets no gamepad input. Cross / circle / START close a QR code or a
// notice; in the picker the D-pad moves, cross picks, circle cancels. Touch and the desktop's
// Enter / Escape / arrow keys work too.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "host.h"
#include "qrcodegen.h"

#define IMG_W 560
#define MARGIN 16
#define QR_MAX 300 // largest QR code in pixels
#define TEXT_COLS ((IMG_W - 2 * MARGIN) / 8)
#define PICK_ROWS 12 // file names shown at once
#define PICK_MAX 256

enum { D_NONE, D_LINK, D_NOTICE, D_PICK };
static int mode;
static bool dirty, mute_until_release, swallow_keyup;
static uint8_t *img;
static int img_w, img_h, img_x, img_y;

// picker
static char pick_title[256], pick_dir[512];
static char *pick_names[PICK_MAX];
static int pick_n, pick_sel, pick_top, pick_result; // result: 0 open/none, 1 chosen, -1 cancelled
static char pick_choice[512];
static bool choice; // the picker lists options (dialog_choice_open), not files
static int choice_result = -1; // option chosen, reported once
static int list_top_y; // image y of the picker's first row (for touches)

bool link_active(void) { return mode != D_NONE; }
bool link_mutes_pad(void) { return mode != D_NONE || mute_until_release; }

static const uint8_t c_bg[4] = {32, 32, 44, 240}, c_white[4] = {255, 255, 255, 255}, c_black[4] = {0, 0, 0, 255},
                     c_grey[4] = {170, 170, 190, 255}, c_yellow[4] = {248, 208, 88, 255}, c_sel[4] = {70, 80, 140, 255};

static void fill(int x, int y, int w, int h, const uint8_t c[4]) {
    for (int j = y; j < y + h && j < img_h; j++)
        for (int i = x; i < x + w && i < img_w; i++) memcpy(img + ((size_t)j * img_w + i) * 4, c, 4);
}

// Splits text into lines of at most TEXT_COLS characters (URLs have no spaces to break at).
static int wrap(const char *text, char lines[][TEXT_COLS + 1], int max_lines) {
    int n = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len && n < max_lines; n++) {
        size_t take = len - i;
        const char *nl = memchr(text + i, '\n', take < TEXT_COLS ? take : TEXT_COLS);
        if (nl) take = (size_t)(nl - (text + i));
        else if (take > TEXT_COLS) {
            take = TEXT_COLS;
            for (size_t k = TEXT_COLS; k > TEXT_COLS / 2; k--) // prefer a break after a space
                if (text[i + k - 1] == ' ') {
                    take = k;
                    break;
                }
        }
        memcpy(lines[n], text + i, take);
        lines[n][take] = 0;
        i += take + (nl ? 1 : 0);
    }
    return n;
}

static bool alloc_img(int h) {
    img_w = IMG_W, img_h = h;
    free(img);
    img = malloc((size_t)img_w * img_h * 4);
    if (!img) return false;
    fill(0, 0, img_w, img_h, c_bg);
    return true;
}

static void hint(int y, const char *text) {
    overlay_blit_text(img, img_w, img_h, img_w - MARGIN - (int)strlen(text) * 8, y, text, 1, c_grey);
}

void link_show(const char *url, const char *caption) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    int size = ok ? qrcodegen_getSize(qr) : 0, quiet = 4;
    int scale = size ? QR_MAX / (size + 2 * quiet) : 0;
    if (scale > 8) scale = 8;
    if (size && scale < 2) scale = 2;
    int qr_px = size ? (size + 2 * quiet) * scale : 0;

    char cap[4][TEXT_COLS + 1], addr[6][TEXT_COLS + 1];
    int ncap = wrap(caption, cap, 4), naddr = wrap(url, addr, 6);
    if (!alloc_img(MARGIN + ncap * 12 + 8 + qr_px + 10 + naddr * 11 + 10 + 10 + MARGIN)) return;
    int y = MARGIN;
    for (int i = 0; i < ncap; i++, y += 12) overlay_blit_text(img, img_w, img_h, MARGIN, y, cap[i], 1, c_white);
    y += 8;
    if (qr_px) {
        int x0 = (img_w - qr_px) / 2;
        fill(x0, y, qr_px, qr_px, c_white);
        for (int my = 0; my < size; my++)
            for (int mx = 0; mx < size; mx++)
                if (qrcodegen_getModule(qr, mx, my)) fill(x0 + (quiet + mx) * scale, y + (quiet + my) * scale, scale, scale, c_black);
    }
    y += qr_px + 10;
    for (int i = 0; i < naddr; i++, y += 11) overlay_blit_text(img, img_w, img_h, MARGIN, y, addr[i], 1, c_yellow);
    hint(y + 10, "X / O / START: close");
    mode = D_LINK, dirty = true;
    host_log("[link] shown as a QR code: %s", url);
}

void dialog_notice(const char *text) {
    char lines[16][TEXT_COLS + 1];
    int n = wrap(text, lines, 16);
    if (!alloc_img(MARGIN + n * 12 + 14 + 10 + MARGIN)) return;
    for (int i = 0; i < n; i++) overlay_blit_text(img, img_w, img_h, MARGIN, MARGIN + i * 12, lines[i], 1, c_white);
    hint(MARGIN + n * 12 + 14, "X / O / START: close");
    mode = D_NOTICE, dirty = true;
    host_log("[dialog] %s", text);
}

// ---------------------------------------------------------------- file picker

static void pick_render(void) {
    char lines[8][TEXT_COLS + 1];
    int nt = wrap(pick_title, lines, choice ? 8 : 3);
    int list_y = MARGIN + nt * 12 + 8, rows = choice ? pick_n : PICK_ROWS;
    list_top_y = list_y;
    if (!alloc_img(list_y + (pick_n ? rows : 2) * 14 + 10 + 10 + MARGIN)) return;
    for (int i = 0; i < nt; i++) overlay_blit_text(img, img_w, img_h, MARGIN, MARGIN + i * 12, lines[i], 1, c_white);
    if (!pick_n) {
        overlay_blit_text(img, img_w, img_h, MARGIN, list_y, "(no files here yet)", 1, c_grey);
    } else {
        if (pick_sel < pick_top) pick_top = pick_sel;
        if (pick_sel >= pick_top + PICK_ROWS) pick_top = pick_sel - PICK_ROWS + 1;
        for (int r = 0; r < PICK_ROWS && pick_top + r < pick_n; r++) {
            int i = pick_top + r, y = list_y + r * 14;
            if (i == pick_sel) fill(MARGIN - 4, y - 3, img_w - 2 * MARGIN + 8, 14, c_sel);
            char name[TEXT_COLS + 1];
            snprintf(name, sizeof name, "%s", pick_names[i]);
            overlay_blit_text(img, img_w, img_h, MARGIN, y, name, 1, i == pick_sel ? c_yellow : c_white);
        }
    }
    hint(img_h - MARGIN - 10, choice ? "X: choose" : pick_n ? "X: choose   O: cancel" : "O: cancel");
    dirty = true;
}

static int cmp_mtime(const void *a, const void *b) {
    // newest first: names were prefixed with a sortable modification time by pick_open
    return strcmp(*(char *const *)b, *(char *const *)a);
}

static bool ext_ok(const char *name, const char *exts) {
    if (!exts || !*exts) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", exts);
    for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", "))
        if (!strcasecmp(t, dot)) return true;
    return false;
}

// Opens the picker on dir (absolute) for files with one of the extensions (".a, .b"; empty: any).
void dialog_pick_open(const char *title, const char *dir, const char *exts) {
    for (int i = 0; i < pick_n; i++) free(pick_names[i]);
    pick_n = pick_sel = pick_top = pick_result = 0;
    choice = false;
    snprintf(pick_title, sizeof pick_title, "%s", title);
    snprintf(pick_dir, sizeof pick_dir, "%s", dir);
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && pick_n < PICK_MAX) {
        if (e->d_name[0] == '.' || !ext_ok(e->d_name, exts)) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat stt;
        if (stat(full, &stt) || !S_ISREG(stt.st_mode)) continue;
        char *s = malloc(strlen(e->d_name) + 24);
        if (!s) break;
        sprintf(s, "%020lld|%s", (long long)stt.st_mtime, e->d_name);
        pick_names[pick_n++] = s;
    }
    if (d) closedir(d);
    qsort(pick_names, pick_n, sizeof *pick_names, cmp_mtime);
    for (int i = 0; i < pick_n; i++) memmove(pick_names[i], strchr(pick_names[i], '|') + 1, strlen(strchr(pick_names[i], '|')));
    mode = D_PICK;
    pick_render();
    host_log("[dialog] file picker on %s: %d files", dir, pick_n);
}

// A choice between options (one per line of opts); the first is selected.
void dialog_choice_open(const char *title, const char *opts) {
    for (int i = 0; i < pick_n; i++) free(pick_names[i]);
    pick_n = pick_sel = pick_top = pick_result = 0;
    choice = true, choice_result = -1;
    snprintf(pick_title, sizeof pick_title, "%s", title);
    for (const char *p = opts; *p && pick_n < PICK_ROWS;) {
        size_t len = strcspn(p, "\n");
        char *s = malloc(len + 1);
        if (!s) break;
        memcpy(s, p, len), s[len] = 0;
        pick_names[pick_n++] = s;
        p += len + (p[len] ? 1 : 0);
    }
    mode = D_PICK;
    pick_render();
    host_log("[dialog] %s", title);
}

// -> the option chosen (reported once), or -1 while the choice is open or none was opened.
int dialog_choice_poll(void) {
    int r = choice_result;
    choice_result = -1;
    return r;
}

// -> 0 still open (or never opened), 1 chosen (path in *path), -1 cancelled; a result is reported once.
int dialog_pick_poll(const char **path) {
    int r = pick_result;
    if (r) pick_result = 0;
    *path = pick_choice;
    return r;
}

static void close_dialog(int pick) {
    if (mode == D_PICK && choice) {
        if (pick <= 0) return; // a choice can't be cancelled
        choice_result = pick_sel;
        host_log("[dialog] chose \"%s\"", pick_names[pick_sel]);
    } else if (mode == D_PICK) {
        pick_result = pick;
        if (pick > 0) snprintf(pick_choice, sizeof pick_choice, "%s/%s", pick_dir, pick_names[pick_sel]);
    }
    mode = D_NONE;
    mute_until_release = true; // the closing button's release isn't the game's
    swallow_keyup = true;
}

static void pick_move(int d) {
    if (!pick_n) return;
    pick_sel = (pick_sel + d + pick_n) % pick_n;
    pick_render();
}

bool link_event(const plat_event *ev) {
    if (mode == D_NONE) {
        if (swallow_keyup && ev->type == PEV_KEY_UP) {
            swallow_keyup = false;
            return true;
        }
        return false;
    }
    const char *k = ev->type == PEV_KEY_DOWN && ev->key ? ev->key : "";
    if (mode == D_PICK) {
        if (!strcmp(k, "ArrowUp")) pick_move(-1);
        else if (!strcmp(k, "ArrowDown")) pick_move(1);
        else if (!strcmp(k, "Enter") && pick_n) close_dialog(1);
        else if (!strcmp(k, "Escape")) close_dialog(-1);
        else if (ev->type == PEV_POINTER_DOWN && choice) {
            // a touch on an option picks it
            int dy = (int)ev->y - img_y - (list_top_y - 3), row = dy / 14;
            if (ev->x >= img_x && ev->x < img_x + img_w && dy >= 0 && row < pick_n) pick_sel = row, close_dialog(1);
        } else if (ev->type == PEV_POINTER_DOWN && pick_n) close_dialog(1); // a touch picks the highlighted entry
    } else if (ev->type == PEV_POINTER_DOWN || !strcmp(k, "Escape") || !strcmp(k, "Enter")) {
        close_dialog(0);
    }
    return true; // nothing reaches the game while a dialog is open
}

void link_frame(const plat_gamepad *pad) {
    // Standard gamepad indices: 0 cross, 1 circle, 9 start, 12/13 D-pad up/down
    static bool prev[18];
    static double repeat_at;
    bool now[18], any = false;
    for (int i = 0; i < 18; i++) any |= now[i] = pad && pad->buttons[i] > 0.5f;
    float ay = pad ? pad->axes[1] : 0;
    bool up = now[12] || ay < -0.6f, down = now[13] || ay > 0.6f;
    any |= up || down || (pad && (pad->axes[0] < -0.6f || pad->axes[0] > 0.6f));
    if (mute_until_release && mode == D_NONE && !any) mute_until_release = false;
    static bool pup, pdown;
    if (mode == D_PICK) {
        double t = host_now_ms();
        if ((up && !pup) || (down && !pdown)) {
            pick_move(up ? -1 : 1);
            repeat_at = t + 400;
        } else if ((up || down) && t >= repeat_at) {
            pick_move(up ? -1 : 1);
            repeat_at = t + 120;
        }
        if (now[0] && !prev[0] && pick_n) close_dialog(1);
        else if (now[1] && !prev[1]) close_dialog(-1);
    } else if (mode != D_NONE && ((now[0] && !prev[0]) || (now[1] && !prev[1]) || (now[9] && !prev[9]))) {
        close_dialog(0);
    }
    pup = up, pdown = down;
    memcpy(prev, now, sizeof prev);
}

bool link_draw(void) {
    if (mode == D_NONE || !img) return false;
    overlay_image_draw(img, img_w, img_h, dirty, &img_x, &img_y);
    dirty = false;
    return true;
}
