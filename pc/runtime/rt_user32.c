/* USER32/GDI32 for bring-up: windows exist only inside the runtime (no host window yet). The
   guest's window procedures are called for SendMessage; the message queue stays empty until a
   real surface is attached together with Direct3D. The screen is 1280x720, 32 bpp, 60 Hz. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))
#define SCREEN_W 1280
#define SCREEN_H 720

typedef struct { char name[64]; uint32_t atom, wndproc; } WinClass;
typedef struct { int used; uint32_t wndproc, style, exstyle, x, y, w, h, userdata, visible; void *host; } Win;

void *host_window_create(int w, int h, const char *title);
int host_peek_message(void **hwnd, uint32_t *msg, uint32_t *wp, uint32_t *lp, uint32_t *time, int32_t *x, int32_t *y,
                      int remove);
void host_translate_last(void);
uint32_t host_def_window_proc(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void host_window_show(void *hwnd, int show);
void host_dispatch_last(void);
int host_post_message(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void host_post_quit(int code);
void host_validate(void *hwnd);

static WinClass classes[64];
static int nclasses;
static Win wins[256];
static uint32_t focus, foreground, capture;
static int32_t cursor_x = SCREEN_W / 2, cursor_y = SCREEN_H / 2, cursor_count;

#define HWND_BASE 0x00010010u
static Win *win(uint32_t h)
{
    uint32_t i = (h - HWND_BASE) / 4;
    if (h < HWND_BASE || (h & 3) || i >= 256 || !wins[i].used) return NULL;
    return &wins[i];
}

/* Guest <-> host windows (Direct3D presents into the host window of a guest window). */
void *rt_user32_host_hwnd(uint32_t h)
{
    Win *w = win(h);
    return w ? w->host : NULL;
}

uint32_t rt_user32_guest_hwnd(void *host)
{
    for (int i = 0; host && i < 256; i++)
        if (wins[i].used && wins[i].host == host) return HWND_BASE + 4 * (uint32_t)i;
    return 0;
}

static void class_name(uint32_t p, int wide, char *out, size_t n)
{
    if (p < 0x10000) { snprintf(out, n, "#%u", p); return; } /* atom */
    size_t i = 0;
    if (wide) for (; i + 1 < n && ld16(p + 2 * (uint32_t)i); i++) out[i] = (char)ld16(p + 2 * (uint32_t)i);
    else for (; i + 1 < n && ld8(p + (uint32_t)i); i++) out[i] = (char)ld8(p + (uint32_t)i);
    out[i] = 0;
}

static uint32_t register_class(uint32_t wndproc, uint32_t name, int wide)
{
    if (nclasses == 64) return 0;
    WinClass *c = &classes[nclasses++];
    class_name(name, wide, c->name, sizeof c->name);
    c->wndproc = wndproc;
    c->atom = 0xC000 + (uint32_t)nclasses;
    rt_log("RegisterClass \"%s\" wndproc %#x", c->name, wndproc);
    return c->atom;
}

/* WNDCLASSW: style, lpfnWndProc at +4, ..., lpszClassName at +36; WNDCLASSEXW has cbSize first. */
static void u_RegisterClassW(X86Ctx *ctx) { RET(register_class(ld32(ARG(0) + 4), ld32(ARG(0) + 36), 1)); }
static void u_RegisterClassExW(X86Ctx *ctx) { RET(register_class(ld32(ARG(0) + 8), ld32(ARG(0) + 40), 1)); }
static void u_RegisterClassExA(X86Ctx *ctx) { RET(register_class(ld32(ARG(0) + 8), ld32(ARG(0) + 40), 0)); }

static uint32_t find_class(uint32_t p, int wide)
{
    char n[64];
    class_name(p, wide, n, sizeof n);
    for (int i = 0; i < nclasses; i++)
        if (!_stricmp(classes[i].name, n) || (n[0] == '#' && classes[i].atom == (uint32_t)atoi(n + 1)))
            return classes[i].wndproc;
    return 0;
}

static uint32_t send(X86Ctx *ctx, uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp)
{
    Win *w = win(h);
    if (!w || !w->wndproc) return 0;
    uint32_t a[4] = {h, msg, wp, lp};
    return rt_call(ctx, w->wndproc, 4, a);
}

