/* DirectInput 8, implemented by the runtime (Android has no DirectInput). Guest COM objects:
   +0 vtable (HLE thunks), +4 kind (0 = IDirectInput8, 1 = keyboard, 2 = mouse, 3 = other),
   +8 reference count. Device state comes from rt_input_* (all keys up and a still mouse until a
   host input source is attached); there are no buffered events and no joysticks (pads go
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

/* IDirectInputDevice8::GetDeviceState(this, size, data) */
static void v_GetDeviceState(X86Ctx *ctx)
{
    uint32_t o = ARG(0), size = ARG(1), data = ARG(2), kind = ld32(o + 4);
    memset(rt_ptr(data), 0, size);
    if (kind == K_KEYBOARD) {
        memcpy(rt_ptr(data), rt_input_keys, size < 256 ? size : 256);
    } else if (kind == K_MOUSE && size >= 16) {
        st32(data, (uint32_t)rt_input_mouse[0]); st32(data + 4, (uint32_t)rt_input_mouse[1]);
        st32(data + 8, (uint32_t)rt_input_mouse[2]);
        memcpy(rt_ptr(data + 12), rt_input_buttons, size >= 20 ? 8 : 4);
        rt_input_mouse[0] = rt_input_mouse[1] = rt_input_mouse[2] = 0;
    }
    RET(DI_OK);
}

/* GetDeviceData(this, cbObjectData, rgdod, pdwInOut, flags): no buffered events. */
static void v_GetDeviceData(X86Ctx *ctx) { if (ARG(3)) st32(ARG(3), 0); RET(DI_OK); }

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

/* XInput 1.3 by ordinal: #2 XInputGetState, #3 XInputSetState. No pad source yet: not connected. */
static void x_NotConnected(X86Ctx *ctx) { RET(1167u); } /* ERROR_DEVICE_NOT_CONNECTED */

static const HleDef dinput_exports[] = {
    {"dinput8.dll", "DirectInput8Create", i_DirectInput8Create, 5},
    {"xinput1_3.dll", "#2", x_NotConnected, 2}, {"xinput1_3.dll", "#3", x_NotConnected, 2},
};

void rt_dinput_register(void) { rt_hle_register(dinput_exports, sizeof dinput_exports / sizeof *dinput_exports); }
