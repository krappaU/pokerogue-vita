// SPDX-License-Identifier: AGPL-3.0-only
// Start screen: play the live site (online) or the game copied to the memory card (offline).
// Shown before anything else loads, since the choice decides which game and which saves are used.
// D-pad / left stick and cross (or START), or touch; the choice is remembered for next time.
// Debug builds add the shader benchmark as a mode (when the build carries bench/) and the on/off
// options of config.txt as toggles, written back to config.txt; all of them are read after this.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gles.h"
#include "host.h"

#define W 600
#define H_MAX 540
#define BOX_X 24
#define BOX_W (W - 2 * BOX_X)
#define BOX_Y0 70
// Two options: tall boxes with two description lines; more (an offline update, the benchmark, the
// toggles below): compact ones with one line.
static bool compact(int n, int t) { return n > 2 || t > 0; }
static int box_h(int n, int t) { return compact(n, t) ? 58 : 96; }
static int box_gap(int n, int t) { return compact(n, t) ? 10 : 16; }
static int box_y(int i, int n, int t) { return BOX_Y0 + i * (box_h(n, t) + box_gap(n, t)); }

enum { M_ONLINE, M_OFFLINE, M_DOWNLOAD, M_UPDATE, M_BENCH };

#ifdef PKR_DEBUG
typedef struct {
    const char *label, *key;
    bool *v;
    bool inverted; // the field says "off" (no_*)
} toggle;
static const toggle toggles[] = {
    {"Game fixes (gamefix)", "gamefix", &g_cfg.no_gamefix, true},
    {"Shader fixes (shaderfix)", "shaderfix", &g_cfg.no_shader_fix, true},
    {"Precompile shaders", "precompile", &g_cfg.no_precompile, true},
    {"Profiler (sample)", "sample", &g_cfg.sample, false},
    {"Debug line (hud)", "hud", &g_cfg.hud, false},
    {"GPU profile (gpuprofile)", "gpuprofile", &g_cfg.gpu_profile, false},
    {"Plain shaders (measuring)", "plainshaders", &g_cfg.plain_shaders, false},
    {"API headers (nettrace)", "nettrace", &g_cfg.net_trace, false},
};
#define N_TOGGLES (int)(sizeof toggles / sizeof toggles[0])
static bool toggle_on(int i) { return *toggles[i].v != toggles[i].inverted; }
#else
#define N_TOGGLES 0
#endif
#define TOG_H 26
#define TOG_GAP 6
#define TOG_W ((BOX_W - 10) / 2)
static int tog_y0(int n) { return box_y(n, n, N_TOGGLES) + 8; }
static void tog_rect(int i, int n, int *x, int *y) { *x = BOX_X + (i % 2) * (TOG_W + 10), *y = tog_y0(n) + (i / 2) * (TOG_H + TOG_GAP); }
static int img_h(int n) {
    if (!N_TOGGLES) return 330;
    return tog_y0(n) + (N_TOGGLES + 1) / 2 * (TOG_H + TOG_GAP) - TOG_GAP + 56;
}

static uint8_t img[W * H_MAX * 4];
static int H;

static void fill(int x, int y, int w, int h, const uint8_t c[4]) {
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < W && j < H) memcpy(img + ((size_t)j * W + i) * 4, c, 4);
}

static void frame_rect(int x, int y, int w, int h, int t, const uint8_t c[4]) {
    fill(x, y, w, t, c), fill(x, y + h - t, w, t, c), fill(x, y, t, h, c), fill(x + w - t, y, t, h, c);
}

static void text(int x, int y, const char *s, int scale, const uint8_t c[4]) { overlay_blit_text(img, W, H, x, y, s, scale, c); }

