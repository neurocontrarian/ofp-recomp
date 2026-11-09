/*
 * dinput8.c - DirectInput 8 (keyboard and mouse) on SDL2, for the lifted
 * Operation Flashpoint on runtime/win32hle.
 *
 * The game imports DirectInput8Create. A device reads SDL's own state, which
 * screen.c's pump keeps current on the game's thread: the keyboard from
 * SDL_GetKeyboardState (a scancode is a DIK code through k_dik), the mouse
 * from SDL's relative motion and buttons. Buffered reads (GetDeviceData) are
 * the differences since the last read, as DirectInput reports them.
 *
 * No joystick is offered: EnumDevices lists the keyboard and the mouse.
 *
 * OFP_DITRACE=1 prints every call.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "win32hle.h"
#include "ofp.h"

#define DI_OK              0u
#define S_FALSE            1u
#define E_NOINTERFACE      0x80004002u
#define DIERR_NOTFOUND     0x80070002u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_UNSUPPORTED  0x80004001u
#define DI_BUFFEROVERFLOW  1u

static int g_trace = -1;
static int trace(void) { if (g_trace < 0) g_trace = getenv("OFP_DITRACE") != NULL; return g_trace; }
#define T(...) do { if (trace()) fprintf(stderr, "[dinput8] " __VA_ARGS__); } while (0)

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2 };
typedef struct {
    uint32_t self;
    int kind, acquired, exclusive;
    uint32_t data_size;                /* the data format's: 256, 16 (DIMOUSESTATE), 20 (DIMOUSESTATE2) */
    uint32_t buffer_size;              /* DIPROP_BUFFERSIZE: 0 = no buffered data */
    uint8_t last_keys[256];            /* what GetDeviceData last reported */
    uint8_t last_buttons;
    uint32_t seq;
    int acc_x, acc_y, acc_z;           /* motion not yet read by GetDeviceState */
    int buf_x, buf_y, buf_z;           /* motion not yet read by GetDeviceData */
} didev_t;
#define HOST(o) ((didev_t *)hle_ptr(MEM32((o) + 8)))

static uint32_t g_vt_di, g_vt_dev, g_di;
static didev_t *g_mouse;

