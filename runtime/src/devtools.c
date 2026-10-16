// SPDX-License-Identifier: AGPL-3.0-only
// Dev server: test builds onto the console and logs off it without FTP. Only active when
// <data>/config.txt names a server (devserver=<url>, devkey=<secret>, optional devbranch=<branch>);
// tools/devserver/devserver.py is the other end.
//
// The console makes every connection, so the server can be anywhere on the internet and nothing on
// the home network has to be opened.
//   - Logs: at start, the previous run's log (log-prev.txt, kept so a crash's log survives the
//     restart) is uploaded in the background; L + R + START in game uploads the current one.
//   - Test builds: before the start screen, the server is asked for its newest build (the latest
//     CI .vpk, or one built on the server). If it isn't the one running, the app offers to install
//     it: the app's files (eboot.bin, js/, cacert.pem) in ux0:app/<title id> are replaced and the
//     app restarts. sce_sys/ (LiveArea, param.sfo) is left alone; changes there need VitaShell.

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "miniz.h"

#include "host.h"

#ifdef __vita__
static const char *const app_dir = "ux0:app/" PKR_TITLE_ID;
#else
static const char *const app_dir = NULL; // the desktop runtime runs from the build folder: log upload only
#endif

static void dev_url(char *out, size_t n, const char *path, const char *query) {
    const char *s = g_cfg.dev_server;
    size_t len = strlen(s);
    while (len && s[len - 1] == '/') len--;
    snprintf(out, n, "%.*s/%s?key=%s%s%s", (int)len, s, path, g_cfg.dev_key ? g_cfg.dev_key : "", query ? "&" : "",
             query ? query : "");
}

// The commit this build comes from, without "-modified".
static void running_commit(char *out, size_t n) {
    snprintf(out, n, "%s", PKR_COMMIT);
    char *m = strstr(out, "-modified");
    if (m) *m = 0;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = n >= 0 ? malloc((size_t)n + 1) : NULL;
    size_t got = d ? fread(d, 1, (size_t)n, f) : 0;
    fclose(f);
    if (d) d[got] = 0;
    *len = got;
    return d;
}

// ---------------------------------------------------------------- log upload

static pthread_mutex_t up_lock = PTHREAD_MUTEX_INITIALIZER;
static bool up_busy;
static char up_result[300]; // shown by dev_frame on the main thread, then cleared

typedef struct {
    char path[600];
    char kind[8]; // "prev" (the previous run's log) or "now"
    bool notify;
} upload_job;

static void *upload_thread(void *arg) {
    upload_job *j = arg;
    size_t len = 0;
    char *body = read_file(j->path, &len);
    char msg[300];
    if (!body) {
        snprintf(msg, sizeof msg, "No log to send (%s).", j->path);
    } else {
        char url[1200], query[64], key[200], build[200], err[256] = "";
        snprintf(query, sizeof query, "kind=%s", j->kind);
        dev_url(url, sizeof url, "log", query);
        snprintf(key, sizeof key, "X-Dev-Key: %s", g_cfg.dev_key ? g_cfg.dev_key : "");
        snprintf(build, sizeof build, "X-Build: %s", host_version_string());
        const char *headers[] = {"Content-Type: text/plain; charset=utf-8", key, build, NULL};
        long status = net_post_threadsafe(url, headers, body, len, err, sizeof err);
        if (status == 200) snprintf(msg, sizeof msg, "Log sent to the dev server (%lu KB).", (unsigned long)((len + 1023) / 1024));
        else if (status) snprintf(msg, sizeof msg, "The dev server refused the log (HTTP %ld).", status);
        else snprintf(msg, sizeof msg, "Couldn't reach the dev server: %s", err);
        host_log("[dev] %s log upload: %s", j->kind, msg);
    }
    free(body);
    pthread_mutex_lock(&up_lock);
    if (j->notify) snprintf(up_result, sizeof up_result, "%s", msg);
    up_busy = false;
    pthread_mutex_unlock(&up_lock);
    free(j);
    return NULL;
}

static void upload_start(const char *path, const char *kind, bool notify) {
    pthread_mutex_lock(&up_lock);
    bool busy = up_busy;
    up_busy = true;
    pthread_mutex_unlock(&up_lock);
    if (busy) {
        if (notify) dialog_notice("A log is being sent already.");
        return;
    }
    upload_job *j = calloc(1, sizeof *j);
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->kind, sizeof j->kind, "%s", kind);
    j->notify = notify;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024); // TLS handshakes need a deep stack
    bool ok = pthread_create(&t, &attr, upload_thread, j) == 0;
    pthread_attr_destroy(&attr);
    if (ok) {
        pthread_detach(t);
        return;
    }
    free(j);
    pthread_mutex_lock(&up_lock);
    up_busy = false;
    pthread_mutex_unlock(&up_lock);
}

