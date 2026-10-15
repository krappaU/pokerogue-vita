// SPDX-License-Identifier: AGPL-3.0-only
// Downloads and unpacks the offline game into <data>/game, from the start screen.
//
// The build is the one Admiral-Billy's PokeRogue App uses for its offline mode: game.zip from the
// latest release of github.com/Admiral-Billy/pokerogue (about 530 MB; 735 MB unpacked). The zip is
// downloaded to <data>/game.zip.part (an interrupted download resumes), unpacked into
// <data>/game.tmp (source maps skipped), which is then renamed to <data>/game, and deleted.

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

static void mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (p[-1] != ':') mkdir(tmp, 0777);
        *p = '/';
    }
    mkdir(tmp, 0777);
}

static void remove_tree(const char *path); // below

static size_t write_cb(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
    (void)ofs;
    return fwrite(buf, 1, n, (FILE *)opaque);
}

static bool unpack(const char *zip_path, const char *dest, char *err, size_t errn) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        snprintf(err, errn, "the downloaded file isn't a valid zip (%s)", mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return false;
    }
    int n = (int)mz_zip_reader_get_num_files(&zip);
    long long total = 0, done = 0;
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (mz_zip_reader_file_stat(&zip, i, &st)) total += (long long)st.m_uncomp_size;
    }
    progress_ui ui = {.t0 = host_now_ms(), .what = "Unpacking the offline game"};
    mkdirs(dest);
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const char *name = st.m_filename;
        size_t len = strlen(name);
        if (strstr(name, "..") || name[0] == '/' || strchr(name, ':')) continue; // stay inside dest
        char out[1024];
        snprintf(out, sizeof out, "%s/%s", dest, name);
        if (st.m_is_directory) {
            mkdirs(out);
            continue;
        }
        done += (long long)st.m_uncomp_size;
        if (len > 4 && !strcmp(name + len - 4, ".map")) continue; // source maps: not needed
        char *slash = strrchr(out, '/');
        if (slash) {
            *slash = 0;
            mkdirs(out);
            *slash = '/';
        }
        FILE *f = fopen(out, "wb");
        if (!f) {
            snprintf(err, errn, "can't write %s (memory card full?)", out);
            ok = false;
            break;
        }
        bool wrote = mz_zip_reader_extract_to_callback(&zip, i, write_cb, f, 0);
        if (fclose(f) != 0) wrote = false;
        if (!wrote) {
            snprintf(err, errn, "unpacking %s failed (memory card full?)", name);
            ok = false;
            break;
        }
        if (!show_progress(done, total, &ui)) {
            snprintf(err, errn, "cancelled");
            ok = false;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

#ifdef __vita__
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
static void remove_tree(const char *path) {
    SceUID d = sceIoDopen(path);
    if (d >= 0) {
        SceIoDirent e;
        while (sceIoDread(d, &e) > 0) {
            if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, "..")) continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e.d_name);
            if (SCE_S_ISDIR(e.d_stat.st_mode)) remove_tree(p);
            else sceIoRemove(p);
        }
        sceIoDclose(d);
    }
    sceIoRmdir(path);
}
#else
#include <dirent.h>
#include <unistd.h>
static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p[1024];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(p);
            else remove(p);
        }
        closedir(d);
    }
    rmdir(path);
}
#endif

// Returns true when <data>/game is ready.
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

    char zip_path[700], tmp_dir[700], game_dir[700];
    snprintf(zip_path, sizeof zip_path, "%s/game.zip.part", g_cfg.data_dir);
    snprintf(tmp_dir, sizeof tmp_dir, "%s/game.tmp", g_cfg.data_dir);
    snprintf(game_dir, sizeof game_dir, "%s/game", g_cfg.data_dir);
    // Space: the zip plus the unpacked game (about 1.4x the zip) at the same time.
    long long need = size > 0 ? size + size * 3 / 2 : 0, have_part = 0, free_b = plat_free_space(g_cfg.data_dir);
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
    remove_tree(tmp_dir);
    if (!unpack(zip_path, tmp_dir, err, sizeof err)) {
        host_log("[offline] unpacking failed: %s", err);
        remove_tree(tmp_dir);
        if (strcmp(err, "cancelled")) {
            char m[700];
            snprintf(m, sizeof m, "Unpacking failed:\n%s", err);
            message_wait(m);
        }
        return false;
    }
    remove_tree(game_dir);
    if (rename(tmp_dir, game_dir) != 0) {
        message_wait("Couldn't move the unpacked game into place.");
        return false;
    }
    remove(zip_path);
    host_log("[offline] installed release %s in %s", tag, game_dir);
    return true;
}
