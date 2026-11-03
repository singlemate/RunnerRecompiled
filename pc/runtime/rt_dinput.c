/* DirectInput 8, implemented by the runtime (Android has no DirectInput). Guest COM objects:
   +0 vtable (HLE thunks), +4 kind (0 = IDirectInput8, 1 = keyboard, 2 = mouse, 3 = other),
   +8 reference count. Device state comes from rt_input_* (filled from the window messages; on
   Windows the mouse is captured and read relative, host_mouse_poll); no buffered events, no joysticks (pads go
   through XInput). */
#include "rt.h"

#include <stdlib.h>
#include <string.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))
#define DI_OK 0u
#define DIERR_INVALIDPARAM 0x80070057u

enum { K_DINPUT, K_KEYBOARD, K_MOUSE, K_OTHER };

static uint32_t vt_dinput, vt_device;
int host_mouse_poll(int32_t *dx, int32_t *dy, uint8_t buttons[3]);
uint32_t host_pad_get(uint32_t index, uint8_t state[16]);
uint32_t host_pad_rumble(uint32_t index, uint16_t left, uint16_t right);

/* Host input state, filled from the window messages (rt_user32.c) or the platform layer. */
uint8_t rt_input_keys[256];      /* DIK_* scan codes, 0x80 = down */
int32_t rt_input_mouse[3];       /* relative x, y, wheel since the last read */
uint8_t rt_input_buttons[8];

static uint32_t new_object(uint32_t vt, uint32_t kind)
{
    uint32_t o = rt_heap_alloc(16, 1);
    st32(o, vt);
    st32(o + 4, kind);
    st32(o + 8, 1);
    return o;
}

static void m_AddRef(X86Ctx *ctx) { uint32_t o = ARG(0); st32(o + 8, ld32(o + 8) + 1); RET(ld32(o + 8)); }
static void m_Release(X86Ctx *ctx) { uint32_t o = ARG(0), n = ld32(o + 8) - 1; st32(o + 8, n); RET(n); }
static void m_QueryInterface(X86Ctx *ctx) { if (ARG(2)) st32(ARG(2), ARG(0)); m_AddRef(ctx); RET(DI_OK); }
static void m_Ok(X86Ctx *ctx) { RET(DI_OK); }
static void m_Unsupported(X86Ctx *ctx) { RET(0x80004001u); } /* E_NOTIMPL */

/* IDirectInput8::CreateDevice(this, rguid, out, outer) */
static void d_CreateDevice(X86Ctx *ctx)
{
    uint32_t g = ARG(1);
    /* GUID_SysKeyboard 6F1D2B61-D5A0-11CF-BFC7-444553540000, GUID_SysMouse 6F1D2B60-... */
    uint32_t d1 = ld32(g);
    uint32_t kind = d1 == 0x6F1D2B61u ? K_KEYBOARD : d1 == 0x6F1D2B60u ? K_MOUSE : K_OTHER;
    rt_log("DirectInput CreateDevice %s", kind == K_KEYBOARD ? "keyboard" : kind == K_MOUSE ? "mouse" : "other");
    if (ARG(2)) st32(ARG(2), new_object(vt_device, kind));
    RET(DI_OK);
}

/* Mouse motion since the last read: the captured host mouse, else the window-message totals. */
static void mouse_take(int32_t *dx, int32_t *dy, int32_t *dz, uint8_t b[3])
{
    int32_t hx, hy;
    uint8_t hb[3];
    if (host_mouse_poll(&hx, &hy, hb)) { rt_input_mouse[0] += hx; rt_input_mouse[1] += hy; memcpy(rt_input_buttons, hb, 3); }
    *dx = rt_input_mouse[0]; *dy = rt_input_mouse[1]; *dz = rt_input_mouse[2];
    rt_input_mouse[0] = rt_input_mouse[1] = rt_input_mouse[2] = 0;
    memcpy(b, rt_input_buttons, 3);
}

/* IDirectInputDevice8::GetDeviceState(this, size, data) */
static void v_GetDeviceState(X86Ctx *ctx)
{
    uint32_t o = ARG(0), size = ARG(1), data = ARG(2), kind = ld32(o + 4);
    memset(rt_ptr(data), 0, size);
    if (kind == K_KEYBOARD) {
        memcpy(rt_ptr(data), rt_input_keys, size < 256 ? size : 256);
    } else if (kind == K_MOUSE && size >= 16) {
        int32_t dx, dy, dz;
        uint8_t b[3];
        mouse_take(&dx, &dy, &dz, b);
        st32(data, (uint32_t)dx); st32(data + 4, (uint32_t)dy); st32(data + 8, (uint32_t)dz);
        memcpy(rt_ptr(data + 12), b, 3);
    }
    RET(DI_OK);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, flags): buffered mouse events, made from the
   motion and button changes since the previous call (DIDEVICEOBJECTDATA: ofs, data, time, seq, app;
   DIMOFS_X 0, Y 4, Z 8, BUTTON0.. 12..). The keyboard is read from window messages: none. */