void dev_start(const char *prev_log) {
    if (!g_cfg.dev_server) return;
    host_log("[dev] dev server %s%s%s", g_cfg.dev_server, g_cfg.dev_branch ? ", branch " : "",
             g_cfg.dev_branch ? g_cfg.dev_branch : "");
    struct stat st;
    if (prev_log && stat(prev_log, &st) == 0 && st.st_size > 0) upload_start(prev_log, "prev", false);
}

void dev_send_log(void) {
    if (!g_cfg.dev_server) {
        dialog_notice("No dev server is set up (devserver= in config.txt).");
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/log.txt", g_cfg.data_dir);
    host_log("[dev] sending the log");
    upload_start(path, "now", true);
}

void dev_frame(void) {
    char msg[300] = "";
    pthread_mutex_lock(&up_lock);
    if (up_result[0]) snprintf(msg, sizeof msg, "%s", up_result), up_result[0] = 0;
    pthread_mutex_unlock(&up_lock);
    if (msg[0]) dialog_notice(msg);
}

// ---------------------------------------------------------------- test builds

// <data>/dev-build.txt: "<build id> <commit>" of the build installed from the dev server. The
// commit is filled in by the first start of that build ("pending" until then), so a build
// installed some other way (VitaShell) doesn't count as the server's.
static void build_file(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", g_cfg.data_dir, name); }

static bool read_pair(const char *name, char *a, size_t an, char *b, size_t bn) {
    char path[700], line[300];
    build_file(path, sizeof path, name);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok) return false;
    line[strcspn(line, "\r\n")] = 0;
    char *sp = strchr(line, ' ');
    if (sp) *sp++ = 0;
    snprintf(a, an, "%s", line);
    snprintf(b, bn, "%s", sp ? sp : "");
    return a[0] != 0;
}

static void write_pair(const char *name, const char *a, const char *b) {
    char path[700];
    build_file(path, sizeof path, name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s %s\n", a, b);
    fclose(f);
}

static void mkdirs_for(const char *file) {
    char tmp[700];
    snprintf(tmp, sizeof tmp, "%s", file);
    char *start = strchr(tmp, ':'); // skip a "ux0:" device prefix
    for (char *p = start ? start + 1 : tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
}

// Unpacks the .vpk into the app folder: every file is written next to its old version first
// (".new"), and only once all are written are they swapped in, eboot.bin last.
static bool install_vpk(const char *vpk, char *err, size_t errn) {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, vpk, 0)) {
        snprintf(err, errn, "the download isn't a valid .vpk");
        return false;
    }
    int n = (int)mz_zip_reader_get_num_files(&z);
    char (*names)[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE] = calloc((size_t)n + 1, sizeof *names);
    int count = 0, eboot = -1;
    bool ok = names != NULL;
    for (int i = 0; ok && i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, (mz_uint)i, &st) || st.m_is_directory) continue;
        if (!strncmp(st.m_filename, "sce_sys/", 8) || strstr(st.m_filename, "..")) continue;
        char tmp[700];
        snprintf(tmp, sizeof tmp, "%s/%s.new", app_dir, st.m_filename);
        mkdirs_for(tmp);
        if (!mz_zip_reader_extract_to_file(&z, (mz_uint)i, tmp, 0)) {
            snprintf(err, errn, "couldn't write %s", tmp);
            ok = false;
            break;
        }
        snprintf(names[count], sizeof names[count], "%s", st.m_filename);
        if (!strcmp(st.m_filename, "eboot.bin")) eboot = count;
        count++;
    }
    mz_zip_reader_end(&z);
    if (ok && eboot < 0) snprintf(err, errn, "the .vpk has no eboot.bin"), ok = false;
    // Swap: eboot.bin goes last, so a failure leaves the old program with at most some new files.
    for (int k = 0; ok && k < count; k++) {
        int i = k < eboot ? k : k + 1 < count ? k + 1 : eboot; // every other file in order, then eboot.bin
        char dst[700], tmp[720];
        snprintf(dst, sizeof dst, "%s/%s", app_dir, names[i]);
        snprintf(tmp, sizeof tmp, "%s.new", dst);
        remove(dst);
        if (rename(tmp, dst) != 0) {
            snprintf(err, errn, "couldn't replace %s", dst);
            ok = false;
        }
    }
    host_log("[dev] installed %d files into %s%s", count, app_dir, ok ? "" : " (failed)");
    free(names);
    return ok;
}

