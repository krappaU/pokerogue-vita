// SPDX-License-Identifier: AGPL-3.0-only
// Links the game opens (window.open: the Wiki, Discord, GitHub... entries of its menu) can't be
// followed on the Vita. They are shown as a QR code to scan with a phone, with the address below.
// Cross, circle or START (or a touch, or Enter/Escape on the desktop) closes it.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "qrcodegen.h"

#define IMG_W 560
#define MARGIN 16
#define QR_MAX 300 // largest QR code in pixels
#define TEXT_COLS ((IMG_W - 2 * MARGIN) / 8)

static bool open_, dirty, mute_until_release, swallow_keyup;
static uint8_t *img;
static int img_w, img_h;

bool link_active(void) { return open_; }
bool link_mutes_pad(void) { return open_ || mute_until_release; }

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
        if (take > TEXT_COLS) {
            take = TEXT_COLS;
            for (size_t k = TEXT_COLS; k > TEXT_COLS / 2; k--) // prefer a break after a space
                if (text[i + k - 1] == ' ') {
                    take = k;
                    break;
                }
        }
        memcpy(lines[n], text + i, take);
        lines[n][take] = 0;
        i += take;
    }
    return n;
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
    const char *hint = "X / O / START: close";
    img_w = IMG_W;
    img_h = MARGIN + ncap * 12 + 8 + qr_px + 10 + naddr * 11 + 10 + 10 + MARGIN;
    free(img);
    img = malloc((size_t)img_w * img_h * 4);
    if (!img) return;
    static const uint8_t bg[4] = {32, 32, 44, 240}, white[4] = {255, 255, 255, 255}, black[4] = {0, 0, 0, 255},
                         grey[4] = {170, 170, 190, 255}, yellow[4] = {248, 208, 88, 255};
    fill(0, 0, img_w, img_h, bg);
    int y = MARGIN;
    for (int i = 0; i < ncap; i++, y += 12) overlay_blit_text(img, img_w, img_h, MARGIN, y, cap[i], 1, white);
    y += 8;
    if (qr_px) {
        int x0 = (img_w - qr_px) / 2;
        fill(x0, y, qr_px, qr_px, white);
        for (int my = 0; my < size; my++)
            for (int mx = 0; mx < size; mx++)
                if (qrcodegen_getModule(qr, mx, my)) fill(x0 + (quiet + mx) * scale, y + (quiet + my) * scale, scale, scale, black);
    }
    y += qr_px + 10;
    for (int i = 0; i < naddr; i++, y += 11) overlay_blit_text(img, img_w, img_h, MARGIN, y, addr[i], 1, yellow);
    y += 10;
    overlay_blit_text(img, img_w, img_h, img_w - MARGIN - (int)strlen(hint) * 8, y, hint, 1, grey);
    open_ = dirty = true;
    host_log("[link] shown as a QR code: %s", url);
}

static void close_link(void) {
    open_ = false;
    mute_until_release = true; // the closing button's release isn't the game's
    swallow_keyup = true;
}

bool link_event(const plat_event *ev) {
    if (!open_) {
        if (swallow_keyup && ev->type == PEV_KEY_UP) {
            swallow_keyup = false;
            return true;
        }
        return false;
    }
    if (ev->type == PEV_POINTER_DOWN ||
        (ev->type == PEV_KEY_DOWN && ev->key && (!strcmp(ev->key, "Escape") || !strcmp(ev->key, "Enter"))))
        close_link();
    return true; // nothing reaches the game while the code is shown
}

void link_frame(const plat_gamepad *pad) {
    // Standard gamepad indices: 0 cross, 1 circle, 9 start
    static bool prev[18];
    bool now[18], any = false;
    for (int i = 0; i < 18; i++) any |= now[i] = pad && pad->buttons[i] > 0.5f;
    if (pad) any |= pad->axes[0] < -0.6f || pad->axes[0] > 0.6f || pad->axes[1] < -0.6f || pad->axes[1] > 0.6f;
    if (mute_until_release && !open_ && !any) mute_until_release = false;
    if (open_ && ((now[0] && !prev[0]) || (now[1] && !prev[1]) || (now[9] && !prev[9]))) close_link();
    memcpy(prev, now, sizeof prev);
}

bool link_draw(void) {
    if (!open_ || !img) return false;
    int ox, oy;
    overlay_image_draw(img, img_w, img_h, dirty, &ox, &oy);
    dirty = false;
    return true;
}