static void render(int sel, const int *modes, int n, const char *offline_version, const char *update) {
    static const uint8_t bg[4] = {40, 30, 48, 255}, red[4] = {200, 60, 50, 255}, box[4] = {64, 52, 74, 255},
                         hi[4] = {232, 168, 56, 255}, fg[4] = {245, 235, 250, 255}, dim[4] = {150, 140, 160, 255},
                         dark[4] = {30, 22, 36, 255};
    H = img_h(n);
    fill(0, 0, W, H, bg);
    frame_rect(0, 0, W, H, 3, red);
    text(BOX_X, 24, "PokeRogue", 3, fg);
    char line1[120], line2[120];
    bool small = compact(n, N_TOGGLES);
    for (int i = 0; i < n; i++) {
        int y = box_y(i, n, N_TOGGLES);
        bool s = i == sel;
        fill(BOX_X, y, BOX_W, box_h(n, N_TOGGLES), s ? hi : box);
        const uint8_t *tc = s ? dark : fg;
        const char *title, *a, *b;
        if (modes[i] == M_ONLINE) {
            title = "Play online";
            a = "pokerogue.net: your account and cloud saves.";
            b = "Needs Wi-Fi; downloads the game as you play.";
        } else if (modes[i] == M_OFFLINE) {
            title = "Play offline";
            snprintf(line1, sizeof line1, "The game on the memory card (version %s).", offline_version);
            a = line1;
            b = "No login; saves stay on this Vita.";
        } else if (modes[i] == M_DOWNLOAD) {
            title = "Download offline game";
            a = small ? "About 530 MB to the memory card; no login, no downloads in game."
                      : "About 530 MB to the memory card, played straight";
            b = "from the download: no login, no downloads in game.";
        } else if (modes[i] == M_UPDATE) {
            title = "Update offline game";
            snprintf(line2, sizeof line2, "Version %s is out. About 530 MB; saves are kept.", update);
            a = line2;
            b = NULL;
        } else {
            title = "Shader benchmark";
            a = "GPU time of the game's shaders; the log is sent by itself.";
            b = NULL;
        }
        text(BOX_X + 16, y + (small ? 10 : 14), title, 2, tc);
        text(BOX_X + 16, y + (small ? 36 : 44), a, 1, tc);
        if (!small && b) text(BOX_X + 16, y + 60, b, 1, tc);
    }
#ifdef PKR_DEBUG
    static const uint8_t on[4] = {120, 220, 130, 255};
    for (int i = 0; i < N_TOGGLES; i++) {
        int x, y;
        tog_rect(i, n, &x, &y);
        bool s = n + i == sel;
        fill(x, y, TOG_W, TOG_H, s ? hi : box);
        text(x + 10, y + 9, toggles[i].label, 1, s ? dark : fg);
        const char *st = toggle_on(i) ? "ON" : "off";
        text(x + TOG_W - 10 - 8 * (int)strlen(st), y + 9, st, 1, s ? dark : toggle_on(i) ? on : dim);
    }
    text(BOX_X, H - 46, "D-pad and X (or touch): play, or switch an option (saved).", 1, dim);
#else
    text(BOX_X, H - 46, "D-pad and X (or touch) to choose. Your choice is remembered.", 1, dim);
#endif
    text(BOX_X, H - 26, host_version_string(), 1, dim);
}

// The mode box or toggle (n + its index) under a touch, or -1.
static int hit(int n, float px, float py, int ox, int oy) {
    int x = (int)px - ox, y = (int)py - oy;
    for (int b = 0; b < n; b++) {
        int by = box_y(b, n, N_TOGGLES);
        if (x >= BOX_X && x < BOX_X + BOX_W && y >= by && y < by + box_h(n, N_TOGGLES)) return b;
    }
    for (int i = 0; i < N_TOGGLES; i++) {
        int tx, ty;
        tog_rect(i, n, &tx, &ty);
        if (x >= tx && x < tx + TOG_W && y >= ty && y < ty + TOG_H) return n + i;
    }
    return -1;
}

// Up/down through the modes and then the toggles' rows (two per row); left/right between columns.
static int move(int sel, int n, int dy, int dx) {
    int last = n + N_TOGGLES - 1, t = sel - n;
    if (dx) return t >= 0 && (t ^ 1) < N_TOGGLES ? n + (t ^ 1) : sel;
    if (dy < 0) return sel == 0 ? last : t < 0 ? sel - 1 : t < 2 ? n - 1 : sel - 2;
    if (t < 0) return sel < last ? sel + 1 : 0;
    return t + 2 < N_TOGGLES ? sel + 2 : 0;
}

// Cross on a toggle: flips it, for this run and in config.txt.
static void flip(int i) {
#ifdef PKR_DEBUG
    *toggles[i].v = !*toggles[i].v;
    config_set(toggles[i].key, toggle_on(i) ? "1" : "0");
    host_log("[start] %s=%d", toggles[i].key, toggle_on(i));
#else
    (void)i;
#endif
}

static void choice_path(char *out, size_t n) { snprintf(out, n, "%s/launcher.txt", g_cfg.data_dir); }

static int list_modes(int *modes, bool offline_ok, const char *update, bool bench_ok) {
    int n = 0;
    modes[n++] = M_ONLINE;
    modes[n++] = offline_ok ? M_OFFLINE : M_DOWNLOAD;
    if (bench_ok) modes[n++] = M_BENCH;
    if (update) modes[n++] = M_UPDATE;
    return n;
}

