/* Guest->host Direct3D 9 bridge: proxies, guest vtables and the methods that cannot be generated
   (pointer-carrying structs, Lock/Unlock, IUnknown). See rt_d3d9.h for the proxy layout. */
#include "rt_d3d9.h"

#include <stdlib.h>
#include <string.h>

#define RET(v) (ctx->eax = (uint32_t)(v))
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_NOINTERFACE 0x80004002u

void *host_d3d9_create(uint32_t sdk_version);   /* rt_host_*.c */
void *rt_user32_host_hwnd(uint32_t guest_hwnd); /* rt_user32.c */
uint32_t rt_user32_guest_hwnd(void *host_hwnd);

static uint32_t vtables[D3D_IFACE_COUNT];
static HostMutex *d3d_lock;

/* ---- proxies ---- */
#define MAP_BITS 16
static struct { void *host; uint32_t proxy; } map[1u << MAP_BITS];

static uint32_t map_slot(void *host)
{
    uint32_t h = (uint32_t)(((uintptr_t)host >> 4) * 2654435761u) >> (32 - MAP_BITS);
    while (map[h].host && map[h].host != host) h = (h + 1) & ((1u << MAP_BITS) - 1);
    return h;
}

void *d3d_host(uint32_t proxy)
{
    if (!proxy) return NULL;
    void *p;
    memcpy(&p, rt_ptr(proxy + 8), sizeof p);
    return p;
}

uint32_t d3d_iface(uint32_t proxy) { return ld32(proxy + 4); }

uint32_t d3d_wrap(void *host, int iface)
{
    if (!host) return 0;
    host_lock(d3d_lock);
    uint32_t s = map_slot(host);
    if (map[s].host) {
        uint32_t p = map[s].proxy;
        if (ld32(p + 4) == (uint32_t)iface) {
            host_unlock(d3d_lock);
            return p;
        }
        /* Same address, another interface: the old object died without its proxy being forgotten
           (surfaces of a texture live and die with the texture; their Release never reaches 0)
           and the host reused the memory (DXVK on Android does, e.g. for a vertex buffer). The
           old proxy stays allocated: the guest may still hold it. */
        static int logged;
        if (logged++ < 20) rt_log("D3D9: host object %p reused (proxy %#x interface %u -> %d)", host, p, ld32(p + 4), iface);
    }
    uint32_t p = rt_heap_alloc(16, 1);
    st32(p, vtables[iface]);
    st32(p + 4, (uint32_t)iface);
    memcpy(rt_ptr(p + 8), &host, sizeof host);
    map[s].host = host;
    map[s].proxy = p;
    host_unlock(d3d_lock);
    return p;
}

/* Removes the proxy of a host object whose last reference is gone (linear-probing delete). */
static void d3d_forget(void *host)
{
    host_lock(d3d_lock);
    uint32_t s = map_slot(host);
    if (map[s].host) {
        rt_heap_free(map[s].proxy);
        map[s].host = NULL;
        for (uint32_t j = (s + 1) & ((1u << MAP_BITS) - 1); map[j].host; j = (j + 1) & ((1u << MAP_BITS) - 1)) {
            void *h = map[j].host;
            uint32_t pr = map[j].proxy;
            map[j].host = NULL;
            uint32_t k = map_slot(h);
            map[k].host = h;
            map[k].proxy = pr;
        }
    }
    host_unlock(d3d_lock);
}

void *d3d_host_hwnd(uint32_t guest_hwnd) { return guest_hwnd ? rt_user32_host_hwnd(guest_hwnd) : NULL; }

