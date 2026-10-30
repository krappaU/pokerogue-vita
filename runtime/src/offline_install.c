// SPDX-License-Identifier: AGPL-3.0-only
// Downloads the offline game to <data>/game.zip, from the start screen.
//
// The build is the one Admiral-Billy's PokeRogue App uses for its offline mode: game.zip from the
// latest release of github.com/Admiral-Billy/pokerogue (about 530 MB). It is downloaded to
// <data>/game.zip.part (an interrupted download resumes) and renamed when complete. The game is
// played straight from the zip (net.c): unpacking its 34,000 files took hours on the Vita.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "host.h"
#include "miniz.h"

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
void ui_message_wait(const char *msg) {
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
bool json_str(const char *from, const char *key, char *out, size_t n) {
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

long long json_num(const char *from, const char *key) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(from, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    return atoll(p);
}

// ---------------------------------------------------------------- update check
// The start screen asks GitHub for the latest release in the background (at most every 6 hours;
// the answer is kept in <data>/offline-latest.txt) and offers an update when its tag differs from
// the installed version.
#define CHECK_EVERY_S (6 * 3600)
static pthread_mutex_t check_lock = PTHREAD_MUTEX_INITIALIZER;
static char latest[64];   // newest release tag without "release-", "" while unknown
static bool check_running;

static const char *strip_tag(const char *tag) { return !strncmp(tag, "release-", 8) ? tag + 8 : tag; }

static void latest_path(char *out, size_t n) { snprintf(out, n, "%s/offline-latest.txt", g_cfg.data_dir); }

static void *check_thread(void *arg) {
    (void)arg;
    size_t len = 0;
    long status = 0;
    char *json = net_get_threadsafe(RELEASE_API, &len, &status);
    char tag[64] = "";
    if (json && status == 200 && json_str(json, "tag_name", tag, sizeof tag) &&
        (strstr(json, "\"name\":\"" ASSET_NAME "\"") || strstr(json, "\"name\": \"" ASSET_NAME "\""))) {
        char path[700];
        latest_path(path, sizeof path);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%lld %s\n", (long long)time(NULL), strip_tag(tag));
            fclose(f);
        }
        host_log("[offline] latest release on GitHub: %s", tag);
    } else {
        host_log("[offline] update check failed (HTTP %ld)", status);
        tag[0] = 0;
    }
    free(json);
    pthread_mutex_lock(&check_lock);
    if (tag[0]) snprintf(latest, sizeof latest, "%s", strip_tag(tag));
    check_running = false;
    pthread_mutex_unlock(&check_lock);
    return NULL;
}

void offline_update_check_start(void) {
    char path[700], tag[64] = "";
    long long when = 0;
    latest_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        if (fscanf(f, "%lld %63s", &when, tag) != 2) tag[0] = 0;
        fclose(f);
    }
    pthread_mutex_lock(&check_lock);
    if (tag[0]) snprintf(latest, sizeof latest, "%s", tag);
    bool due = !tag[0] || time(NULL) - when > CHECK_EVERY_S || time(NULL) < when;
    if (due && !check_running) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 256 * 1024); // TLS handshakes need a deep stack
        check_running = pthread_create(&t, &attr, check_thread, NULL) == 0;
        if (check_running) pthread_detach(t);
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_unlock(&check_lock);
}

// The newer release's tag, or NULL (no update known, or `installed` is the latest).
const char *offline_update_available(const char *installed) {
    static char out[64];
    pthread_mutex_lock(&check_lock);
    bool newer = latest[0] && installed && installed[0] && strcmp(latest, installed) != 0;
    if (newer) snprintf(out, sizeof out, "%s", latest);
    pthread_mutex_unlock(&check_lock);
    return newer ? out : NULL;
}

// Asks a yes/no question: X yes, O no.
bool ui_confirm(const char *msg) {
    char full[900];
    snprintf(full, sizeof full, "%s\n\nX: yes      O: no", msg);
    double t0 = host_now_ms();
    for (;;) {
        overlay_present(full);
        plat_event ev;
        while (plat_poll_event(&ev)) {
            if (ev.type == PEV_QUIT || (ev.type == PEV_KEY_DOWN && ev.key && !strcmp(ev.key, "Escape"))) return false;
            if (ev.type == PEV_KEY_DOWN && ev.key && !strcmp(ev.key, "Enter")) return true;
        }
        plat_gamepad pad;
        if (host_now_ms() - t0 > 500 && plat_gamepads(&pad, 1) > 0) {
            if (pad.buttons[0] > 0.5f) return true;
            if (pad.buttons[1] > 0.5f) return false;
        }
#ifndef __vita__
        struct timespec ts = {0, 30 * 1000000L};
        nanosleep(&ts, NULL);
#endif
    }
}

// ---------------------------------------------------------------- download

bool ui_download(const char *url, const char *path, const char *what, char *err, size_t errn) {
    progress_ui ui = {.t0 = host_now_ms(), .what = what};
    return net_download_file(url, path, show_progress, &ui, err, errn);
}

