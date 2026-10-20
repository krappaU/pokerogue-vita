// SPDX-License-Identifier: AGPL-3.0-only
// PokeRogue updater (title PKRVUPD01): a second app that installs a test build of the game. An app
// can't replace itself while it runs, so the game's dev build (src/devtools.c) unpacks a downloaded
// build to ux0:data/pokerogue/update/files/, writes update/ready.txt, installs this app from the
// updater.vpk it carries, starts it and quits. This app installs the build with the system's
// installer (as VitaShell installs a .vpk), writes update/result.txt ("ok" or "error <why>") for the
// game to show, and starts the game again, which uninstalls this app. Like VitaShell's VSUPDATER.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>

#include "promoter.h"

#define GAME_TITLE_ID "PKRV00001"
#define UPDATE_DIR "ux0:data/pokerogue/update"

// TITLE_ID from a param.sfo, so nothing but the game is installed from here.
static bool sfo_title_id(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    static unsigned char buf[64 * 1024];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    if (len < 20 || memcmp(buf, "\0PSF", 4)) return false;
    uint32_t keys, vals, count;
    memcpy(&keys, buf + 8, 4);
    memcpy(&vals, buf + 12, 4);
    memcpy(&count, buf + 16, 4);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= len; i++) {
        const unsigned char *e = buf + 20 + i * 16;
        uint16_t name;
        uint32_t data;
        memcpy(&name, e, 2);
        memcpy(&data, e + 12, 4);
        if (keys + name >= len || vals + data >= len) break;
        if (!strcmp((const char *)buf + keys + name, "TITLE_ID")) {
            snprintf(out, n, "%s", (const char *)buf + vals + data);
            return true;
        }
    }
    return false;
}

int main(void) {
    sceAppMgrDestroyOtherApp(); // the game, if it is still suspended in the background
    FILE *f = fopen(UPDATE_DIR "/ready.txt", "r");
    if (f) {
        fclose(f);
        char msg[300], title[16] = "";
        bool ok = false;
        if (!sfo_title_id(UPDATE_DIR "/files/sce_sys/param.sfo", title, sizeof title)) {
            snprintf(msg, sizeof msg, "no readable sce_sys/param.sfo in the build");
        } else if (strcmp(title, GAME_TITLE_ID)) {
            snprintf(msg, sizeof msg, "the build is for title %s, not " GAME_TITLE_ID, title);
        } else {
            int r = promote_app(UPDATE_DIR "/files");
            ok = r >= 0;
            snprintf(msg, sizeof msg, ok ? "installed" : "the system installer failed (0x%08X)", (unsigned)r);
        }
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