bool dev_offer_update(void) {
    if (!g_cfg.dev_server || !app_dir) return false;
    char commit[64], inst_id[128] = "", inst_commit[64] = "";
    running_commit(commit, sizeof commit);
    if (read_pair("dev-build.txt", inst_id, sizeof inst_id, inst_commit, sizeof inst_commit)) {
        if (!strcmp(inst_commit, "pending")) write_pair("dev-build.txt", inst_id, commit); // first start after an install
        else if (strcmp(inst_commit, commit)) inst_id[0] = 0; // installed some other way since
    }

    overlay_present("PokeRogue\n\nAsking the dev server for a test build...");
    char url[1200], query[300] = "";
    if (g_cfg.dev_branch) snprintf(query, sizeof query, "branch=%s", g_cfg.dev_branch);
    dev_url(url, sizeof url, "build.json", query[0] ? query : NULL);
    size_t len = 0;
    long status = 0;
    char *json = net_get_threadsafe(url, &len, &status);
    char id[128] = "", srv_commit[64] = "", label[200] = "";
    if (!json || status != 200 || !json_str(json, "id", id, sizeof id)) {
        host_log("[dev] no test build from the dev server (HTTP %ld)", status);
        free(json);
        return false;
    }
    json_str(json, "commit", srv_commit, sizeof srv_commit);
    json_str(json, "label", label, sizeof label);
    long long size = json_num(json, "size");
    free(json);
    srv_commit[strcspn(srv_commit, "-")] = 0;
    // CI builds name their commit; builds made on the server are known by the id recorded at install.
    bool same = (strlen(srv_commit) >= 7 && strlen(commit) >= 7 && !strncmp(srv_commit, commit, 7)) || !strcmp(id, inst_id);
    host_log("[dev] newest test build: %s (%s)%s", label, id, same ? ": running it" : "");
    if (same) return false;
    char declined[128] = "", unused[8];
    if (read_pair("dev-declined.txt", declined, sizeof declined, unused, sizeof unused) && !strcmp(declined, id))
        return false;

    char msg[900];
    snprintf(msg, sizeof msg, "Test build on the dev server:\n%s\n(%.1f MB)\n\nRunning now: %s\n\nInstall it? The app restarts.",
             label, size > 0 ? size / 1048576.0 : 0, host_version_string());
    if (!ui_confirm(msg)) {
        write_pair("dev-declined.txt", id, "-"); // not asked again for this build
        return false;
    }
    // A partial download resumes only for the same build.
    char vpk[700], part_id[128] = "", err[400] = "";
    build_file(vpk, sizeof vpk, "dev-update.vpk");
    if (!read_pair("dev-update.id", part_id, sizeof part_id, unused, sizeof unused) || strcmp(part_id, id)) {
        remove(vpk);
        write_pair("dev-update.id", id, "-");
    }
    char q[200];
    snprintf(q, sizeof q, "id=%s", id);
    dev_url(url, sizeof url, "build.vpk", q);
    char what[300];
    snprintf(what, sizeof what, "Downloading test build %s", label);
    if (!ui_download(url, vpk, what, err, sizeof err)) {
        host_log("[dev] download stopped: %s", err);
        if (strcmp(err, "cancelled")) {
            snprintf(msg, sizeof msg, "The download stopped:\n%s", err);
            ui_message_wait(msg);
        }
        return false;
    }
    overlay_present("PokeRogue\n\nInstalling the test build...");
    if (!install_vpk(vpk, err, sizeof err)) {
        // The .vpk stays in the data folder: VitaShell can install it by hand.
        host_log("[dev] install failed: %s", err);
        snprintf(msg, sizeof msg, "Installing failed: %s\n\nThe download is in %s:\ninstall it with VitaShell.", err, vpk);
        ui_message_wait(msg);
        return false;
    }
    write_pair("dev-build.txt", id, "pending");
    remove(vpk);
    build_file(vpk, sizeof vpk, "dev-update.id");
    remove(vpk);
    host_log("[dev] test build %s installed; restarting", id);
    overlay_present("PokeRogue\n\nTest build installed. Restarting...");
    plat_relaunch(0, NULL);
    ui_message_wait("Installed, but the app couldn't restart itself.\nClose it and start it again.");
    return true;
}
