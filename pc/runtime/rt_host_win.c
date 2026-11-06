/* Host layer for a Windows x64 host (bring-up and debugging on the PC). */
#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/stat.h>
#include <stdio.h>
#include "rt.h"

void *host_reserve(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS); }
int host_commit(void *p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) != NULL; }
int host_decommit(void *p, size_t size) { return VirtualFree(p, size, MEM_DECOMMIT) != 0; }

struct HostMutex { CRITICAL_SECTION cs; };

HostMutex *host_mutex_new(void)
{
    HostMutex *m = malloc(sizeof *m);
    InitializeCriticalSection(&m->cs);
    return m;
}

void host_lock(HostMutex *m) { EnterCriticalSection(&m->cs); }
void host_unlock(HostMutex *m) { LeaveCriticalSection(&m->cs); }

uint64_t host_ticks_ns(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

uint64_t host_filetime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

void host_sleep_ms(uint32_t ms) { Sleep(ms); }
uint32_t host_thread_id(void) { return GetCurrentThreadId(); }

/* Host faults: report the guest address and the last guest target that went through the
   dispatcher, then stop. */
uint32_t rt_self_last_target(void), rt_self_last_ret(void); /* rt_core.c: the faulting thread's last dispatch */

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    uintptr_t base = x86_mem_base;
    if (a >= base && a - base < 0x100000000ull)
        rt_fatal("host fault %#lx: guest address %#x (%s), host rip %p (image +%#llx); last dispatch %#x (return %#x)",
                 code, (uint32_t)(a - base), ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 (void *)ep->ContextRecord->Rip,
                 (unsigned long long)(ep->ContextRecord->Rip - (uintptr_t)GetModuleHandleW(NULL)),
                 rt_self_last_target(), rt_self_last_ret());
    extern const char *rt_thunk_name(uint32_t t);
    rt_fatal("host fault %#lx at host address %p, rip %p; last dispatch %#x %s (return %#x)", code, (void *)a,
             (void *)ep->ContextRecord->Rip, rt_self_last_target(), rt_thunk_name(rt_self_last_target()), rt_self_last_ret());
}

void host_install_fault_handler(void) { AddVectoredExceptionHandler(1, on_fault); }

struct HostCond { CONDITION_VARIABLE cv; };

HostCond *host_cond_new(void)
{
    HostCond *c = malloc(sizeof *c);
    InitializeConditionVariable(&c->cv);
    return c;
}

int host_cond_wait(HostCond *c, HostMutex *m, uint32_t timeout_ms)
{
    return SleepConditionVariableCS(&c->cv, &m->cs, timeout_ms == 0xFFFFFFFFu ? INFINITE : timeout_ms) ? 1 : 0;
}

void host_cond_broadcast(HostCond *c) { WakeAllConditionVariable(&c->cv); }

typedef struct { void (*fn)(void *); void *arg; } ThreadStart;

static DWORD WINAPI thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return 0;
}

/* ---- Direct3D 9: the system d3d9.dll of the Windows host (bring-up of the bridge) ---- */
void host_wait_window(void) {} /* Windows has its window from the start */

void *host_d3d9_create(uint32_t sdk_version)
{
    HMODULE m = LoadLibraryA("d3d9.dll");
    if (!m) return NULL;
    void *(WINAPI *create)(UINT) = (void *(WINAPI *)(UINT))(void *)GetProcAddress(m, "Direct3DCreate9");
    return create ? create(sdk_version) : NULL;
}

