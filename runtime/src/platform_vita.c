// SPDX-License-Identifier: AGPL-3.0-only
// Platform layer for PS Vita: vitaGL for OpenGL ES, sceCtrl buttons as a W3C "standard" gamepad,
// front touchscreen as a pointer. vitaGL compiles the game's GLSL shaders at runtime through
// libshacccg.suprx, which each user installs on their own console (ur0:data/).

#include <stdio.h>
#include <string.h>

#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/devctl.h>
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
#ifdef PKR_DEBUG
    // Debug aid: with a file named "dump-shaders" in the data folder, the shader sources that vitaGL
    // hands to the Cg compiler (after GLSL translation) are written to <data>/shaders/.
    char flag[512];
    snprintf(flag, sizeof flag, "%s/dump-shaders", g_cfg.data_dir);
    if (file_exists(flag)) {
        snprintf(vgl_shader_dump_path, sizeof vgl_shader_dump_path, "%s/shaders", g_cfg.data_dir);
        sceIoMkdir(vgl_shader_dump_path, 0777);
        host_log("[gl] writing translated shaders to %s", vgl_shader_dump_path);
    }
#endif
    // Full float precision: the game samples big texture atlases where half floats misplace pixels.
    vglSetupRuntimeShaderCompiler(SHARK_OPT_FAST, SHARK_ENABLE, SHARK_DISABLE, SHARK_ENABLE);
    // Explicit GPU memory pools (explicit sizes also avoid vitaGL's free-memory query, which the
    // Vita3K emulator does not implement):
    //  - 95 MB of video RAM (CDRAM). An app gets 112 MB; libgxm takes 16 MB of it for the GPU's
    //    parameter buffer (vitaGL keeps the default size), and 1 MB stays free as a margin.
    //  - 64 MB of main RAM and 16 MB of physically contiguous RAM: slower for the GPU than CDRAM.
    //  - 4 MB of the system dialogs' memory budget (8.8 MB of main RAM). No system dialog is used
    //    (text is typed on the built-in keyboard, osk.c), but the rest stays free for the PAF module
    //    the system's installer needs in dev builds (0.59 gave vitaGL all of it, and PAF failed to
    //    load with "no free physical page"; devtools.c also calls the installer before this init).
    // vitaGL fills them in that order (video RAM first for textures); after them comes the heap.
    // Compiled shaders are kept in <data>/shader-cache (vitaGL's HAVE_SHADER_CACHE, keyed by a hash
    // of each shader's source): compiling one takes ~1 s here, so later starts and shader variants
    // (gl.c) skip that after the first time.
    static char cache[128];
    snprintf(cache, sizeof cache, "%s/shader-cache", g_cfg.data_dir);
    sceIoMkdir(cache, 0777);
    vglSetShaderCachePath(cache);
    // Note: the return value only says whether vitaGL fell back to another resolution.
    if (vglInitWithCustomSizes(0, SCREEN_W, SCREEN_H, 64 * 1024 * 1024, 95 * 1024 * 1024, 16 * 1024 * 1024,
                               4 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE))
        host_log("vitaGL: requested resolution unavailable, using a fallback");
    vglWaitVblankStart(GL_TRUE);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    host_log("GL: %s | %s | shader compiler: %s", (const char *)glGetString(GL_VERSION),
             (const char *)glGetString(GL_RENDERER), shacccg);
    (void)w, (void)h;
    return true;
}

void plat_shutdown(void) {}

#define HUD_COMBO (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)

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
        // SELECT pressed while holding L + R: debug line on/off.
        static bool select_was_down;
        bool select_down = pad.buttons & SCE_CTRL_SELECT;
        bool lr = (pad.buttons & HUD_COMBO) == HUD_COMBO;
        if (select_down && !select_was_down && lr) {
            select_was_down = true;
            memset(ev, 0, sizeof *ev);
            ev->type = PEV_TOGGLE_HUD;
            return true;
        }
        select_was_down = select_down;
        // START pressed while holding L + R: send the log to the dev server.
        static bool start_was_down;
        bool start_down = pad.buttons & SCE_CTRL_START;
        if (start_down && !start_was_down && lr) {
            start_was_down = true;
            memset(ev, 0, sizeof *ev);
            ev->type = PEV_SEND_LOG;
            return true;
        }
        start_was_down = start_down;
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
    // (The Sony id above, vendor 054c, makes the game pick its DualShock layout and PS button icons.)
    unsigned b = pad.buttons;
    static const unsigned map[17] = {
        SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE, SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,
        SCE_CTRL_L2, SCE_CTRL_R2, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_L3, SCE_CTRL_R3,
        SCE_CTRL_UP, SCE_CTRL_DOWN, SCE_CTRL_LEFT, SCE_CTRL_RIGHT, SCE_CTRL_PSBUTTON,
    };
    if ((b & HUD_COMBO) == HUD_COMBO) b &= ~SCE_CTRL_SELECT; // SELECT belongs to the debug toggle then
    for (int i = 0; i < 17; i++) p->buttons[i] = (b & map[i]) ? 1.0f : 0.0f;
    // The rear touchpad is not used: fingers resting on the back of the console kept "pressing" the
    // game's submit button (the DualShock touchpad click) and sent the login form over and over.
    // Submit by tapping the button on screen, or selecting it and pressing cross.
    p->axes[0] = stick(pad.lx);
    p->axes[1] = stick(pad.ly);
    p->axes[2] = stick(pad.rx);
    p->axes[3] = stick(pad.ry);
    return 1;
}

