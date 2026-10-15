// SPDX-License-Identifier: AGPL-3.0-only
// Start screen: play the live site (online) or the game copied to the memory card (offline).
// Shown before anything else loads, since the choice decides which game and which saves are used.
// D-pad / left stick and cross (or START), or touch; the choice is remembered for next time.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gles.h"
#include "host.h"

#define W 600
#define H 330
#define BOX_X 24
#define BOX_W (W - 2 * BOX_X)
#define BOX_H 96
#define BOX_Y0 70
#define BOX_GAP 16

static uint8_t img[W * H * 4];

static void fill(int x, int y, int w, int h, const uint8_t c[4]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < W && j < H) memcpy(img + ((size_t)j * W + i) * 4, c, 4);
}

static void frame_rect(int x, int y, int w, int h, int t, const uint8_t c[4]) {
    fill(x, y, w, t, c), fill(x, y + h - t, w, t, c), fill(x, y, t, h, c), fill(x + w - t, y, t, h, c);
}

static void text(int x, int y, const char *s, int scale, const uint8_t c[4]) { overlay_blit_text(img, W, H, x, y, s, scale, c); }

static void render(int sel, bool offline_ok, const char *offline_version) {
    static const uint8_t bg[4] = {40, 30, 48, 255}, red[4] = {200, 60, 50, 255}, box[4] = {64, 52, 74, 255},
                         hi[4] = {232, 168, 56, 255}, fg[4] = {245, 235, 250, 255}, dim[4] = {150, 140, 160, 255},
                         dark[4] = {30, 22, 36, 255};
    fill(0, 0, W, H, bg);
    frame_rect(0, 0, W, H, 3, red);
    text(BOX_X, 24, "PokeRogue", 3, fg);
    char line1[120], line2[120];
    for (int i = 0; i < 2; i++) {
        int y = BOX_Y0 + i * (BOX_H + BOX_GAP);
        bool s = i == sel, disabled = i == 1 && !offline_ok;
        fill(BOX_X, y, BOX_W, BOX_H, s ? hi : box);
        const uint8_t *tc = s ? dark : disabled ? dim : fg;
        if (i == 0) {
            text(BOX_X + 16, y + 14, "Play online", 2, tc);
            text(BOX_X + 16, y + 44, "pokerogue.net: your account and cloud saves.", 1, tc);
            text(BOX_X + 16, y + 60, "Needs Wi-Fi; downloads the game as you play.", 1, tc);
        } else if (offline_ok) {
            text(BOX_X + 16, y + 14, "Play offline", 2, tc);
            snprintf(line1, sizeof line1, "The game on the memory card (version %s).", offline_version);
            snprintf(line2, sizeof line2, "No login; saves stay on this Vita, apart from online ones.");
            text(BOX_X + 16, y + 44, line1, 1, tc);
            text(BOX_X + 16, y + 60, line2, 1, tc);
        } else {
            text(BOX_X + 16, y + 14, "Play offline", 2, tc);
            text(BOX_X + 16, y + 44, "Not installed: copy the offline game to", 1, tc);
            text(BOX_X + 16, y + 60, "ux0:data/pokerogue/game (see the download page).", 1, tc);
        }
    }
    text(BOX_X, H - 46, "D-pad and X (or touch) to choose. Your choice is remembered.", 1, dim);
    text(BOX_X, H - 26, host_version_string(), 1, dim);
}

static bool hit(int box, float px, float py, int ox, int oy) {
    int x = (int)px - ox, y = (int)py - oy, by = BOX_Y0 + box * (BOX_H + BOX_GAP);
    return x >= BOX_X && x < BOX_X + BOX_W && y >= by && y < by + BOX_H;
}

static void choice_path(char *out, size_t n) { snprintf(out, n, "%s/launcher.txt", g_cfg.data_dir); }

// Returns 0 for online, 1 for offline, -1 if the user quit (START + SELECT).
int launcher_run(bool offline_ok, const char *offline_version, void (*poll)(bool *quit)) {
    char path[600], saved[32] = "";
    choice_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(saved, sizeof saved, f)) saved[strcspn(saved, "\r\n")] = 0;
        fclose(f);
    }
    int sel = offline_ok && !strcmp(saved, "offline") ? 1 : 0;
    bool prev[18] = {0}, prev_dir[2] = {0}, dirty = true, chosen = false, quit = false;
    int ox = 0, oy = 0;
    // Buttons held from the previous app (or from launching) don't count until released.
    bool armed = false;
    while (!chosen && !quit) {
        poll(&quit);
        plat_event ev;
        while (plat_poll_event(&ev)) {
            if (ev.type == PEV_QUIT) quit = true;
            else if (ev.type == PEV_KEY_DOWN && ev.key) {
                if (!strcmp(ev.key, "ArrowUp") || !strcmp(ev.key, "ArrowDown")) sel = !sel, dirty = true;
                else if (!strcmp(ev.key, "Enter") || !strcmp(ev.key, " ")) chosen = true;
            } else if (ev.type == PEV_POINTER_DOWN) {
                for (int b = 0; b < 2; b++)
                    if (hit(b, ev.x, ev.y, ox, oy)) sel = b, chosen = true, dirty = true;
            }
        }
        plat_gamepad pad;
        if (plat_gamepads(&pad, 1) > 0) {
            bool now[18], dir[2] = {pad.buttons[12] > 0.5f || pad.axes[1] < -0.6f, pad.buttons[13] > 0.5f || pad.axes[1] > 0.6f};
            bool any = dir[0] || dir[1];
            for (int i = 0; i < 18; i++) now[i] = pad.buttons[i] > 0.5f, any |= now[i];
            if (!armed) armed = !any;
            else {
                if ((dir[0] && !prev_dir[0]) || (dir[1] && !prev_dir[1])) sel = !sel, dirty = true;
                if ((now[0] && !prev[0]) || (now[9] && !prev[9])) chosen = true;
            }
            memcpy(prev, now, sizeof prev), memcpy(prev_dir, dir, sizeof prev_dir);
        }
        if (chosen && sel == 1 && !offline_ok) chosen = false; // not installed: stay
        int sw, sh;
        plat_drawable_size(&sw, &sh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, sw, sh);
        glClearColor(0.10f, 0.07f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (dirty) render(sel, offline_ok, offline_version);
        overlay_image_draw(img, W, H, dirty, &ox, &oy);
        dirty = false;
        plat_swap();
#ifndef __vita__
        struct timespec ts = {0, 16 * 1000000L};
        nanosleep(&ts, NULL);
#endif
    }
    if (quit) return -1;
    f = fopen(path, "w");
    if (f) {
        fputs(sel ? "offline\n" : "online\n", f);
        fclose(f);
    }
    return sel;
}
