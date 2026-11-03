/* HLE of KERNEL32 functions used by the CRT start-up and the game (stdcall; arguments are read
   from the guest stack, the dispatcher pops them). The guest sees Windows XP SP3. */
#include "rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define ARG(i) rt_arg(ctx, i)
#define RET(v) (ctx->eax = (uint32_t)(v))

/* Guest-visible paths: the game sees itself installed here. */
#define GUEST_EXE_PATH "C:\\MirrorsEdge\\Binaries\\MirrorsEdge.exe"
#define GUEST_CMDLINE "\"" GUEST_EXE_PATH "\""

static void set_last_error(uint32_t e) { st32(rt_thread_self()->teb + 0x34, e); }

/* Guest copies of constant strings, allocated once. */
static uint32_t guest_cstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, p = rt_heap_alloc(n, 0);
    memcpy(rt_ptr(p), s, n);
    return p;
}

static uint32_t guest_wstr(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, p = rt_heap_alloc(2 * n, 0);
    for (uint32_t i = 0; i < n; i++) st16(p + 2 * i, (uint8_t)s[i]);
    return p;
}

/* ---- version ---- */
static void k_GetVersion(X86Ctx *ctx) { RET((2600u << 16) | (1u << 8) | 5u); }

static void k_GetVersionExA(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    st32(p + 4, 5); st32(p + 8, 1); st32(p + 12, 2600); st32(p + 16, 2);
    memset(rt_ptr(p + 20), 0, 128);
    strcpy((char *)rt_ptr(p + 20), "Service Pack 3");
    if (ld32(p) >= 156) { st16(p + 148, 3); st16(p + 150, 0); st16(p + 152, 0x100); st16(p + 154, 1); }
    RET(1);
}

static void k_GetVersionExW(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    st32(p + 4, 5); st32(p + 8, 1); st32(p + 12, 2600); st32(p + 16, 2);
    memset(rt_ptr(p + 20), 0, 256);
    const char *sp = "Service Pack 3";
    for (int i = 0; sp[i]; i++) st16(p + 20 + 2 * i, (uint8_t)sp[i]);
    RET(1);
}

/* ---- heaps: every heap shares the runtime allocator ---- */
#define HEAP_ZERO 0x8u
static void k_GetProcessHeap(X86Ctx *ctx) { RET(0x00150000u); }
static void k_HeapCreate(X86Ctx *ctx) { RET(0x00160000u); }
static void k_HeapDestroy(X86Ctx *ctx) { RET(1); }
static void k_HeapAlloc(X86Ctx *ctx) { RET(rt_heap_alloc(ARG(2), (ARG(1) & HEAP_ZERO) != 0)); }
static void k_HeapFree(X86Ctx *ctx) { rt_heap_free(ARG(2)); RET(1); }
static void k_HeapReAlloc(X86Ctx *ctx) { RET(rt_heap_realloc(ARG(2), ARG(3), (ARG(1) & HEAP_ZERO) != 0)); }
static void k_HeapSize(X86Ctx *ctx) { RET(rt_heap_size(ARG(2))); }
static void k_HeapValidate(X86Ctx *ctx) { RET(1); }
static void k_HeapCompact(X86Ctx *ctx) { RET(0x100000); }
static void k_HeapSetInformation(X86Ctx *ctx) { RET(1); }

/* ---- virtual memory ---- */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u

static void k_VirtualAlloc(X86Ctx *ctx)
{
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr && !(type & MEM_RESERVE)) { /* commit inside an existing reservation */
        RET(rt_vm_commit(addr & ~(RT_PAGE - 1), size + (addr & (RT_PAGE - 1))) ? (addr & ~(RT_PAGE - 1)) : 0);
        return;
    }
    uint32_t r = rt_vm_reserve(addr & ~(RT_GRANULE - 1), size);
    if (r && (type & MEM_COMMIT) && !rt_vm_commit(r, size)) r = 0;
    if (!r) set_last_error(8); /* ERROR_NOT_ENOUGH_MEMORY */
    RET(r);
}

static void k_VirtualFree(X86Ctx *ctx)
{
    uint32_t type = ARG(2);
    RET(type & MEM_RELEASE ? rt_vm_release(ARG(0)) : 1); /* decommit: pages stay (harmless) */
}

static void k_VirtualQuery(X86Ctx *ctx)
{
    uint32_t addr = ARG(0), info = ARG(1), base;
    int committed;
    uint32_t size = rt_vm_region_size(addr, &base, &committed);
    memset(rt_ptr(info), 0, 28);
    st32(info + 0, addr & ~(RT_PAGE - 1));
    st32(info + 4, size ? base : 0);
    st32(info + 8, size ? 4 : 0);
    st32(info + 12, size ? base + size - (addr & ~(RT_PAGE - 1)) : RT_GRANULE);
    st32(info + 16, !size ? 0x10000u : committed ? MEM_COMMIT : MEM_RESERVE);
    st32(info + 20, committed ? 4 : 0); /* PAGE_READWRITE */
    st32(info + 24, size ? 0x20000u : 0); /* MEM_PRIVATE */
    RET(28);
}

static void k_VirtualProtect(X86Ctx *ctx)
{
    if (ARG(3)) st32(ARG(3), 4);
    RET(1);
}

/* ---- TLS, errors, ids ---- */
static void k_TlsAlloc(X86Ctx *ctx) { RET(rt_tls_alloc()); }
static void k_TlsFree(X86Ctx *ctx) { RET(rt_tls_free(ARG(0))); }
static void k_TlsGetValue(X86Ctx *ctx) { set_last_error(0); RET(rt_tls_get(ARG(0))); }
static void k_TlsSetValue(X86Ctx *ctx) { RET(rt_tls_set(ARG(0), ARG(1))); }
static void k_GetLastError(X86Ctx *ctx) { RET(ld32(rt_thread_self()->teb + 0x34)); }
static void k_SetLastError(X86Ctx *ctx) { set_last_error(ARG(0)); }
static void k_GetCurrentThreadId(X86Ctx *ctx) { RET(rt_thread_self()->id); }
static void k_GetCurrentProcessId(X86Ctx *ctx) { RET(1); }
static void k_GetCurrentThread(X86Ctx *ctx) { RET(0xFFFFFFFEu); }
static void k_GetCurrentProcess(X86Ctx *ctx) { RET(0xFFFFFFFFu); }

/* ---- critical sections: a host recursive mutex per guest CRITICAL_SECTION, its index kept
   in the LockSemaphore field (+16) ---- */
