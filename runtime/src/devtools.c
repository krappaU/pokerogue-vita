// SPDX-License-Identifier: AGPL-3.0-only
// Dev builds only: test builds onto the console and logs off it without FTP.
//   - Test builds: before the start screen, the app reads build.json of the "dev-build" GitHub
//     pre-release (.github/workflows/publish-dev.yml; config.txt devbuilds=<url> for another, or
//     devbuilds=off). If it isn't the build running, the app offers to install it: the updater app
//     it carries (updater/updater.c) installs it with the system's installer, as VitaShell would,
//     and starts the game again.
//   - Logs, when config.txt names a log server (devserver=<url>, devkey=<secret>;
//     tools/devserver/devserver.py): at start, the previous run's log (log-prev.txt, kept so a
//     crash's log survives the restart) is uploaded in the background; L + R + START in game
//     uploads the current one. The console makes every connection, so the server can be anywhere.

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "miniz.h"

#include "host.h"

#ifdef __vita__
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#include "../updater/promoter.h"
#endif

#define DEV_BUILDS_URL "https://github.com/krappaU/pokerogue-vita/releases/download/dev-build/build.json"

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
    host_log_flush(); // the lines logged so far, when it's this run's log
    char *body = read_file(j->path, &len);
    char msg[300];
    if (!body) {
        snprintf(msg, sizeof msg, "No log to send (%s).", j->path);
    } else {
        char url[1200], query[64], key[200], build[200], err[256] = "";
        snprintf(query, sizeof query, "kind=%s", j->kind);
        dev_url(url, sizeof url, "log", query);
        snprintf(key, sizeof key, "X-Dev-Key: %s", g_cfg.dev_key ? g_cfg.dev_key : "");
        // A benchmark run (bench=shaders) is told apart in the server's list by its build column
        // (and by its kind on servers that know "bench").
        snprintf(build, sizeof build, "X-Build: %s%s", host_version_string(),
                 !strcmp(j->kind, "bench") ? " | shader benchmark" : "");
        const char *headers[] = {"Content-Type: text/plain; charset=utf-8", key, build, NULL};
        long status = net_post_threadsafe(url, headers, body, len, err, sizeof err);
        if (status == 200) snprintf(msg, sizeof msg, "Log sent to the log server (%lu KB).", (unsigned long)((len + 1023) / 1024));
        else if (status) snprintf(msg, sizeof msg, "The log server refused the log (HTTP %ld).", status);
        else snprintf(msg, sizeof msg, "Couldn't reach the log server: %s", err);
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
    host_log("[dev] log server %s", g_cfg.dev_server);
    struct stat st;
    if (prev_log && stat(prev_log, &st) == 0 && st.st_size > 0) upload_start(prev_log, "prev", false);
}

void dev_send_log(void) {
    if (!g_cfg.dev_server) {
        dialog_notice("No log server is set up (devserver= in config.txt).");
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/log.txt", g_cfg.data_dir);
    host_log("[dev] sending the log");
    upload_start(path, "now", true);
}

// The benchmark page (bench/, config.txt bench=shaders) is done: its log goes up by itself.
void dev_send_bench_log(void) {
    if (!g_cfg.dev_server) {
        dialog_notice("Benchmark done. No log server is set up (devserver= in config.txt).");
        return;
    }
    char path[600];
    snprintf(path, sizeof path, "%s/log.txt", g_cfg.data_dir);
    host_log("[dev] sending the benchmark log");
    upload_start(path, "bench", true);
}

void dev_frame(void) {
    char msg[300] = "";
    pthread_mutex_lock(&up_lock);
    if (up_result[0]) snprintf(msg, sizeof msg, "%s", up_result), up_result[0] = 0;
    pthread_mutex_unlock(&up_lock);
    if (msg[0]) dialog_notice(msg);
}

// ---------------------------------------------------------------- test builds

// An app can't replace itself while it runs, so a test build is installed by a second app, the
// updater (updater/updater.c, title PKRVUPD01), the way VitaShell updates itself: the build is
// unpacked to <data>/update/files/, the updater (app0:updater.vpk, unpacked to <data>/update/updater/)
// is installed with the system's installer, <data>/update/ready.txt says all is ready, the updater is
// started and this app quits. The updater installs the build, writes <data>/update/result.txt and
// starts the game again, which shows the result and uninstalls the updater (dev_offer_update).
#define UPDATER_TITLE_ID "PKRVUPD01"
#define HEAD_BIN "sce_sys/package/head.bin" // the package header the installer needs (scripts/make_head_bin.py)

// <data>/dev-build.txt: "<build id> <commit>" of the build installed from the dev-build release. The
// commit is filled in by the first start of that build ("pending" until then), so a build installed
// some other way (VitaShell) doesn't count as the release's.
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

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        remove(path);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[900];
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        if (is_dir(sub)) remove_tree(sub);
        else remove(sub);
    }
    closedir(d);
    rmdir(path);
}