static void create_window(X86Ctx *ctx, int wide)
{
    uint32_t proc = find_class(ARG(1), wide);
    int i = 0;
    while (i < 256 && wins[i].used) i++;
    if (i == 256) { RET(0); return; }
    Win *w = &wins[i];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->exstyle = ARG(0);
    w->style = ARG(3);
    w->x = ARG(4) == 0x80000000u ? 0 : ARG(4);
    w->y = ARG(5) == 0x80000000u ? 0 : ARG(5);
    w->w = ARG(6) == 0x80000000u ? SCREEN_W : ARG(6);
    w->h = ARG(7) == 0x80000000u ? SCREEN_H : ARG(7);
    w->wndproc = proc;
    uint32_t h = HWND_BASE + 4 * (uint32_t)i;
    char cn[64];
    class_name(ARG(1), wide, cn, sizeof cn);
    if (!(w->style & 0x40000000u)) { /* not WS_CHILD: a real window on the host */
        w->host = host_window_create((int)w->w, (int)w->h, "Mirror's Edge (RunnerRecompiled PC)");
        w->visible = 1;
    }
    rt_log("CreateWindowEx class \"%s\" %ux%u style %#x -> hwnd %#x (wndproc %#x)", cn, w->w, w->h, w->style, h, proc);
    /* The caller's registers survive the nested guest calls: rt_call works on a copy of the stack. */
    uint32_t cs = rt_heap_alloc(48, 1); /* CREATESTRUCT */
    st32(cs + 0, ARG(11)); st32(cs + 4, ARG(9)); st32(cs + 8, ARG(8)); st32(cs + 12, ARG(10));
    st32(cs + 16, w->h); st32(cs + 20, w->w); st32(cs + 24, w->y); st32(cs + 28, w->x);
    st32(cs + 32, w->style); st32(cs + 36, ARG(2)); st32(cs + 40, ARG(1)); st32(cs + 44, w->exstyle);
    X86Ctx saved = *ctx;
    send(ctx, h, 0x0081, 0, cs);  /* WM_NCCREATE */
    send(ctx, h, 0x0001, 0, cs);  /* WM_CREATE */
    uint32_t esp = saved.esp;
    *ctx = saved;
    ctx->esp = esp;
    RET(h);
}

static void u_CreateWindowExW(X86Ctx *ctx) { create_window(ctx, 1); }
static void u_CreateWindowExA(X86Ctx *ctx) { create_window(ctx, 0); }

static void u_DestroyWindow(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    if (w) w->used = 0;
    RET(w != NULL);
}

static void u_GetSystemMetrics(X86Ctx *ctx)
{
    switch (ARG(0)) {
    case 0: case 16: case 78: RET(SCREEN_W); break;   /* SM_CXSCREEN, SM_CXFULLSCREEN, SM_CXVIRTUALSCREEN */
    case 1: case 17: case 79: RET(SCREEN_H); break;
    case 80: RET(1); break;                         /* SM_CMONITORS */
    case 4: RET(20); break;                         /* SM_CYCAPTION */
    case 5: case 6: case 32: case 33: RET(4); break; /* borders / frames */
    case 11: case 12: RET(32); break;               /* SM_CXICON */
    case 13: case 14: RET(32); break;               /* SM_CXCURSOR */
    case 43: RET(3); break;                         /* SM_CMOUSEBUTTONS */
    case 19: RET(1); break;                         /* SM_MOUSEPRESENT */
    default: RET(0); break;
    }
}

static void u_SystemParametersInfoW(X86Ctx *ctx)
{
    uint32_t action = ARG(0), p = ARG(2);
    if (action == 0x0030 && p) { /* SPI_GETWORKAREA */
        st32(p, 0); st32(p + 4, 0); st32(p + 8, SCREEN_W); st32(p + 12, SCREEN_H);
    } else if (p && (action == 0x003A || action == 0x0034 || action == 0x0032)) { /* accessibility structs */
        uint32_t sz = ld32(p);
        if (sz >= 8 && sz < 256) memset(rt_ptr(p + 4), 0, sz - 4);
    }
    RET(1);
}

static void u_MonitorFromWindow(X86Ctx *ctx) { RET(0x00020001u); }