static HostMutex **cs_table;
static uint32_t cs_count, cs_cap;
static HostMutex *cs_global;

static HostMutex *cs_get(uint32_t cs)
{
    uint32_t idx = ld32(cs + 16);
    if (idx >= 0xC5000000u && idx - 0xC5000000u < cs_count) return cs_table[idx - 0xC5000000u];
    if (!cs_global) cs_global = host_mutex_new();
    host_lock(cs_global);
    idx = ld32(cs + 16);
    if (!(idx >= 0xC5000000u && idx - 0xC5000000u < cs_count)) {
        if (cs_count == cs_cap) {
            cs_cap = cs_cap ? cs_cap * 2 : 256;
            cs_table = realloc(cs_table, cs_cap * sizeof *cs_table);
        }
        cs_table[cs_count] = host_mutex_new();
        idx = 0xC5000000u + cs_count++;
        st32(cs + 16, idx);
    }
    host_unlock(cs_global);
    return cs_table[idx - 0xC5000000u];
}

static void k_InitializeCriticalSection(X86Ctx *ctx)
{
    uint32_t cs = ARG(0);
    memset(rt_ptr(cs), 0, 24);
    st32(cs + 4, 0xFFFFFFFFu);
    cs_get(cs);
}

static void k_InitializeCriticalSectionAndSpinCount(X86Ctx *ctx) { k_InitializeCriticalSection(ctx); RET(1); }
static void k_DeleteCriticalSection(X86Ctx *ctx) { (void)ctx; }

static void k_EnterCriticalSection(X86Ctx *ctx)
{
    uint32_t cs = ARG(0);
    host_lock(cs_get(cs));
    st32(cs + 8, ld32(cs + 8) + 1);
    st32(cs + 12, rt_thread_self()->id);
}

static void k_LeaveCriticalSection(X86Ctx *ctx)
{
    uint32_t cs = ARG(0), n = ld32(cs + 8) - 1;
    st32(cs + 8, n);
    if (!n) st32(cs + 12, 0);
    host_unlock(cs_get(cs));
}

static void k_TryEnterCriticalSection(X86Ctx *ctx) { k_EnterCriticalSection(ctx); RET(1); }

