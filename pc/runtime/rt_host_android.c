/* Host layer for Android (ARM64): the same contract as rt_host_win.c, on POSIX and NDK APIs.

   - Memory: one mmap reservation; commit = mprotect read/write, decommit = MADV_DONTNEED (pages
     read back as zero, as Windows gives after a new commit).
   - Locks are recursive (Windows critical sections are); waits use CLOCK_MONOTONIC.
   - Files: Linux paths are case-sensitive, the game's are not: every path is resolved one
     component at a time, matching names without regard to case when the exact one is missing.
   - Windows: Android has one surface. The first guest top-level window is the game view; the
     app hands its ANativeWindow over with rt_android_set_window(). Messages are queues per
     thread (posted messages, input from the app: rt_android_key/rt_android_mouse_*).
   - Direct3D 9: DXVK Native (libdxvk_d3d9.so, loaded at run time) once it is part of the APK.
   - Audio: AAudio (libaaudio.so, API 26+), 48 kHz 16-bit stereo, the runtime's mixer as callback.
   - Pads: XInput state set by the app (rt_android_pad). */
#ifdef __ANDROID__
#define _GNU_SOURCE
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include "rt.h"

/* ---- memory ---- */
void *host_reserve(size_t size)
{
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int host_commit(void *p, size_t size) { return mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }

int host_decommit(void *p, size_t size)
{
    madvise(p, size, MADV_DONTNEED);
    return mprotect(p, size, PROT_NONE) == 0;
}

/* ---- locks, time, threads ---- */
struct HostMutex { pthread_mutex_t m; };
struct HostCond { pthread_cond_t c; };

HostMutex *host_mutex_new(void)
{
    HostMutex *m = malloc(sizeof *m);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

void host_lock(HostMutex *m) { pthread_mutex_lock(&m->m); }
void host_unlock(HostMutex *m) { pthread_mutex_unlock(&m->m); }

uint64_t host_ticks_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

uint64_t host_filetime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return ((uint64_t)t.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)t.tv_nsec / 100u;
}

void host_sleep_ms(uint32_t ms)
{
    if (!ms) { sched_yield(); return; }
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) && errno == EINTR) {}
}

uint32_t host_thread_id(void) { return (uint32_t)gettid(); }

HostCond *host_cond_new(void)
{
    HostCond *c = malloc(sizeof *c);
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&c->c, &a);
    pthread_condattr_destroy(&a);
    return c;
}

int host_cond_wait(HostCond *c, HostMutex *m, uint32_t timeout_ms)
{
    if (timeout_ms == 0xFFFFFFFFu) return pthread_cond_wait(&c->c, &m->m) == 0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += timeout_ms / 1000;
    t.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return pthread_cond_timedwait(&c->c, &m->m, &t) == 0;
}

void host_cond_broadcast(HostCond *c) { pthread_cond_broadcast(&c->c); }

typedef struct { void (*fn)(void *); void *arg; } ThreadStart;

static void *thread_entry(void *p)
{
    ThreadStart s = *(ThreadStart *)p;
    free(p);
    s.fn(s.arg);
    return NULL;
}

int host_thread_start(void (*fn)(void *), void *arg)
{
    ThreadStart *s = malloc(sizeof *s);
    s->fn = fn;
    s->arg = arg;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    int ok = pthread_create(&t, &a, thread_entry, s) == 0;
    pthread_attr_destroy(&a);
    if (!ok) free(s);
    return ok;
}

/* ---- faults: report the guest address and the last dispatch, then stop ---- */
uint32_t rt_self_last_target(void), rt_self_last_ret(void); /* rt_core.c: the faulting thread's last dispatch */

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    uintptr_t a = (uintptr_t)si->si_addr, base = x86_mem_base;
    uintptr_t pc = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
    Dl_info info;
    uintptr_t image = dladdr((void *)pc, &info) ? (uintptr_t)info.dli_fbase : 0;
    if (a >= base && a - base < 0x100000000ull)
        rt_fatal("host signal %d: guest address %#x, host pc %p (image +%#lx); last dispatch %#x (return %#x)", sig,
                 (uint32_t)(a - base), (void *)pc, (unsigned long)(pc - image), rt_self_last_target(), rt_self_last_ret());
    extern const char *rt_thunk_name(uint32_t t);
    rt_fatal("host signal %d at host address %p, pc %p (image +%#lx); last dispatch %#x %s (return %#x)", sig, (void *)a,
             (void *)pc, (unsigned long)(pc - image), rt_self_last_target(), rt_thunk_name(rt_self_last_target()), rt_self_last_ret());
}

