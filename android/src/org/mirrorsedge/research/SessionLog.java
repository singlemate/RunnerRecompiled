package org.mirrorsedge.research;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.Uri;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Test session record for sending to the developer: session.csv gets a row every 5 s
 * (elapsed seconds, battery temperature, Android thermal status and headroom), next to the
 * runtime's game.log (frame_rates with fps, max_frame_ms and scene). Apps cannot read the
 * CPU/GPU sensors on most phones; the thermal status shows when the system starts
 * throttling. bundle() zips summary + session.csv + game.log (and the previous session's
 * files, kept when the next session starts, so a crash can still be reported) and share()
 * hands the zip to the system share sheet (Telegram etc.) through LogsProvider.
 * Contents: device model, Android version, settings and game events only.
 */
final class SessionLog {
    private final Context context;
    private final File work;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final long startMs = System.currentTimeMillis();
    private Writer csv;
    private boolean running;

    SessionLog(Context context, File work) {
        this.context = context;
        this.work = work;
    }

    /** Keeps the previous session's files (game.prev.log, session.prev.csv) and starts a new record. */
    void start(String settings) {
        rotate("game.log", "game.prev.log");
        rotate("session.csv", "session.prev.csv");
        rotate("session-info.txt", "session-info.prev.txt");
        try (Writer info = new OutputStreamWriter(new FileOutputStream(new File(work, "session-info.txt")), "UTF-8")) {
            info.write("started " + stamp(startMs) + "\nsettings " + settings + "\n");
        } catch (IOException ignored) {
        }
        try {
            csv = new OutputStreamWriter(new FileOutputStream(new File(work, "session.csv")), "UTF-8");
            csv.write("t_s,battery_c,thermal_status,thermal_headroom\n");
        } catch (IOException e) {
            csv = null;
        }
        running = true;
        tick();
    }

    void stop() {
        running = false;
        handler.removeCallbacksAndMessages(null);
        try { if (csv != null) csv.flush(); } catch (IOException ignored) {}
    }

    private void tick() {
        if (!running || csv == null) return;
        try {
            Intent battery = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            float batteryC = battery == null ? -1 : battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, -10) / 10f;
            PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
            int status = Build.VERSION.SDK_INT >= 29 ? power.getCurrentThermalStatus() : -1;
            float headroom = Build.VERSION.SDK_INT >= 30 ? power.getThermalHeadroom(10) : -1;
            long t = (System.currentTimeMillis() - startMs) / 1000;
            csv.write(String.format(Locale.US, "%d,%.1f,%d,%.3f\n", t, batteryC, status, headroom));
            csv.flush();
        } catch (IOException | RuntimeException ignored) {
        }
        handler.postDelayed(this::tick, 5000);
    }

    private void rotate(String name, String previous) {
        File current = new File(work, name), old = new File(work, previous);
        if (current.isFile() && current.length() > 0) { old.delete(); current.renameTo(old); }
    }

    private static String stamp(long ms) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(new Date(ms));
    }

    /** Builds the zip in the app cache (shared via LogsProvider). */
    static File bundle(Context context, File work) throws IOException {
        File dir = new File(context.getCacheDir(), "logs");
        dir.mkdirs();
        File[] old = dir.listFiles();
        if (old != null) for (File f : old) f.delete();
        String name = "runner-logs-" + Build.MODEL.replaceAll("[^A-Za-z0-9]", "") + "-"
            + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".zip";
        File zip = new File(dir, name);
        try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(zip))) {
            out.putNextEntry(new ZipEntry("summary.txt"));
            String summary = "device " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ")\n"
                + "soc " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MANUFACTURER + " " + Build.SOC_MODEL : Build.HARDWARE) + "\n"
                + "android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + ")\n"
                + "build " + Build.DISPLAY + "\n"
                + "created " + stamp(System.currentTimeMillis()) + "\n";
            out.write(summary.getBytes("UTF-8"));
            out.closeEntry();
            for (String file : new String[] {"session-info.txt", "session.csv", "game.log",
                                             "session-info.prev.txt", "session.prev.csv", "game.prev.log"}) {
                File f = new File(work, file);
                if (!f.isFile()) continue;
                out.putNextEntry(new ZipEntry(file));
                try (InputStream in = new FileInputStream(f)) {
                    byte[] buffer = new byte[1 << 16];
                    for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
                }
                out.closeEntry();
            }
        }
        return zip;
    }

    /**
     * Copy of the zip in Download/RunnerRecompiled (MediaStore, no permission needed, Android 10+),
     * so the logs can be found and sent by hand when sharing fails. Returns the shown path or null.
     */
    static String saveToDownloads(Context context, File zip) {
        if (Build.VERSION.SDK_INT < 29) return null;
        android.content.ContentValues values = new android.content.ContentValues();
        values.put(android.provider.MediaStore.MediaColumns.DISPLAY_NAME, zip.getName());
        values.put(android.provider.MediaStore.MediaColumns.MIME_TYPE, "application/zip");
        values.put(android.provider.MediaStore.MediaColumns.RELATIVE_PATH, "Download/RunnerRecompiled");
        android.content.ContentResolver resolver = context.getContentResolver();
        Uri item = resolver.insert(android.provider.MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
        if (item == null) return null;
        try (InputStream in = new FileInputStream(zip); OutputStream out = resolver.openOutputStream(item)) {
            if (out == null) throw new IOException("no stream");
            byte[] buffer = new byte[1 << 16];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        } catch (IOException e) {
            resolver.delete(item, null, null);
            return null;
        }
        return "Загрузки/RunnerRecompiled/" + zip.getName();
    }

    /** Opens the system share sheet with the zip (provider of the calling process). */
    static void share(Context context, File zip) { share(context, zip, LogsProvider.AUTHORITY); }
    static void share(Context context, File zip, String authority) {
        Uri uri = Uri.parse("content://" + authority + "/" + zip.getName());
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("application/zip");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, zip.getName());
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        Intent chooser = Intent.createChooser(send, "Отправить логи");
        chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        context.startActivity(chooser);
    }
}
