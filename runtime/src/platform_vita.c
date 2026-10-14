// SPDX-License-Identifier: AGPL-3.0-only
// Platform layer for PS Vita: vitaGL for OpenGL ES, sceCtrl buttons as a W3C "standard" gamepad,
// front touchscreen as a pointer. vitaGL compiles the game's GLSL shaders at runtime through
// libshacccg.suprx, which each user installs on their own console (ur0:data/).

#include <stdio.h>
#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <vitaGL.h>

#include "host.h"

#define SCREEN_W 960
#define SCREEN_H 544

static plat_event injected[32];
static int n_injected;
static SceCtrlData pad;
static bool touch_down;
static bool ime_active;
static bool mute_pad; // after the keyboard closes, hide buttons until they are released

void plat_inject_key(const char *name, bool down) {
    // Keys are only injected by the control channel; the Vita has no keyboard.
    static const struct { const char *name, *key, *code; int key_code; } keys[] = {
        {"Space", " ", "Space", 32}, {"Enter", "Enter", "Enter", 13}, {"Backspace", "Backspace", "Backspace", 8},
        {"Escape", "Escape", "Escape", 27}, {"ArrowUp", "ArrowUp", "ArrowUp", 38}, {"ArrowDown", "ArrowDown", "ArrowDown", 40},
        {"ArrowLeft", "ArrowLeft", "ArrowLeft", 37}, {"ArrowRight", "ArrowRight", "ArrowRight", 39},
    };
    if (n_injected >= 32) return;
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (!strcmp(keys[i].name, name)) {
            injected[n_injected++] = (plat_event){.type = down ? PEV_KEY_DOWN : PEV_KEY_UP, .key = keys[i].key,
                                                  .code = keys[i].code, .key_code = keys[i].key_code};
            return;
        }
    }
    host_log("[input] unknown key name: %s", name);
}

static bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

extern char vgl_shader_dump_path[256]; // vitaGL, patches/vitagl-0002

bool plat_init(int w, int h) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    const char *shacccg = file_exists("ur0:data/libshacccg.suprx")            ? "ur0:data/libshacccg.suprx"
                          : file_exists("ur0:data/external/libshacccg.suprx") ? "ur0:data/external/libshacccg.suprx"
                                                                              : NULL;
    if (!shacccg) {
        host_log("libshacccg.suprx not found in ur0:data/ or ur0:data/external/. "
                 "Install it (e.g. with PIB-Configuration-Tool) to compile shaders.");
        return false;
    }
    // Debug aid: with a file named "dump-shaders" in the data folder, the shader sources that vitaGL
    // hands to the Cg compiler (after GLSL translation) are written to <data>/shaders/.
    char flag[512];
    snprintf(flag, sizeof flag, "%s/dump-shaders", g_cfg.data_dir);
    if (file_exists(flag)) {
        snprintf(vgl_shader_dump_path, sizeof vgl_shader_dump_path, "%s/shaders", g_cfg.data_dir);
        sceIoMkdir(vgl_shader_dump_path, 0777);
        host_log("[gl] writing translated shaders to %s", vgl_shader_dump_path);
    }
    // Full float precision: the game samples big texture atlases where half floats misplace pixels.
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
    // Explicit GPU memory pools: 96 MB video RAM (CDRAM) first, then 48 MB of main RAM, 16 MB physically
    // contiguous RAM, 4 MB for system dialogs (the on-screen keyboard). Explicit sizes also avoid
    // vitaGL's free-memory query, which the Vita3K emulator does not implement.
    // Note: the return value only says whether vitaGL fell back to another resolution.
    if (vglInitWithCustomSizes(0, SCREEN_W, SCREEN_H, 48 * 1024 * 1024, 96 * 1024 * 1024, 16 * 1024 * 1024,
                               4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE))
        host_log("vitaGL: requested resolution unavailable, using a fallback");
    vglWaitVblankStart(GL_TRUE);

    // System dialogs (the on-screen keyboard) follow the console's language and confirm button.
    SceAppUtilInitParam ainit = {0};
    SceAppUtilBootParam aboot = {0};
    sceAppUtilInit(&ainit, &aboot);
    SceCommonDialogConfigParam cdcfg;
    sceCommonDialogConfigParamInit(&cdcfg);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, (int *)&cdcfg.language);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, (int *)&cdcfg.enterButtonAssign);
    sceCommonDialogSetConfigParam(&cdcfg);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    host_log("GL: %s | %s | shader compiler: %s", (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_RENDERER), shacccg);
    (void)w, (void)h;
    return true;
}

void plat_shutdown(void) {}

bool plat_poll_event(plat_event *ev) {
    if (n_injected) {
        *ev = injected[0];
        memmove(injected, injected + 1, --n_injected * sizeof *injected);
        return true;
    }
    static int phase; // 0: read controls, 1: touch, 2: done for this frame
    if (phase == 0) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        phase = 1;
        // START + SELECT held: quit.
        if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT)) {
            memset(ev, 0, sizeof *ev);
            ev->type = PEV_QUIT;
            return true;
        }
    }
    if (phase == 1) {
        phase = 2;
        SceTouchData t;
        sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1);
        memset(ev, 0, sizeof *ev);
        if (t.reportNum > 0) {
            // Touch panel coordinates are 1920x1088 for the 960x544 screen.
            ev->x = t.report[0].x / 2.0f;
            ev->y = t.report[0].y / 2.0f;
            ev->type = touch_down ? PEV_POINTER_MOVE : PEV_POINTER_DOWN;
            touch_down = true;
            return true;
        }
        if (touch_down) {
            touch_down = false;
            ev->type = PEV_POINTER_UP;
            return true;
        }
    }
    phase = 0;
    return false;
}