static void u_GetMonitorInfoW(X86Ctx *ctx)
{
    uint32_t p = ARG(1);
    st32(p + 4, 0); st32(p + 8, 0); st32(p + 12, SCREEN_W); st32(p + 16, SCREEN_H);   /* rcMonitor */
    st32(p + 20, 0); st32(p + 24, 0); st32(p + 28, SCREEN_W); st32(p + 32, SCREEN_H); /* rcWork */
    st32(p + 36, 1);                                                                   /* MONITORINFOF_PRIMARY */
    if (ld32(p) >= 104) { const char *n = "\\\\.\\DISPLAY1"; for (int i = 0; i < 13; i++) st16(p + 40 + 2 * i, (uint8_t)n[i]); }
    RET(1);
}

static void rect_of(Win *w, uint32_t p, int client)
{
    st32(p, client ? 0 : w->x);
    st32(p + 4, client ? 0 : w->y);
    st32(p + 8, client ? w->w : w->x + w->w);
    st32(p + 12, client ? w->h : w->y + w->h);
}

static void u_GetWindowRect(X86Ctx *ctx) { Win *w = win(ARG(0)); if (w) rect_of(w, ARG(1), 0); RET(w != NULL); }
static void u_GetClientRect(X86Ctx *ctx) { Win *w = win(ARG(0)); if (w) rect_of(w, ARG(1), 1); RET(w != NULL); }
static void u_AdjustWindowRect(X86Ctx *ctx) { RET(1); } /* windows have no frame here */

static void u_SetWindowPos(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    uint32_t flags = ARG(6);
    if (w) {
        if (!(flags & 0x2)) { w->x = ARG(2); w->y = ARG(3); }     /* SWP_NOMOVE */
        if (!(flags & 0x1)) { w->w = ARG(4); w->h = ARG(5); }     /* SWP_NOSIZE */
        if (flags & 0x40) w->visible = 1;                         /* SWP_SHOWWINDOW */
    }
    RET(1);
}

static void u_MoveWindow(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    if (w) { w->x = ARG(1); w->y = ARG(2); w->w = ARG(3); w->h = ARG(4); }
    RET(1);
}

static void u_ShowWindow(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    uint32_t was = w ? w->visible : 0;
    if (w) { w->visible = ARG(1) != 0; if (w->host) host_window_show(w->host, (int)w->visible); }
    RET(was);
}

static void u_GetWindowLong(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    int32_t i = (int32_t)ARG(1);
    RET(!w ? 0 : i == -16 ? w->style : i == -20 ? w->exstyle : i == -4 ? w->wndproc : i == -21 ? w->userdata : 0);
}

static void u_SetWindowLong(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    int32_t i = (int32_t)ARG(1);
    uint32_t v = ARG(2), old = 0;
    if (w) {
        if (i == -16) { old = w->style; w->style = v; }
        else if (i == -20) { old = w->exstyle; w->exstyle = v; }
        else if (i == -4) { old = w->wndproc; w->wndproc = v; }
        else if (i == -21) { old = w->userdata; w->userdata = v; }
    }
    RET(old);
}

static void u_SetFocus(X86Ctx *ctx) { uint32_t o = focus; focus = ARG(0); RET(o); }
/* Focus and activation follow the host windows (a window shown in front has the keyboard focus,
   as on Windows); ME_PC_ASSUME_FOCUS=1 reports the main window focused even when another program
   is in front (automated tests that post input to a background window). */
void *host_focus_window(void);
void *host_foreground_window(void);
static uint32_t assumed_focus(void)
{
    static int assume = -1;
    if (assume < 0) { const char *e = getenv("ME_PC_ASSUME_FOCUS"); assume = e && e[0] == '1'; }
    return assume ? (foreground ? foreground : rt_user32_guest_hwnd(NULL)) : 0;
}

static void u_GetFocus(X86Ctx *ctx)
{
    uint32_t g = rt_user32_guest_hwnd(host_focus_window());
    RET(g ? g : focus ? focus : assumed_focus());
}
static void u_SetForegroundWindow(X86Ctx *ctx) { foreground = ARG(0); RET(1); }
static void u_GetForegroundWindow(X86Ctx *ctx)
{
    uint32_t g = rt_user32_guest_hwnd(host_foreground_window());
    RET(g ? g : assumed_focus() ? assumed_focus() : foreground);
}
static void u_SetActiveWindow(X86Ctx *ctx) { uint32_t o = foreground; foreground = ARG(0); RET(o); }
static void u_SetCapture(X86Ctx *ctx) { uint32_t o = capture; capture = ARG(0); RET(o); }
static void u_GetCapture(X86Ctx *ctx) { RET(capture); }
static void u_ReleaseCapture(X86Ctx *ctx) { capture = 0; RET(1); }