/* ---- guest vtables and Direct3DCreate9 ---- */
static void build_vtables(void)
{
    d3d_lock = host_mutex_new();
    for (int i = 0; i < D3D_IFACE_COUNT; i++) {
        const D3dIfaceTable *t = &d3d_iface_tables[i];
        if (!t->methods) continue;
        HleDef *defs = calloc(t->count, sizeof *defs);
        for (uint32_t k = 0; k < t->count; k++) {
            defs[k].dll = "d3d9.bridge";
            defs[k].name = t->methods[k].name;
            defs[k].fn = t->methods[k].fn;
            defs[k].nargs = t->methods[k].nargs;
        }
        rt_hle_register(defs, t->count);
        vtables[i] = rt_heap_alloc(4 * t->count, 1);
        for (uint32_t k = 0; k < t->count; k++) st32(vtables[i] + 4 * k, rt_hle_thunk("d3d9.bridge", t->methods[k].name));
    }
}

static void d_Direct3DCreate9(X86Ctx *ctx)
{
    if (!vtables[D3D_IDirect3D9]) build_vtables();
    void *d3d = host_d3d9_create(ARG(0));
    rt_log("Direct3DCreate9(%u) -> host %p", ARG(0), d3d);
    RET(d3d ? d3d_wrap(d3d, D3D_IDirect3D9) : 0);
}

/* D3DX 9 (d3dx9_35): the game compiles shaders only when its shader cache misses; the mesh
   functions belong to the editor. Until a compiler is wired in, they fail visibly. */
static void x_CheckVersion(X86Ctx *ctx) { RET(1); }
static void x_DebugMute(X86Ctx *ctx) { RET(0); }

static void x_Fail(X86Ctx *ctx)
{
    rt_log("D3DX function called (not available yet), return address %#x", ld32(ctx->esp));
    RET(0x80004005u); /* E_FAIL */
}

/* D3DXMatrixInverse(out, determinant or NULL, m): cofactor expansion in double, stored as float;
   NULL when the matrix is singular, as D3DX does. */
static void x_MatrixInverse(X86Ctx *ctx)
{
    uint32_t out = ARG(0), pdet = ARG(1), mp = ARG(2);
    double m[16], inv[16];
    for (int i = 0; i < 16; i++) m[i] = ldf(mp + 4u * (uint32_t)i);
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (pdet) stf(pdet, (float)det);
    if (det == 0.0) { RET(0); return; }
    for (int i = 0; i < 16; i++) stf(out + 4u * (uint32_t)i, (float)(inv[i] / det));
    RET(out);
}

static const HleDef d3d9_exports[] = {
    {"d3dx10_35.dll", "D3DXMatrixInverse", x_MatrixInverse, 3},
    {"d3dx10_35.dll", "D3DX10CheckVersion", x_CheckVersion, 2},
    {"d3d9.dll", "Direct3DCreate9", d_Direct3DCreate9, 1},
    {"d3dx9_35.dll", "D3DXCheckVersion", x_CheckVersion, 2},
    {"d3dx9_35.dll", "D3DXDebugMute", x_DebugMute, 1},
    {"d3dx9_35.dll", "D3DXCompileShader", x_Fail, 10},
    {"d3dx9_35.dll", "D3DXPreprocessShader", x_Fail, 6},
    {"d3dx9_35.dll", "D3DXDisassembleShader", x_Fail, 4},
    {"d3dx9_35.dll", "D3DXCreateMesh", x_Fail, 6},
    {"d3dx9_35.dll", "D3DXSimplifyMesh", x_Fail, 7},
    {"d3dx9_35.dll", "D3DXWeldVertices", x_Fail, 7},
    {"d3dx9_35.dll", "D3DXCleanMesh", x_Fail, 5},
    {"d3dx9_35.dll", "D3DXUVAtlasCreate", x_Fail, 18},
};

void rt_d3d9_register(void) { rt_hle_register(d3d9_exports, sizeof d3d9_exports / sizeof *d3d9_exports); }

/* ---- calling host methods by index ---- */
typedef uint32_t (D3D_CALL *Fn0)(void *);
typedef uint32_t (D3D_CALL *Fn1p)(void *, void *);
typedef uint32_t (D3D_CALL *Fn2pp)(void *, void *, void *);
#define CALL(obj, idx, type, ...) (((type)D3D_VT(obj)[idx])(obj, ##__VA_ARGS__))