static float stick(unsigned char v) {
    float f = (v - 128) / 127.0f;
    if (f > 1) f = 1;
    if (f > -0.15f && f < 0.15f) f = 0; // dead zone
    return f;
}

int plat_gamepads(plat_gamepad *out, int max) {
    if (max < 1) return 0;
    plat_gamepad *p = &out[0];
    memset(p, 0, sizeof *p);
    p->connected = true;
    p->id = "PS Vita Controls (STANDARD GAMEPAD Vendor: 054c Product: 09cc)";
    // While the keyboard is open (and until its confirm press is released) the game sees no input.
    if (mute_pad && !ime_active && !(pad.buttons & 0xFFFF)) mute_pad = false;
    if (ime_active || mute_pad) return 1;
    // (The Sony id above, vendor 054c, makes the game pick its DualShock layout and PS button icons.)
    unsigned b = pad.buttons;
    static const unsigned map[17] = {
        SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
        SCE_CTRL_L2, SCE_CTRL_R2, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_L3, SCE_CTRL_R3,
        SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_PSBUTTON,
    };
    for (int i = 0; i < 17; i++) p->buttons[i] = (b & map[i]) ? 1.0f : 0.0f;
    // The rear touchpad stands in for the DualShock touchpad click, which the game uses as "submit"
    // (open the login form, send it).
    SceTouchData back;
    sceTouchPeek(SCE_TOUCH_PORT_BACK, &back, 1);
    p->buttons[17] = back.reportNum > 0 ? 1.0f : 0.0f;
    p->axes[0] = stick(pad.lx);
    p->axes[1] = stick(pad.ly);
    p->axes[2] = stick(pad.rx);
    p->axes[3] = stick(pad.ry);
    return 1;
}

void plat_swap(void) { vglSwapBuffers(ime_active ? GL_TRUE : GL_FALSE); }

// ---------------------------------------------------------------- text entry (system keyboard)

static uint16_t ime_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static uint16_t ime_initial[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t ime_buf[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

static void utf8_to_utf16(const char *in, uint16_t *out, size_t max) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < max) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        if (c >= 0x10000) {
            if (o + 2 >= max) break;
            c -= 0x10000;
            out[o++] = 0xD800 | (c >> 10);
            out[o++] = 0xDC00 | (c & 0x3FF);
        } else {
            out[o++] = (uint16_t)c;
        }
    }
    out[o] = 0;
}

static void utf16_to_utf8(const uint16_t *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        uint32_t c = *in;
        if (c >= 0xD800 && c < 0xDC00 && in[1] >= 0xDC00 && in[1] < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (*++in - 0xDC00);
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = 0xC0 | (c >> 6); out[o++] = 0x80 | (c & 63); }
        else if (c < 0x10000) { out[o++] = 0xE0 | (c >> 12); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
        else { out[o++] = 0xF0 | (c >> 18); out[o++] = 0x80 | ((c >> 12) & 63); out[o++] = 0x80 | ((c >> 6) & 63); out[o++] = 0x80 | (c & 63); }
    }
    out[o] = 0;
}

bool plat_ime_open(const char *title, const char *initial, int max_len, bool password) {
    if (ime_active) return false;
    utf8_to_utf16(title, ime_title, SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1);
    utf8_to_utf16(initial, ime_initial, SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1);
    memset(ime_buf, 0, sizeof ime_buf);
    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0; // the console's own keyboard languages
    param.languagesForced = SCE_FALSE;
    param.type = password ? SCE_IME_TYPE_BASIC_LATIN : SCE_IME_TYPE_DEFAULT;
    param.option = 0;
    param.textBoxMode = password ? SCE_IME_DIALOG_TEXTBOX_MODE_PASSWORD : SCE_IME_DIALOG_TEXTBOX_MODE_DEFAULT;
    param.title = ime_title;
    param.maxTextLength = max_len > 0 && max_len < SCE_IME_DIALOG_MAX_TEXT_LENGTH ? max_len : SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_buf;
    int rc = sceImeDialogInit(&param);
    if (rc < 0) {
        host_log("[ime] sceImeDialogInit: 0x%08x", rc);
        return false;
    }
    ime_active = true;
    mute_pad = true;
    return true;
}

int plat_ime_poll(char *out, size_t n) {
    if (!ime_active) return -1;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) return 0;
    SceImeDialogResult res;
    memset(&res, 0, sizeof res);
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    ime_active = false;
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return -1;
    utf16_to_utf8(ime_buf, out, n);
    return 1;
}

void plat_ime_answer(const char *text) { (void)text; }

void plat_drawable_size(int *w, int *h) {
    *w = SCREEN_W;
    *h = SCREEN_H;
}

void plat_inject_pointer(plat_event_type type, float x, float y) {
    if (n_injected >= 32) return;
    injected[n_injected++] = (plat_event){.type = type, .x = x, .y = y};
}
