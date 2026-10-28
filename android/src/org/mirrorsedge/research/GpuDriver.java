package org.mirrorsedge.research;

import android.content.Context;
import android.content.SharedPreferences;
import android.net.Uri;
import java.io.*;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONObject;

/**
 * Custom Vulkan driver (Mesa Turnip and the like) chosen by the user, in the AdrenoTools package
 * format used by Turnip builds and other Android ports: a .zip with meta.json ("name",
 * "libraryName", optionally "driverVersion", "description", "minApi") and the driver library.
 * The package is unpacked into the app's internal storage (libadrenotools loads the driver only
 * from there, not from shared storage) and passed to the game as ME_TURNIP=1,
 * ME_GPU_DRIVER_DIR, ME_GPU_DRIVER_LIB. No driver chosen = the device's own Vulkan driver.
 */
final class GpuDriver {
    static final class Info {
        final String name, library, version, folder;
        Info(String name, String library, String version, String folder) {
            this.name = name; this.library = library; this.version = version; this.folder = folder;
        }
        String label() { return version.isEmpty() ? name : name + " · " + version; }
    }

    private static File root(Context c) { return new File(c.getFilesDir(), "gpu_drivers"); }

    /** The driver currently chosen, or null for the device's own driver. */
    static Info current(Context c, SharedPreferences prefs) {
        String folder = prefs.getString("gpuDriver", null);
        if (folder == null) return null;
        File dir = new File(root(c), folder);
        try {
            JSONObject meta = new JSONObject(read(new File(dir, "meta.json")));
            String library = meta.getString("libraryName");
            if (!new File(dir, library).isFile()) return null;
            return new Info(meta.optString("name", folder), library, meta.optString("driverVersion", ""), folder);
        } catch (Exception e) {
            return null;
        }
    }

    /** Environment for the game process (leading space), empty for the device's own driver. */
    static String env(Context c, SharedPreferences prefs) {
        Info info = current(c, prefs);
        if (info == null) return "";
        String env = " ME_TURNIP=1 ME_GPU_DRIVER_DIR=" + new File(root(c), info.folder).getAbsolutePath() + "/ ME_GPU_DRIVER_LIB=" + info.library;
        // Mesa Turnip without tiled (GMEM) rendering: needed by some Adreno 8xx builds.
        if (prefs.getBoolean("gpuDriverSysmem", false)) env += " TU_DEBUG=sysmem";
        return env;
    }

    static void useSystem(SharedPreferences prefs) { prefs.edit().remove("gpuDriver").apply(); }

    /** Unpacks and selects a driver package; returns its description. Throws with a user message. */
    static Info install(Context c, SharedPreferences prefs, Uri uri) throws IOException {
        File staging = new File(root(c), ".import");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("нет доступа к памяти приложения");
        try (InputStream raw = c.getContentResolver().openInputStream(uri)) {
            if (raw == null) throw new IOException("файл не открывается");
            try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(raw))) {
                ZipEntry e;
                long total = 0;
                while ((e = zip.getNextEntry()) != null) {
                    if (e.isDirectory()) continue;
                    String name = new File(e.getName()).getName();  // flatten, no path traversal
                    if (name.isEmpty() || name.startsWith(".")) continue;
                    File out = new File(staging, name);
                    try (OutputStream o = new FileOutputStream(out)) {
                        byte[] buf = new byte[1 << 16];
                        int n;
                        while ((n = zip.read(buf)) > 0) {
                            total += n;
                            if (total > (256L << 20)) throw new IOException("архив слишком большой для драйвера");
                            o.write(buf, 0, n);
                        }
                    }
                }
            }
        }
        File metaFile = new File(staging, "meta.json");
        if (!metaFile.isFile()) throw new IOException("в архиве нет meta.json — это не пакет драйвера AdrenoTools");
        JSONObject meta;
        try { meta = new JSONObject(read(metaFile)); } catch (Exception ex) { throw new IOException("meta.json не читается"); }
        String library = meta.optString("libraryName", "");
        File lib = new File(staging, new File(library).getName());
        if (library.isEmpty() || !lib.isFile()) throw new IOException("в архиве нет библиотеки драйвера (" + library + ")");
        if (!isArm64Elf(lib)) throw new IOException("библиотека драйвера не для 64-битного ARM");
        int minApi = meta.optInt("minApi", 0);
        if (minApi > android.os.Build.VERSION.SDK_INT) throw new IOException("драйверу нужен Android API " + minApi);
        String name = meta.optString("name", "Драйвер");
        String folder = name.replaceAll("[^A-Za-z0-9._-]+", "_").toLowerCase(Locale.ROOT);
        if (folder.isEmpty()) folder = "driver";
        File dir = new File(root(c), folder);
        deleteTree(dir);
        if (!staging.renameTo(dir)) throw new IOException("не удалось сохранить драйвер");
        prefs.edit().putString("gpuDriver", folder).apply();
        return new Info(name, lib.getName(), meta.optString("driverVersion", ""), folder);
    }

    private static boolean isArm64Elf(File f) throws IOException {
        byte[] h = new byte[20];
        try (FileInputStream in = new FileInputStream(f)) { if (in.read(h) != h.length) return false; }
        // \x7fELF, 64-bit (EI_CLASS 2), little endian, e_machine 183 (EM_AARCH64).
        return h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F' && h[4] == 2 && h[5] == 1
            && (h[18] & 0xff) == 183 && h[19] == 0;
    }

    private static String read(File f) throws IOException {
        try (FileInputStream in = new FileInputStream(f)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toString("UTF-8");
        }
    }

    private static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File x : list) deleteTree(x);
        f.delete();
    }
}