void host_install_fault_handler(void)
{
    /* the handler needs its own stack: a stack overflow is one of the faults */
    static char alt[64 * 1024];
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* ---- Direct3D 9: DXVK Native (with the project's Android WSI: the view is handed over with
   dxvk_android_set_window, at load and whenever the app's surface changes) ---- */
static ANativeWindow *volatile surface;            /* the app's view (rt_android_set_window) */
static void (*dxvk_set_window)(ANativeWindow *);

/* Vulkan needs the app's surface, and there is none while the screen is off or the app is in
   the background: creating or resetting the device waits for it instead of failing. */
void host_wait_window(void)
{
    if (!surface) rt_log("Direct3D 9: waiting for the app's surface");
    while (!surface) host_sleep_ms(50);
}

void *host_d3d9_create(uint32_t sdk_version)
{
    /* DXVK Native picks its window system by name (pc/dxvk patch: "Android"); its logs go next
       to the runtime's own log (otherwise to stderr, which Android drops). */
    setenv("DXVK_WSI_DRIVER", "Android", 0);
    const char *log = getenv("ME_PC_LOG_PATH");
    if (log && !getenv("DXVK_LOG_PATH")) {
        char dir[512];
        snprintf(dir, sizeof dir, "%s", log);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; setenv("DXVK_LOG_PATH", dir, 0); }
    }
    void *lib = dlopen("libdxvk_d3d9.so", RTLD_NOW);
    if (!lib) { rt_log("Direct3D 9: libdxvk_d3d9.so not available (%s)", dlerror()); return NULL; }
    dxvk_set_window = (void (*)(ANativeWindow *))dlsym(lib, "dxvk_android_set_window");
    if (dxvk_set_window) dxvk_set_window(surface);
    else rt_log("Direct3D 9: libdxvk_d3d9.so has no Android WSI (dxvk_android_set_window)");
    void *(*create)(uint32_t) = (void *(*)(uint32_t))dlsym(lib, "Direct3DCreate9");
    return create ? create(sdk_version) : NULL;
}

/* ---- windows and messages ---- */
typedef struct { void *hwnd; uint32_t msg, wp, lp, time; } HostMsg;
typedef struct MsgQueue {
    pid_t tid;
    HostMsg q[512];
    int head, n;
    struct MsgQueue *next;
} MsgQueue;

static pthread_mutex_t mq_lock = PTHREAD_MUTEX_INITIALIZER;
static MsgQueue *queues;
static __thread MsgQueue *my_queue;

typedef struct { int used; pid_t owner; int w, h; } HostWindow;
static HostWindow windows[16];
static volatile int app_in_front = 1;

static MsgQueue *queue_of(pid_t tid)
{
    for (MsgQueue *q = queues; q; q = q->next)
        if (q->tid == tid) return q;
    MsgQueue *q = calloc(1, sizeof *q);
    q->tid = tid;
    q->next = queues;
    queues = q;
    return q;
}

static int post_to(pid_t tid, void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    pthread_mutex_lock(&mq_lock);
    MsgQueue *q = queue_of(tid);
    int ok = q->n < (int)(sizeof q->q / sizeof q->q[0]);
    if (ok) {
        HostMsg *m = &q->q[(q->head + q->n) % (int)(sizeof q->q / sizeof q->q[0])];
        m->hwnd = hwnd; m->msg = msg; m->wp = wp; m->lp = lp; m->time = (uint32_t)(host_ticks_ns() / 1000000u);
        q->n++;
    }
    pthread_mutex_unlock(&mq_lock);
    return ok;
}

void *host_window_create(int w, int h, const char *title)
{
    (void)title;
    for (int i = 0; i < 16; i++)
        if (!windows[i].used) {
            windows[i].used = 1;
            windows[i].owner = gettid();
            windows[i].w = w;
            windows[i].h = h;
            rt_log("window %d: %dx%d (%s)", i, w, h, i == 0 ? "the game view" : "off screen");
            return &windows[i];
        }
    return NULL;
}

int host_peek_message(void **hwnd, uint32_t *msg, uint32_t *wp, uint32_t *lp, uint32_t *time, int32_t *x, int32_t *y,
                      int remove)
{
    pthread_mutex_lock(&mq_lock);
    if (!my_queue) my_queue = queue_of(gettid());
    MsgQueue *q = my_queue;
    int have = q->n > 0;
    if (have) {
        HostMsg *m = &q->q[q->head];
        *hwnd = m->hwnd; *msg = m->msg; *wp = m->wp; *lp = m->lp; *time = m->time;
        *x = (int32_t)(int16_t)(m->lp & 0xFFFF);
        *y = (int32_t)(int16_t)(m->lp >> 16);
        if (remove) { q->head = (q->head + 1) % (int)(sizeof q->q / sizeof q->q[0]); q->n--; }
    }
    pthread_mutex_unlock(&mq_lock);
    return have;
}

void host_translate_last(void) {}   /* WM_CHAR comes from the app as text input when needed */
void host_dispatch_last(void) {}
void host_validate(void *hwnd) { (void)hwnd; }
uint32_t host_def_window_proc(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp) { (void)hwnd; (void)msg; (void)wp; (void)lp; return 0; }
void host_window_show(void *hwnd, int show) { (void)hwnd; (void)show; }

int host_post_message(void *hwnd, uint32_t msg, uint32_t wp, uint32_t lp)
{
    HostWindow *w = hwnd;
    return post_to(w ? w->owner : gettid(), hwnd, msg, wp, lp);
}

int host_post_thread_message(uint32_t host_tid, uint32_t msg, uint32_t wp, uint32_t lp) { return post_to((pid_t)host_tid, NULL, msg, wp, lp); }
void host_post_quit(int code) { post_to(gettid(), NULL, 0x0012, (uint32_t)code, 0); }
void *host_focus_window(void) { return app_in_front && windows[0].used && windows[0].owner == gettid() ? &windows[0] : NULL; }
void *host_foreground_window(void) { return app_in_front && windows[0].used ? &windows[0] : NULL; }

/* ---- input and surface from the app (JNI side) ---- */
void rt_android_set_window(ANativeWindow *w)
{
    surface = w;
    if (dxvk_set_window) dxvk_set_window(w);
}
ANativeWindow *rt_android_window(void) { return surface; }
void rt_android_set_foreground(int in_front) { app_in_front = in_front; }

/* A key as Windows sees it: virtual key, scan code (set 1), extended flag. */
void rt_android_key(uint32_t vk, uint32_t scan, int extended, int down)
{
    if (!windows[0].used) return;
    uint32_t lp = 1u | (scan & 0xFF) << 16 | (extended ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
    post_to(windows[0].owner, &windows[0], down ? 0x0100 : 0x0101, vk, lp);
}

/* Pointer in view coordinates; buttons: 0 left, 1 right, 2 middle. */
void rt_android_mouse_move(int x, int y)
{
    if (windows[0].used) post_to(windows[0].owner, &windows[0], 0x0200, 0, (uint32_t)(x & 0xFFFF) | (uint32_t)y << 16);
}

void rt_android_mouse_button(int button, int down, int x, int y)
{
    static const uint32_t msgs[3][2] = {{0x0202, 0x0201}, {0x0205, 0x0204}, {0x0208, 0x0207}};
    if (windows[0].used && button >= 0 && button < 3)
        post_to(windows[0].owner, &windows[0], msgs[button][down ? 1 : 0], 0, (uint32_t)(x & 0xFFFF) | (uint32_t)y << 16);
}

int host_mouse_poll(int32_t *dx, int32_t *dy, uint8_t buttons[3]) { (void)dx; (void)dy; (void)buttons; return 0; }

/* XInput: XINPUT_STATE (packet, buttons, triggers, sticks) as the app reports it. */
static uint8_t pad_state[4][16];
static int pad_connected[4];
static uint16_t pad_rumble[4][2];

void rt_android_pad(uint32_t index, int connected, uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly,
                    int16_t rx, int16_t ry)
{
    if (index >= 4) return;
    uint8_t *s = pad_state[index];
    uint32_t packet;
    memcpy(&packet, s, 4);
    packet++;
    memcpy(s, &packet, 4);
    memcpy(s + 4, &buttons, 2);
    s[6] = lt; s[7] = rt;
    memcpy(s + 8, &lx, 2); memcpy(s + 10, &ly, 2); memcpy(s + 12, &rx, 2); memcpy(s + 14, &ry, 2);
    pad_connected[index] = connected;
}

void rt_android_pad_rumble(uint32_t index, uint16_t *left, uint16_t *right)
{
    if (index < 4) { *left = pad_rumble[index][0]; *right = pad_rumble[index][1]; }
}

uint32_t host_pad_get(uint32_t index, uint8_t state[16])
{
    if (index >= 4 || !pad_connected[index]) return 1167u; /* ERROR_DEVICE_NOT_CONNECTED */
    memcpy(state, pad_state[index], 16);
    return 0;
}

uint32_t host_pad_rumble(uint32_t index, uint16_t left, uint16_t right)
{
    if (index >= 4 || !pad_connected[index]) return 1167u;
    pad_rumble[index][0] = left;
    pad_rumble[index][1] = right;
    return 0;
}

/* ---- files: case-insensitive resolution of every component ---- */
/* Resolves path into out. Components that exist under another case are replaced; a missing
   component (and everything after it) is kept as given, so creating a new file still works.
   Returns 1 when the whole path exists. */
static int resolve_ci(const char *path, char *out, size_t n)
{
    struct stat st;
    if (stat(path, &st) == 0) { snprintf(out, n, "%s", path); return 1; }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    size_t o = 0;
    out[0] = 0;
    char *p = buf;
    if (*p == '/') { out[o++] = '/'; out[o] = 0; p++; }
    int exists = 1;
    while (*p) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        char cand[1024];
        snprintf(cand, sizeof cand, "%s%s", out, p);
        if (exists && stat(cand, &st) != 0) {
            exists = 0;
            DIR *d = opendir(o ? out : ".");
            if (d) {
                struct dirent *e;
                while ((e = readdir(d)))
                    if (!strcasecmp(e->d_name, p)) { snprintf(cand, sizeof cand, "%s%s", out, e->d_name); exists = 1; break; }
                closedir(d);
            }
        }
        o = (size_t)snprintf(out, n, "%s", cand);
        if (!slash) break;
        if (o + 1 < n) { out[o++] = '/'; out[o] = 0; }
        p = slash + 1;
    }
    return exists;
}

int host_open(const char *path, int write, int create, int truncate)
{
    char r[1024];
    resolve_ci(path, r, sizeof r);
    int fl = (write ? O_RDWR : O_RDONLY) | (create ? O_CREAT : 0) | (truncate ? O_TRUNC : 0) | O_CLOEXEC;
    return open(r, fl, 0664);
}

int64_t host_read(int fd, void *buf, uint32_t n) { return read(fd, buf, n); }
int64_t host_write(int fd, const void *buf, uint32_t n) { return write(fd, buf, n); }
int64_t host_seek(int fd, int64_t off, int whence) { return lseek64(fd, off, whence); }
int host_truncate(int fd, int64_t size) { return ftruncate64(fd, size) == 0; }
void host_close(int fd) { close(fd); }

int host_stat(const char *path, int64_t *size, uint64_t *filetime)
{
    char r[1024];
    if (!resolve_ci(path, r, sizeof r)) return 0;
    struct stat st;
    if (stat(r, &st) != 0) return 0;
    if (size) *size = st.st_size;
    if (filetime) *filetime = ((uint64_t)st.st_mtim.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)st.st_mtim.tv_nsec / 100u;
    return S_ISDIR(st.st_mode) ? 2 : 1;
}

int host_mkdir(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return mkdir(r, 0775) == 0; }
int host_rmdir(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return rmdir(r) == 0; }
int host_unlink(const char *path) { char r[1024]; resolve_ci(path, r, sizeof r); return unlink(r) == 0; }

int host_rename(const char *from, const char *to)
{
    char a[1024], b[1024];
    resolve_ci(from, a, sizeof a);
    resolve_ci(to, b, sizeof b);
    return rename(a, b) == 0;
}

int host_list_dir(const char *path, void (*fn)(void *u, const char *name), void *u)
{
    char r[1024];
    if (!resolve_ci(path, r, sizeof r)) return 0;
    DIR *d = opendir(r);
    if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) fn(u, e->d_name);
    closedir(d);
    return 1;
}