static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    bool ok = in && out;
    char buf[4096];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (in) fclose(in);
    if (out && fclose(out) != 0) ok = false;
    return ok;
}

// Unpacks a whole .vpk into dir, ready for the system's installer.
static bool unpack_vpk(const char *vpk, const char *dir, char *err, size_t errn) {
    mz_zip_archive z;
    memset(&z, 0, sizeof z);
    if (!mz_zip_reader_init_file(&z, vpk, 0)) {
        snprintf(err, errn, "the download isn't a valid .vpk");
        return false;
    }
    bool ok = true, eboot = false;
    int n = (int)mz_zip_reader_get_num_files(&z);
    for (int i = 0; ok && i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, (mz_uint)i, &st) || st.m_is_directory) continue;
        if (strstr(st.m_filename, "..")) continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", dir, st.m_filename);
        mkdirs_for(path);
        if (!mz_zip_reader_extract_to_file(&z, (mz_uint)i, path, 0)) {
            snprintf(err, errn, "couldn't write %s (%s)", path, strerror(errno));
            ok = false;
        }
        eboot |= !strcmp(st.m_filename, "eboot.bin");
    }
    mz_zip_reader_end(&z);
    if (ok && !eboot) snprintf(err, errn, "the .vpk has no eboot.bin"), ok = false;
    return ok;
}

// Unpacks a test build. Builds before 0.58 have no head.bin: the installed game's fits every build.
static bool unpack_build(const char *vpk, const char *dir, char *err, size_t errn) {
    if (!unpack_vpk(vpk, dir, err, errn)) return false;
    char head[900];
    snprintf(head, sizeof head, "%s/" HEAD_BIN, dir);
    if (file_exists(head) || copy_file("app0:" HEAD_BIN, head)) return true;
    snprintf(err, errn, "the .vpk has no %s", HEAD_BIN);
    return false;
}

// What the updater reported after the last install, shown once.
static void updater_result(void) {
    char dir[700], path[760], line[700] = "";
    build_file(dir, sizeof dir, "update");
    snprintf(path, sizeof path, "%s/result.txt", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        if (is_dir(dir)) remove_tree(dir); // an install that never reached the updater
        return;
    }
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    remove_tree(dir);
    host_log("[dev] updater: %s", line);
    if (strncmp(line, "ok", 2)) {
        build_file(path, sizeof path, "dev-build.txt");
        remove(path); // the build running now is the old one
        char msg[900];
        snprintf(msg, sizeof msg, "The updater couldn't install the test build:\n%s\n\nThe download can be installed with VitaShell\nfrom the dev-build release.", line[0] ? line : "(no result)");
        ui_message_wait(msg);
    }
}

// The system's installer (updater/promoter.h) is only used early in a start, before vitaGL takes
// the GPU and dialog memory: 0.59 called it after graphics init, its PAF module failed to load (no
// free physical page) and the game crashed (and once the console hung). Every call is bracketed by
// <data>/installer.txt ("<step> <commit>"); one still there at the next start never returned, and
// the installer stays off for this build (<data>/installer-off.txt) until another build is
// installed, so a crash or hang can't repeat at every start.
static bool installer_off;
static char installer_hung[100]; // the step that never returned, shown once the UI is up