/* Input state seen through the window messages, shared with DirectInput (rt_dinput.c): virtual
   keys for GetKeyState/GetAsyncKeyState, DIK scan codes for the DirectInput keyboard, mouse
   buttons and relative motion for the DirectInput mouse. */
extern uint8_t rt_input_keys[256];
extern int32_t rt_input_mouse[3];
extern uint8_t rt_input_buttons[8];
static volatile uint8_t vk_down[256];
static int32_t last_mx, last_my, have_mouse;

static void track_input(uint32_t msg, uint32_t wp, uint32_t lp)
{
    if (msg == 0x0100 || msg == 0x0104 || msg == 0x0101 || msg == 0x0105) { /* WM_(SYS)KEYDOWN/UP */
        int down = msg == 0x0100 || msg == 0x0104;
        vk_down[wp & 0xFF] = (uint8_t)down;
        uint32_t sc = ((lp >> 16) & 0x7F) | ((lp >> 17) & 0x80); /* scan code, 0x80 = extended key */
        rt_input_keys[sc & 0xFF] = down ? 0x80 : 0;
        return;
    }
    static const struct { uint32_t down, up, vk, button; } mb[] = {
        {0x0201, 0x0202, 0x01, 0}, {0x0204, 0x0205, 0x02, 1}, {0x0207, 0x0208, 0x04, 2}};
    for (int i = 0; i < 3; i++)
        if (msg == mb[i].down || msg == mb[i].up) {
            vk_down[mb[i].vk] = msg == mb[i].down;
            rt_input_buttons[mb[i].button] = msg == mb[i].down ? 0x80 : 0;
        }
    if (msg == 0x0200 || (msg >= 0x0201 && msg <= 0x0208)) { /* WM_MOUSEMOVE and buttons: relative motion */
        int32_t x = (int16_t)(lp & 0xFFFF), y = (int16_t)(lp >> 16);
        if (have_mouse) { rt_input_mouse[0] += x - last_mx; rt_input_mouse[1] += y - last_my; }
        last_mx = x; last_my = y; have_mouse = 1;
    }
    if (msg == 0x020A) rt_input_mouse[2] += (int16_t)(wp >> 16); /* WM_MOUSEWHEEL */
}

/* Messages of the host queue for our windows, as guest MSGs. */
static int next_message(uint32_t m, int remove)
{
    void *hh;
    uint32_t msg, wp, lp, t;
    int32_t x, y;
    while (host_peek_message(&hh, &msg, &wp, &lp, &t, &x, &y, remove)) {
        uint32_t g = rt_user32_guest_hwnd(hh);
        if (!g && hh) {
            if (!remove) host_peek_message(&hh, &msg, &wp, &lp, &t, &x, &y, 1);
            host_dispatch_last();
            continue;
        }
        if (msg == 0x000F && hh) host_validate(hh); /* WM_PAINT: deliver once, then validated */
        if (remove) track_input(msg, wp, lp);
        st32(m, g); st32(m + 4, msg); st32(m + 8, wp); st32(m + 12, lp);
        st32(m + 16, t); st32(m + 20, (uint32_t)x); st32(m + 24, (uint32_t)y);
        return 1;
    }
    return 0;
}

static void u_PeekMessageW(X86Ctx *ctx) { RET(next_message(ARG(0), (ARG(4) & 1) != 0)); }

static void u_GetMessageW(X86Ctx *ctx)
{
    while (!next_message(ARG(0), 1)) host_sleep_ms(1);
    RET(ld32(ARG(0) + 4) != 0x0012); /* WM_QUIT ends the loop */
}

static void u_DispatchMessageW(X86Ctx *ctx)
{
    uint32_t m = ARG(0);
    RET(send(ctx, ld32(m), ld32(m + 4), ld32(m + 8), ld32(m + 12)));
}

static void u_SendMessageW(X86Ctx *ctx) { RET(send(ctx, ARG(0), ARG(1), ARG(2), ARG(3))); }
static void u_PostMessageW(X86Ctx *ctx)
{
    void *hh = ARG(0) ? rt_user32_host_hwnd(ARG(0)) : NULL;
    if (ARG(0) && !hh) { RET(0); return; } /* not a window of ours */
    RET(host_post_message(hh, ARG(1), ARG(2), ARG(3)));
}

