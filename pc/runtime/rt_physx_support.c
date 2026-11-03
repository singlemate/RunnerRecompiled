/* System imports of the PhysX 2.8 DLLs: CUDA (GPU PhysX), SETUPAPI (AGEIA PPU card search) and
   the Winsock calls of the PhysX Visual Debugger connection. None of that hardware exists for
   the guest, so physics runs on the CPU path: CUDA reports "no device", the PPU search finds
   nothing, sockets fail. CUDA's runtime API is __stdcall on Win32 (CUDARTAPI); the argument
   counts follow the CUDA 6.5 headers. */
#include "rt.h"

#include <string.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))
#define CUDA_ERROR_NO_DEVICE 38u

static void c_NoDevice(X86Ctx *ctx) { RET(CUDA_ERROR_NO_DEVICE); }
static void c_Void(X86Ctx *ctx) { RET(0); }

static void c_RegisterFatBinary(X86Ctx *ctx)
{
    static uint32_t handle;
    if (!handle) handle = rt_heap_alloc(16, 1); /* a non-null handle the caller keeps and passes back */
    RET(handle);
}

static void c_CreateChannelDesc(X86Ctx *ctx)
{
    /* Returns a 20-byte struct by value through the hidden first argument. */
    uint32_t out = ARG(0);
    st32(out, ARG(1)); st32(out + 4, ARG(2)); st32(out + 8, ARG(3)); st32(out + 12, ARG(4)); st32(out + 16, ARG(5));
    RET(out);
}

static void s_Invalid(X86Ctx *ctx) { RET(0xFFFFFFFFu); }  /* INVALID_HANDLE_VALUE / SOCKET_ERROR */
static void s_False(X86Ctx *ctx) { RET(0); }
static void s_True(X86Ctx *ctx) { RET(1); }
static void s_htons(X86Ctx *ctx) { uint32_t v = ARG(0) & 0xFFFF; RET(((v & 0xFF) << 8) | (v >> 8)); }
static void s_htonl(X86Ctx *ctx) { uint32_t v = ARG(0); RET((v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24)); }

static void s_inet_addr(X86Ctx *ctx)
{
    const char *s = rt_str(ARG(0));
    uint32_t a = 0, part = 0, n = 0;
    for (; s && *s; s++) {
        if (*s == '.') { a |= (part & 0xFF) << (8 * n++); part = 0; }
        else if (*s >= '0' && *s <= '9') part = part * 10 + (uint32_t)(*s - '0');
        else { RET(0xFFFFFFFFu); return; }
    }
    a |= (part & 0xFF) << (8 * n);
    RET(n == 3 ? a : 0xFFFFFFFFu);
}