/* ---- interlocked ---- */
#define AP(a) ((uint32_t *)rt_ptr(a))
static void k_InterlockedIncrement(X86Ctx *ctx) { RET(__atomic_add_fetch(AP(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
static void k_InterlockedDecrement(X86Ctx *ctx) { RET(__atomic_sub_fetch(AP(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
static void k_InterlockedExchange(X86Ctx *ctx) { RET(__atomic_exchange_n(AP(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }
static void k_InterlockedExchangeAdd(X86Ctx *ctx) { RET(__atomic_fetch_add(AP(ARG(0)), ARG(1), __ATOMIC_SEQ_CST)); }

static void k_InterlockedCompareExchange(X86Ctx *ctx)
{
    uint32_t expected = ARG(2);
    __atomic_compare_exchange_n(AP(ARG(0)), &expected, ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(expected);
}

/* ---- modules ---- */
/* System DLLs are not loaded; they get pseudo handles so GetProcAddress can serve HLE exports. */
static const char *sys_dlls[] = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "winmm.dll",
                                 "ole32.dll", "oleaut32.dll", "shell32.dll", "ws2_32.dll", "wsock32.dll",
                                 "ntdll.dll", "d3d9.dll", "dinput8.dll", "xinput1_3.dll", "dbghelp.dll"};
#define SYS_HANDLE(i) (0xFD000000u + 0x10000u * (uint32_t)(i))

static int sys_index(const char *name)
{
    char l[64];
    size_t i = 0;
    for (; name[i] && i < 60; i++) l[i] = (char)tolower((unsigned char)name[i]);
    l[i] = 0;
    if (!strchr(l, '.')) strcat(l, ".dll");
    const char *base = strrchr(l, '\\') ? strrchr(l, '\\') + 1 : l;
    for (size_t k = 0; k < sizeof sys_dlls / sizeof *sys_dlls; k++)
        if (!strcmp(base, sys_dlls[k])) return (int)k;
    return -1;
}

static uint32_t module_handle(const char *name)
{
    if (!name) return 0x400000;
    const char *base = strrchr(name, '\\') ? strrchr(name, '\\') + 1 : name;
    RtModule *m = rt_find_module(base);
    if (m) return m->base;
    int k = sys_index(base);
    return k >= 0 ? SYS_HANDLE(k) : 0;
}

static void wide_to_ascii(uint32_t w, char *out, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && ld16(w + 2 * (uint32_t)i); i++) out[i] = (char)ld16(w + 2 * (uint32_t)i);
    out[i] = 0;
}

static void k_GetModuleHandleA(X86Ctx *ctx)
{
    uint32_t h = module_handle(rt_str(ARG(0)));
    if (!h) set_last_error(126);
    RET(h);
}

static void k_GetModuleHandleW(X86Ctx *ctx)
{
    char name[260];
    if (!ARG(0)) { RET(0x400000); return; }
    wide_to_ascii(ARG(0), name, sizeof name);
    uint32_t h = module_handle(name);
    if (!h) set_last_error(126);
    RET(h);
}

static void k_LoadLibraryA(X86Ctx *ctx)
{
    const char *name = rt_str(ARG(0));
    uint32_t h = module_handle(name);
    if (!h) { rt_log("LoadLibraryA(%s): not available", name); set_last_error(126); }
    RET(h);
}

static void k_LoadLibraryW(X86Ctx *ctx)
{
    char name[260];
    wide_to_ascii(ARG(0), name, sizeof name);
    uint32_t h = module_handle(name);
    if (!h) { rt_log("LoadLibraryW(%s): not available", name); set_last_error(126); }
    RET(h);
}

static void k_FreeLibrary(X86Ctx *ctx) { RET(1); }
static void k_DisableThreadLibraryCalls(X86Ctx *ctx) { RET(1); }

static void k_GetProcAddress(X86Ctx *ctx)
{
    uint32_t h = ARG(0), p = ARG(1);
    char name[512];
    if (p < 0x10000) snprintf(name, sizeof name, "#%u", p);
    else snprintf(name, sizeof name, "%s", rt_str(p));
    RtModule *m = rt_module_at(h);
    if (m) { RET(rt_export(m, name)); return; }
    if (h >= 0xFD000000u && h < 0xFD000000u + 0x10000u * (sizeof sys_dlls / sizeof *sys_dlls)) {
        uint32_t t = rt_hle_thunk(sys_dlls[(h - 0xFD000000u) / 0x10000u], name);
        /* Report only implemented functions, so the caller takes its fallback for the rest. */
        extern int rt_thunk_implemented(uint32_t);
        if (rt_thunk_implemented(t)) { RET(t); return; }
        rt_log("GetProcAddress(%s, %s): not implemented, returning 0", sys_dlls[(h - 0xFD000000u) / 0x10000u], name);
    }
    set_last_error(127);
    RET(0);
}

static void k_GetModuleFileNameA(X86Ctx *ctx)
{
    uint32_t buf = ARG(1), n = ARG(2);
    const char *path = GUEST_EXE_PATH;
    RtModule *m = ARG(0) ? rt_module_at(ARG(0)) : NULL;
    char tmp[260];
    if (m && m->is_dll) { snprintf(tmp, sizeof tmp, "C:\\MirrorsEdge\\Binaries\\%s", m->name); path = tmp; }
    uint32_t len = (uint32_t)strlen(path);
    if (!n) { RET(0); return; }
    uint32_t c = len < n - 1 ? len : n - 1;
    memcpy(rt_ptr(buf), path, c);
    st8(buf + c, 0);
    RET(c);
}

static void k_GetModuleFileNameW(X86Ctx *ctx)
{
    uint32_t buf = ARG(1), n = ARG(2);
    const char *path = GUEST_EXE_PATH;
    uint32_t len = (uint32_t)strlen(path);
    if (!n) { RET(0); return; }
    uint32_t c = len < n - 1 ? len : n - 1;
    for (uint32_t i = 0; i < c; i++) st16(buf + 2 * i, (uint8_t)path[i]);
    st16(buf + 2 * c, 0);
    RET(c);
}

/* ---- process environment ---- */
static uint32_t cmdline_a, cmdline_w, env_a, env_w;

static void k_GetCommandLineA(X86Ctx *ctx) { if (!cmdline_a) cmdline_a = guest_cstr(GUEST_CMDLINE); RET(cmdline_a); }
static void k_GetCommandLineW(X86Ctx *ctx) { if (!cmdline_w) cmdline_w = guest_wstr(GUEST_CMDLINE); RET(cmdline_w); }

static const char env_block[] = "PATH=C:\\Windows\\system32;C:\\Windows\0SystemRoot=C:\\Windows\0\0";

static void k_GetEnvironmentStrings(X86Ctx *ctx)
{
    if (!env_a) { env_a = rt_heap_alloc(sizeof env_block, 0); memcpy(rt_ptr(env_a), env_block, sizeof env_block); }
    RET(env_a);
}

static void k_GetEnvironmentStringsW(X86Ctx *ctx)
{
    if (!env_w) {
        env_w = rt_heap_alloc(2 * sizeof env_block, 0);
        for (uint32_t i = 0; i < sizeof env_block; i++) st16(env_w + 2 * i, (uint8_t)env_block[i]);
    }
    RET(env_w);
}

static void k_FreeEnvironmentStrings(X86Ctx *ctx) { RET(1); }

static void k_GetEnvironmentVariableA(X86Ctx *ctx)
{
    set_last_error(203); /* ERROR_ENVVAR_NOT_FOUND */
    RET(0);
}

static void k_GetStartupInfo(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    memset(rt_ptr(p), 0, 68);
    st32(p, 68);
}

static void k_GetStdHandle(X86Ctx *ctx)
{
    uint32_t n = ARG(0);
    RET(n == 0xFFFFFFF6u ? 0x10 : n == 0xFFFFFFF5u ? 0x11 : n == 0xFFFFFFF4u ? 0x12 : 0xFFFFFFFFu);
}

static void k_GetFileType(X86Ctx *ctx)
{
    uint32_t h = ARG(0);
    RET(h >= 0x10 && h <= 0x12 ? 2 : 1); /* FILE_TYPE_CHAR for the console handles, else disk */
}

static void k_SetHandleCount(X86Ctx *ctx) { RET(ARG(0)); }

/* ---- code pages and strings (Windows-1252 treated as Latin-1) ---- */
static void k_GetACP(X86Ctx *ctx) { RET(1252); }
static void k_GetOEMCP(X86Ctx *ctx) { RET(437); }
static void k_IsValidCodePage(X86Ctx *ctx) { RET(1); }

static void k_GetCPInfo(X86Ctx *ctx)
{
    uint32_t p = ARG(1);
    memset(rt_ptr(p), 0, 20);
    st32(p, 1);       /* MaxCharSize */
    st8(p + 4, '?');  /* DefaultChar */
    RET(1);
}

static void k_MultiByteToWideChar(X86Ctx *ctx)
{
    uint32_t src = ARG(2), dst = ARG(4);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) len = (int32_t)strlen(rt_str(src)) + 1;
    if (!cap) { RET(len); return; }
    if (cap < len) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < len; i++) st16(dst + 2u * (uint32_t)i, ld8(src + (uint32_t)i));
    RET(len);
}

static void k_WideCharToMultiByte(X86Ctx *ctx)
{
    uint32_t src = ARG(2), dst = ARG(4), used = ARG(7);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) { len = 0; while (ld16(src + 2u * (uint32_t)len)) len++; len++; }
    if (used) st32(used, 0);
    if (!cap) { RET(len); return; }
    if (cap < len) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < len; i++) {
        uint16_t c = ld16(src + 2u * (uint32_t)i);
        if (c > 0xFF) { c = '?'; if (used) st32(used, 1); }
        st8(dst + (uint32_t)i, (uint8_t)c);
    }
    RET(len);
}

static uint16_t ctype1(unsigned c)
{
    uint16_t t = 0;
    if (c < 0x80) {
        if (isupper((int)c)) t |= 0x1;
        if (islower((int)c)) t |= 0x2;
        if (isdigit((int)c)) t |= 0x4;
        if (isspace((int)c)) t |= 0x8;
        if (ispunct((int)c)) t |= 0x10;
        if (iscntrl((int)c)) t |= 0x20;
        if (c == ' ' || c == '\t') t |= 0x40;
        if (isxdigit((int)c)) t |= 0x80;
        if (isalpha((int)c)) t |= 0x100;
    } else if (c >= 0xC0 && c != 0xD7 && c != 0xF7) {
        t |= 0x100 | (c < 0xDF ? 0x1 : 0x2);
    } else if (c >= 0xA0) {
        t |= 0x10;
    } else {
        t |= 0x20;
    }
    return t;
}

static void k_GetStringTypeW(X86Ctx *ctx)
{
    uint32_t src = ARG(1), out = ARG(3);
    int32_t len = (int32_t)ARG(2);
    if (len < 0) { len = 0; while (ld16(src + 2u * (uint32_t)len)) len++; len++; }
    for (int32_t i = 0; i < len; i++) st16(out + 2u * (uint32_t)i, ctype1(ld16(src + 2u * (uint32_t)i)));
    RET(1);
}

static void k_GetStringTypeA(X86Ctx *ctx)
{
    uint32_t src = ARG(2), out = ARG(4);
    int32_t len = (int32_t)ARG(3);
    if (len < 0) len = (int32_t)strlen(rt_str(src)) + 1;
    for (int32_t i = 0; i < len; i++) st16(out + 2u * (uint32_t)i, ctype1(ld8(src + (uint32_t)i)));
    RET(1);
}

static unsigned map_case(unsigned c, uint32_t flags)
{
    if (flags & 0x100) return c < 0x80 ? (unsigned)tolower((int)c) : (c >= 0xC0 && c < 0xDF && c != 0xD7) ? c + 0x20 : c;
    if (flags & 0x200) return c < 0x80 ? (unsigned)toupper((int)c) : (c >= 0xE0 && c < 0xFF && c != 0xF7) ? c - 0x20 : c;
    return c;
}

static void k_LCMapStringW(X86Ctx *ctx)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) { len = 0; while (ld16(src + 2u * (uint32_t)len)) len++; len++; }
    if (!cap) { RET(len); return; }
    if (cap < len) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < len; i++) st16(dst + 2u * (uint32_t)i, (uint16_t)map_case(ld16(src + 2u * (uint32_t)i), flags));
    RET(len);
}

static void k_LCMapStringA(X86Ctx *ctx)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t len = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (len < 0) len = (int32_t)strlen(rt_str(src)) + 1;
    if (!cap) { RET(len); return; }
    if (cap < len) { set_last_error(122); RET(0); return; }
    for (int32_t i = 0; i < len; i++) st8(dst + (uint32_t)i, (uint8_t)map_case(ld8(src + (uint32_t)i), flags));
    RET(len);
}

static void k_GetLocaleInfoA(X86Ctx *ctx)
{
    rt_log("GetLocaleInfoA(lcid %#x, type %#x): not implemented, returning 0", ARG(0), ARG(1));
    set_last_error(87);
    RET(0);
}

static void k_GetUserDefaultLCID(X86Ctx *ctx) { RET(0x0409); }

/* ---- time ---- */
static void k_QueryPerformanceFrequency(X86Ctx *ctx) { st32(ARG(0), 10000000); st32(ARG(0) + 4, 0); RET(1); }

static void k_QueryPerformanceCounter(X86Ctx *ctx)
{
    uint64_t t = host_ticks_ns() / 100;
    st32(ARG(0), (uint32_t)t);
    st32(ARG(0) + 4, (uint32_t)(t >> 32));
    RET(1);
}

static void k_GetTickCount(X86Ctx *ctx) { RET((uint32_t)(host_ticks_ns() / 1000000)); }

static void k_GetSystemTimeAsFileTime(X86Ctx *ctx)
{
    uint64_t t = host_filetime();
    st32(ARG(0), (uint32_t)t);
    st32(ARG(0) + 4, (uint32_t)(t >> 32));
}

static void k_Sleep(X86Ctx *ctx) { host_sleep_ms(ARG(0)); }

static void k_GetTimeZoneInformation(X86Ctx *ctx) { memset(rt_ptr(ARG(0)), 0, 172); RET(0); }

/* ---- system ---- */
static void k_GetSystemInfo(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    memset(rt_ptr(p), 0, 36);
    st32(p + 4, RT_PAGE);
    st32(p + 8, 0x10000);
    st32(p + 12, 0x7FFEFFFF);
    st32(p + 16, 1);         /* active processor mask */
    st32(p + 20, 1);         /* number of processors */
    st32(p + 24, 586);       /* PROCESSOR_INTEL_PENTIUM */
    st32(p + 28, RT_GRANULE);
    st16(p + 32, 6);         /* processor level */
}

static void k_IsProcessorFeaturePresent(X86Ctx *ctx)
{
    uint32_t f = ARG(0);
    RET(f == 6 /* SSE */ || f == 3 /* MMX */ || f == 2 /* CMPXCHG8B */ ? 1 : 0);
}

static void k_IsDebuggerPresent(X86Ctx *ctx) { RET(0); }
static void k_SetUnhandledExceptionFilter(X86Ctx *ctx) { RET(0); }
static void k_UnhandledExceptionFilter(X86Ctx *ctx) { RET(0); }
static void k_SetConsoleCtrlHandler(X86Ctx *ctx) { RET(1); }
static void k_EncodePointer(X86Ctx *ctx) { RET(ARG(0)); }
static void k_SetErrorMode(X86Ctx *ctx) { RET(0); }

static void k_OutputDebugStringA(X86Ctx *ctx) { rt_log("guest debug: %s", rt_str(ARG(0))); }

static void k_ExitProcess(X86Ctx *ctx)
{
    rt_log("guest ExitProcess(%u)", ARG(0));
    exit((int)ARG(0));
}

static void k_TerminateProcess(X86Ctx *ctx)
{
    rt_log("guest TerminateProcess(%u)", ARG(1));
    exit((int)ARG(1));
}

static void k_RaiseException(X86Ctx *ctx)
{
    /* MS_VC_EXCEPTION: "name this thread" for a debugger, raised inside
       __try/__except(EXCEPTION_CONTINUE_EXECUTION). Returning is what happens without one. */
    if (ARG(0) == 0x406D1388u) {
        uint32_t info = ARG(3);
        if (ARG(2) >= 3 && ld32(info) == 0x1000)
            rt_log("thread %#x named \"%s\"", ld32(info + 8), rt_str(ld32(info + 4)) ? rt_str(ld32(info + 4)) : "");
        return;
    }
    rt_fatal("guest RaiseException(code %#x, flags %#x, %u args) - SEH is not implemented yet (return %#x)",
             ARG(0), ARG(1), ARG(2), ld32(ctx->esp));
}

/* ---- simple batch: spin counts, strings, Global/Local memory, time, affinity, console ---- */
static void k_SetCriticalSectionSpinCount(X86Ctx *ctx) { RET(ld32(ARG(0) + 20)); st32(ARG(0) + 20, ARG(1)); }
static void k_lstrlenA(X86Ctx *ctx) { RET(ARG(0) ? strlen(rt_str(ARG(0))) : 0); }

static void k_lstrlenW(X86Ctx *ctx)
{
    uint32_t p = ARG(0), n = 0;
    if (p) while (ld16(p + 2 * n)) n++;
    RET(n);
}

static void k_GetThreadLocale(X86Ctx *ctx) { RET(0x0409); }
#define GMEM_ZEROINIT 0x40u
static void k_GlobalAlloc(X86Ctx *ctx) { RET(rt_heap_alloc(ARG(1) ? ARG(1) : 1, (ARG(0) & GMEM_ZEROINIT) != 0)); }
static void k_GlobalFree(X86Ctx *ctx) { rt_heap_free(ARG(0)); RET(0); }
static void k_GlobalLock(X86Ctx *ctx) { RET(ARG(0)); }
static void k_GlobalUnlock(X86Ctx *ctx) { RET(1); }
static void k_GlobalSize(X86Ctx *ctx) { RET(rt_heap_size(ARG(0))); }

static void k_GlobalMemoryStatus(X86Ctx *ctx)
{
    uint32_t p = ARG(0);
    st32(p, 32);
    st32(p + 4, 30);                 /* memory load, % */
    st32(p + 8, 0x7FFF0000u);        /* total physical: 2 GB */
    st32(p + 12, 0x60000000u);       /* available physical */
    st32(p + 16, 0x7FFF0000u);       /* page file */
    st32(p + 20, 0x60000000u);
    st32(p + 24, 0xFFFE0000u);       /* virtual: large-address aware */
    st32(p + 28, 0xC0000000u);
}

/* FILETIME (100 ns since 1601, UTC; the guest's time zone is UTC) <-> SYSTEMTIME. */
static void ft_to_st(uint64_t ft100, uint32_t p)
{
    uint64_t ft = ft100 / 10000; /* ms since 1601 */
    uint64_t days = ft / 86400000ull, ms = ft % 86400000ull;
    int64_t z = (int64_t)days - 134774 + 719468; /* days since 1970-01-01, then civil-from-days */
    int64_t era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    st16(p, (uint16_t)y); st16(p + 2, (uint16_t)m); st16(p + 4, (uint16_t)((days + 1) % 7)); st16(p + 6, (uint16_t)d);
    st16(p + 8, (uint16_t)(ms / 3600000)); st16(p + 10, (uint16_t)(ms / 60000 % 60));
    st16(p + 12, (uint16_t)(ms / 1000 % 60)); st16(p + 14, (uint16_t)(ms % 1000));
}

static uint64_t st_to_ft(uint32_t p)
{
    int64_t y = ld16(p), m = ld16(p + 2), d = ld16(p + 6);
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468 + 134774; /* days since 1601 */
    uint64_t ms = (uint64_t)days * 86400000ull + ld16(p + 8) * 3600000ull + ld16(p + 10) * 60000ull +
                  ld16(p + 12) * 1000ull + ld16(p + 14);
    return ms * 10000ull;
}

static void k_GetLocalTime(X86Ctx *ctx) { ft_to_st(host_filetime(), ARG(0)); }

static void k_FileTimeToSystemTime(X86Ctx *ctx)
{
    ft_to_st(((uint64_t)ld32(ARG(0) + 4) << 32) | ld32(ARG(0)), ARG(1));
    RET(1);
}

static void k_SystemTimeToFileTime(X86Ctx *ctx)
{
    uint64_t ft = st_to_ft(ARG(0));
    st32(ARG(1), (uint32_t)ft);
    st32(ARG(1) + 4, (uint32_t)(ft >> 32));
    RET(1);
}

static void k_FileTimeCopy(X86Ctx *ctx) /* FileTimeToLocalFileTime / LocalFileTimeToFileTime: UTC */
{
    st32(ARG(1), ld32(ARG(0)));
    st32(ARG(1) + 4, ld32(ARG(0) + 4));
    RET(1);
}

static void k_GetProcessAffinityMask(X86Ctx *ctx) { st32(ARG(1), 0xF); st32(ARG(2), 0xF); RET(1); }
static void k_SetThreadAffinityMask(X86Ctx *ctx) { RET(0xF); }
static void k_SetProcessAffinityMask(X86Ctx *ctx) { RET(1); }
static void k_SetThreadPriority(X86Ctx *ctx) { RET(1); }
static void k_SetThreadIdealProcessor(X86Ctx *ctx) { RET(0); }
static void k_RetTrue(X86Ctx *ctx) { RET(1); }
static void k_RetZero(X86Ctx *ctx) { RET(0); }

static void k_OutputDebugStringW(X86Ctx *ctx)
{
    char s[1024];
    wide_to_ascii(ARG(0), s, sizeof s);
    rt_log("guest debug: %s", s);
}

static void k_GetCurrentDirectoryA(X86Ctx *ctx)
{
    const char *d = "C:\\MirrorsEdge\\Binaries";
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) { RET(len + 1); return; }
    memcpy(rt_ptr(ARG(1)), d, len + 1);
    RET(len);
}

static void k_GetCurrentDirectoryW(X86Ctx *ctx)
{
    const char *d = "C:\\MirrorsEdge\\Binaries";
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) { RET(len + 1); return; }
    for (uint32_t i = 0; i <= len; i++) st16(ARG(1) + 2 * i, (uint8_t)d[i]);
    RET(len);
}