/* Thread messages: only to the calling thread (guest thread ids are not host ids). */
static void u_PostThreadMessageW(X86Ctx *ctx)
{
    if (ARG(0) == rt_thread_self()->id) { RET(host_post_message(NULL, ARG(1), ARG(2), ARG(3))); return; }
    rt_log("PostThreadMessageW(thread %#x, msg %#x) to another thread: dropped", ARG(0), ARG(1));
    RET(1);
}

static void u_PostQuitMessage(X86Ctx *ctx) { host_post_quit((int)ARG(0)); }
static void u_TranslateMessage(X86Ctx *ctx) { host_translate_last(); RET(1); }

/* Only messages whose parameters are plain values go to the host's default handling. */
static void u_DefWindowProc(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    uint32_t msg = ARG(1);
    if (msg == 0x0081) { RET(1); return; } /* WM_NCCREATE: continue creation */
    static const uint32_t pass[] = {0x0006, 0x0010, 0x0020, 0x0084, 0x00A1, 0x00A2, 0x0104, 0x0105, 0x0106, 0x0112};
    for (unsigned i = 0; w && w->host && i < sizeof pass / sizeof *pass; i++)
        if (msg == pass[i]) { RET(host_def_window_proc(w->host, msg, ARG(2), ARG(3))); return; }
    RET(0);
}

static void u_GetCursorPos(X86Ctx *ctx) { st32(ARG(0), (uint32_t)cursor_x); st32(ARG(0) + 4, (uint32_t)cursor_y); RET(1); }
static void u_SetCursorPos(X86Ctx *ctx) { cursor_x = (int32_t)ARG(0); cursor_y = (int32_t)ARG(1); RET(1); }
static void u_ShowCursor(X86Ctx *ctx) { cursor_count += ARG(0) ? 1 : -1; RET((uint32_t)cursor_count); }

static void u_GetCursorInfo(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    st32(p + 4, cursor_count >= 0 ? 1 : 0);
    st32(p + 8, 0x00030001u);
    st32(p + 12, (uint32_t)cursor_x);
    st32(p + 16, (uint32_t)cursor_y);
    RET(1);
}

static void u_GetClipCursor(X86Ctx *ctx) { st32(ARG(0), 0); st32(ARG(0) + 4, 0); st32(ARG(0) + 8, SCREEN_W); st32(ARG(0) + 12, SCREEN_H); RET(1); }

static void u_ScreenToClient(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    if (w) { st32(ARG(1), ld32(ARG(1)) - w->x); st32(ARG(1) + 4, ld32(ARG(1) + 4) - w->y); }
    RET(1);
}

static void u_ClientToScreen(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    if (w) { st32(ARG(1), ld32(ARG(1)) + w->x); st32(ARG(1) + 4, ld32(ARG(1) + 4) + w->y); }
    RET(1);
}

static void u_MapWindowPoints(X86Ctx *ctx) { RET(0); } /* windows sit at the origin */
static void u_GetKeyState(X86Ctx *ctx) { RET(vk_down[ARG(0) & 0xFF] ? 0xFFFF8000u : 0); }
static void u_GetWindowThreadProcessId(X86Ctx *ctx) { if (ARG(1)) st32(ARG(1), 1); RET(rt_thread_self()->id); }

static void u_MessageBox(X86Ctx *ctx)
{
    rt_log("MessageBox: %s", ARG(1) ? (const char *)rt_ptr(ARG(1)) : "");
    RET(1); /* IDOK */
}

static void u_MessageBoxW(X86Ctx *ctx)
{
    char s[512];
    uint32_t p = ARG(1), i = 0;
    for (; p && i < sizeof s - 1 && ld16(p + 2 * i); i++) s[i] = (char)ld16(p + 2 * i);
    s[i] = 0;
    rt_log("MessageBoxW: %s", s);
    RET(1);
}

static void u_FakeHandle(X86Ctx *ctx) { RET(0x00030001u); }
static void u_True(X86Ctx *ctx) { RET(1); }
static void u_Zero(X86Ctx *ctx) { RET(0); }

static void g_GetDeviceCaps(X86Ctx *ctx)
{
    switch (ARG(1)) {
    case 8: RET(SCREEN_W); break;   /* HORZRES */
    case 10: RET(SCREEN_H); break;  /* VERTRES */
    case 12: RET(32); break;        /* BITSPIXEL */
    case 14: RET(1); break;         /* PLANES */
    case 88: case 90: RET(96); break; /* LOGPIXELSX/Y */
    case 116: RET(60); break;       /* VREFRESH */
    default: RET(0); break;
    }
}

