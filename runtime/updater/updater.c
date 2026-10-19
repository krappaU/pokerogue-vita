// SPDX-License-Identifier: AGPL-3.0-only
// PokeRogue updater (title PKRVUPD01): a second app that installs a test build into the game's app
// folder. A running app can't write its own ux0:app/<title id> folder, so the game's dev build
// (src/devtools.c) unpacks a downloaded build to ux0:data/pokerogue/update/files/, writes
// update/ready.txt, starts this app and quits. This app copies the files over, eboot.bin last,
// writes update/result.txt ("ok" or "error <why>") for the game to show, and starts the game again.
// Like VitaShell's VSUPDATER, but copying files instead of using the system installer.
//
// Built with the dev build (scripts/build_vita.sh with PKR_DEV=1: build-vita-dev/pokerogue-updater.vpk)
// and installed once with VitaShell. On Linux (cc -DUPDATER_TEST) it copies argv[1] into argv[2].

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __vita__
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#define GAME_TITLE_ID "PKRV00001"
#define APP_DIR "ux0:app/" GAME_TITLE_ID
#define UPDATE_DIR "ux0:data/pokerogue/update"
#endif

typedef struct {
    char (*paths)[512]; // relative to the source folder
    int n, cap;
} file_list;

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void mkdirs_for(const char *file) {
    char tmp[1024];
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

// Every file under root/rel, except sce_sys/ (LiveArea and param.sfo stay as installed).
static bool list_files(const char *root, const char *rel, file_list *out, char *err, size_t errn) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel);
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errn, "can't read %s (%s)", dir, strerror(errno));
        return false;
    }
    bool ok = true;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char sub[512], full[1024];
        snprintf(sub, sizeof sub, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", root, sub);
        if (!rel[0] && !strcmp(e->d_name, "sce_sys")) continue;
        if (is_dir(full)) {
            ok = list_files(root, sub, out, err, errn);
        } else {
            if (out->n == out->cap) {
                out->cap = out->cap ? out->cap * 2 : 64;
                out->paths = realloc(out->paths, (size_t)out->cap * sizeof *out->paths);
            }
            snprintf(out->paths[out->n++], sizeof *out->paths, "%s", sub);
        }
    }
    closedir(d);
    return ok;
}

static bool copy_file(const char *from, const char *to, char *err, size_t errn) {
    FILE *in = fopen(from, "rb");
    if (!in) {
        snprintf(err, errn, "can't read %s (%s)", from, strerror(errno));
        return false;
    }
    mkdirs_for(to);
    FILE *out = fopen(to, "wb");
    if (!out) {
        snprintf(err, errn, "can't write %s (%s)", to, strerror(errno));
        fclose(in);
        return false;
    }
    static char buf[256 * 1024];
    size_t n;
    bool ok = true;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0) ok = fwrite(buf, 1, n, out) == n;
    if (ferror(in)) ok = false;
    if (fclose(out) != 0) ok = false;
    fclose(in);
    if (!ok) snprintf(err, errn, "couldn't copy %s to %s (%s)", from, to, strerror(errno));
    return ok;
}

// Copies every file of src into dst: each one next to its old version first (".new"), and only once
// all are written are they swapped in, eboot.bin last, so a failure leaves the old program working.
static bool apply_update(const char *src, const char *dst, char *err, size_t errn) {
    file_list files = {0};
    bool ok = list_files(src, "", &files, err, errn);
    int eboot = -1;
    for (int i = 0; ok && i < files.n; i++) {
        char from[1024], tmp[1100];
        snprintf(from, sizeof from, "%s/%s", src, files.paths[i]);
        snprintf(tmp, sizeof tmp, "%s/%s.new", dst, files.paths[i]);
        ok = copy_file(from, tmp, err, errn);
        if (!strcmp(files.paths[i], "eboot.bin")) eboot = i;
    }
    if (ok && eboot < 0) snprintf(err, errn, "the update has no eboot.bin"), ok = false;
    for (int i = 0; !ok && i < files.n; i++) { // nothing swapped in yet: drop the copies
        char tmp[1100];
        snprintf(tmp, sizeof tmp, "%s/%s.new", dst, files.paths[i]);
        remove(tmp);
    }
    for (int k = 0; ok && k < files.n; k++) {
        int i = k < eboot ? k : k + 1 < files.n ? k + 1 : eboot; // every other file in order, then eboot.bin
        char to[1024], tmp[1100];
        snprintf(to, sizeof to, "%s/%s", dst, files.paths[i]);
        snprintf(tmp, sizeof tmp, "%s.new", to);
        remove(to);
        if (rename(tmp, to) != 0) {
            snprintf(err, errn, "couldn't replace %s (%s)", to, strerror(errno));
            ok = false;
        }
    }
    if (ok) snprintf(err, errn, "%d files", files.n);
    free(files.paths);
    return ok;
}

#ifdef __vita__
int main(void) {
    sceAppMgrDestroyOtherApp(); // the game, if it is still suspended in the background
    FILE *f = fopen(UPDATE_DIR "/ready.txt", "r");
    if (f) {
        fclose(f);
        char msg[600] = "";
        bool ok = apply_update(UPDATE_DIR "/files", APP_DIR, msg, sizeof msg);
        remove(UPDATE_DIR "/ready.txt");
        if ((f = fopen(UPDATE_DIR "/result.txt", "w"))) {
            fprintf(f, "%s %s\n", ok ? "ok" : "error", msg);
            fclose(f);
        }
    }
    sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" GAME_TITLE_ID);
    sceKernelExitProcess(0);
    return 0;
}
#elif defined(UPDATER_TEST)
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: updater SRC DST\n");
        return 2;
    }
    char msg[600] = "";
    bool ok = apply_update(argv[1], argv[2], msg, sizeof msg);
    printf("%s %s\n", ok ? "ok" : "error", msg);
    return ok ? 0 : 1;
}
#endif