void d3d_unsupported(X86Ctx *ctx)
{
    rt_log("D3D9: unsupported method called on proxy %#x (return %#x)", ARG(0), ld32(ctx->esp));
    RET(D3DERR_INVALIDCALL);
}

/* ---- IUnknown ---- */
static int iid_iface(uint32_t riid)
{
    for (uint32_t i = 0; i < d3d_iids_count; i++)
        if (!memcmp(rt_ptr(riid), &d3d_iids[i].iid, 16)) return d3d_iids[i].iface;
    return -1;
}

static void query(X86Ctx *ctx, uint32_t self_proxy, int idx, uint32_t riid, uint32_t ppv, int has_riid_first)
{
    (void)has_riid_first;
    void *self = d3d_host(self_proxy), *out = NULL;
    uint32_t hr = CALL(self, idx, Fn2pp, rt_ptr(riid), &out);
    int iface = iid_iface(riid);
    if (iface < 0 || iface == D3D_IDirect3DResource9 || iface == D3D_IDirect3DBaseTexture9)
        iface = (int)d3d_iface(self_proxy); /* IUnknown or a base interface: same object, same proxy */
    if (ppv) st32(ppv, out && !(hr & 0x80000000u) ? d3d_wrap(out, iface) : 0);
    RET(hr);
}

void d3d_special_QueryInterface(X86Ctx *ctx) { query(ctx, ARG(0), 0, ARG(1), ARG(2), 1); }

void d3d_special_AddRef(X86Ctx *ctx) { RET(CALL(d3d_host(ARG(0)), 1, Fn0)); }

void d3d_special_Release(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    uint32_t n = CALL(self, 2, Fn0);
    if (!n) d3d_forget(self);
    RET(n);
}

void d3d_special_GetContainer(X86Ctx *ctx)
{
    int idx = d3d_iface(ARG(0)) == D3D_IDirect3DSurface9 ? D3D_IDX_IDirect3DSurface9_GetContainer
                                                         : D3D_IDX_IDirect3DVolume9_GetContainer;
    query(ctx, ARG(0), idx, ARG(1), ARG(2), 1);
}

/* ---- present parameters: 32-bit guest layout (56 bytes) <-> host layout (HWND is pointer-wide) ---- */
typedef struct {
    uint32_t w, h, format, count, ms_type, ms_quality, swap_effect;
    void *hwnd;
    uint32_t windowed, auto_depth, depth_format, flags, refresh, interval;
} HostPresent;

static void pp_in(uint32_t g, HostPresent *p, int force_windowed)
{
    p->w = ld32(g); p->h = ld32(g + 4); p->format = ld32(g + 8); p->count = ld32(g + 12);
    p->ms_type = ld32(g + 16); p->ms_quality = ld32(g + 20); p->swap_effect = ld32(g + 24);
    p->hwnd = d3d_host_hwnd(ld32(g + 28));
    p->windowed = ld32(g + 32); p->auto_depth = ld32(g + 36); p->depth_format = ld32(g + 40);
    p->flags = ld32(g + 44); p->refresh = ld32(g + 48); p->interval = ld32(g + 52);
    rt_log("D3D9 present parameters: %ux%u format %u x%u ms %u swap %u windowed %u depth %u/%u flags %#x refresh %u interval %#x",
           p->w, p->h, p->format, p->count, p->ms_type, p->swap_effect, p->windowed, p->auto_depth, p->depth_format,
           p->flags, p->refresh, p->interval);
    {
        extern void rt_ue_set_buffer_size(uint32_t width, uint32_t height);
        rt_ue_set_buffer_size(p->w, p->h);
    }
    if (force_windowed && !p->windowed) {
        /* Bring-up on the PC: never take over the user's display. */
        p->windowed = 1;
        p->refresh = 0;
    }
}