/* ---- OLE32: COM initializes; no class is registered yet ---- */
static void o_CoCreateInstance(X86Ctx *ctx)
{
    uint32_t c = ARG(0);
    rt_log("CoCreateInstance {%08X-%04X-%04X-...}: class not registered", ld32(c), ld16(c + 4), ld16(c + 6));
    if (ARG(4)) st32(ARG(4), 0);
    RET(0x80040154u); /* REGDB_E_CLASSNOTREG */
}

static void o_CoCreateGuid(X86Ctx *ctx)
{
    static uint32_t seed = 0x9E3779B9u;
    for (int i = 0; i < 4; i++) {
        seed = seed * 1664525u + 1013904223u + (uint32_t)host_ticks_ns();
        st32(ARG(0) + 4 * (uint32_t)i, seed);
    }
    RET(0);
}

static void o_StringFromGUID2(X86Ctx *ctx)
{
    uint32_t g = ARG(0), out = ARG(1);
    char s[40];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", ld32(g), ld16(g + 4), ld16(g + 6),
             ld8(g + 8), ld8(g + 9), ld8(g + 10), ld8(g + 11), ld8(g + 12), ld8(g + 13), ld8(g + 14), ld8(g + 15));
    if (ARG(2) < 39) { RET(0); return; }
    for (int i = 0; i < 39; i++) st16(out + 2 * (uint32_t)i, (uint8_t)s[i]);
    RET(39);
}

/* ---- OLEAUT32 BSTR: a 4-byte byte length before the wide string ---- */
static void o_SysAllocString(X86Ctx *ctx)
{
    uint32_t s = ARG(0), n = 0;
    if (!s) { RET(0); return; }
    while (ld16(s + 2 * n)) n++;
    uint32_t b = rt_heap_alloc(4 + 2 * n + 2, 0);
    st32(b, 2 * n);
    memcpy(rt_ptr(b + 4), rt_ptr(s), 2 * n + 2);
    RET(b + 4);
}

static void o_SysFreeString(X86Ctx *ctx) { if (ARG(0)) rt_heap_free(ARG(0) - 4); }

/* POWRPROF: ProcessorInformation (level 11) for the one processor the guest sees, 3 GHz. */
static void p_CallNtPowerInformation(X86Ctx *ctx)
{
    uint32_t out = ARG(3), len = ARG(4);
    if (ARG(0) != 11 || len < 24) { RET(0xC0000002u); return; } /* STATUS_NOT_IMPLEMENTED */
    memset(rt_ptr(out), 0, len);
    st32(out + 4, 3000); st32(out + 8, 3000); st32(out + 12, 3000);
    RET(0);
}

/* WINDOWPLACEMENT: length, flags, showCmd, ptMin, ptMax, rcNormalPosition (44 bytes). */
static void u_GetWindowPlacement(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    uint32_t p = ARG(1);
    if (!w) { RET(0); return; }
    memset(rt_ptr(p + 4), 0, 40);
    st32(p + 8, w->visible ? 1 : 0); /* SW_SHOWNORMAL / SW_HIDE */
    st32(p + 28, w->x); st32(p + 32, w->y); st32(p + 36, w->x + w->w); st32(p + 40, w->y + w->h);
    RET(1);
}

static void u_SetWindowPlacement(X86Ctx *ctx)
{
    Win *w = win(ARG(0));
    uint32_t p = ARG(1);
    if (w) { w->x = ld32(p + 28); w->y = ld32(p + 32); w->w = ld32(p + 36) - w->x; w->h = ld32(p + 40) - w->y; }
    RET(w != NULL);
}