/* ---- windows: one real top-level window per guest top-level window ---- */
static LRESULT CALLBACK host_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DESTROY) {
        rt_log("host window closed: exiting");
        ExitProcess(0);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void *host_window_create(int w, int h, const char *title)
{
    static int registered;
    if (!registered) {
        WNDCLASSW c = {0};
        c.lpfnWndProc = host_wndproc;
        c.hInstance = GetModuleHandleW(NULL);
        c.hCursor = LoadCursor(NULL, IDC_ARROW);
        c.lpszClassName = L"RunnerRecompiledPC";
        RegisterClassW(&c);
        registered = 1;
    }
    RECT r = {0, 0, w, h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t wt[128];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wt, 128);
    HWND hw = CreateWindowExW(0, L"RunnerRecompiledPC", wt, WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                              CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, GetModuleHandleW(NULL), NULL);
    return hw;
}

static __thread MSG last_msg; /* per thread: each guest thread pumps its own host queue */

/* Next queued host message of this thread (removed if remove). */
int host_peek_message(void **hwnd, uint32_t *msg, uint32_t *wp, uint32_t *lp, uint32_t *time, int32_t *x, int32_t *y,
                      int remove)
{
    MSG m;
    if (!PeekMessageW(&m, NULL, 0, 0, remove ? PM_REMOVE : PM_NOREMOVE)) return 0;
    if (remove) last_msg = m;
    *hwnd = m.hwnd;
    *msg = m.message;
    *wp = (uint32_t)m.wParam;
    *lp = (uint32_t)m.lParam;
    *time = m.time;
    *x = m.pt.x;
    *y = m.pt.y;
    return 1;
}

void host_translate_last(void) { TranslateMessage(&last_msg); }

/* Posted messages go through the host queue of the window's thread (or of this thread). */
int host_post_message(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    return PostMessageW((HWND)hwnd, msg, (WPARAM)wp, (LPARAM)lp) != 0;
}

void host_post_quit(int code) { PostQuitMessage(code); }

/* A message of a host window the guest does not know (IME, Direct3D helpers): handled by the host. */
void host_dispatch_last(void) { DispatchMessageW(&last_msg); }

/* WM_PAINT stays queued until the window is validated. */
void host_validate(void *hwnd) { ValidateRect((HWND)hwnd, NULL); }

uint32_t host_def_window_proc(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    return (uint32_t)DefWindowProcW((HWND)hwnd, msg, (WPARAM)wp, (LPARAM)(int32_t)lp);
}

void host_window_show(void *hwnd, int show) { ShowWindow((HWND)hwnd, show ? SW_SHOW : SW_HIDE); }

int host_thread_start(void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    HANDLE h = CreateThread(NULL, 8u << 20, thread_entry, s, 0, NULL);
    if (!h) return 0;
    CloseHandle(h);
    return 1;
}
#endif

#ifdef _WIN32
/* ---- file system (narrow CRT and A-APIs: the game's paths are ASCII) ---- */
int host_open(const char *path, int write, int create, int truncate)
{
    int fl = _O_BINARY | (write ? _O_RDWR : _O_RDONLY) | (create ? _O_CREAT : 0) | (truncate ? _O_TRUNC : 0);
    return _open(path, fl, _S_IREAD | _S_IWRITE);
}

int64_t host_read(int fd, void *buf, uint32_t n) { return _read(fd, buf, n); }
int64_t host_write(int fd, const void *buf, uint32_t n) { return _write(fd, buf, n); }
int64_t host_seek(int fd, int64_t off, int whence) { return _lseeki64(fd, off, whence); }
int host_truncate(int fd, int64_t size) { return _chsize_s(fd, size) == 0; }
void host_close(int fd) { _close(fd); }

int host_stat(const char *path, int64_t *size, uint64_t *filetime)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return 0;
    if (size) *size = ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (filetime) *filetime = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
}

int host_mkdir(const char *path) { return _mkdir(path) == 0; }
int host_rmdir(const char *path) { return _rmdir(path) == 0; }
int host_unlink(const char *path) { return _unlink(path) == 0; }
int host_rename(const char *from, const char *to) { return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0; }

int host_list_dir(const char *path, void (*fn)(void *u, const char *name), void *u)
{
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) fn(u, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 1;
}
#endif

#ifdef _WIN32
/* ---- audio output: waveOut, 16-bit stereo, four blocks of 1024 frames (about 85 ms queued) ---- */
#include <mmsystem.h>

static void (*audio_mix)(int16_t *out, uint32_t frames);
static HWAVEOUT audio_out;
static HANDLE audio_event;
#define AUDIO_BLOCKS 4
#define AUDIO_FRAMES 1024
static WAVEHDR audio_hdr[AUDIO_BLOCKS];
static int16_t audio_data[AUDIO_BLOCKS][AUDIO_FRAMES * 2];