static void k_GetTempPathW(X86Ctx *ctx)
{
    const char *d = "C:\\MirrorsEdge\\Temp\\";
    uint32_t len = (uint32_t)strlen(d);
    if (ARG(0) <= len) { RET(len + 1); return; }
    for (uint32_t i = 0; i <= len; i++) st16(ARG(1) + 2 * i, (uint8_t)d[i]);
    RET(len);
}

static void k_GetComputerNameW(X86Ctx *ctx)
{
    const char *n = "RUNNER";
    for (uint32_t i = 0; i <= 6; i++) st16(ARG(0) + 2 * i, (uint8_t)n[i]);
    st32(ARG(1), 6);
    RET(1);
}

/* SHELL32 CommandLineToArgvW: split on spaces outside double quotes; one LocalAlloc block
   holding the pointer array followed by the strings, as on Windows. */
static void s_CommandLineToArgvW(X86Ctx *ctx)
{
    uint32_t cmd = ARG(0), pargc = ARG(1);
    uint32_t len = 0;
    while (ld16(cmd + 2 * len)) len++;
    uint32_t args[64], argc = 0, i = 0;
    uint32_t buf = rt_heap_alloc(64 * 4 + 2 * (len + 64), 1), out = buf + 64 * 4;
    while (i < len && argc < 64) {
        while (i < len && ld16(cmd + 2 * i) == ' ') i++;
        if (i >= len) break;
        args[argc++] = out;
        int quoted = 0;
        for (; i < len; i++) {
            uint16_t c = ld16(cmd + 2 * i);
            if (c == '"') { quoted = !quoted; continue; }
            if (c == ' ' && !quoted) break;
            st16(out, c);
            out += 2;
        }
        st16(out, 0);
        out += 2;
    }
    for (uint32_t k = 0; k < argc; k++) st32(buf + 4 * k, args[k]);
    st32(pargc, argc);
    RET(buf);
}