static const HleDef physx_support[] = {
    {"cudart32_65.dll", "cudaEventElapsedTime", c_NoDevice, 3},
    {"cudart32_65.dll", "cudaEventQuery", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaEventRecord", c_NoDevice, 2},
    {"cudart32_65.dll", "__cudaRegisterTexture", c_Void, 7},
    {"cudart32_65.dll", "cudaMallocHost", c_NoDevice, 2},
    {"cudart32_65.dll", "cudaEventSynchronize", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaFreeHost", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaFree", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaMemcpyAsync", c_NoDevice, 5},
    {"cudart32_65.dll", "cudaThreadSynchronize", c_NoDevice, 0},
    {"cudart32_65.dll", "cudaMemcpy", c_NoDevice, 4},
    {"cudart32_65.dll", "cudaGetDeviceProperties", c_NoDevice, 2},
    {"cudart32_65.dll", "cudaMemset", c_NoDevice, 3},
    {"cudart32_65.dll", "cudaMemcpyToSymbolAsync", c_NoDevice, 6},
    {"cudart32_65.dll", "__cudaUnregisterFatBinary", c_Void, 1},
    {"cudart32_65.dll", "__cudaRegisterVar", c_Void, 8},
    {"cudart32_65.dll", "__cudaRegisterFatBinary", c_RegisterFatBinary, 1},
    {"cudart32_65.dll", "cudaMemcpyToSymbol", c_NoDevice, 5},
    {"cudart32_65.dll", "cudaLaunch", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaSetupArgument", c_NoDevice, 3},
    {"cudart32_65.dll", "__cudaRegisterFunction", c_Void, 10},
    {"cudart32_65.dll", "cudaConfigureCall", c_NoDevice, 8},  /* dim3 grid, dim3 block by value */
    {"cudart32_65.dll", "cudaCreateChannelDesc", c_CreateChannelDesc, 6},
    {"cudart32_65.dll", "cudaUnbindTexture", c_NoDevice, 1},
    {"cudart32_65.dll", "cudaMalloc", c_NoDevice, 2},
    {"cudart32_65.dll", "cudaBindTexture", c_NoDevice, 5},
    {"cudart32_65.dll", "cudaEventCreate", c_NoDevice, 1},
    {"setupapi.dll", "SetupDiGetClassDevsA", s_Invalid, 4},
    {"setupapi.dll", "SetupDiEnumDeviceInterfaces", s_False, 5},
    {"setupapi.dll", "SetupDiGetDeviceInterfaceDetailA", s_False, 6},
    {"setupapi.dll", "SetupDiDestroyDeviceInfoList", s_True, 1},
    /* Winsock by ordinal, for ws2_32 and wsock32 alike. */
    {"ws2_32.dll", "#3", s_Invalid, 1}, {"wsock32.dll", "#3", s_Invalid, 1},        /* closesocket */
    {"ws2_32.dll", "#4", s_Invalid, 3}, {"wsock32.dll", "#4", s_Invalid, 3},        /* connect */
    {"ws2_32.dll", "#9", s_htons, 1}, {"wsock32.dll", "#9", s_htons, 1},            /* htons */
    {"ws2_32.dll", "#10", s_Invalid, 3}, {"wsock32.dll", "#10", s_Invalid, 3},      /* ioctlsocket */
    {"ws2_32.dll", "#11", s_inet_addr, 1}, {"wsock32.dll", "#11", s_inet_addr, 1},  /* inet_addr */
    {"ws2_32.dll", "#12", s_False, 1}, {"wsock32.dll", "#12", s_False, 1},          /* inet_ntoa */
    {"ws2_32.dll", "#16", s_Invalid, 4}, {"wsock32.dll", "#16", s_Invalid, 4},      /* recv */
    {"ws2_32.dll", "#19", s_Invalid, 4}, {"wsock32.dll", "#19", s_Invalid, 4},      /* send */
    {"ws2_32.dll", "#23", s_Invalid, 3}, {"wsock32.dll", "#23", s_Invalid, 3},      /* socket */
    {"ws2_32.dll", "#51", s_False, 3}, {"wsock32.dll", "#51", s_False, 3},          /* gethostbyaddr */
    {"ws2_32.dll", "#52", s_False, 1}, {"wsock32.dll", "#52", s_False, 1},          /* gethostbyname */
    {"ws2_32.dll", "#1", s_Invalid, 3}, {"wsock32.dll", "#1", s_Invalid, 3},        /* accept */
    {"ws2_32.dll", "#2", s_Invalid, 3}, {"wsock32.dll", "#2", s_Invalid, 3},        /* bind */
    {"ws2_32.dll", "#5", s_Invalid, 3}, {"wsock32.dll", "#5", s_Invalid, 3},        /* getpeername */
    {"ws2_32.dll", "#6", s_Invalid, 3}, {"wsock32.dll", "#6", s_Invalid, 3},        /* getsockname */
    {"ws2_32.dll", "#7", s_Invalid, 5}, {"wsock32.dll", "#7", s_Invalid, 5},        /* getsockopt */
    {"ws2_32.dll", "#8", s_htonl, 1}, {"wsock32.dll", "#8", s_htonl, 1},            /* htonl */
    {"ws2_32.dll", "#13", s_Invalid, 2}, {"wsock32.dll", "#13", s_Invalid, 2},      /* listen */
    {"ws2_32.dll", "#14", s_htonl, 1}, {"wsock32.dll", "#14", s_htonl, 1},          /* ntohl */
    {"ws2_32.dll", "#15", s_htons, 1}, {"wsock32.dll", "#15", s_htons, 1},          /* ntohs */
    {"ws2_32.dll", "#17", s_Invalid, 6}, {"wsock32.dll", "#17", s_Invalid, 6},      /* recvfrom */
    {"ws2_32.dll", "#18", s_Invalid, 5}, {"wsock32.dll", "#18", s_Invalid, 5},      /* select */
    {"ws2_32.dll", "#20", s_Invalid, 6}, {"wsock32.dll", "#20", s_Invalid, 6},      /* sendto */
    {"ws2_32.dll", "#21", s_Invalid, 5}, {"wsock32.dll", "#21", s_Invalid, 5},      /* setsockopt */
    {"ws2_32.dll", "#22", s_Invalid, 2}, {"wsock32.dll", "#22", s_Invalid, 2},      /* shutdown */
    /* Overlapped Winsock: there are no sockets, so every request fails (WSAGetLastError: WSAENETDOWN). */
    {"ws2_32.dll", "WSARecv", s_Invalid, 7}, {"ws2_32.dll", "WSARecvFrom", s_Invalid, 9},
    {"ws2_32.dll", "WSAIoctl", s_Invalid, 9}, {"ws2_32.dll", "WSAGetOverlappedResult", s_False, 5},
};

void rt_physx_support_register(void) { rt_hle_register(physx_support, sizeof physx_support / sizeof *physx_support); }