static void installer_check(void) {
    char commit[64], step[100] = "", off_commit[64] = "";
    running_commit(commit, sizeof commit);
    if (read_pair("installer.txt", step, sizeof step, off_commit, sizeof off_commit)) {
        snprintf(installer_hung, sizeof installer_hung, "%s", step);
        host_log("[dev] the last start never came back from the system installer (%s); it stays off for this build",
                 step);
        write_pair("installer-off.txt", commit, step);
        char path[700];
        build_file(path, sizeof path, "installer.txt");
        remove(path);
    }
    if (read_pair("installer-off.txt", off_commit, sizeof off_commit, step, sizeof step)) {
        installer_off = !strcmp(off_commit, commit);
        if (!installer_off) {
            char path[700];
            build_file(path, sizeof path, "installer-off.txt");
            remove(path);
        }
    }
}

#ifdef __vita__
static int installer_call(const char *step, const char *arg, bool install) {
    char commit[64], path[700];
    running_commit(commit, sizeof commit);
    write_pair("installer.txt", step, commit);
    host_log("[dev] system installer: %s...", step);
    int r = install ? promote_app(arg) : delete_app(arg);
    build_file(path, sizeof path, "installer.txt");
    remove(path);
    host_log("[dev] system installer: %s: %s (0x%08X)", step, r < 0 ? "failed" : "done", (unsigned)r);
    return r;
}
#endif

static void write_result(const char *line) {
    char path[700];
    build_file(path, sizeof path, "update/result.txt");
    mkdirs_for(path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%s\n", line);
        fclose(f);
    }
}

// Before graphics init. An update in progress (<data>/update/install-updater.txt, written by
// dev_offer_update before it restarts the app): install the updater from the updater.vpk this app
// carries, start it and quit. Right after an update (update/result.txt): uninstall the updater, as
// VitaShell does with its own; also one installed by hand before the game carried its own.
void dev_early(void) {
    if (!app_dir) return;
    installer_check();
#ifdef __vita__
    char dir[700], step[760], upd[760], err[600] = "";
    build_file(dir, sizeof dir, "update");
    snprintf(step, sizeof step, "%s/install-updater.txt", dir);
    snprintf(upd, sizeof upd, "%s/updater", dir);
    if (file_exists(step)) {
        remove(step);
        if (installer_off) {
            write_result("error the system installer is off for this build (it never came back once)");
            return;
        }
        if (!unpack_vpk("app0:updater.vpk", upd, err, sizeof err)) {
            char line[700];
            snprintf(line, sizeof line, "error couldn't unpack the updater: %s", err);
            write_result(line);
            return;
        }
        int r = installer_call("installing the updater", upd, true);
        if (r < 0) {
            char line[700];
            snprintf(line, sizeof line, "error the system installer couldn't install the updater (0x%08X)", (unsigned)r);
            write_result(line);
            return;
        }
        char path[700];
        build_file(path, sizeof path, "dev-update.vpk");
        remove(path);
        build_file(path, sizeof path, "dev-update.id");
        remove(path);
        host_log("[dev] starting the updater");
        host_log_flush();
        sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" UPDATER_TITLE_ID);
        sceKernelExitProcess(0);
        write_result("error the updater didn't start");
        return;
    }
    snprintf(step, sizeof step, "%s/result.txt", dir);
    if (installer_off || !file_exists(step) || !file_exists("ux0:app/" UPDATER_TITLE_ID "/eboot.bin")) return;
    sceAppMgrDestroyOtherApp(); // the updater, if it hasn't quit yet
    installer_call("uninstalling the updater", UPDATER_TITLE_ID, false);
#endif
}