/* SDL scancode -> DIK (the PC's set 1 make codes, E0-prefixed ones | 0x80) */
static uint8_t dik_of(SDL_Scancode s) {
    static uint8_t t[SDL_NUM_SCANCODES];
    static int init;
    if (!init) {
        init = 1;
        static const struct { SDL_Scancode s; uint8_t d; } m[] = {
            { SDL_SCANCODE_ESCAPE, 0x01 }, { SDL_SCANCODE_1, 0x02 }, { SDL_SCANCODE_2, 0x03 }, { SDL_SCANCODE_3, 0x04 },
            { SDL_SCANCODE_4, 0x05 }, { SDL_SCANCODE_5, 0x06 }, { SDL_SCANCODE_6, 0x07 }, { SDL_SCANCODE_7, 0x08 },
            { SDL_SCANCODE_8, 0x09 }, { SDL_SCANCODE_9, 0x0A }, { SDL_SCANCODE_0, 0x0B }, { SDL_SCANCODE_MINUS, 0x0C },
            { SDL_SCANCODE_EQUALS, 0x0D }, { SDL_SCANCODE_BACKSPACE, 0x0E }, { SDL_SCANCODE_TAB, 0x0F },
            { SDL_SCANCODE_Q, 0x10 }, { SDL_SCANCODE_W, 0x11 }, { SDL_SCANCODE_E, 0x12 }, { SDL_SCANCODE_R, 0x13 },
            { SDL_SCANCODE_T, 0x14 }, { SDL_SCANCODE_Y, 0x15 }, { SDL_SCANCODE_U, 0x16 }, { SDL_SCANCODE_I, 0x17 },
            { SDL_SCANCODE_O, 0x18 }, { SDL_SCANCODE_P, 0x19 }, { SDL_SCANCODE_LEFTBRACKET, 0x1A },
            { SDL_SCANCODE_RIGHTBRACKET, 0x1B }, { SDL_SCANCODE_RETURN, 0x1C }, { SDL_SCANCODE_LCTRL, 0x1D },
            { SDL_SCANCODE_A, 0x1E }, { SDL_SCANCODE_S, 0x1F }, { SDL_SCANCODE_D, 0x20 }, { SDL_SCANCODE_F, 0x21 },
            { SDL_SCANCODE_G, 0x22 }, { SDL_SCANCODE_H, 0x23 }, { SDL_SCANCODE_J, 0x24 }, { SDL_SCANCODE_K, 0x25 },
            { SDL_SCANCODE_L, 0x26 }, { SDL_SCANCODE_SEMICOLON, 0x27 }, { SDL_SCANCODE_APOSTROPHE, 0x28 },
            { SDL_SCANCODE_GRAVE, 0x29 }, { SDL_SCANCODE_LSHIFT, 0x2A }, { SDL_SCANCODE_BACKSLASH, 0x2B },
            { SDL_SCANCODE_Z, 0x2C }, { SDL_SCANCODE_X, 0x2D }, { SDL_SCANCODE_C, 0x2E }, { SDL_SCANCODE_V, 0x2F },
            { SDL_SCANCODE_B, 0x30 }, { SDL_SCANCODE_N, 0x31 }, { SDL_SCANCODE_M, 0x32 }, { SDL_SCANCODE_COMMA, 0x33 },
            { SDL_SCANCODE_PERIOD, 0x34 }, { SDL_SCANCODE_SLASH, 0x35 }, { SDL_SCANCODE_RSHIFT, 0x36 },
            { SDL_SCANCODE_KP_MULTIPLY, 0x37 }, { SDL_SCANCODE_LALT, 0x38 }, { SDL_SCANCODE_SPACE, 0x39 },
            { SDL_SCANCODE_CAPSLOCK, 0x3A }, { SDL_SCANCODE_F1, 0x3B }, { SDL_SCANCODE_F2, 0x3C }, { SDL_SCANCODE_F3, 0x3D },
            { SDL_SCANCODE_F4, 0x3E }, { SDL_SCANCODE_F5, 0x3F }, { SDL_SCANCODE_F6, 0x40 }, { SDL_SCANCODE_F7, 0x41 },
            { SDL_SCANCODE_F8, 0x42 }, { SDL_SCANCODE_F9, 0x43 }, { SDL_SCANCODE_F10, 0x44 },
            { SDL_SCANCODE_NUMLOCKCLEAR, 0x45 }, { SDL_SCANCODE_SCROLLLOCK, 0x46 }, { SDL_SCANCODE_KP_7, 0x47 },
            { SDL_SCANCODE_KP_8, 0x48 }, { SDL_SCANCODE_KP_9, 0x49 }, { SDL_SCANCODE_KP_MINUS, 0x4A },
            { SDL_SCANCODE_KP_4, 0x4B }, { SDL_SCANCODE_KP_5, 0x4C }, { SDL_SCANCODE_KP_6, 0x4D },
            { SDL_SCANCODE_KP_PLUS, 0x4E }, { SDL_SCANCODE_KP_1, 0x4F }, { SDL_SCANCODE_KP_2, 0x50 },
            { SDL_SCANCODE_KP_3, 0x51 }, { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_PERIOD, 0x53 },
            { SDL_SCANCODE_NONUSBACKSLASH, 0x56 }, { SDL_SCANCODE_F11, 0x57 }, { SDL_SCANCODE_F12, 0x58 },
            { SDL_SCANCODE_KP_ENTER, 0x9C }, { SDL_SCANCODE_RCTRL, 0x9D }, { SDL_SCANCODE_KP_DIVIDE, 0xB5 },
            { SDL_SCANCODE_PRINTSCREEN, 0xB7 }, { SDL_SCANCODE_RALT, 0xB8 }, { SDL_SCANCODE_PAUSE, 0xC5 },
            { SDL_SCANCODE_HOME, 0xC7 }, { SDL_SCANCODE_UP, 0xC8 }, { SDL_SCANCODE_PAGEUP, 0xC9 },
            { SDL_SCANCODE_LEFT, 0xCB }, { SDL_SCANCODE_RIGHT, 0xCD }, { SDL_SCANCODE_END, 0xCF },
            { SDL_SCANCODE_DOWN, 0xD0 }, { SDL_SCANCODE_PAGEDOWN, 0xD1 }, { SDL_SCANCODE_INSERT, 0xD2 },
            { SDL_SCANCODE_DELETE, 0xD3 }, { SDL_SCANCODE_LGUI, 0xDB }, { SDL_SCANCODE_RGUI, 0xDC },
            { SDL_SCANCODE_APPLICATION, 0xDD },
        };
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) t[m[i].s] = m[i].d;
    }
    return (unsigned)s < SDL_NUM_SCANCODES ? t[s] : 0;
}
static void keys_now(uint8_t *out) {
    int n = 0;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    memset(out, 0, 256);
    if (overlay_blocks_input()) return;               /* the recomp's options have the keys */
    for (int s = 0; s < n; s++) if (k[s]) { uint8_t d = dik_of((SDL_Scancode)s); if (d) out[d] = 0x80; }
}
/* motion since the last call, into both the state's and the buffer's sums */
static void mouse_pull(void) {
    int dx = 0, dy = 0;
    SDL_GetRelativeMouseState(&dx, &dy);
    if (overlay_blocks_input()) return;
    if (g_mouse) g_mouse->acc_x += dx, g_mouse->acc_y += dy, g_mouse->buf_x += dx, g_mouse->buf_y += dy;
}
/* the wheel comes as events; screen.c consumes them, so it is watched here */
static int wheel_watch(void *u, SDL_Event *e) {
    (void)u;
    if (e->type == SDL_MOUSEWHEEL && g_mouse) {
        int z = e->wheel.y * 120;
        g_mouse->acc_z += z, g_mouse->buf_z += z;
    }
    return 1;
}
static uint8_t buttons_now(void) {
    uint32_t b = overlay_blocks_input() ? 0 : SDL_GetMouseState(NULL, NULL);
    uint8_t r = 0;
    if (b & SDL_BUTTON(SDL_BUTTON_LEFT)) r |= 1;
    if (b & SDL_BUTTON(SDL_BUTTON_RIGHT)) r |= 2;
    if (b & SDL_BUTTON(SDL_BUTTON_MIDDLE)) r |= 4;
    if (b & SDL_BUTTON(SDL_BUTTON_X1)) r |= 8;
    if (b & SDL_BUTTON(SDL_BUTTON_X2)) r |= 16;
    return r;
}