static void pp_out(uint32_t g, const HostPresent *p)
{
    st32(g, p->w); st32(g + 4, p->h); st32(g + 8, p->format); st32(g + 12, p->count);
    st32(g + 16, p->ms_type); st32(g + 20, p->ms_quality); st32(g + 24, p->swap_effect);
    st32(g + 28, rt_user32_guest_hwnd(p->hwnd));
    st32(g + 36, p->auto_depth); st32(g + 40, p->depth_format); st32(g + 44, p->flags);
    st32(g + 48, p->refresh); st32(g + 52, p->interval);
    /* Windowed stays as the guest asked. */
}

void host_wait_window(void);
typedef uint32_t (D3D_CALL *FnCreateDevice)(void *, uint32_t, uint32_t, void *, uint32_t, HostPresent *, void **);

void d3d_special_CreateDevice(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0)), *dev = NULL;
    uint32_t flags = ARG(4);
    HostPresent p;
    pp_in(ARG(5), &p, 1);
    host_wait_window(); /* Android: no surface while the screen is off */
    uint32_t hr = ((FnCreateDevice)D3D_VT(self)[D3D_IDX_IDirect3D9_CreateDevice])(
        self, ARG(1), ARG(2), d3d_host_hwnd(ARG(3)), flags | 0x4 /* MULTITHREADED */, &p, &dev);
    rt_log("D3D9 CreateDevice(adapter %u, type %u, focus %#x -> %p, device window %p, flags %#x) = %#x, device %p",
           ARG(1), ARG(2), ARG(3), d3d_host_hwnd(ARG(3)), p.hwnd, flags, hr, dev);
    if (!(hr & 0x80000000u)) pp_out(ARG(5), &p);
    if (ARG(6)) st32(ARG(6), dev ? d3d_wrap(dev, D3D_IDirect3DDevice9) : 0);
    /* Without D3DCREATE_FPU_PRESERVE, D3D9 sets the calling thread's x87 to 24-bit precision,
       round to nearest; the guest's x87 model follows (x87_round). */
    if (!(flags & 0x2) && !(hr & 0x80000000u)) ctx->fcw = (ctx->fcw & ~0x0F00u);
    RET(hr);
}

typedef uint32_t (D3D_CALL *FnReset)(void *, HostPresent *);

void d3d_special_Reset(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    HostPresent p;
    pp_in(ARG(1), &p, 1);
    host_wait_window();
    uint32_t hr = ((FnReset)D3D_VT(self)[D3D_IDX_IDirect3DDevice9_Reset])(self, &p);
    if (!(hr & 0x80000000u)) pp_out(ARG(1), &p);
    RET(hr);
}

typedef uint32_t (D3D_CALL *FnSwapChain)(void *, HostPresent *, void **);

void d3d_special_CreateAdditionalSwapChain(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0)), *sc = NULL;
    HostPresent p;
    pp_in(ARG(1), &p, 1);
    uint32_t hr = ((FnSwapChain)D3D_VT(self)[D3D_IDX_IDirect3DDevice9_CreateAdditionalSwapChain])(self, &p, &sc);
    if (ARG(2)) st32(ARG(2), sc ? d3d_wrap(sc, D3D_IDirect3DSwapChain9) : 0);
    RET(hr);
}

void d3d_special_GetPresentParameters(X86Ctx *ctx)
{
    void *self = d3d_host(ARG(0));
    HostPresent p;
    memset(&p, 0, sizeof p);
    uint32_t hr = CALL(self, D3D_IDX_IDirect3DSwapChain9_GetPresentParameters, Fn1p, &p);
    if (!(hr & 0x80000000u)) { pp_out(ARG(1), &p); st32(ARG(1) + 32, p.windowed); }
    RET(hr);
}

void d3d_special_GetCreationParameters(X86Ctx *ctx)
{
    struct { uint32_t adapter, type; void *hwnd; uint32_t flags; } c;
    void *self = d3d_host(ARG(0));
    uint32_t hr = CALL(self, D3D_IDX_IDirect3DDevice9_GetCreationParameters, Fn1p, &c);
    uint32_t g = ARG(1);
    st32(g, c.adapter); st32(g + 4, c.type); st32(g + 8, rt_user32_guest_hwnd(c.hwnd)); st32(g + 12, c.flags);
    RET(hr);
}

