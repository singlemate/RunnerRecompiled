/* Guest->host Direct3D 9 bridge. A guest COM pointer is a proxy in guest memory:
     +0  guest vtable (HLE thunks, one per method)
     +4  interface id (D3D_*), so shared special methods know what they were called on
     +8  host object pointer (64-bit)
   Host objects are called through their own vtables by index (D3D_VT), so the same generated
   code serves the system d3d9 of a Windows x64 host and DXVK Native on ARM64. */
#ifndef RT_D3D9_H
#define RT_D3D9_H

#include "rt.h"
#include "rt_d3d9_idx.h"

#ifdef _WIN32
#define D3D_CALL __stdcall
#else
#define D3D_CALL
#endif

enum {
    D3D_IDirect3D9, D3D_IDirect3DDevice9, D3D_IDirect3DSwapChain9, D3D_IDirect3DResource9, D3D_IDirect3DSurface9,
    D3D_IDirect3DVolume9, D3D_IDirect3DVertexBuffer9, D3D_IDirect3DIndexBuffer9, D3D_IDirect3DBaseTexture9,
    D3D_IDirect3DTexture9, D3D_IDirect3DCubeTexture9, D3D_IDirect3DVolumeTexture9, D3D_IDirect3DVertexDeclaration9,
    D3D_IDirect3DVertexShader9, D3D_IDirect3DPixelShader9, D3D_IDirect3DStateBlock9, D3D_IDirect3DQuery9,
    D3D_IFACE_COUNT
};

typedef struct { const char *name; HleFn fn; uint8_t nargs; } D3dMethod;
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } D3dGuid;
typedef struct { int iface; D3dGuid iid; } D3dIid;
extern const D3dIid d3d_iids[];
extern const uint32_t d3d_iids_count;
typedef struct { const char *name; const D3dMethod *methods; uint32_t count; } D3dIfaceTable;
extern const D3dIfaceTable d3d_iface_tables[D3D_IFACE_COUNT];

#define ARG(i) rt_arg(ctx, i)
#define D3D_VT(obj) (*(void ***)(obj))
#define D3D_PTR(a) ((a) ? rt_ptr(a) : NULL)

void *d3d_host(uint32_t proxy);
uint32_t d3d_iface(uint32_t proxy);
uint32_t d3d_wrap(void *host, int iface);   /* existing proxy of host, or a new one */
void *d3d_host_hwnd(uint32_t guest_hwnd);

/* Hand-written methods (rt_d3d9.c), shared by every interface that has them. */
void d3d_unsupported(X86Ctx *ctx);
void d3d_special_QueryInterface(X86Ctx *ctx);
void d3d_special_AddRef(X86Ctx *ctx);
void d3d_special_Release(X86Ctx *ctx);
void d3d_special_CreateDevice(X86Ctx *ctx);
void d3d_special_Reset(X86Ctx *ctx);
void d3d_special_CreateAdditionalSwapChain(X86Ctx *ctx);
void d3d_special_GetPresentParameters(X86Ctx *ctx);
void d3d_special_GetCreationParameters(X86Ctx *ctx);
void d3d_special_GetAdapterMonitor(X86Ctx *ctx);
void d3d_special_LockRect(X86Ctx *ctx);
void d3d_special_UnlockRect(X86Ctx *ctx);
void d3d_special_LockBox(X86Ctx *ctx);
void d3d_special_UnlockBox(X86Ctx *ctx);
void d3d_special_Lock(X86Ctx *ctx);
void d3d_special_Unlock(X86Ctx *ctx);
void d3d_special_GetDC(X86Ctx *ctx);
void d3d_special_ReleaseDC(X86Ctx *ctx);
void d3d_special_GetContainer(X86Ctx *ctx);

#endif
