// SPDX-License-Identifier: AGPL-3.0-only
// Downloads the offline game to <data>/game.zip, from the start screen.
//
// The build is the one Admiral-Billy's PokeRogue App uses for its offline mode: game.zip from the
// latest release of github.com/Admiral-Billy/pokerogue (about 530 MB). It is downloaded to
// <data>/game.zip.part (an interrupted download resumes) and renamed when complete. The game is
// played straight from the zip (net.c): unpacking its 34,000 files took hours on the Vita.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "host.h"

#define RELEASE_API "https://api.github.com/repos/Admiral-Billy/pokerogue/releases/latest"
#define ASSET_NAME "game.zip"

typedef struct {
    double t0, last_draw;
    long long last_bytes;
    double speed; // bytes per second, smoothed
    const char *what;
} progress_ui;

// ○ (circle) or START + SELECT cancels.
static bool cancel_pressed(void) {
    plat_event ev;
    bool cancel = false;
    while (plat_poll_event(&ev))
        if (ev.type == PEV_QUIT || (ev.type == PEV_KEY_DOWN && ev.key && !strcmp(ev.key, "Escape"))) cancel = true;
    plat_gamepad pad;
    if (plat_gamepads(&pad, 1) > 0 && pad.buttons[1] > 0.5f) cancel = true;
    return cancel;
}

static bool show_progress(long long done, long long total, void *ud) {
    progress_ui *p = ud;
    double now = host_now_ms();
    if (now - p->last_draw < 250 && done != total) return !cancel_pressed();
    if (p->last_draw > 0) {
        double inst = (done - p->last_bytes) * 1000.0 / (now - p->last_draw);
        p->speed = p->speed > 0 ? p->speed * 0.8 + inst * 0.2 : inst;
    }
    p->last_draw = now, p->last_bytes = done;
    plat_keep_awake();
    char msg[600], eta[64] = "";
    if (total > 0 && p->speed > 1000) {
        int secs = (int)((total - done) / p->speed);
        snprintf(eta, sizeof eta, ", about %d:%02d left", secs / 60, secs % 60);
    }
    if (total > 0)
        snprintf(msg, sizeof msg, "%s\n\n%.0f of %.0f MB (%.1f MB/s%s)\n\nPress O to cancel. A cancelled download\ncontinues where it stopped next time.",
                 p->what, done / 1048576.0, total / 1048576.0, p->speed / 1048576.0, eta);
    else
        snprintf(msg, sizeof msg, "%s\n\n%.0f MB\n\nPress O to cancel.", p->what, done / 1048576.0);
    overlay_present(msg);
    return !cancel_pressed();
}

// Shows a message until a button is pressed (so errors can be read).
static void message_wait(const char *msg) {
    char full[800];
    snprintf(full, sizeof full, "%s\n\nPress X to go back.", msg);
    double t0 = host_now_ms();
    for (;;) {
        overlay_present(full);
        plat_event ev;
        bool done = false;
        while (plat_poll_event(&ev))
            if (ev.type == PEV_QUIT || (ev.type == PEV_KEY_DOWN && ev.key && !strcmp(ev.key, "Enter")) ||
                ev.type == PEV_POINTER_DOWN)
                done = true;
        plat_gamepad pad;
        if (host_now_ms() - t0 > 500 && plat_gamepads(&pad, 1) > 0 && (pad.buttons[0] > 0.5f || pad.buttons[1] > 0.5f))
            done = true;
        if (done) return;
#ifndef __vita__
        struct timespec ts = {0, 30 * 1000000L};
        nanosleep(&ts, NULL);
#endif
    }
}

// Finds "key":"value" after `from` in a JSON text (enough for GitHub's release JSON).
static bool json_str(const char *from, const char *key, char *out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(from, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ') p++;
    if (*p != '"') return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"' && o < n - 1) out[o++] = *p++;
    out[o] = 0;
    return true;
}

static long long json_num(const char *from, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(from, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    return atoll(p);
}

// Returns true when <data>/game.zip is ready.
bool offline_install(void) {
    char err[600] = "";
    overlay_present("PokeRogue\n\nLooking for the offline game on GitHub...");
    size_t len = 0;
    long status = 0;
    char *json = net_get_sync(RELEASE_API, &len, &status);
    if (!json || status != 200) {
        free(json);
        snprintf(err, sizeof err, "Couldn't reach GitHub (%s).\nCheck that Wi-Fi is connected.", status ? "HTTP error" : "no connection");
        message_wait(err);
        return false;
    }
    char tag[64] = "?", url[1024] = "";
    json_str(json, "tag_name", tag, sizeof tag);
    const char *asset = strstr(json, "\"name\":\"" ASSET_NAME "\"");
    if (!asset) asset = strstr(json, "\"name\": \"" ASSET_NAME "\"");
    long long size = asset ? json_num(asset, "size") : -1;
    if (!asset || !json_str(asset, "browser_download_url", url, sizeof url)) {
        free(json);
        message_wait("The latest release has no game.zip.\nTry again later.");
        return false;
    }
    free(json);

    char zip_path[700], game_zip[700], version_path[700];
    snprintf(zip_path, sizeof zip_path, "%s/game.zip.part", g_cfg.data_dir);
    snprintf(game_zip, sizeof game_zip, "%s/game.zip", g_cfg.data_dir);
    snprintf(version_path, sizeof version_path, "%s/game.zip.version", g_cfg.data_dir);
    long long need = size > 0 ? size + 16 * 1024 * 1024 : 0, have_part = 0, free_b = plat_free_space(g_cfg.data_dir);
    struct stat st;
    if (stat(zip_path, &st) == 0) have_part = st.st_size;
    if (free_b >= 0 && need > 0 && free_b + have_part < need) {
        snprintf(err, sizeof err, "Not enough space on the memory card:\n%.0f MB needed, %.0f MB free.",
                 (need - have_part) / 1048576.0, free_b / 1048576.0);
        message_wait(err);
        return false;
    }
    host_log("[offline] downloading %s (release %s, %.0f MB)", url, tag, size / 1048576.0);
    char what[160];
    snprintf(what, sizeof what, "Downloading the offline game (%s)", tag);
    progress_ui ui = {.t0 = host_now_ms(), .what = what};
    if (!net_download_file(url, zip_path, show_progress, &ui, err, sizeof err)) {
        host_log("[offline] download stopped: %s", err);
        if (strcmp(err, "cancelled")) {
            char m[700];
            snprintf(m, sizeof m, "The download stopped:\n%s\n\nIt continues where it stopped next time.", err);
            message_wait(m);
        }
        return false;
    }
    // Done: the game is played straight from the zip (no unpacking).
    if (rename(zip_path, game_zip) != 0) {
        message_wait("Couldn't rename the downloaded file.");
        return false;
    }
    FILE *vf = fopen(version_path, "w");
    if (vf) {
        fputs(!strncmp(tag, "release-", 8) ? tag + 8 : tag, vf);
        fclose(vf);
    }
    host_log("[offline] installed release %s as %s", tag, game_zip);
    return true;
}
