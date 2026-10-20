// SPDX-License-Identifier: AGPL-3.0-only
// The Vita's package installer (ScePromoterUtil), used by the game's dev build (before its graphics
// take the memory PAF needs) and by the updater, the way VitaShell uses it: it needs the PAF module,
// loaded with its own heap, and the promoter module.
// promote_app() installs the unpacked app in dir (eboot.bin, sce_sys/param.sfo and
// sce_sys/package/head.bin at least) and moves it away; delete_app() uninstalls a title.
#pragma once

#include <stdint.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

// How far promoter_load got: only what loaded may be unloaded. A call into the promoter module
// when it didn't load jumps to address 0 (0.59: PAF failed with 0x80024302, no free physical page,
// and the cleanup crashed the game).
static int promoter_loaded;

static inline int promoter_load(void) {
    static uint32_t argp[] = {0x180000, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF};
    int result = -1;
    uint32_t buf[4] = {sizeof buf, (uint32_t)&result, 0xFFFFFFFF, 0xFFFFFFFF};
    promoter_loaded = 0;
    int r = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof argp, argp, (void *)buf);
    if (r < 0) return r;
    promoter_loaded = 1;
    r = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (r < 0) return r;
    promoter_loaded = 2;
    r = scePromoterUtilityInit();
    if (r < 0) return r;
    promoter_loaded = 3;
    return r;
}

static inline void promoter_unload(void) {
    uint32_t buf = 0;
    if (promoter_loaded >= 3) scePromoterUtilityExit();
    if (promoter_loaded >= 2) sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (promoter_loaded >= 1) sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, (void *)&buf);
    promoter_loaded = 0;
}

static inline int promote_app(const char *dir) {
    int r = promoter_load();
    if (r >= 0) r = scePromoterUtilityPromotePkgWithRif(dir, 1);
    promoter_unload();
    return r;
}

static inline int delete_app(const char *title_id) {
    int r = promoter_load();
    if (r >= 0) r = scePromoterUtilityDeletePkg(title_id);
    promoter_unload();
    return r;
}