// Debug builds: every 30th frame, the GPU's time for the frame is measured by waiting for it before
// the swap. glFinish ends the scene, and on this tile-based GPU most of a frame's work (the pixels)
// only starts then, so the wait is close to the frame's GPU time. The frame before is waited for
// right after its swap (untimed): otherwise the GPU may still be busy with it, and the wait counted
// about two frames (0.56: 52 ms on a title screen that ran at 24 ms per frame). Costs those two
// frames their CPU/GPU overlap.
static double gpu_sum, gpu_max;
static int gpu_n;

void plat_swap(void) {
#ifdef PKR_DEBUG
    static unsigned frame;
    if (++frame % 30 == 0) {
        double t0 = host_now_ms();
        glFinish();
        double ms = host_now_ms() - t0;
        gpu_sum += ms;
        if (ms > gpu_max) gpu_max = ms;
        gpu_n++;
    }
    vglSwapBuffers(GL_FALSE);
    if (frame % 30 == 29) glFinish(); // the GPU is idle when the measured frame starts
#else
    vglSwapBuffers(GL_FALSE);
#endif
}

void plat_gpu_stats(double *avg_ms, double *max_ms, int *samples) {
    *avg_ms = gpu_n ? gpu_sum / gpu_n : 0, *max_ms = gpu_max, *samples = gpu_n;
    gpu_sum = gpu_max = 0, gpu_n = 0;
}

void plat_drawable_size(int *w, int *h) {
    *w = SCREEN_W;
    *h = SCREEN_H;
}

void plat_inject_pointer(plat_event_type type, float x, float y) {
    if (n_injected >= 32) return;
    injected[n_injected++] = (plat_event){.type = type, .x = x, .y = y};
}

void plat_keep_awake(void) { sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); }

long long plat_free_space(const char *path) {
    // sceIoDevctl 0x3001 on "ux0:" reports the device's size and free space.
    struct { uint64_t max_size, free_size; uint32_t cluster_size; void *unk; } info;
    char dev[16];
    const char *colon = strchr(path, ':');
    if (!colon || colon - path > 10) return -1;
    snprintf(dev, sizeof dev, "%.*s:", (int)(colon - path), path);
    memset(&info, 0, sizeof info);
    if (sceIoDevctl(dev, 0x3001, NULL, 0, &info, sizeof info) < 0) return -1;
    return (long long)info.free_size;
}

// ---------------------------------------------------------------- audio

#define AUDIO_GRAIN 1024 // frames per output block (~21 ms at 48 kHz)
static void (*audio_mix_fn)(int16_t *, int);
static int audio_port = -1;

static int audio_thread(SceSize args, void *argp) {
    (void)args, (void)argp;
    static int16_t buf[2][AUDIO_GRAIN * 2];
    int i = 0;
    for (;;) {
        audio_mix_fn(buf[i], AUDIO_GRAIN);
        sceAudioOutOutput(audio_port, buf[i]); // blocks until the previous block has been taken
        i ^= 1;
    }
    return 0;
}

bool plat_audio_start(int rate, void (*mix)(int16_t *out, int frames)) {
    audio_mix_fn = mix;
    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, AUDIO_GRAIN, rate, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0) {
        host_log("[audio] sceAudioOutOpenPort: 0x%08x", audio_port);
        return false;
    }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(audio_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    // Core 1, high priority: the game's JS runs on core 0.
    SceUID t = sceKernelCreateThread("pkr_audio_out", audio_thread, 0x10000100 - 20, 0x20000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) {
        host_log("[audio] could not start the output thread: 0x%08x", t);
        return false;
    }
    return true;
}

void plat_relaunch(int argc, char **argv) {
    (void)argc, (void)argv;
    sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
}

void plat_thread_setup(const char *name, int core, int priority) {
    (void)name;
    int mask = core == 1 ? SCE_KERNEL_CPU_MASK_USER_1 : core == 2 ? SCE_KERNEL_CPU_MASK_USER_2 : SCE_KERNEL_CPU_MASK_USER_0;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), mask);
    // The kernel doesn't time-slice threads of equal priority: a busy decoder would keep another
    // thread on its core from running at all (downloads timed out while sounds were decoded).
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x10000100 + priority);
}
