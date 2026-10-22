#include <jni.h>
/* Independent arithmetic probe; no game executable or assets are embedded. */
JNIEXPORT jint JNICALL Java_org_mirrorsedge_research_MainActivity_nativeProbe(
    JNIEnv *env, jclass type, jint a, jint b) {
    (void)env; (void)type;
    return (jint)((unsigned int)a + 23u * (unsigned int)b);
}