/* D3DADAPTER_IDENTIFIER9 is packed to 4 bytes on x86 (1100 bytes) and naturally aligned on 64-bit
   hosts (1104: the trailing padding after WHQLLevel); the fields line up, so the first 1100 bytes
   are copied. Writing the host size straight into the guest overran a stack buffer by 4 bytes. */
typedef uint32_t (D3D_CALL *FnAdapterId)(void *, uint32_t, uint32_t, void *);
void d3d_special_GetAdapterIdentifier(X86Ctx *ctx)
{
    union { uint64_t align; uint8_t b[1112]; } id;
    void *self = d3d_host(ARG(0));
    uint32_t hr = CALL(self, D3D_IDX_IDirect3D9_GetAdapterIdentifier, FnAdapterId, ARG(1), ARG(2), id.b);
    if (hr == 0 && ARG(3)) memcpy(rt_ptr(ARG(3)), id.b, 1100);
    RET(hr);
}

void d3d_special_GetAdapterMonitor(X86Ctx *ctx) { RET(0x00020001u); } /* the HMONITOR USER32 reports */

void d3d_special_GetDC(X86Ctx *ctx) { RET(D3DERR_INVALIDCALL); }
void d3d_special_ReleaseDC(X86Ctx *ctx) { RET(D3DERR_INVALIDCALL); }

/* ---- Lock/Unlock through guest staging buffers ----
   Host-locked memory lies outside the guest's 4 GB, so a lock hands the guest a buffer in guest
   memory: filled from the host on lock (unless the contents are discarded) and copied back on
   unlock (unless read-only). Keyed by host object and subresource. */
#define LOCK_READONLY 0x10u
#define LOCK_DISCARD 0x2000u

typedef struct { void *host; uint32_t sub; uint32_t guest, size, readonly; void *bits; } Staging;
/* Level streaming keeps many textures locked across frames; the scan stops at the high-water mark. */
#define MAX_STAGING 8192
static Staging staging[MAX_STAGING];
static int staging_top;

static Staging *stage_get(void *host, uint32_t sub, int create)
{
    host_lock(d3d_lock);
    Staging *free_slot = NULL;
    for (int i = 0; i < staging_top; i++) {
        if (staging[i].host == host && staging[i].sub == sub) { host_unlock(d3d_lock); return &staging[i]; }
        if (!staging[i].host && !free_slot) free_slot = &staging[i];
    }
    if (create && !free_slot && staging_top < MAX_STAGING) free_slot = &staging[staging_top++];
    if (create && free_slot) { free_slot->host = host; free_slot->sub = sub; free_slot->guest = 0; free_slot->size = 0; }
    host_unlock(d3d_lock);
    if (!free_slot && create) rt_fatal("D3D9: more than %d resources locked at once", MAX_STAGING);
    return create ? free_slot : NULL;
}

static uint32_t stage_begin(void *host, uint32_t sub, void *bits, uint32_t size, uint32_t flags)
{
    Staging *s = stage_get(host, sub, 1);
    if (s->size != size || !s->guest) {
        if (s->guest) rt_heap_free(s->guest);
        s->guest = rt_heap_alloc(size ? size : 1, 0);
        s->size = size;
    }
    s->bits = bits;
    s->readonly = (flags & LOCK_READONLY) != 0;
    if (!(flags & LOCK_DISCARD) && size) memcpy(rt_ptr(s->guest), bits, size);
    return s->guest;
}

static void stage_end(void *host, uint32_t sub)
{
    Staging *s = stage_get(host, sub, 0);
    if (!s) return;
    if (!s->readonly && s->size) memcpy(s->bits, rt_ptr(s->guest), s->size);
    /* Only resources that are locked right now hold a slot and a guest buffer. */
    host_lock(d3d_lock);
    rt_heap_free(s->guest);
    memset(s, 0, sizeof *s);
    host_unlock(d3d_lock);
}

