// SPDX-License-Identifier: AGPL-3.0-only
// Platform layer on SDL2: window + OpenGL ES 2 context, keyboard/mouse/gamepad input.
// SDL2 also exists for the Vita, so this file is meant to serve both (with vitaGL there).

#include <string.h>
#include <sys/statvfs.h>

#include <GLES2/gl2.h>
#include <SDL.h>

#include "host.h"

static SDL_Window *win;
static SDL_GLContext glctx;

// DOM key / code / legacy keyCode for the keys the game cares about.
typedef struct {
    SDL_Keycode sdl;
    const char *key, *code;
    int key_code;
} keymap;

static const keymap keys[] = {
    {SDLK_SPACE, " ", "Space", 32},          {SDLK_RETURN, "Enter", "Enter", 13},
    {SDLK_BACKSPACE, "Backspace", "Backspace", 8}, {SDLK_ESCAPE, "Escape", "Escape", 27},
    {SDLK_TAB, "Tab", "Tab", 9},             {SDLK_UP, "ArrowUp", "ArrowUp", 38},
    {SDLK_DOWN, "ArrowDown", "ArrowDown", 40}, {SDLK_LEFT, "ArrowLeft", "ArrowLeft", 37},
    {SDLK_RIGHT, "ArrowRight", "ArrowRight", 39}, {SDLK_LSHIFT, "Shift", "ShiftLeft", 16},
    {SDLK_RSHIFT, "Shift", "ShiftRight", 16}, {SDLK_LCTRL, "Control", "ControlLeft", 17},
    {SDLK_RCTRL, "Control", "ControlRight", 17}, {SDLK_LALT, "Alt", "AltLeft", 18},
    {SDLK_DELETE, "Delete", "Delete", 46},   {SDLK_HOME, "Home", "Home", 36},
    {SDLK_END, "End", "End", 35},            {SDLK_PAGEUP, "PageUp", "PageUp", 33},
    {SDLK_PAGEDOWN, "PageDown", "PageDown", 34}, {SDLK_MINUS, "-", "Minus", 189},
    {SDLK_EQUALS, "=", "Equal", 187},        {SDLK_COMMA, ",", "Comma", 188},
    {SDLK_PERIOD, ".", "Period", 190},       {SDLK_SLASH, "/", "Slash", 191},
    {SDLK_SEMICOLON, ";", "Semicolon", 186}, {SDLK_QUOTE, "'", "Quote", 222},
    {SDLK_LEFTBRACKET, "[", "BracketLeft", 219}, {SDLK_RIGHTBRACKET, "]", "BracketRight", 221},
    {SDLK_BACKSLASH, "\\", "Backslash", 220}, {SDLK_BACKQUOTE, "`", "Backquote", 192},
};

static char letter_key[26][2], letter_code[26][5], digit_key[10][2], digit_code[10][7];
static char fkey[12][4];

static bool map_key(SDL_Keycode k, plat_event *ev) {
    if (k >= SDLK_a && k <= SDLK_z) {
        int i = k - SDLK_a;
        ev->key = letter_key[i];
        ev->code = letter_code[i];
        ev->key_code = 'A' + i;
        return true;
    }
    if (k >= SDLK_0 && k <= SDLK_9) {
        int i = k - SDLK_0;
        ev->key = digit_key[i];
        ev->code = digit_code[i];
        ev->key_code = '0' + i;
        return true;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12) {
        int i = k - SDLK_F1;
        ev->key = ev->code = fkey[i];
        ev->key_code = 112 + i;
        return true;
    }
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (keys[i].sdl == k) {
            ev->key = keys[i].key;
            ev->code = keys[i].code;
            ev->key_code = keys[i].key_code;
            return true;
        }
    }
    return false;
}

// Synthetic events from the control channel, delivered before SDL's.
static plat_event injected[32];
static int n_injected;

void plat_inject_key(const char *name, bool down) {
    if (n_injected >= 32) return;
    plat_event ev = {.type = down ? PEV_KEY_DOWN : PEV_KEY_UP};
    SDL_Keycode k = SDLK_UNKNOWN;
    if (!strcmp(name, "Space") || !strcmp(name, " ")) k = SDLK_SPACE;
    else if (strlen(name) == 1 && name[0] >= 'a' && name[0] <= 'z') k = SDLK_a + (name[0] - 'a');
    else if (strlen(name) == 1 && name[0] >= 'A' && name[0] <= 'Z') k = SDLK_a + (name[0] - 'A');
    else if (strlen(name) == 1 && name[0] >= '0' && name[0] <= '9') k = SDLK_0 + (name[0] - '0');
    else {
        for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
            if (!strcmp(keys[i].code, name) || !strcmp(keys[i].key, name)) k = keys[i].sdl;
    }
    if (k == SDLK_UNKNOWN || !map_key(k, &ev)) {
        host_log("[input] unknown key name: %s", name);
        return;
    }
    injected[n_injected++] = ev;
}

static SDL_GameController *pads[4];