static void w_timeGetTime(X86Ctx *ctx) { RET((uint32_t)(host_ticks_ns() / 1000000)); }

static const HleDef winmm[] = {
    {"winmm.dll", "timeBeginPeriod", k_RetZero, 1},
    {"winmm.dll", "timeEndPeriod", k_RetZero, 1},
    {"winmm.dll", "timeGetTime", w_timeGetTime, 0},
};

/* Winsock: initialization succeeds, networking itself is off (no sockets yet). */
static void ws_WSAStartup(X86Ctx *ctx)
{
    uint32_t p = ARG(1);
    memset(rt_ptr(p), 0, 400);
    st16(p, (uint16_t)ARG(0));
    st16(p + 2, 0x0202);
    strcpy((char *)rt_ptr(p + 4), "WinSock 2.0");
    st16(p + 390, 32767);   /* iMaxSockets */
    st16(p + 392, 65467);   /* iMaxUdpDg */
    RET(0);
}

static void ws_gethostname(X86Ctx *ctx)
{
    if (ARG(1) < 7) { RET(0xFFFFFFFFu); return; }
    strcpy((char *)rt_ptr(ARG(0)), "RUNNER");
    RET(0);
}

static void ws_WSAGetLastError(X86Ctx *ctx) { RET(10050); } /* WSAENETDOWN */