/* profiling: not on Android yet (simpleperf from the NDK works on the device) */
void host_profile_start(void) {}

/* ---- audio: AAudio (loaded at run time so the library also loads on systems without it) ---- */
static void (*audio_mix)(int16_t *out, uint32_t frames);

static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)s; (void)user;
    int16_t *out = data;
    while (frames > 0) {
        uint32_t k = frames > 4096 ? 4096 : (uint32_t)frames;
        audio_mix(out, k);
        out += 2 * k;
        frames -= (int32_t)k;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int host_audio_start(uint32_t rate, void (*mix)(int16_t *out, uint32_t frames))
{
    void *lib = dlopen("libaaudio.so", RTLD_NOW);
    if (!lib) return 0;
#define SYM(name) __typeof__(&name) p_##name = (__typeof__(&name))dlsym(lib, #name); if (!p_##name) return 0
    SYM(AAudio_createStreamBuilder);
    SYM(AAudioStreamBuilder_setSampleRate);
    SYM(AAudioStreamBuilder_setChannelCount);
    SYM(AAudioStreamBuilder_setFormat);
    SYM(AAudioStreamBuilder_setPerformanceMode);
    SYM(AAudioStreamBuilder_setDataCallback);
    SYM(AAudioStreamBuilder_openStream);
    SYM(AAudioStreamBuilder_delete);
    SYM(AAudioStream_requestStart);
#undef SYM
    audio_mix = mix;
    AAudioStreamBuilder *b;
    if (p_AAudio_createStreamBuilder(&b) != AAUDIO_OK) return 0;
    p_AAudioStreamBuilder_setSampleRate(b, (int32_t)rate);
    p_AAudioStreamBuilder_setChannelCount(b, 2);
    p_AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    p_AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    p_AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    AAudioStream *s;
    aaudio_result_t r = p_AAudioStreamBuilder_openStream(b, &s);
    p_AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) return 0;
    return p_AAudioStream_requestStart(s) == AAUDIO_OK;
}
#endif