static void v_GetDeviceData(X86Ctx *ctx)
{
    uint32_t o = ARG(0), cb = ARG(1), out = ARG(2), io = ARG(3), flags = ARG(4), kind = ld32(o + 4);
    uint32_t room = io ? ld32(io) : 0, n = 0;
    static uint8_t prev[3];
    static uint32_t seq;
    if (kind == K_MOUSE && !(flags & 1)) { /* DIGDD_PEEK leaves the data: report none */
        int32_t dx, dy, dz;
        uint8_t b[3];
        mouse_take(&dx, &dy, &dz, b);
        uint32_t ev[6][2];
        uint32_t k = 0;
        if (dx) { ev[k][0] = 0; ev[k][1] = (uint32_t)dx; k++; }
        if (dy) { ev[k][0] = 4; ev[k][1] = (uint32_t)dy; k++; }
        if (dz) { ev[k][0] = 8; ev[k][1] = (uint32_t)dz; k++; }
        for (int i = 0; i < 3; i++)
            if (b[i] != prev[i]) { ev[k][0] = 12u + (uint32_t)i; ev[k][1] = b[i]; k++; prev[i] = b[i]; }
        if (k && getenv("ME_PC_LOG_INPUT")) rt_log("DirectInput mouse: %u events (dx %d dy %d dz %d), room %u", k, dx, dy, dz, room);
        uint32_t now = (uint32_t)(host_ticks_ns() / 1000000u);
        for (uint32_t i = 0; i < k && n < room; i++, n++) {
            if (!out) continue;
            uint32_t e = out + n * cb;
            memset(rt_ptr(e), 0, cb);
            st32(e, ev[i][0]); st32(e + 4, ev[i][1]); st32(e + 8, now); st32(e + 12, ++seq);
        }
    }
    if (io) st32(io, n);
    RET(DI_OK);
}

/* GetCapabilities(this, DIDEVCAPS*) */
static void v_GetCapabilities(X86Ctx *ctx)
{
    uint32_t o = ARG(0), caps = ARG(1), kind = ld32(o + 4);
    uint32_t size = ld32(caps);
    memset(rt_ptr(caps + 4), 0, size > 4 ? size - 4 : 0);
    st32(caps + 4, 1);                                     /* DIDC_ATTACHED */
    st32(caps + 8, kind == K_KEYBOARD ? 0x13u : 0x12u);    /* DI8DEVTYPE_KEYBOARD / MOUSE */
    st32(caps + 12, kind == K_MOUSE ? 3 : 0);              /* axes */
    st32(caps + 16, kind == K_KEYBOARD ? 128 : 5);         /* buttons */
    RET(DI_OK);
}

static void v_GetProperty(X86Ctx *ctx) { RET(0x80004001u); }

typedef struct { const char *name; HleFn fn; uint8_t nargs; } Method;

static const Method dinput_methods[] = {
    {"IDirectInput8::QueryInterface", m_QueryInterface, 3}, {"IDirectInput8::AddRef", m_AddRef, 1},
    {"IDirectInput8::Release", m_Release, 1}, {"IDirectInput8::CreateDevice", d_CreateDevice, 4},
    {"IDirectInput8::EnumDevices", m_Ok, 5}, {"IDirectInput8::GetDeviceStatus", m_Ok, 2},
    {"IDirectInput8::RunControlPanel", m_Ok, 3}, {"IDirectInput8::Initialize", m_Ok, 3},
    {"IDirectInput8::FindDevice", m_Unsupported, 4}, {"IDirectInput8::EnumDevicesBySemantics", m_Ok, 6},
    {"IDirectInput8::ConfigureDevices", m_Unsupported, 5},
};