static const HleDef winsock[] = {
    {"wsock32.dll", "#115", ws_WSAStartup, 2}, {"ws2_32.dll", "#115", ws_WSAStartup, 2},
    {"wsock32.dll", "#116", k_RetZero, 0}, {"ws2_32.dll", "#116", k_RetZero, 0},
    {"wsock32.dll", "#57", ws_gethostname, 2}, {"ws2_32.dll", "#57", ws_gethostname, 2},
    {"wsock32.dll", "#111", ws_WSAGetLastError, 0}, {"ws2_32.dll", "#111", ws_WSAGetLastError, 0},
};

static const HleDef shell32[] = {
    {"shell32.dll", "CommandLineToArgvW", s_CommandLineToArgvW, 2},
};

/* ---- resources: walk the loaded module's .rsrc (type -> name -> first language) ---- */
static int rsrc_match(uint32_t root, uint32_t entry_name, uint32_t want, int wide)
{
    if (want < 0x10000) return !(entry_name & 0x80000000u) && entry_name == want;
    char w[128];
    if (wide) wide_to_ascii(want, w, sizeof w);
    else snprintf(w, sizeof w, "%s", rt_str(want));
    if (w[0] == '#') return !(entry_name & 0x80000000u) && entry_name == (uint32_t)atoi(w + 1);
    if (!(entry_name & 0x80000000u)) return 0;
    uint32_t s = root + (entry_name & 0x7FFFFFFFu);
    uint16_t n = ld16(s);
    if (n != strlen(w)) return 0;
    for (uint16_t i = 0; i < n; i++)
        if (toupper(ld16(s + 2 + 2 * i) & 0xFF) != toupper((unsigned char)w[i])) return 0;
    return 1;
}

static uint32_t rsrc_find(uint32_t dir, uint32_t root, uint32_t want, int wide)
{
    uint32_t n = ld16(dir + 12) + ld16(dir + 14);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = dir + 16 + 8 * i;
        if (want == 0xFFFFFFFFu || rsrc_match(root, ld32(e), want, wide)) return ld32(e + 4);
    }
    return 0xFFFFFFFFu;
}

static uint32_t find_resource(uint32_t hmod, uint32_t name, uint32_t type, int wide)
{
    RtModule *m = rt_module_at(hmod ? hmod : 0x400000);
    if (!m) return 0;
    uint32_t pe = ld32(m->base + 0x3C), rva = ld32(m->base + pe + 24 + 96 + 2 * 8);
    if (!rva) return 0;
    uint32_t root = m->base + rva;
    uint32_t t = rsrc_find(root, root, type, wide);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u)) return 0;
    uint32_t nm = rsrc_find(root + (t & 0x7FFFFFFFu), root, name, wide);
    if (nm == 0xFFFFFFFFu || !(nm & 0x80000000u)) return 0;
    uint32_t lang = rsrc_find(root + (nm & 0x7FFFFFFFu), root, 0xFFFFFFFFu, wide);
    if (lang == 0xFFFFFFFFu || (lang & 0x80000000u)) return 0;
    return root + lang; /* HRSRC: the data entry */
}