static DWORD WINAPI audio_thread(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (;;) {
        WaitForSingleObject(audio_event, INFINITE);
        for (int i = 0; i < AUDIO_BLOCKS; i++)
            if (audio_hdr[i].dwFlags & WHDR_DONE) {
                audio_mix(audio_data[i], AUDIO_FRAMES);
                audio_hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(audio_out, &audio_hdr[i], sizeof audio_hdr[i]);
            }
    }
    return 0;
}

int host_audio_start(uint32_t rate, void (*mix)(int16_t *out, uint32_t frames))
{
    WAVEFORMATEX f = {WAVE_FORMAT_PCM, 2, rate, rate * 4, 4, 16, 0};
    audio_mix = mix;
    audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&audio_out, WAVE_MAPPER, &f, (DWORD_PTR)audio_event, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR)
        return 0;
    for (int i = 0; i < AUDIO_BLOCKS; i++) {
        audio_hdr[i].lpData = (LPSTR)audio_data[i];
        audio_hdr[i].dwBufferLength = sizeof audio_data[i];
        waveOutPrepareHeader(audio_out, &audio_hdr[i], sizeof audio_hdr[i]);
        audio_hdr[i].dwFlags |= WHDR_DONE; /* filled by the thread on its first pass */
    }
    CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    SetEvent(audio_event);
    return 1;
}

/* ---- mouse and pads for the guest's DirectInput/XInput (rt_dinput.c) ---- */

/* Relative mouse while a window of ours is in front: the cursor is hidden, held in the middle of
   the window and confined to it; motion is measured from the middle. Returns 0 (no capture) when
   another program is in front or ME_PC_MOUSE_CAPTURE=0. */
int host_mouse_poll(int32_t *dx, int32_t *dy, uint8_t buttons[3])
{
    static int captured, disabled = -1;
    if (disabled < 0) { const char *e = getenv("ME_PC_MOUSE_CAPTURE"); disabled = e && e[0] == '0'; }
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (disabled || !fg || pid != GetCurrentProcessId() || IsIconic(fg)) {
        if (captured) { ClipCursor(NULL); ShowCursor(TRUE); captured = 0; }
        return 0;
    }
    RECT r;
    GetClientRect(fg, &r);
    POINT c = {(r.right - r.left) / 2, (r.bottom - r.top) / 2}, tl = {0, 0}, br = {r.right, r.bottom}, p;
    ClientToScreen(fg, &c);
    ClientToScreen(fg, &tl);
    ClientToScreen(fg, &br);
    GetCursorPos(&p);
    *dx = captured ? p.x - c.x : 0;
    *dy = captured ? p.y - c.y : 0;
    if (!captured) {
        RECT clip = {tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        ShowCursor(FALSE);
        captured = 1;
    }
    SetCursorPos(c.x, c.y);
    buttons[0] = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 0x80 : 0;
    buttons[1] = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? 0x80 : 0;
    buttons[2] = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) ? 0x80 : 0;
    return 1;
}

/* XInput of the host (xinput1_4.dll, Windows 8+): XINPUT_STATE is 16 bytes in both ABIs. */
typedef DWORD (WINAPI *XGetState)(DWORD, void *);
typedef DWORD (WINAPI *XSetState)(DWORD, void *);
static XGetState x_get;
static XSetState x_set;
static int x_loaded;

static void xinput_load(void)
{
    if (x_loaded) return;
    x_loaded = 1;
    HMODULE h = LoadLibraryA("xinput1_4.dll");
    if (!h) h = LoadLibraryA("xinput9_1_0.dll");
    if (h) { x_get = (XGetState)(void *)GetProcAddress(h, "XInputGetState"); x_set = (XSetState)(void *)GetProcAddress(h, "XInputSetState"); }
}

uint32_t host_pad_get(uint32_t index, uint8_t state[16])
{
    xinput_load();
    return x_get ? x_get(index, state) : 1167u; /* ERROR_DEVICE_NOT_CONNECTED */
}