static const Method device_methods[] = {
    {"IDirectInputDevice8::QueryInterface", m_QueryInterface, 3}, {"IDirectInputDevice8::AddRef", m_AddRef, 1},
    {"IDirectInputDevice8::Release", m_Release, 1}, {"IDirectInputDevice8::GetCapabilities", v_GetCapabilities, 2},
    {"IDirectInputDevice8::EnumObjects", m_Ok, 4}, {"IDirectInputDevice8::GetProperty", v_GetProperty, 3},
    {"IDirectInputDevice8::SetProperty", m_Ok, 3}, {"IDirectInputDevice8::Acquire", m_Ok, 1},
    {"IDirectInputDevice8::Unacquire", m_Ok, 1}, {"IDirectInputDevice8::GetDeviceState", v_GetDeviceState, 3},
    {"IDirectInputDevice8::GetDeviceData", v_GetDeviceData, 5}, {"IDirectInputDevice8::SetDataFormat", m_Ok, 2},
    {"IDirectInputDevice8::SetEventNotification", m_Ok, 2}, {"IDirectInputDevice8::SetCooperativeLevel", m_Ok, 3},
    {"IDirectInputDevice8::GetObjectInfo", m_Unsupported, 4}, {"IDirectInputDevice8::GetDeviceInfo", m_Unsupported, 2},
    {"IDirectInputDevice8::RunControlPanel", m_Ok, 3}, {"IDirectInputDevice8::Initialize", m_Ok, 4},
    {"IDirectInputDevice8::CreateEffect", m_Unsupported, 5}, {"IDirectInputDevice8::EnumEffects", m_Ok, 4},
    {"IDirectInputDevice8::GetEffectInfo", m_Unsupported, 3}, {"IDirectInputDevice8::GetForceFeedbackState", m_Unsupported, 2},
    {"IDirectInputDevice8::SendForceFeedbackCommand", m_Unsupported, 2},
    {"IDirectInputDevice8::EnumCreatedEffectObjects", m_Ok, 4}, {"IDirectInputDevice8::Escape", m_Unsupported, 2},
    {"IDirectInputDevice8::Poll", m_Ok, 1}, {"IDirectInputDevice8::SendDeviceData", m_Unsupported, 5},
    {"IDirectInputDevice8::EnumEffectsInFile", m_Unsupported, 5}, {"IDirectInputDevice8::WriteEffectToFile", m_Unsupported, 5},
    {"IDirectInputDevice8::BuildActionMap", m_Unsupported, 4}, {"IDirectInputDevice8::SetActionMap", m_Unsupported, 4},
    {"IDirectInputDevice8::GetImageInfo", m_Unsupported, 2},
};

static uint32_t build_vtable(const Method *m, uint32_t n)
{
    HleDef *defs = calloc(n, sizeof *defs);
    for (uint32_t k = 0; k < n; k++) { defs[k].dll = "dinput8.object"; defs[k].name = m[k].name; defs[k].fn = m[k].fn; defs[k].nargs = m[k].nargs; }
    rt_hle_register(defs, n);
    uint32_t vt = rt_heap_alloc(4 * n, 1);
    for (uint32_t k = 0; k < n; k++) st32(vt + 4 * k, rt_hle_thunk("dinput8.object", m[k].name));
    return vt;
}

/* DirectInput8Create(hinst, version, riid, out, outer) */
static void i_DirectInput8Create(X86Ctx *ctx)
{
    if (!vt_dinput) {
        vt_dinput = build_vtable(dinput_methods, sizeof dinput_methods / sizeof *dinput_methods);
        vt_device = build_vtable(device_methods, sizeof device_methods / sizeof *device_methods);
    }
    if (!ARG(3)) { RET(DIERR_INVALIDPARAM); return; }
    st32(ARG(3), new_object(vt_dinput, K_DINPUT));
    rt_log("DirectInput8Create(version %#x)", ARG(1));
    RET(DI_OK);
}

/* XInput 1.3 by ordinal: #2 XInputGetState(index, XINPUT_STATE*), #3 XInputSetState(index,
   XINPUT_VIBRATION*), passed to the host's pads (the structures have the same layout). */
static void x_GetState(X86Ctx *ctx)
{
    uint8_t st[16];
    uint32_t r = host_pad_get(ARG(0), st);
    if (r == 0 && ARG(1)) memcpy(rt_ptr(ARG(1)), st, 16);
    RET(r);
}

static void x_SetState(X86Ctx *ctx) { RET(ARG(1) ? host_pad_rumble(ARG(0), ld16(ARG(1)), ld16(ARG(1) + 2)) : 87u); }

static const HleDef dinput_exports[] = {
    {"dinput8.dll", "DirectInput8Create", i_DirectInput8Create, 5},
    {"xinput1_3.dll", "#2", x_GetState, 2}, {"xinput1_3.dll", "#3", x_SetState, 2},
};

void rt_dinput_register(void) { rt_hle_register(dinput_exports, sizeof dinput_exports / sizeof *dinput_exports); }