static void k_FindResourceW(X86Ctx *ctx)
{
    uint32_t r = find_resource(ARG(0), ARG(1), ARG(2), 1);
    if (!r) st32(rt_thread_self()->teb + 0x34, 1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
    RET(r);
}

static void k_FindResourceA(X86Ctx *ctx)
{
    uint32_t r = find_resource(ARG(0), ARG(1), ARG(2), 0);
    if (!r) st32(rt_thread_self()->teb + 0x34, 1814);
    RET(r);
}

static void k_LoadResource(X86Ctx *ctx)
{
    RtModule *m = rt_module_at(ARG(0) ? ARG(0) : 0x400000);
    RET(m && ARG(1) ? m->base + ld32(ARG(1)) : 0);
}

static void k_LockResource(X86Ctx *ctx) { RET(ARG(0)); }
static void k_SizeofResource(X86Ctx *ctx) { RET(ARG(1) ? ld32(ARG(1) + 4) : 0); }

static void k_WriteConsoleW(X86Ctx *ctx)
{
    char s[2048];
    uint32_t n = ARG(2), i = 0;
    for (; i < n && i < sizeof s - 1; i++) s[i] = (char)ld16(ARG(1) + 2 * i);
    s[i] = 0;
    rt_log("guest console: %s", s);
    if (ARG(3)) st32(ARG(3), n);
    RET(1);
}

static void k_GetConsoleScreenBufferInfo(X86Ctx *ctx)
{
    uint32_t p = ARG(1);
    memset(rt_ptr(p), 0, 22);
    st16(p, 120); st16(p + 2, 3000);           /* buffer size */
    st16(p + 8, 7);                            /* attributes */
    st16(p + 14, 119); st16(p + 16, 49);       /* window */
    st16(p + 18, 120); st16(p + 20, 3000);     /* maximum window size */
    RET(1);
}

static const HleDef kernel32[] = {
    {"kernel32.dll", "SetStdHandle", k_RetTrue, 2},
    {"kernel32.dll", "FindResourceW", k_FindResourceW, 3},
    {"kernel32.dll", "FindResourceA", k_FindResourceA, 3},
    {"kernel32.dll", "LoadResource", k_LoadResource, 2},
    {"kernel32.dll", "LockResource", k_LockResource, 1},
    {"kernel32.dll", "SizeofResource", k_SizeofResource, 2},
    {"kernel32.dll", "FreeResource", k_RetZero, 1},
    {"kernel32.dll", "WriteConsoleW", k_WriteConsoleW, 5},
    {"kernel32.dll", "GetConsoleScreenBufferInfo", k_GetConsoleScreenBufferInfo, 2},
    {"kernel32.dll", "SetConsoleScreenBufferSize", k_RetTrue, 2},
    {"kernel32.dll", "SetConsoleWindowInfo", k_RetTrue, 3},
    {"kernel32.dll", "GetConsoleMode", k_RetZero, 2},
    {"kernel32.dll", "SetCriticalSectionSpinCount", k_SetCriticalSectionSpinCount, 2},
    {"kernel32.dll", "lstrlenA", k_lstrlenA, 1},
    {"kernel32.dll", "lstrlenW", k_lstrlenW, 1},
    {"kernel32.dll", "GetThreadLocale", k_GetThreadLocale, 0},
    {"kernel32.dll", "GlobalAlloc", k_GlobalAlloc, 2},
    {"kernel32.dll", "GlobalFree", k_GlobalFree, 1},
    {"kernel32.dll", "GlobalLock", k_GlobalLock, 1},
    {"kernel32.dll", "GlobalUnlock", k_GlobalUnlock, 1},
    {"kernel32.dll", "GlobalSize", k_GlobalSize, 1},
    {"kernel32.dll", "LocalAlloc", k_GlobalAlloc, 2},
    {"kernel32.dll", "LocalFree", k_GlobalFree, 1},
    {"kernel32.dll", "GlobalMemoryStatus", k_GlobalMemoryStatus, 1},
    {"kernel32.dll", "GetLocalTime", k_GetLocalTime, 1},
    {"kernel32.dll", "GetSystemTime", k_GetLocalTime, 1},
    {"kernel32.dll", "FileTimeToSystemTime", k_FileTimeToSystemTime, 2},
    {"kernel32.dll", "SystemTimeToFileTime", k_SystemTimeToFileTime, 2},
    {"kernel32.dll", "FileTimeToLocalFileTime", k_FileTimeCopy, 2},
    {"kernel32.dll", "LocalFileTimeToFileTime", k_FileTimeCopy, 2},
    {"kernel32.dll", "GetProcessAffinityMask", k_GetProcessAffinityMask, 3},
    {"kernel32.dll", "SetThreadAffinityMask", k_SetThreadAffinityMask, 2},
    {"kernel32.dll", "SetProcessAffinityMask", k_SetProcessAffinityMask, 2},
    {"kernel32.dll", "SetThreadPriority", k_SetThreadPriority, 2},
    {"kernel32.dll", "SetThreadIdealProcessor", k_SetThreadIdealProcessor, 2},
    {"kernel32.dll", "OutputDebugStringW", k_OutputDebugStringW, 1},
    {"kernel32.dll", "GetCurrentDirectoryA", k_GetCurrentDirectoryA, 2},
    {"kernel32.dll", "GetCurrentDirectoryW", k_GetCurrentDirectoryW, 2},
    {"kernel32.dll", "GetTempPathW", k_GetTempPathW, 2},
    {"kernel32.dll", "GetComputerNameW", k_GetComputerNameW, 2},
    {"kernel32.dll", "SetCurrentDirectoryW", k_RetTrue, 1},
    {"kernel32.dll", "SetCurrentDirectoryA", k_RetTrue, 1},
    {"kernel32.dll", "AllocConsole", k_RetZero, 0},
    {"kernel32.dll", "FreeConsole", k_RetTrue, 0},
    {"kernel32.dll", "GetConsoleWindow", k_RetZero, 0},
    {"kernel32.dll", "SetConsoleTitleW", k_RetTrue, 1},
    {"kernel32.dll", "SetConsoleTextAttribute", k_RetTrue, 2},
    {"kernel32.dll", "GetVersion", k_GetVersion, 0},
    {"kernel32.dll", "GetVersionExA", k_GetVersionExA, 1},
    {"kernel32.dll", "GetVersionExW", k_GetVersionExW, 1},
    {"kernel32.dll", "GetProcessHeap", k_GetProcessHeap, 0},
    {"kernel32.dll", "HeapCreate", k_HeapCreate, 3},
    {"kernel32.dll", "HeapDestroy", k_HeapDestroy, 1},
    {"kernel32.dll", "HeapAlloc", k_HeapAlloc, 3},
    {"kernel32.dll", "HeapFree", k_HeapFree, 3},
    {"kernel32.dll", "HeapReAlloc", k_HeapReAlloc, 4},
    {"kernel32.dll", "HeapSize", k_HeapSize, 3},
    {"kernel32.dll", "HeapValidate", k_HeapValidate, 3},
    {"kernel32.dll", "HeapCompact", k_HeapCompact, 2},
    {"kernel32.dll", "HeapSetInformation", k_HeapSetInformation, 4},
    {"kernel32.dll", "VirtualAlloc", k_VirtualAlloc, 4},
    {"kernel32.dll", "VirtualFree", k_VirtualFree, 3},
    {"kernel32.dll", "VirtualQuery", k_VirtualQuery, 3},
    {"kernel32.dll", "VirtualProtect", k_VirtualProtect, 4},
    {"kernel32.dll", "TlsAlloc", k_TlsAlloc, 0},
    {"kernel32.dll", "TlsFree", k_TlsFree, 1},
    {"kernel32.dll", "TlsGetValue", k_TlsGetValue, 1},
    {"kernel32.dll", "TlsSetValue", k_TlsSetValue, 2},
    {"kernel32.dll", "GetLastError", k_GetLastError, 0},
    {"kernel32.dll", "SetLastError", k_SetLastError, 1},
    {"kernel32.dll", "GetCurrentThreadId", k_GetCurrentThreadId, 0},
    {"kernel32.dll", "GetCurrentProcessId", k_GetCurrentProcessId, 0},
    {"kernel32.dll", "GetCurrentThread", k_GetCurrentThread, 0},
    {"kernel32.dll", "GetCurrentProcess", k_GetCurrentProcess, 0},
    {"kernel32.dll", "InitializeCriticalSection", k_InitializeCriticalSection, 1},
    {"kernel32.dll", "InitializeCriticalSectionAndSpinCount", k_InitializeCriticalSectionAndSpinCount, 2},
    {"kernel32.dll", "DeleteCriticalSection", k_DeleteCriticalSection, 1},
    {"kernel32.dll", "EnterCriticalSection", k_EnterCriticalSection, 1},
    {"kernel32.dll", "LeaveCriticalSection", k_LeaveCriticalSection, 1},
    {"kernel32.dll", "TryEnterCriticalSection", k_TryEnterCriticalSection, 1},
    {"kernel32.dll", "InterlockedIncrement", k_InterlockedIncrement, 1},
    {"kernel32.dll", "InterlockedDecrement", k_InterlockedDecrement, 1},
    {"kernel32.dll", "InterlockedExchange", k_InterlockedExchange, 2},
    {"kernel32.dll", "InterlockedExchangeAdd", k_InterlockedExchangeAdd, 2},
    {"kernel32.dll", "InterlockedCompareExchange", k_InterlockedCompareExchange, 3},
    {"kernel32.dll", "GetModuleHandleA", k_GetModuleHandleA, 1},
    {"kernel32.dll", "GetModuleHandleW", k_GetModuleHandleW, 1},
    {"kernel32.dll", "LoadLibraryA", k_LoadLibraryA, 1},
    {"kernel32.dll", "LoadLibraryW", k_LoadLibraryW, 1},
    {"kernel32.dll", "FreeLibrary", k_FreeLibrary, 1},
    {"kernel32.dll", "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls, 1},
    {"kernel32.dll", "GetProcAddress", k_GetProcAddress, 2},
    {"kernel32.dll", "GetModuleFileNameA", k_GetModuleFileNameA, 3},
    {"kernel32.dll", "GetModuleFileNameW", k_GetModuleFileNameW, 3},
    {"kernel32.dll", "GetCommandLineA", k_GetCommandLineA, 0},
    {"kernel32.dll", "GetCommandLineW", k_GetCommandLineW, 0},
    {"kernel32.dll", "GetEnvironmentStrings", k_GetEnvironmentStrings, 0},
    {"kernel32.dll", "GetEnvironmentStringsA", k_GetEnvironmentStrings, 0},
    {"kernel32.dll", "GetEnvironmentStringsW", k_GetEnvironmentStringsW, 0},
    {"kernel32.dll", "FreeEnvironmentStringsA", k_FreeEnvironmentStrings, 1},
    {"kernel32.dll", "FreeEnvironmentStringsW", k_FreeEnvironmentStrings, 1},
    {"kernel32.dll", "GetEnvironmentVariableA", k_GetEnvironmentVariableA, 3},
    {"kernel32.dll", "GetStartupInfoA", k_GetStartupInfo, 1},
    {"kernel32.dll", "GetStartupInfoW", k_GetStartupInfo, 1},
    {"kernel32.dll", "GetStdHandle", k_GetStdHandle, 1},
    {"kernel32.dll", "GetFileType", k_GetFileType, 1},
    {"kernel32.dll", "SetHandleCount", k_SetHandleCount, 1},
    {"kernel32.dll", "GetACP", k_GetACP, 0},
    {"kernel32.dll", "GetOEMCP", k_GetOEMCP, 0},
    {"kernel32.dll", "IsValidCodePage", k_IsValidCodePage, 1},
    {"kernel32.dll", "GetCPInfo", k_GetCPInfo, 2},
    {"kernel32.dll", "MultiByteToWideChar", k_MultiByteToWideChar, 6},
    {"kernel32.dll", "WideCharToMultiByte", k_WideCharToMultiByte, 8},
    {"kernel32.dll", "GetStringTypeA", k_GetStringTypeA, 5},
    {"kernel32.dll", "GetStringTypeW", k_GetStringTypeW, 4},
    {"kernel32.dll", "LCMapStringA", k_LCMapStringA, 6},
    {"kernel32.dll", "LCMapStringW", k_LCMapStringW, 6},
    {"kernel32.dll", "GetLocaleInfoA", k_GetLocaleInfoA, 4},
    {"kernel32.dll", "GetUserDefaultLCID", k_GetUserDefaultLCID, 0},
    {"kernel32.dll", "QueryPerformanceFrequency", k_QueryPerformanceFrequency, 1},
    {"kernel32.dll", "QueryPerformanceCounter", k_QueryPerformanceCounter, 1},
    {"kernel32.dll", "GetTickCount", k_GetTickCount, 0},
    {"kernel32.dll", "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime, 1},
    {"kernel32.dll", "Sleep", k_Sleep, 1},
    {"kernel32.dll", "GetTimeZoneInformation", k_GetTimeZoneInformation, 1},
    {"kernel32.dll", "GetSystemInfo", k_GetSystemInfo, 1},
    {"kernel32.dll", "IsProcessorFeaturePresent", k_IsProcessorFeaturePresent, 1},
    {"kernel32.dll", "IsDebuggerPresent", k_IsDebuggerPresent, 0},
    {"kernel32.dll", "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter, 1},
    {"kernel32.dll", "UnhandledExceptionFilter", k_UnhandledExceptionFilter, 1},
    {"kernel32.dll", "SetConsoleCtrlHandler", k_SetConsoleCtrlHandler, 2},
    {"kernel32.dll", "EncodePointer", k_EncodePointer, 1},
    {"kernel32.dll", "DecodePointer", k_EncodePointer, 1},
    {"kernel32.dll", "SetErrorMode", k_SetErrorMode, 1},
    {"kernel32.dll", "OutputDebugStringA", k_OutputDebugStringA, 1},
    {"kernel32.dll", "ExitProcess", k_ExitProcess, 1},
    {"kernel32.dll", "TerminateProcess", k_TerminateProcess, 2},
    {"kernel32.dll", "RaiseException", k_RaiseException, 4},
};

void rt_kernel32_register(void)
{
    rt_hle_register(kernel32, sizeof kernel32 / sizeof *kernel32);
    rt_hle_register(shell32, sizeof shell32 / sizeof *shell32);
    rt_hle_register(winmm, sizeof winmm / sizeof *winmm);
    rt_hle_register(winsock, sizeof winsock / sizeof *winsock);
}
