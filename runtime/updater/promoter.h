// SPDX-License-Identifier: AGPL-3.0-only
// The Vita's package installer (ScePromoterUtil), used by the game's dev build and the updater the
// way VitaShell uses it: it needs the PAF module, loaded with its own heap, and the promoter module.
// promote_app() installs the unpacked app in dir (eboot.bin, sce_sys/param.sfo and
// sce_sys/package/head.bin at least) and moves it away; delete_app() uninstalls a title.
#pragma once

#include <stdint.h>
#include <psp2/promoterutil.h>
#include <psp2/sysmodule.h>

static inline int promoter_load(void) {
    static uint32_t argp[] = {0x180000, 0xFFFFFFFF, 0xFFFFFFFF, 1, 0xFFFFFFFF, 0xFFFFFFFF};
    int result = -1;
    uint32_t buf[4] = {sizeof buf, (uint32_t)&result, 0xFFFFFFFF, 0xFFFFFFFF};
    int r = sceSysmoduleLoadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, sizeof argp, argp, (void *)buf);
    if (r < 0) return r;
    r = sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    if (r >= 0) r = scePromoterUtilityInit();
    return r;
}

static inline void promoter_unload(void) {
    uint32_t buf = 0;
    scePromoterUtilityExit();
    sceSysmoduleUnloadModuleInternal(SCE_SYSMODULE_INTERNAL_PROMOTER_UTIL);
    sceSysmoduleUnloadModuleInternalWithArg(SCE_SYSMODULE_INTERNAL_PAF, 0, NULL, (void *)&buf);
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