bool dev_offer_update(void) {
    const char *builds = g_cfg.dev_builds ? g_cfg.dev_builds : DEV_BUILDS_URL;
    if (!app_dir) return false;
    updater_result();
    if (installer_hung[0]) {
        char msg[400];
        snprintf(msg, sizeof msg, "The last start never came back from the system's installer\n(%s).\n\n"
                 "Test builds from here are off until another build\nis installed with VitaShell.", installer_hung);
        installer_hung[0] = 0;
        ui_message_wait(msg);
    }
    if (!strcmp(builds, "off")) return false;
    char commit[64], inst_id[128] = "", inst_commit[64] = "";
    running_commit(commit, sizeof commit);
    if (read_pair("dev-build.txt", inst_id, sizeof inst_id, inst_commit, sizeof inst_commit)) {
        if (!strcmp(inst_commit, "pending")) write_pair("dev-build.txt", inst_id, commit); // first start after an install
        else if (strcmp(inst_commit, commit)) inst_id[0] = 0; // installed some other way since
    }

    overlay_present("PokeRogue\n\nLooking for a new test build...");
    size_t len = 0;
    long status = 0;
    char *json = net_get_threadsafe(builds, &len, &status);
    char id[128] = "", srv_commit[64] = "", label[200] = "", url[1200] = "";
    if (!json || status != 200 || !json_str(json, "id", id, sizeof id) || !json_str(json, "url", url, sizeof url)) {
        host_log("[dev] no test build at %s (HTTP %ld)", builds, status);
        free(json);
        return false;
    }
    json_str(json, "commit", srv_commit, sizeof srv_commit);
    json_str(json, "label", label, sizeof label);
    long long size = json_num(json, "size");
    free(json);
    srv_commit[strcspn(srv_commit, "-")] = 0;
    // CI builds name their commit; builds made elsewhere are known by the id recorded at install.
    bool same = (strlen(srv_commit) >= 7 && strlen(commit) >= 7 && !strncmp(srv_commit, commit, 7)) || !strcmp(id, inst_id);
    host_log("[dev] newest test build: %s (%s)%s", label, id, same ? ": running it" : "");
    if (same) return false;
    char declined[128] = "", unused[8];
    if (read_pair("dev-declined.txt", declined, sizeof declined, unused, sizeof unused) && !strcmp(declined, id))
        return false;

    char msg[900];
    snprintf(msg, sizeof msg, "New test build:\n%s\n(%.1f MB)\n\nRunning now: %s\n\nInstall it? The app restarts.", label,
             size > 0 ? size / 1048576.0 : 0, host_version_string());
    if (!ui_confirm(msg)) {
        write_pair("dev-declined.txt", id, "-"); // not asked again for this build
        return false;
    }
    // A partial download resumes only for the same build.
    char vpk[700], part_id[128] = "", err[600] = "";
    build_file(vpk, sizeof vpk, "dev-update.vpk");
    if (!read_pair("dev-update.id", part_id, sizeof part_id, unused, sizeof unused) || strcmp(part_id, id)) {
        remove(vpk);
        write_pair("dev-update.id", id, "-");
    }
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
    if (installer_off) {
        host_log("[dev] the system installer is off for this build; the download is in %s", vpk);
        snprintf(msg, sizeof msg, "Downloaded to %s.\n\nInstall it with VitaShell (press X on the file).", vpk);
        ui_message_wait(msg);
        return false;
    }
    overlay_present("PokeRogue\n\nUnpacking the test build...");
    char dir[700], files[760], path[760];
    build_file(dir, sizeof dir, "update");
    remove_tree(dir);
    snprintf(files, sizeof files, "%s/files", dir);
    if (!unpack_build(vpk, files, err, sizeof err)) {
        host_log("[dev] unpacking failed: %s", err);
        remove_tree(dir);
        snprintf(msg, sizeof msg, "Unpacking failed: %s\n\nThe download is in %s:\ninstall it with VitaShell.", err, vpk);
        ui_message_wait(msg);
        return false;
    }
    // The updater reads ready.txt; this app, restarted, installs the updater first (dev_early).
    // The download stays until the updater is started, for VitaShell if something fails.
    const char *steps[] = {"ready.txt", "install-updater.txt"};
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, steps[i]);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%s\n", id);
            fclose(f);
        }
    }
    write_pair("dev-build.txt", id, "pending");
    host_log("[dev] test build %s unpacked; restarting to install the updater", id);
    overlay_present("PokeRogue\n\nInstalling the test build...");
    host_log_flush();
    plat_relaunch(0, NULL);
    ui_message_wait("The app didn't restart.\nClose it and start it again to install the test build.");
    return true;
}
