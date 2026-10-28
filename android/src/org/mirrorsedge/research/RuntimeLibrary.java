package org.mirrorsedge.research;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.util.zip.ZipFile;

/**
 * Where libmeruntime.so (the recompiled game code) comes from. Public builds never contain
 * it: the user builds it from their own copy and the launcher stages it in the external
 * files directory. Private builds (android/build.ps1 -Runtime) carry it inside the APK as
 * lib/arm64-v8a/libmeruntime.so, loaded from nativeLibraryDir (extractNativeLibs=true since
 * libadrenotools' hook libraries must be extracted there).
 */
final class RuntimeLibrary {
    static final String NAME = "libmeruntime.so";
    private static Boolean bundled;

    /** True when this APK carries the runtime (checked in the APK file, without loading it). */
    static synchronized boolean bundled(Context context) {
        if (bundled == null) {
            boolean found = false;
            try (ZipFile apk = new ZipFile(context.getApplicationInfo().sourceDir)) {
                found = apk.getEntry("lib/arm64-v8a/" + NAME) != null;
            } catch (IOException ignored) {
            }
            bundled = found;
        }
        return bundled;
    }

    /** The staged copy for public builds (external files directory, copied to private storage before dlopen). */
    static File staged(Context context) {
        return new File(context.getExternalFilesDir(null), NAME);
    }
}