// Downloads the latest release to <data>/game.zip (replacing an installed one only once the
// download is complete). Returns true when <data>/game.zip is ready.
bool offline_install(void) {
    char err[600] = "";
    overlay_present("PokeRogue\n\nLooking for the offline game on GitHub...");
    size_t len = 0;
    long status = 0;
    char *json = net_get_sync(RELEASE_API, &len, &status);
    if (!json || status != 200) {
        free(json);
        snprintf(err, sizeof err, "Couldn't reach GitHub (%s).\nCheck that Wi-Fi is connected.", status ? "HTTP error" : "no connection");
        ui_message_wait(err);
        return false;
    }
    char tag[64] = "?", url[1024] = "";
    json_str(json, "tag_name", tag, sizeof tag);
    const char *asset = strstr(json, "\"name\":\"" ASSET_NAME "\"");
    if (!asset) asset = strstr(json, "\"name\": \"" ASSET_NAME "\"");
    long long size = asset ? json_num(asset, "size") : -1;
    if (!asset || !json_str(asset, "browser_download_url", url, sizeof url)) {
        free(json);
        ui_message_wait("The latest release has no game.zip.\nTry again later.");
        return false;
    }
    free(json);

    char zip_path[700], game_zip[700], version_path[700];
    snprintf(zip_path, sizeof zip_path, "%s/game.zip.part", g_cfg.data_dir);
    snprintf(game_zip, sizeof game_zip, "%s/game.zip", g_cfg.data_dir);
    snprintf(version_path, sizeof version_path, "%s/game.zip.version", g_cfg.data_dir);
    // A partial download of a different release can't be resumed.
    char part_tag_path[720], part_tag[64] = "";
    snprintf(part_tag_path, sizeof part_tag_path, "%s.tag", zip_path);
    FILE *pt = fopen(part_tag_path, "r");
    if (pt) {
        if (!fgets(part_tag, sizeof part_tag, pt)) part_tag[0] = 0;
        part_tag[strcspn(part_tag, "\r\n")] = 0;
        fclose(pt);
    }
    if (strcmp(part_tag, tag)) {
        remove(zip_path);
        if ((pt = fopen(part_tag_path, "w"))) {
            fputs(tag, pt);
            fclose(pt);
        }
    }
    long long need = size > 0 ? size + 16 * 1024 * 1024 : 0, have_part = 0, free_b = plat_free_space(g_cfg.data_dir);
    struct stat st;
    if (stat(zip_path, &st) == 0) have_part = st.st_size;
    // An update needs room for both versions until it's done; if there isn't, the old one can go first.
    if (free_b >= 0 && need > 0 && free_b + have_part < need && stat(game_zip, &st) == 0 &&
        free_b + have_part + st.st_size >= need) {
        snprintf(err, sizeof err, "Not enough space to keep the installed version while the update\n"
                                  "downloads (%.0f MB needed, %.0f MB free).\n\nDelete the installed version first?\n"
                                  "Your saves are kept. Until the download finishes,\nonly online play is available.",
                 (need - have_part) / 1048576.0, free_b / 1048576.0);
        if (!ui_confirm(err)) return false;
        remove(game_zip);
        remove(version_path);
        free_b = plat_free_space(g_cfg.data_dir);
    }
    if (free_b >= 0 && need > 0 && free_b + have_part < need) {
        snprintf(err, sizeof err, "Not enough space on the memory card:\n%.0f MB needed, %.0f MB free.",
                 (need - have_part) / 1048576.0, free_b / 1048576.0);
        ui_message_wait(err);
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
            ui_message_wait(m);
        }
        return false;
    }
    // Done: check it before it replaces anything. A download that stopped early, was cut off or
    // came back damaged (a server answering 416 to the resume of a wrong part counts as complete)
    // would otherwise replace a working game with one that doesn't open: its size must be the
    // release's, and it must open as a zip (that reads only the zip's directory at its end).
    struct stat dl;
    bool size_ok = stat(zip_path, &dl) == 0 && (size <= 0 || dl.st_size == size);
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    bool zip_ok = size_ok && mz_zip_reader_init_file(&z, zip_path, 0);
    unsigned files = zip_ok ? mz_zip_reader_get_num_files(&z) : 0;
    if (zip_ok) mz_zip_reader_end(&z);
    if (!size_ok || !zip_ok || !files) {
        host_log("[offline] downloaded file rejected: %lld bytes (release: %lld), %s", (long long)(size_ok ? dl.st_size : -1),
                 size, !size_ok ? "wrong size" : !zip_ok ? "not a zip" : "empty zip");
        remove(zip_path);
        ui_message_wait("The downloaded file is damaged (wrong size or not a zip)\n"
                        "and was deleted. The installed game, if any, is kept.\n\nTry the download again.");
        return false;
    }
    host_log("[offline] download checked: %lld bytes, %u files", (long long)dl.st_size, files);
    // The game is played straight from the zip (no unpacking). An older version goes now.
    remove(game_zip);
    if (rename(zip_path, game_zip) != 0) {
        ui_message_wait("Couldn't rename the downloaded file.");
        return false;
    }
    remove(part_tag_path);
    FILE *vf = fopen(version_path, "w");
    if (vf) {
        fputs(strip_tag(tag), vf);
        fclose(vf);
    }
    host_log("[offline] installed release %s as %s", tag, game_zip);
    return true;
}
