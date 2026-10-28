// In-app runtime (libmeruntime.so): the same loader as the ADB executable, entered from
// org.mirrorsedge.research.GameActivity (exe_main.cpp is the executable's entry). The library holds generated game code, so it is never
// packaged in the APK: the app loads it from its private files directory.
#include "gpu_bridge.h"
#include <android/log.h>
#include <android/native_window_jni.h>
#include <fcntl.h>
#include <jni.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

int me_runtime_main(int argc, char** argv);
extern "C" void me_runtime_set_paused(bool paused);
extern "C" void me_overlay_stats(uint64_t* swaps, int64_t* max_frame_ns);

namespace {
std::atomic<bool> started{false};
std::atomic<int> exit_code{-1};
ANativeWindow* window = nullptr;  // owned here; touched only from the UI thread

std::string java_string(JNIEnv* env, jstring value) {
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return result;
}
}  // namespace

// nativeStart(workDir, args, env): env entries are "NAME=value"; stdout/stderr go to
// workDir/game.log (the same JSON event log the ADB runs write). Returns false if the
// runtime was already started in this process (it cannot be restarted).
extern "C" JNIEXPORT jboolean JNICALL Java_org_mirrorsedge_research_GameActivity_nativeStart(
    JNIEnv* env, jclass, jstring work_dir, jobjectArray args, jobjectArray variables) {
    if (started.exchange(true)) return JNI_FALSE;
    const std::string dir = java_string(env, work_dir);
    for (jsize i = 0, n = variables ? env->GetArrayLength(variables) : 0; i < n; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(variables, i));
        const std::string pair = java_string(env, item);
        env->DeleteLocalRef(item);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && eq) setenv(pair.substr(0, eq).c_str(), pair.substr(eq + 1).c_str(), 1);
    }
    auto argv_storage = std::make_shared<std::vector<std::string>>();
    argv_storage->push_back("libmeruntime");
    for (jsize i = 0, n = args ? env->GetArrayLength(args) : 0; i < n; ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        argv_storage->push_back(java_string(env, item));
        env->DeleteLocalRef(item);
    }
    if (chdir(dir.c_str())) __android_log_print(ANDROID_LOG_ERROR, "MirrorsEdge", "chdir %s failed", dir.c_str());
    const int log = open((dir + "/game.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (log >= 0) { dup2(log, STDOUT_FILENO); dup2(log, STDERR_FILENO); close(log); }
    __android_log_print(ANDROID_LOG_INFO, "MirrorsEdge", "runtime start in %s (%zu args)", dir.c_str(), argv_storage->size() - 1);
    std::thread([argv_storage] {
        std::vector<char*> argv;
        for (auto& value : *argv_storage) argv.push_back(value.data());
        argv.push_back(nullptr);
        const int code = me_runtime_main(int(argv.size() - 1), argv.data());
        std::fflush(stdout);
        exit_code.store(code);
        __android_log_print(ANDROID_LOG_WARN, "MirrorsEdge", "runtime main returned %d", code);
    }).detach();
    return JNI_TRUE;
}

// -1 while running; otherwise the value main returned (the runtime may also _exit the process).
extern "C" JNIEXPORT jint JNICALL Java_org_mirrorsedge_research_GameActivity_nativeExitCode(JNIEnv*, jclass) {
    return exit_code.load();
}

// surfaceChanged/surfaceDestroyed: hand the Surface to the display thread, or null to
// take it back (waits until the presenter has stopped using the old window).
extern "C" JNIEXPORT void JNICALL Java_org_mirrorsedge_research_GameActivity_nativeSetSurface(
    JNIEnv* env, jclass, jobject surface) {
    ANativeWindow* next = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (next && next == window) { ANativeWindow_release(next); return; }
    me_display_set_window(next);
    if (window) ANativeWindow_release(window);
    window = next;
}

// onStop/onStart: stop the guest and its audio while the game is not visible.
extern "C" JNIEXPORT void JNICALL Java_org_mirrorsedge_research_GameActivity_nativeSetPaused(
    JNIEnv*, jclass, jboolean paused) {
    if (started.load()) me_runtime_set_paused(paused == JNI_TRUE);
}

// Performance overlay: {guest swaps so far, longest frame in ns since the previous call}.
extern "C" JNIEXPORT jlongArray JNICALL Java_org_mirrorsedge_research_GameActivity_nativeFrameStats(JNIEnv* env, jclass) {
    uint64_t swaps = 0;
    int64_t max_frame_ns = 0;
    if (started.load()) me_overlay_stats(&swaps, &max_frame_ns);
    const jlong values[2] = {jlong(swaps), jlong(max_frame_ns)};
    jlongArray result = env->NewLongArray(2);
    if (result) env->SetLongArrayRegion(result, 0, 2, values);
    return result;
}