// Returns 0 for online, 1 for offline, 2 to download the offline game, 3 to update it, 4 for the
// shader benchmark, -1 if the user quit. With the offline game installed, a newer release is
// looked for in the background.
int launcher_run(bool offline_ok, const char *offline_version, bool bench_ok, void (*poll)(bool *quit)) {
    if (offline_ok) offline_update_check_start();
    const char *update = NULL;
    char path[600], saved[32] = "";
    choice_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fgets(saved, sizeof saved, f)) saved[strcspn(saved, "\r\n")] = 0;
        fclose(f);
    }
    int modes[5], n = list_modes(modes, offline_ok, update, bench_ok);
    int sel = 0;
    for (int i = 0; i < n; i++)
        if ((modes[i] == M_OFFLINE && !strcmp(saved, "offline")) || (modes[i] == M_BENCH && !strcmp(saved, "bench"))) sel = i;
    bool prev[18] = {0}, prev_dir[4] = {0}, dirty = true, chosen = false, quit = false;
    int ox = 0, oy = 0;
    // Buttons held from the previous app (or from launching) don't count until released.
    bool armed = false;
    while (!chosen && !quit) {
        poll(&quit);
        if (offline_ok && !update && (update = offline_update_available(offline_version))) {
            n = list_modes(modes, offline_ok, update, bench_ok);
            if (sel >= n - 1) sel++; // a toggle stays the one selected
            dirty = true;
        }
        int pick = -1; // a mode chosen or a toggle pressed
        plat_event ev;
        while (plat_poll_event(&ev)) {
            if (ev.type == PEV_QUIT) quit = true;
            else if (ev.type == PEV_KEY_DOWN && ev.key) {
                if (!strcmp(ev.key, "ArrowUp")) sel = move(sel, n, -1, 0), dirty = true;
                else if (!strcmp(ev.key, "ArrowDown")) sel = move(sel, n, 1, 0), dirty = true;
                else if (!strcmp(ev.key, "ArrowLeft") || !strcmp(ev.key, "ArrowRight")) sel = move(sel, n, 0, 1), dirty = true;
                else if (!strcmp(ev.key, "Enter") || !strcmp(ev.key, " ")) pick = sel;
            } else if (ev.type == PEV_POINTER_DOWN) {
                int b = hit(n, ev.x, ev.y, ox, oy);
                if (b >= 0) sel = pick = b, dirty = true;
            }
        }
        plat_gamepad pad;
        if (plat_gamepads(&pad, 1) > 0) {
            bool now[18], dir[4] = {pad.buttons[12] > 0.5f || pad.axes[1] < -0.6f, pad.buttons[13] > 0.5f || pad.axes[1] > 0.6f,
                                    pad.buttons[14] > 0.5f || pad.axes[0] < -0.6f, pad.buttons[15] > 0.5f || pad.axes[0] > 0.6f};
            bool any = dir[0] || dir[1] || dir[2] || dir[3];
            for (int i = 0; i < 18; i++) now[i] = pad.buttons[i] > 0.5f, any |= now[i];
            if (!armed) armed = !any;
            else {
                if (dir[0] && !prev_dir[0]) sel = move(sel, n, -1, 0), dirty = true;
                if (dir[1] && !prev_dir[1]) sel = move(sel, n, 1, 0), dirty = true;
                if ((dir[2] && !prev_dir[2]) || (dir[3] && !prev_dir[3])) sel = move(sel, n, 0, 1), dirty = true;
                if ((now[0] && !prev[0]) || (now[9] && !prev[9])) pick = sel;
            }
            memcpy(prev, now, sizeof prev), memcpy(prev_dir, dir, sizeof prev_dir);
        }
        if (pick >= n) flip(pick - n), dirty = true;
        else if (pick >= 0) chosen = true;

        int sw, sh;
        plat_drawable_size(&sw, &sh);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, sw, sh);
        glClearColor(0.10f, 0.07f, 0.12f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        if (dirty) render(sel, modes, n, offline_version, update);
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
    int m = modes[sel];
    if (m == M_DOWNLOAD) return 2;
    if (m == M_UPDATE) return 3;
    f = fopen(path, "w");
    if (f) {
        fputs(m == M_OFFLINE ? "offline\n" : m == M_BENCH ? "bench\n" : "online\n", f);
        fclose(f);
    }
    return m == M_BENCH ? 4 : m == M_OFFLINE ? 1 : 0;
}