/* ---- IDirectInputDevice8A ---- */
#define SELF HOST(A32(0))
static void dd_QueryInterface(void) { uint32_t o = A32(0); if (A32(2)) MEM32(A32(2)) = o; MEM32(o + 4)++; RET(DI_OK, 3); }
static void dd_Release(void) { uint32_t n = MEM32(A32(0) + 4); if (n) MEM32(A32(0) + 4) = --n; RET(n, 1); }
static void dd_GetCapabilities(void) {               /* (this, DIDEVCAPS*) */
    didev_t *d = SELF;
    uint32_t p = A32(1), size = MEM32(p);
    memset(hle_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    MEM32(p + 4) = 1 | 0x4;                            /* ATTACHED, POLLEDDATAFORMAT */
    MEM32(p + 8) = d->kind == DEV_KEYBOARD ? 0x13 : 0x12;   /* DI8DEVTYPE_KEYBOARD / MOUSE */
    MEM32(p + 12) = d->kind == DEV_KEYBOARD ? 0 : 3;   /* axes */
    MEM32(p + 16) = d->kind == DEV_KEYBOARD ? 128 : 5; /* buttons */
    RET(DI_OK, 2);
}
static void dd_EnumObjects(void) { RET(DI_OK, 4); }
static void dd_GetProperty(void) {                   /* (this, guid, DIPROPHEADER*) */
    didev_t *d = SELF;
    uint32_t g = A32(1), p = A32(2);
    if (g == 1) { MEM32(p + 16) = d->buffer_size; RET(DI_OK, 3); }   /* DIPROP_BUFFERSIZE */
    if (g == 2) { MEM32(p + 16) = 0; RET(DI_OK, 3); }   /* DIPROP_AXISMODE: relative */
    RET(DIERR_UNSUPPORTED, 3);
}
static void dd_SetProperty(void) {
    didev_t *d = SELF;
    uint32_t g = A32(1), p = A32(2);
    if (g == 1) d->buffer_size = MEM32(p + 16);
    T("SetProperty %u = %u\n", g, MEM32(p + 16));
    RET(DI_OK, 3);
}
static void dd_Acquire(void) {
    didev_t *d = SELF;
    if (!d->acquired) {
        d->acquired = 1;
        if (d->kind == DEV_KEYBOARD) keys_now(d->last_keys);
        else {
            int dx, dy;
            SDL_GetRelativeMouseState(&dx, &dy);
            d->acc_x = d->acc_y = d->acc_z = d->buf_x = d->buf_y = d->buf_z = 0;
            d->last_buttons = buttons_now();
            if (d->exclusive && !getenv("OFP_NO_MOUSE_GRAB")) SDL_SetRelativeMouseMode(SDL_TRUE);
        }
        T("Acquire %s\n", d->kind == DEV_KEYBOARD ? "keyboard" : "mouse");
    }
    RET(DI_OK, 1);
}
static void dd_Unacquire(void) {
    didev_t *d = SELF;
    if (d->acquired && d->kind == DEV_MOUSE) SDL_SetRelativeMouseMode(SDL_FALSE);
    d->acquired = 0;
    RET(DI_OK, 1);
}
static void dd_GetDeviceState(void) {                /* (this, size, data) */
    didev_t *d = SELF;
    uint32_t n = A32(1);
    uint8_t *out = (uint8_t *)APTR(2);
    if (d->kind == DEV_KEYBOARD) {
        uint8_t k[256];
        keys_now(k);
        memcpy(out, k, n < 256 ? n : 256);
    } else {
        mouse_pull();
        int32_t st[5] = { d->acc_x, d->acc_y, d->acc_z, 0, 0 };
        d->acc_x = d->acc_y = d->acc_z = 0;
        uint8_t b = buttons_now();
        uint8_t btn[8] = { 0 };
        for (int i = 0; i < 5; i++) btn[i] = b & (1 << i) ? 0x80 : 0;
        memset(out, 0, n);
        memcpy(out, st, n < 12 ? n : 12);
        if (n > 12) memcpy(out + 12, btn, n - 12 < 8 ? n - 12 : 8);
    }
    RET(DI_OK, 3);
}
static void dd_GetDeviceData(void) {                 /* (this, objsize, data, &count, flags) */
    didev_t *d = SELF;
    uint32_t osz = A32(1), data = A32(2), pcount = A32(3), flags = A32(4);
    uint32_t room = MEM32(pcount), n = 0, now = SDL_GetTicks();
    int peek = (flags & 1) != 0;                       /* DIGDD_PEEK */
#define PUT(ofs, val) do { if (n < room && data) { uint32_t e = data + n * osz; memset(hle_ptr(e), 0, osz); \
        MEM32(e) = (ofs); MEM32(e + 4) = (uint32_t)(val); MEM32(e + 8) = now; MEM32(e + 12) = ++d->seq; } n++; } while (0)
    if (d->kind == DEV_KEYBOARD) {
        uint8_t k[256];
        keys_now(k);
        for (int i = 0; i < 256; i++) if (k[i] != d->last_keys[i]) {
            if (n < room || !data) { PUT((uint32_t)i, k[i]); if (!peek) d->last_keys[i] = k[i]; }
        }
    } else {
        mouse_pull();
        if (d->buf_x) { PUT(0, d->buf_x); if (!peek) d->buf_x = 0; }
        if (d->buf_y) { PUT(4, d->buf_y); if (!peek) d->buf_y = 0; }
        if (d->buf_z) { PUT(8, d->buf_z); if (!peek) d->buf_z = 0; }
        uint8_t b = buttons_now();
        for (int i = 0; i < 5; i++) if ((b ^ d->last_buttons) & (1 << i)) {
            PUT(12u + (uint32_t)i, b & (1 << i) ? 0x80 : 0);
            if (!peek) d->last_buttons ^= (uint8_t)(1 << i);
        }
    }