static int is_block_format(uint32_t fmt)
{
    return fmt == 0x31545844u || fmt == 0x32545844u || fmt == 0x33545844u || fmt == 0x34545844u ||
           fmt == 0x35545844u || fmt == 0x31495441u || fmt == 0x32495441u; /* DXT1-5, ATI1/ATI2 */
}

static uint32_t rows_of(uint32_t fmt, uint32_t height, uint32_t rect)
{
    uint32_t h = height;
    if (rect) h = ld32(rect + 12) - ld32(rect + 4); /* bottom - top */
    return is_block_format(fmt) ? (h + 3) / 4 : h;
}

typedef struct { uint32_t format, type, usage, pool, ms_type, ms_quality, width, height; } SurfDesc;
typedef struct { int32_t pitch; void *bits; } HostLockedRect;
typedef uint32_t (D3D_CALL *FnLockSurf)(void *, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockLevel)(void *, uint32_t, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockFace)(void *, uint32_t, uint32_t, HostLockedRect *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLevelDesc)(void *, uint32_t, SurfDesc *);
typedef uint32_t (D3D_CALL *FnU32)(void *, uint32_t);
typedef uint32_t (D3D_CALL *FnU32U32)(void *, uint32_t, uint32_t);

void d3d_special_LockRect(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    HostLockedRect lr = {0, NULL};
    SurfDesc d;
    uint32_t hr, sub, rect, flags, out;
    memset(&d, 0, sizeof d);
    if (iface == D3D_IDirect3DSurface9) {
        out = ARG(1); rect = ARG(2); flags = ARG(3); sub = 0;
        hr = ((FnLockSurf)D3D_VT(self)[D3D_IDX_IDirect3DSurface9_LockRect])(self, &lr, D3D_PTR(rect), flags);
        CALL(self, D3D_IDX_IDirect3DSurface9_GetDesc, Fn1p, &d);
    } else if (iface == D3D_IDirect3DTexture9) {
        sub = ARG(1); out = ARG(2); rect = ARG(3); flags = ARG(4);
        hr = ((FnLockLevel)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_LockRect])(self, sub, &lr, D3D_PTR(rect), flags);
        ((FnLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_GetLevelDesc])(self, sub, &d);
    } else if (iface == D3D_IDirect3DCubeTexture9) {
        uint32_t face = ARG(1), level = ARG(2);
        sub = face * 32 + level; out = ARG(3); rect = ARG(4); flags = ARG(5);
        hr = ((FnLockFace)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_LockRect])(self, face, level, &lr, D3D_PTR(rect), flags);
        ((FnLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_GetLevelDesc])(self, level, &d);
    } else {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (hr & 0x80000000u) { RET(hr); return; }
    uint32_t size = (uint32_t)lr.pitch * rows_of(d.format, d.height, rect);
    st32(out, (uint32_t)lr.pitch);
    st32(out + 4, stage_begin(self, sub, lr.bits, size, flags));
    RET(hr);
}

void d3d_special_UnlockRect(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    if (iface == D3D_IDirect3DSurface9) {
        stage_end(self, 0);
        RET(CALL(self, D3D_IDX_IDirect3DSurface9_UnlockRect, Fn0));
    } else if (iface == D3D_IDirect3DTexture9) {
        stage_end(self, ARG(1));
        RET(((FnU32)D3D_VT(self)[D3D_IDX_IDirect3DTexture9_UnlockRect])(self, ARG(1)));
    } else if (iface == D3D_IDirect3DCubeTexture9) {
        stage_end(self, ARG(1) * 32 + ARG(2));
        RET(((FnU32U32)D3D_VT(self)[D3D_IDX_IDirect3DCubeTexture9_UnlockRect])(self, ARG(1), ARG(2)));
    } else {
        RET(D3DERR_INVALIDCALL);
    }
}

