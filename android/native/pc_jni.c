/* JNI side of the PC variant (PcGameActivity): no game code here. It loads libmepc.so (the lifted
   PC game, private builds only) with dlopen, runs me_pc_main on a thread of its own and forwards
   the view surface and the input to the runtime's Android host (pc/runtime/rt_host_android.c). */
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "me_pc_jni"
#define JNI(name) Java_org_mirrorsedge_research_PcGameActivity_##name

static void *lib;
static int (*me_pc_main)(int, char **);
static void (*set_window)(ANativeWindow *);
static void (*set_foreground)(int);
static void (*key)(uint32_t, uint32_t, int, int);
static void (*mouse_move)(int, int);
static void (*mouse_button)(int, int, int, int);
static void (*pad)(uint32_t, int, uint16_t, uint8_t, uint8_t, int16_t, int16_t, int16_t, int16_t);
static ANativeWindow *window;
static int argc;
static char **argv;
static volatile int exit_code = -1;

static void *run(void *arg)
{
    (void)arg;
    exit_code = me_pc_main(argc, argv);
    __android_log_print(ANDROID_LOG_INFO, TAG, "me_pc_main returned %d", exit_code);
    return NULL;
}

/* nativeStart(library path, arguments, environment "NAME=value"): false when the library or an
   entry point is missing. */
JNIEXPORT jboolean JNICALL JNI(nativeStart)(JNIEnv *env, jclass cls, jstring path, jobjectArray args, jobjectArray vars)
{
    (void)cls;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    lib = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) __android_log_print(ANDROID_LOG_ERROR, TAG, "dlopen %s: %s", p, dlerror());
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!lib) return JNI_FALSE;
#define SYM(var, name) *(void **)&var = dlsym(lib, name); if (!var) { __android_log_print(ANDROID_LOG_ERROR, TAG, "missing %s", name); return JNI_FALSE; }
    SYM(me_pc_main, "me_pc_main");
    SYM(set_window, "rt_android_set_window");
    SYM(set_foreground, "rt_android_set_foreground");
    SYM(key, "rt_android_key");
    SYM(mouse_move, "rt_android_mouse_move");
    SYM(mouse_button, "rt_android_mouse_button");
    SYM(pad, "rt_android_pad");
#undef SYM
    for (jsize i = 0, n = (*env)->GetArrayLength(env, vars); i < n; i++) {
        jstring s = (*env)->GetObjectArrayElement(env, vars, i);
        const char *v = (*env)->GetStringUTFChars(env, s, NULL);
        const char *eq = strchr(v, '=');
        if (eq) {
            char name[256];
            size_t k = (size_t)(eq - v) < sizeof name - 1 ? (size_t)(eq - v) : sizeof name - 1;
            memcpy(name, v, k);
            name[k] = 0;
            setenv(name, eq + 1, 1);
        }
        (*env)->ReleaseStringUTFChars(env, s, v);
    }
    argc = (*env)->GetArrayLength(env, args) + 1;
    argv = calloc((size_t)argc + 1, sizeof *argv);
    argv[0] = strdup("me_pc");
    for (int i = 1; i < argc; i++) {
        jstring s = (*env)->GetObjectArrayElement(env, args, i - 1);
        const char *v = (*env)->GetStringUTFChars(env, s, NULL);
        argv[i] = strdup(v);
        (*env)->ReleaseStringUTFChars(env, s, v);
    }
    if (window) set_window(window);
    /* lifted code recurses deeply on the host stack (the Windows build links with 8 MB) */
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t;
    int ok = pthread_create(&t, &a, run, NULL) == 0;
    pthread_attr_destroy(&a);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL JNI(nativeExitCode)(JNIEnv *env, jclass cls) { (void)env; (void)cls; return exit_code; }

/* The view surface (null when it goes away). The previous window is released after the runtime
   switched over. */
JNIEXPORT void JNICALL JNI(nativeSetSurface)(JNIEnv *env, jclass cls, jobject surface)
{
    (void)cls;
    ANativeWindow *old = window;
    window = surface ? ANativeWindow_fromSurface(env, surface) : NULL;
    if (set_window) set_window(window);
    if (old) ANativeWindow_release(old);
}

JNIEXPORT void JNICALL JNI(nativeSetForeground)(JNIEnv *env, jclass cls, jboolean front)
{
    (void)env; (void)cls;
    if (set_foreground) set_foreground(front);
}

JNIEXPORT void JNICALL JNI(nativeKey)(JNIEnv *env, jclass cls, jint vk, jint scan, jboolean extended, jboolean down)
{
    (void)env; (void)cls;
    if (key) key((uint32_t)vk, (uint32_t)scan, extended, down);
}

JNIEXPORT void JNICALL JNI(nativeMouseMove)(JNIEnv *env, jclass cls, jint x, jint y)
{
    (void)env; (void)cls;
    if (mouse_move) mouse_move(x, y);
}

JNIEXPORT void JNICALL JNI(nativeMouseButton)(JNIEnv *env, jclass cls, jint button, jboolean down, jint x, jint y)
{
    (void)env; (void)cls;
    if (mouse_button) mouse_button(button, down, x, y);
}

JNIEXPORT void JNICALL JNI(nativePad)(JNIEnv *env, jclass cls, jint index, jboolean connected, jint buttons, jint lt,
                                      jint rt, jint lx, jint ly, jint rx, jint ry)
{
    (void)env; (void)cls;
    if (pad) pad((uint32_t)index, connected, (uint16_t)buttons, (uint8_t)lt, (uint8_t)rt, (int16_t)lx, (int16_t)ly,
                 (int16_t)rx, (int16_t)ry);
}
