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
#define BOX_Y0 70
// Two options: tall boxes with two description lines; three (an offline update is available):
// compact ones with one line.
static int box_h(int n) { return n > 2 ? 58 : 96; }
static int box_gap(int n) { return n > 2 ? 10 : 16; }

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

static void render(int sel, bool offline_ok, const char *offline_version, const char *update) {
    static const uint8_t bg[4] = {40, 30, 48, 255}, red[4] = {200, 60, 50, 255}, box[4] = {64, 52, 74, 255},
                         hi[4] = {232, 168, 56, 255}, fg[4] = {245, 235, 250, 255}, dim[4] = {150, 140, 160, 255},
                         dark[4] = {30, 22, 36, 255};
    fill(0, 0, W, H, bg);
    frame_rect(0, 0, W, H, 3, red);
    text(BOX_X, 24, "PokeRogue", 3, fg);
    char line1[120], line2[120];
    int n = update ? 3 : 2;
    bool compact = n > 2;
    for (int i = 0; i < n; i++) {
        int y = BOX_Y0 + i * (box_h(n) + box_gap(n));
        bool s = i == sel;
        fill(BOX_X, y, BOX_W, box_h(n), s ? hi : box);
        const uint8_t *tc = s ? dark : fg;
        const char *title, *a, *b;
        if (i == 0) {
            title = "Play online";
            a = "pokerogue.net: your account and cloud saves.";
            b = "Needs Wi-Fi; downloads the game as you play.";
        } else if (i == 1 && offline_ok) {
            title = "Play offline";
            snprintf(line1, sizeof line1, "The game on the memory card (version %s).", offline_version);
            a = line1;
            b = "No login; saves stay on this Vita.";
        } else if (i == 1) {
            title = "Download offline game";
            a = "About 530 MB to the memory card, played straight";
            b = "from the download: no login, no downloads in game.";
        } else {
            title = "Update offline game";
            snprintf(line2, sizeof line2, "Version %s is out. About 530 MB; saves are kept.", update);
            a = line2;
            b = NULL;
        }
        text(BOX_X + 16, y + (compact ? 10 : 14), title, 2, tc);
        text(BOX_X + 16, y + (compact ? 36 : 44), a, 1, tc);
        if (!compact && b) text(BOX_X + 16, y + 60, b, 1, tc);
    }
    text(BOX_X, H - 46, "D-pad and X (or touch) to choose. Your choice is remembered.", 1, dim);
    text(BOX_X, H - 26, host_version_string(), 1, dim);
}

static bool hit(int box, int n, float px, float py, int ox, int oy) {
    int x = (int)px - ox, y = (int)py - oy, by = BOX_Y0 + box * (box_h(n) + box_gap(n));
    return x >= BOX_X && x < BOX_X + BOX_W && y >= by && y < by + box_h(n);
}

static void choice_path(char *out, size_t n) { snprintf(out, n, "%s/launcher.txt", g_cfg.data_dir); }

// Returns 0 for online, 1 for offline, 2 to download the offline game, 3 to update it, -1 if the
// user quit. With the offline game installed, a newer release is looked for in the background.
int launcher_run(bool offline_ok, const char *offline_version, void (*poll)(bool *quit)) {
    if (offline_ok) offline_update_check_start();
    const char *update = NULL;
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
        if (offline_ok && !update && (update = offline_update_available(offline_version))) dirty = true;
        int n = update ? 3 : 2;
        plat_event ev;
        while (plat_poll_event(&ev)) {
            if (ev.type == PEV_QUIT) quit = true;
            else if (ev.type == PEV_KEY_DOWN && ev.key) {
                if (!strcmp(ev.key, "ArrowUp")) sel = (sel + n - 1) % n, dirty = true;
                else if (!strcmp(ev.key, "ArrowDown")) sel = (sel + 1) % n, dirty = true;
                else if (!strcmp(ev.key, "Enter") || !strcmp(ev.key, " ")) chosen = true;
            } else if (ev.type == PEV_POINTER_DOWN) {
                for (int b = 0; b < n; b++)
                    if (hit(b, n, ev.x, ev.y, ox, oy)) sel = b, chosen = true, dirty = true;
            }
        }
        plat_gamepad pad;
        if (plat_gamepads(&pad, 1) > 0) {
            bool now[18], dir[2] = {pad.buttons[12] > 0.5f || pad.axes[1] < -0.6f, pad.buttons[13] > 0.5f || pad.axes[1] > 0.6f};
            bool any = dir[0] || dir[1];
            for (int i = 0; i < 18; i++) now[i] = pad.buttons[i] > 0.5f, any |= now[i];
            if (!armed) armed = !any;
            else {
                if (dir[0] && !prev_dir[0]) sel = (sel + n - 1) % n, dirty = true;
                if (dir[1] && !prev_dir[1]) sel = (sel + 1) % n, dirty = true;
                if ((now[0] && !prev[0]) || (now[9] && !prev[9])) chosen = true;
            }
            memcpy(prev, now, sizeof prev), memcpy(prev_dir, dir, sizeof prev_dir);
        }

        int sw, sh;
        plat_drawable_size(&sw, &sh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, sw, sh);
        glClearColor(0.10f, 0.07f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (dirty) render(sel, offline_ok, offline_version, update);
        overlay_image_draw(img, W, H, dirty, &ox, &oy);
        dirty = false;
        host_frame_drawn();
        plat_swap();
#ifndef __vita__
        struct timespec ts = {0, 16 * 1000000L};
        nanosleep(&ts, NULL);
#endif
    }
    if (quit) return -1;
    if (sel == 1 && !offline_ok) return 2; // download it
    if (sel == 2) return 3;                // update it
    f = fopen(path, "w");
    if (f) {
        fputs(sel ? "offline\n" : "online\n", f);
        fclose(f);
    }
    return sel;
}