#undef PUT
    MEM32(pcount) = n < room || !data ? n : room;
    RET(DI_OK, 5);
}
static void dd_SetDataFormat(void) {                 /* (this, DIDATAFORMAT*) */
    didev_t *d = SELF;
    d->data_size = MEM32(A32(1) + 12);
    T("SetDataFormat %s: %u bytes\n", d->kind == DEV_KEYBOARD ? "keyboard" : "mouse", d->data_size);
    RET(DI_OK, 2);
}
static void dd_SetEventNotification(void) { RET(DI_OK, 2); }
static void dd_SetCooperativeLevel(void) {           /* (this, hwnd, flags) */
    didev_t *d = SELF;
    d->exclusive = (A32(2) & 1) != 0;                  /* DISCL_EXCLUSIVE */
    T("SetCooperativeLevel %s %X\n", d->kind == DEV_KEYBOARD ? "keyboard" : "mouse", A32(2));
    RET(DI_OK, 3);
}
static void dd_GetObjectInfo(void) { RET(DIERR_NOTFOUND, 4); }
static void put_instance(uint32_t p, int kind) {
    uint32_t size = MEM32(p);
    memset(hle_ptr(p + 4), 0, size > 4 ? size - 4 : 0);
    uint32_t d1 = kind == DEV_KEYBOARD ? 0x6F1D2B61u : 0x6F1D2B60u;
    static const uint8_t tail[12] = { 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
    MEM32(p + 4) = d1; memcpy(hle_ptr(p + 8), tail, 12);       /* guidInstance */
    MEM32(p + 20) = d1; memcpy(hle_ptr(p + 24), tail, 12);     /* guidProduct */
    MEM32(p + 36) = kind == DEV_KEYBOARD ? 0x0413 : 0x0212;    /* dwDevType */
    strcpy((char *)hle_ptr(p + 40), kind == DEV_KEYBOARD ? "Keyboard" : "Mouse");
    strcpy((char *)hle_ptr(p + 300), kind == DEV_KEYBOARD ? "Keyboard" : "Mouse");
}
static void dd_GetDeviceInfo(void) { put_instance(A32(1), SELF->kind); RET(DI_OK, 2); }
static void dd_unsupported2(void) { RET(DIERR_UNSUPPORTED, 3); }
static void dd_unsupported3(void) { RET(DIERR_UNSUPPORTED, 4); }
static void dd_unsupported4(void) { RET(DIERR_UNSUPPORTED, 5); }
static void dd_unsupported1(void) { RET(DIERR_UNSUPPORTED, 2); }
static void dd_EnumEffects(void) { RET(DI_OK, 4); }
static void dd_Poll(void) { RET(S_FALSE, 1); }                /* DI_NOEFFECT: no polling needed */
static void dd_Initialize(void) { RET(DI_OK, 4); }

/* ---- IDirectInput8A ---- */
static void di_QueryInterface(void) { uint32_t o = A32(0); if (A32(2)) MEM32(A32(2)) = o; MEM32(o + 4)++; RET(DI_OK, 3); }
static void di_Release(void) { uint32_t n = MEM32(A32(0) + 4); if (n) MEM32(A32(0) + 4) = --n; RET(n, 1); }
static void di_CreateDevice(void) {                  /* (this, guid, out, outer) */
    uint32_t d1 = MEM32(A32(1));
    int kind = d1 == 0x6F1D2B61u ? DEV_KEYBOARD : d1 == 0x6F1D2B60u ? DEV_MOUSE : 0;
    if (!kind) { T("CreateDevice %08X: not found\n", d1); MEM32(A32(2)) = 0; RET(DIERR_NOTFOUND, 4); }
    didev_t *d = (didev_t *)hle_alloc(sizeof *d);     /* in guest memory: its VA is the object's slot */
    d->kind = kind;
    d->self = hle_com_new(g_vt_dev, 12);
    MEM32(d->self + 8) = hle_va(d);
    if (kind == DEV_MOUSE) {
        g_mouse = d;
        static int watched;
        if (!watched) SDL_AddEventWatch(wheel_watch, NULL), watched = 1;
    }
    MEM32(A32(2)) = d->self;
    T("CreateDevice %s -> %08X\n", kind == DEV_KEYBOARD ? "keyboard" : "mouse", d->self);
    RET(DI_OK, 4);
}
static void di_EnumDevices(void) {                   /* (this, type, cb, ref, flags) */
    uint32_t type = A32(1), cb = A32(2), ref = A32(3);
    for (int kind = DEV_MOUSE; kind >= DEV_KEYBOARD; kind--) {
        /* DI8DEVCLASS_ALL 0, POINTER 2, KEYBOARD 3; or a DI8DEVTYPE */
        int want = type == 0 || (kind == DEV_MOUSE ? (type == 2 || type == 0x12) : (type == 3 || type == 0x13));
        if (!want) continue;
        uint32_t inst = hle_va(hle_alloc(580));
        MEM32(inst) = 580;
        put_instance(inst, kind);
        uint32_t args[2] = { inst, ref };
        uint32_t r = hle_call_guest(cb, 2, args);
        hle_free(hle_ptr(inst));
        if (!r) break;                                 /* DIENUM_STOP */
    }
    RET(DI_OK, 5);
}
static void di_GetDeviceStatus(void) {
    uint32_t d1 = MEM32(A32(1));
    RET(d1 == 0x6F1D2B61u || d1 == 0x6F1D2B60u ? DI_OK : 1u, 2);   /* DI_NOTATTACHED */
}
static void di_ok2(void) { RET(DI_OK, 3); }
static void di_FindDevice(void) { RET(DIERR_NOTFOUND, 4); }
static void di_EnumDevicesBySemantics(void) { RET(DI_OK, 6); }
static void di_ConfigureDevices(void) { RET(DIERR_UNSUPPORTED, 5); }

static void vtables(void) {
    if (g_vt_di) return;
    static const win32hle_shim di[] = {
        { "IDirectInput8A::QueryInterface", di_QueryInterface }, { "IDirectInput8A::AddRef", hle_com_AddRef },
        { "IDirectInput8A::Release", di_Release }, { "IDirectInput8A::CreateDevice", di_CreateDevice },
        { "IDirectInput8A::EnumDevices", di_EnumDevices }, { "IDirectInput8A::GetDeviceStatus", di_GetDeviceStatus },
        { "IDirectInput8A::RunControlPanel", di_ok2 }, { "IDirectInput8A::Initialize", di_ok2 },
        { "IDirectInput8A::FindDevice", di_FindDevice }, { "IDirectInput8A::EnumDevicesBySemantics", di_EnumDevicesBySemantics },
        { "IDirectInput8A::ConfigureDevices", di_ConfigureDevices }, { 0, 0 } };
    static const win32hle_shim dv[] = {
        { "IDirectInputDevice8A::QueryInterface", dd_QueryInterface }, { "IDirectInputDevice8A::AddRef", hle_com_AddRef },
        { "IDirectInputDevice8A::Release", dd_Release }, { "IDirectInputDevice8A::GetCapabilities", dd_GetCapabilities },
        { "IDirectInputDevice8A::EnumObjects", dd_EnumObjects }, { "IDirectInputDevice8A::GetProperty", dd_GetProperty },
        { "IDirectInputDevice8A::SetProperty", dd_SetProperty }, { "IDirectInputDevice8A::Acquire", dd_Acquire },
        { "IDirectInputDevice8A::Unacquire", dd_Unacquire }, { "IDirectInputDevice8A::GetDeviceState", dd_GetDeviceState },
        { "IDirectInputDevice8A::GetDeviceData", dd_GetDeviceData }, { "IDirectInputDevice8A::SetDataFormat", dd_SetDataFormat },
        { "IDirectInputDevice8A::SetEventNotification", dd_SetEventNotification },
        { "IDirectInputDevice8A::SetCooperativeLevel", dd_SetCooperativeLevel },
        { "IDirectInputDevice8A::GetObjectInfo", dd_GetObjectInfo }, { "IDirectInputDevice8A::GetDeviceInfo", dd_GetDeviceInfo },
        { "IDirectInputDevice8A::RunControlPanel", dd_unsupported2 }, { "IDirectInputDevice8A::Initialize", dd_Initialize },
        { "IDirectInputDevice8A::CreateEffect", dd_unsupported4 }, { "IDirectInputDevice8A::EnumEffects", dd_EnumEffects },
        { "IDirectInputDevice8A::GetEffectInfo", dd_unsupported2 },
        { "IDirectInputDevice8A::GetForceFeedbackState", dd_unsupported1 },
        { "IDirectInputDevice8A::SendForceFeedbackCommand", dd_unsupported1 },
        { "IDirectInputDevice8A::EnumCreatedEffectObjects", dd_unsupported3 },
        { "IDirectInputDevice8A::Escape", dd_unsupported1 }, { "IDirectInputDevice8A::Poll", dd_Poll },
        { "IDirectInputDevice8A::SendDeviceData", dd_unsupported4 },
        { "IDirectInputDevice8A::EnumEffectsInFile", dd_unsupported4 },
        { "IDirectInputDevice8A::WriteEffectToFile", dd_unsupported4 },
        { "IDirectInputDevice8A::BuildActionMap", dd_unsupported3 },
        { "IDirectInputDevice8A::SetActionMap", dd_unsupported3 },
        { "IDirectInputDevice8A::GetImageInfo", dd_unsupported1 }, { 0, 0 } };
    g_vt_di = hle_com_vtable(di);
    g_vt_dev = hle_com_vtable(dv);
}

static void DirectInput8Create(void) {               /* (hinst, version, riid, out, outer) */
    vtables();
    if (!g_di) g_di = hle_com_new(g_vt_di, 12);
    else MEM32(g_di + 4)++;
    MEM32(A32(3)) = g_di;
    fprintf(stderr, "[dinput8] DirectInput8Create(version %X)\n", A32(1));
    RET(DI_OK, 5);
}

const win32hle_shim ofp_dinput8_shims[] = {
    { "DirectInput8Create", DirectInput8Create },
    { 0, 0 }
};