uint32_t host_pad_rumble(uint32_t index, uint16_t left, uint16_t right)
{
    xinput_load();
    uint16_t v[2] = {left, right};
    return x_set ? x_set(index, v) : 1167u;
}

void *host_focus_window(void) { return GetFocus(); }   /* of the calling thread's queue */
void *host_foreground_window(void)
{
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() ? fg : NULL;
}

/* ---- sampling profiler (ME_PC_PROFILE=<seconds>[,<delay seconds>]) ----
   A host thread stops every guest thread once a millisecond and records its instruction pointer
   as an offset in me_pc.exe; after the delay it collects for the given time and writes
   pc-profile.bin (pairs of uint32: thread index, offset; 0xFFFFFFFF = outside the image) and
   pc-profile-threads.txt (index, host id, guest name). pc/tools/profile_report.py names them. */
static uint32_t prof_seconds, prof_delay;

static DWORD WINAPI prof_run(LPVOID arg)
{
    (void)arg;
    Sleep(prof_delay * 1000u);
    uintptr_t base = (uintptr_t)GetModuleHandleW(NULL);
    enum { MAXT = 64 };
    uint32_t tids[MAXT];
    const char *names[MAXT];
    HANDLE hs[MAXT] = {0};
    uint32_t got[MAXT] = {0}, fail_open[MAXT] = {0}, fail_suspend[MAXT] = {0}, fail_ctx[MAXT] = {0};
    DWORD open_err[MAXT] = {0};
    uint32_t cap = prof_seconds * 1200u * 16u, n = 0;
    uint32_t *s = malloc(sizeof *s * 2 * cap);
    timeBeginPeriod(1);
    ULONGLONG end = GetTickCount64() + prof_seconds * 1000ull;
    int nt = 0;
    while (GetTickCount64() < end && n + MAXT < cap) {
        nt = rt_thread_list(tids, names, MAXT);
        for (int t = 0; t < nt; t++) {
            if (!hs[t]) { hs[t] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tids[t]); if (!hs[t]) open_err[t] = GetLastError(); }
            if (!hs[t] || tids[t] == GetCurrentThreadId()) { fail_open[t]++; continue; }
            if (SuspendThread(hs[t]) == (DWORD)-1) { fail_suspend[t]++; continue; }
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(hs[t], &c)) {
                uintptr_t off = c.Rip - base;
                s[2 * n] = (uint32_t)t;
                s[2 * n + 1] = off < 0x40000000u ? (uint32_t)off : 0xFFFFFFFFu;
                n++;
                got[t]++;
            } else fail_ctx[t]++;
            ResumeThread(hs[t]);
        }
        Sleep(1);
    }
    timeEndPeriod(1);
    FILE *f = fopen("pc-profile.bin", "wb");
    if (f) { fwrite(s, 8, n, f); fclose(f); }
    f = fopen("pc-profile-threads.txt", "w");
    if (f) {
        nt = rt_thread_list(tids, names, MAXT);
        for (int t = 0; t < nt; t++) fprintf(f, "%d %u %s\n", t, tids[t], names[t] ? names[t] : (t == 0 ? "main" : "-"));
        fclose(f);
    }
    rt_log("profile: %u samples of %d threads written to pc-profile.bin", n, nt);
    for (int t = 0; t < nt; t++)
        rt_log("profile:   thread %d (%u %s): %u samples, open failed %u (error %lu), suspend failed %u, context failed %u", t, tids[t],
               names[t] ? names[t] : "-", got[t], fail_open[t], open_err[t], fail_suspend[t], fail_ctx[t]);
    free(s);
    return 0;
}

void host_profile_start(void)
{
    const char *e = getenv("ME_PC_PROFILE");
    if (!e) return;
    prof_seconds = (uint32_t)atoi(e);
    const char *comma = strchr(e, ',');
    prof_delay = comma ? (uint32_t)atoi(comma + 1) : 0;
    if (!prof_seconds) prof_seconds = 30;
    CreateThread(NULL, 0, prof_run, NULL, 0, NULL);
    rt_log("profile: %u s of samples after %u s", prof_seconds, prof_delay);
}
#endif