bool plat_init(int w, int h) {
    for (int i = 0; i < 26; i++) {
        letter_key[i][0] = 'a' + i;
        snprintf(letter_code[i], sizeof letter_code[i], "Key%c", 'A' + i);
    }
    for (int i = 0; i < 10; i++) {
        digit_key[i][0] = '0' + i;
        snprintf(digit_code[i], sizeof digit_code[i], "Digit%d", i);
    }
    for (int i = 0; i < 12; i++) snprintf(fkey[i], sizeof fkey[i], "F%d", i + 1);

    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        host_log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    win = SDL_CreateWindow("PokeRogue", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_OPENGL);
    if (!win) {
        host_log("SDL_CreateWindow: %s", SDL_GetError());
        return false;
    }
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) {
        host_log("SDL_GL_CreateContext: %s", SDL_GetError());
        return false;
    }
    SDL_GL_SetSwapInterval(1);
    host_log("GL: %s | %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return true;
}

void plat_shutdown(void) {
    if (glctx) SDL_GL_DeleteContext(glctx);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

bool plat_poll_event(plat_event *ev) {
    if (n_injected) {
        *ev = injected[0];
        memmove(injected, injected + 1, --n_injected * sizeof *injected);
        return true;
    }
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        memset(ev, 0, sizeof *ev);
        switch (e.type) {
        case SDL_QUIT:
            ev->type = PEV_QUIT;
            return true;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (!map_key(e.key.keysym.sym, ev)) continue;
            ev->type = e.type == SDL_KEYDOWN ? PEV_KEY_DOWN : PEV_KEY_UP;
            return true;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            ev->type = e.type == SDL_MOUSEBUTTONDOWN ? PEV_POINTER_DOWN : PEV_POINTER_UP;
            ev->x = e.button.x;
            ev->y = e.button.y;
            return true;
        case SDL_MOUSEMOTION:
            ev->type = PEV_POINTER_MOVE;
            ev->x = e.motion.x;
            ev->y = e.motion.y;
            return true;
        case SDL_CONTROLLERDEVICEADDED:
            for (int i = 0; i < 4; i++) {
                if (!pads[i]) {
                    pads[i] = SDL_GameControllerOpen(e.cdevice.which);
                    host_log("[input] gamepad %d: %s", i, SDL_GameControllerName(pads[i]));
                    break;
                }
            }
            continue;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int i = 0; i < 4; i++) {
                if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which) {
                    SDL_GameControllerClose(pads[i]);
                    pads[i] = NULL;
                }
            }
            continue;
        default:
            continue;
        }
    }
    return false;
}

int plat_gamepads(plat_gamepad *out, int max) {
    static const SDL_GameControllerButton map[17] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID, // triggers are axes in SDL
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_GUIDE,
    };
    int n = 0;
    for (int i = 0; i < 4 && n < max; i++) {
        plat_gamepad *p = &out[n];
        memset(p, 0, sizeof *p);
        if (!pads[i]) continue;
        p->connected = true;
        p->id = SDL_GameControllerName(pads[i]);
        for (int b = 0; b < 17; b++)
            if (map[b] != SDL_CONTROLLER_BUTTON_INVALID) p->buttons[b] = SDL_GameControllerGetButton(pads[i], map[b]);
        p->buttons[6] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
        p->buttons[7] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
        p->axes[0] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
        p->axes[1] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
        p->axes[2] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
        p->axes[3] = SDL_GameControllerGetAxis(pads[i], SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
        n++;
    }
    return n;
}

void plat_swap(void) { SDL_GL_SwapWindow(win); }

void plat_drawable_size(int *w, int *h) { SDL_GL_GetDrawableSize(win, w, h); }

// ---------------------------------------------------------------- text entry (test stand-in)

static int ime_state;    // 0 none, 1 open, 2 answered, 3 cancelled
static char ime_text[1024];

bool plat_ime_open(const char *title, const char *initial, int max_len, bool password) {
    host_log("[ime] open \"%s\" (max %d%s), initial \"%s\" - answer with: ime <text> / ime-cancel", title, max_len,
             password ? ", password" : "", initial);
    ime_state = 1;
    return true;
}

void plat_ime_answer(const char *text) {
    if (ime_state != 1) return;
    if (text) {
        snprintf(ime_text, sizeof ime_text, "%s", text);
        ime_state = 2;
    } else {
        ime_state = 3;
    }
}

int plat_ime_poll(char *out, size_t n) {
    if (ime_state == 2) {
        snprintf(out, n, "%s", ime_text);
        ime_state = 0;
        return 1;
    }
    if (ime_state == 3) {
        ime_state = 0;
        return -1;
    }
    return 0;
}

void plat_inject_pointer(plat_event_type type, float x, float y) {
    if (n_injected >= 32) return;
    injected[n_injected++] = (plat_event){.type = type, .x = x, .y = y};
}

void plat_keep_awake(void) {}

long long plat_free_space(const char *path) {
    struct statvfs v;
    return statvfs(path, &v) == 0 ? (long long)v.f_bavail * (long long)v.f_frsize : -1;
}