static const HleDef user32[] = {
    {"user32.dll", "GetWindowPlacement", u_GetWindowPlacement, 2},
    {"user32.dll", "SetWindowPlacement", u_SetWindowPlacement, 2},
    {"user32.dll", "OpenClipboard", u_Zero, 1},
    {"user32.dll", "CloseClipboard", u_True, 0},
    {"user32.dll", "EmptyClipboard", u_Zero, 0},
    {"user32.dll", "GetClipboardData", u_Zero, 1},
    {"user32.dll", "SetClipboardData", u_Zero, 2},
    {"powrprof.dll", "CallNtPowerInformation", p_CallNtPowerInformation, 5},
    {"oleaut32.dll", "#2", o_SysAllocString, 1},
    {"oleaut32.dll", "#6", o_SysFreeString, 1},
    {"ole32.dll", "CoInitialize", u_Zero, 1},
    {"ole32.dll", "CoInitializeEx", u_Zero, 2},
    {"ole32.dll", "CoUninitialize", u_Zero, 0},
    {"ole32.dll", "CoCreateInstance", o_CoCreateInstance, 5},
    {"ole32.dll", "CoCreateGuid", o_CoCreateGuid, 1},
    {"ole32.dll", "StringFromGUID2", o_StringFromGUID2, 3},
    {"ole32.dll", "CoSetProxyBlanket", u_Zero, 8},
    {"user32.dll", "GetSystemMetrics", u_GetSystemMetrics, 1},
    {"user32.dll", "SystemParametersInfoW", u_SystemParametersInfoW, 4},
    {"user32.dll", "MonitorFromWindow", u_MonitorFromWindow, 2},
    {"user32.dll", "GetMonitorInfoW", u_GetMonitorInfoW, 2},
    {"user32.dll", "RegisterClassW", u_RegisterClassW, 1},
    {"user32.dll", "RegisterClassExW", u_RegisterClassExW, 1},
    {"user32.dll", "RegisterClassExA", u_RegisterClassExA, 1},
    {"user32.dll", "UnregisterClassW", u_True, 2},
    {"user32.dll", "CreateWindowExW", u_CreateWindowExW, 12},
    {"user32.dll", "CreateWindowExA", u_CreateWindowExA, 12},
    {"user32.dll", "DestroyWindow", u_DestroyWindow, 1},
    {"user32.dll", "ShowWindow", u_ShowWindow, 2},
    {"user32.dll", "UpdateWindow", u_True, 1},
    {"user32.dll", "InvalidateRect", u_True, 3},
    {"user32.dll", "SetWindowPos", u_SetWindowPos, 7},
    {"user32.dll", "MoveWindow", u_MoveWindow, 6},
    {"user32.dll", "GetWindowRect", u_GetWindowRect, 2},
    {"user32.dll", "GetClientRect", u_GetClientRect, 2},
    {"user32.dll", "AdjustWindowRect", u_AdjustWindowRect, 3},
    {"user32.dll", "GetWindowLongW", u_GetWindowLong, 2},
    {"user32.dll", "GetWindowLongA", u_GetWindowLong, 2},
    {"user32.dll", "SetWindowLongW", u_SetWindowLong, 3},
    {"user32.dll", "SetWindowLongA", u_SetWindowLong, 3},
    {"user32.dll", "SetWindowTextW", u_True, 2},
    {"user32.dll", "SetWindowTextA", u_True, 2},
    {"user32.dll", "SetFocus", u_SetFocus, 1},
    {"user32.dll", "GetFocus", u_GetFocus, 0},
    {"user32.dll", "SetForegroundWindow", u_SetForegroundWindow, 1},
    {"user32.dll", "GetForegroundWindow", u_GetForegroundWindow, 0},
    {"user32.dll", "SetActiveWindow", u_SetActiveWindow, 1},
    {"user32.dll", "GetActiveWindow", u_GetForegroundWindow, 0},
    {"user32.dll", "IsIconic", u_Zero, 1},
    {"user32.dll", "SetCapture", u_SetCapture, 1},
    {"user32.dll", "GetCapture", u_GetCapture, 0},
    {"user32.dll", "ReleaseCapture", u_ReleaseCapture, 0},
    {"user32.dll", "PeekMessageW", u_PeekMessageW, 5},
    {"user32.dll", "GetMessageW", u_GetMessageW, 4},
    {"user32.dll", "TranslateMessage", u_TranslateMessage, 1},
    {"user32.dll", "DispatchMessageW", u_DispatchMessageW, 1},
    {"user32.dll", "SendMessageW", u_SendMessageW, 4},
    {"user32.dll", "PostMessageW", u_PostMessageW, 4},
    {"user32.dll", "PostThreadMessageW", u_PostThreadMessageW, 4},
    {"user32.dll", "PostQuitMessage", u_PostQuitMessage, 1},
    /* IMM32: no input method editor; the window has no input context. */
    {"imm32.dll", "ImmGetContext", u_Zero, 1}, {"imm32.dll", "ImmReleaseContext", u_True, 2},
    {"imm32.dll", "ImmAssociateContext", u_Zero, 2}, {"imm32.dll", "ImmCreateContext", u_Zero, 0},
    {"imm32.dll", "ImmDestroyContext", u_True, 1}, {"imm32.dll", "ImmGetCompositionStringW", u_Zero, 4},
    {"user32.dll", "DefWindowProcW", u_DefWindowProc, 4},
    {"user32.dll", "DefWindowProcA", u_DefWindowProc, 4},
    {"user32.dll", "LoadCursorW", u_FakeHandle, 2},
    {"user32.dll", "LoadIconW", u_FakeHandle, 2},
    {"user32.dll", "LoadIconA", u_FakeHandle, 2},
    {"user32.dll", "LoadImageW", u_FakeHandle, 6},
    {"user32.dll", "SetCursor", u_Zero, 1},
    {"user32.dll", "ShowCursor", u_ShowCursor, 1},
    {"user32.dll", "GetCursorPos", u_GetCursorPos, 1},
    {"user32.dll", "SetCursorPos", u_SetCursorPos, 2},
    {"user32.dll", "GetCursorInfo", u_GetCursorInfo, 1},
    {"user32.dll", "ClipCursor", u_True, 1},
    {"user32.dll", "GetClipCursor", u_GetClipCursor, 1},
    {"user32.dll", "ScreenToClient", u_ScreenToClient, 2},
    {"user32.dll", "ClientToScreen", u_ClientToScreen, 2},
    {"user32.dll", "MapWindowPoints", u_MapWindowPoints, 4},
    {"user32.dll", "GetKeyState", u_GetKeyState, 1},
    {"user32.dll", "GetAsyncKeyState", u_GetKeyState, 1},
    {"user32.dll", "GetDC", u_FakeHandle, 1},
    {"user32.dll", "ReleaseDC", u_True, 2},
    {"user32.dll", "MessageBoxA", u_MessageBox, 4},
    {"user32.dll", "MessageBoxW", u_MessageBoxW, 4},
    {"user32.dll", "SetWindowsHookExW", u_FakeHandle, 4},
    {"user32.dll", "UnhookWindowsHookEx", u_True, 1},
    {"user32.dll", "CallNextHookEx", u_Zero, 4},
    {"user32.dll", "RegisterHotKey", u_True, 4},
    {"user32.dll", "UnregisterHotKey", u_True, 2},
    {"user32.dll", "GetWindowThreadProcessId", u_GetWindowThreadProcessId, 2},
    {"user32.dll", "GetTopWindow", u_Zero, 1},
    {"gdi32.dll", "GetDeviceCaps", g_GetDeviceCaps, 2},
    /* GDI drawing (the start-up splash): handles exist, nothing is drawn. */
    {"gdi32.dll", "GetStockObject", u_FakeHandle, 1},
    {"gdi32.dll", "SelectObject", u_FakeHandle, 2},
    {"gdi32.dll", "DeleteObject", u_True, 1},
    {"gdi32.dll", "CreateFontW", u_FakeHandle, 14},
    {"gdi32.dll", "CreateCompatibleDC", u_FakeHandle, 1},
    {"gdi32.dll", "CreateSolidBrush", u_FakeHandle, 1},
    {"gdi32.dll", "CreateBitmap", u_FakeHandle, 5},
    {"gdi32.dll", "CreateDIBSection", u_Zero, 6},
    {"gdi32.dll", "SetTextColor", u_Zero, 2},
    {"gdi32.dll", "SetBkColor", u_Zero, 2},
    {"gdi32.dll", "TextOutW", u_True, 5},
    {"gdi32.dll", "GetPixel", u_Zero, 3},
    {"gdi32.dll", "GetObjectW", u_Zero, 3},
    {"user32.dll", "FillRect", u_True, 3},
    {"user32.dll", "DrawStateW", u_True, 10},
    {"user32.dll", "BeginPaint", u_FakeHandle, 2},
    {"user32.dll", "EndPaint", u_True, 2},
    {"user32.dll", "SetDlgItemTextW", u_True, 3},
    {"user32.dll", "GetDlgItem", u_Zero, 2},
    {"gdi32.dll", "CreateDCA", u_FakeHandle, 4},
    {"gdi32.dll", "DeleteDC", u_True, 1},
    {"gdi32.dll", "ExtEscape", u_Zero, 6},
};

void rt_user32_register(void) { rt_hle_register(user32, sizeof user32 / sizeof *user32); }