typedef struct { uint32_t format, type, usage, pool, width, height, depth; } VolDesc;
typedef struct { int32_t row_pitch, slice_pitch; void *bits; } HostLockedBox;
typedef uint32_t (D3D_CALL *FnLockVol)(void *, HostLockedBox *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnLockVolLevel)(void *, uint32_t, HostLockedBox *, void *, uint32_t);
typedef uint32_t (D3D_CALL *FnVolLevelDesc)(void *, uint32_t, VolDesc *);

void d3d_special_LockBox(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    HostLockedBox lb = {0, 0, NULL};
    VolDesc d;
    uint32_t hr, sub, box, flags, out;
    memset(&d, 0, sizeof d);
    if (iface == D3D_IDirect3DVolume9) {
        out = ARG(1); box = ARG(2); flags = ARG(3); sub = 0;
        hr = ((FnLockVol)D3D_VT(self)[D3D_IDX_IDirect3DVolume9_LockBox])(self, &lb, D3D_PTR(box), flags);
        CALL(self, D3D_IDX_IDirect3DVolume9_GetDesc, Fn1p, &d);
    } else if (iface == D3D_IDirect3DVolumeTexture9) {
        sub = ARG(1); out = ARG(2); box = ARG(3); flags = ARG(4);
        hr = ((FnLockVolLevel)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_LockBox])(self, sub, &lb, D3D_PTR(box), flags);
        ((FnVolLevelDesc)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_GetLevelDesc])(self, sub, &d);
    } else {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (hr & 0x80000000u) { RET(hr); return; }
    uint32_t depth = box ? ld32(box + 20) - ld32(box + 16) : d.depth;
    uint32_t size = (uint32_t)lb.slice_pitch * depth;
    st32(out, (uint32_t)lb.row_pitch);
    st32(out + 4, (uint32_t)lb.slice_pitch);
    st32(out + 8, stage_begin(self, sub, lb.bits, size, flags));
    RET(hr);
}

void d3d_special_UnlockBox(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy);
    if (iface == D3D_IDirect3DVolume9) {
        stage_end(self, 0);
        RET(CALL(self, D3D_IDX_IDirect3DVolume9_UnlockBox, Fn0));
    } else {
        stage_end(self, ARG(1));
        RET(((FnU32)D3D_VT(self)[D3D_IDX_IDirect3DVolumeTexture9_UnlockBox])(self, ARG(1)));
    }
}

typedef uint32_t (D3D_CALL *FnLockBuf)(void *, uint32_t, uint32_t, void **, uint32_t);
typedef struct { uint32_t format, type, usage, pool, size, fvf; } BufDesc;

void d3d_special_Lock(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0), iface = d3d_iface(proxy);
    void *self = d3d_host(proxy), *bits = NULL;
    uint32_t offset = ARG(1), size = ARG(2), out = ARG(3), flags = ARG(4);
    int vb = iface == D3D_IDirect3DVertexBuffer9;
    uint32_t hr = ((FnLockBuf)D3D_VT(self)[vb ? D3D_IDX_IDirect3DVertexBuffer9_Lock : D3D_IDX_IDirect3DIndexBuffer9_Lock])(
        self, offset, size, &bits, flags);
    if (hr & 0x80000000u) { RET(hr); return; }
    if (!size) {
        BufDesc d;
        memset(&d, 0, sizeof d);
        CALL(self, vb ? D3D_IDX_IDirect3DVertexBuffer9_GetDesc : D3D_IDX_IDirect3DIndexBuffer9_GetDesc, Fn1p, &d);
        size = d.size - offset;
    }
    st32(out, stage_begin(self, 0, bits, size, flags));
    RET(hr);
}

void d3d_special_Unlock(X86Ctx *ctx)
{
    uint32_t proxy = ARG(0);
    void *self = d3d_host(proxy);
    int vb = d3d_iface(proxy) == D3D_IDirect3DVertexBuffer9;
    stage_end(self, 0);
    RET(CALL(self, vb ? D3D_IDX_IDirect3DVertexBuffer9_Unlock : D3D_IDX_IDirect3DIndexBuffer9_Unlock, Fn0));
}
