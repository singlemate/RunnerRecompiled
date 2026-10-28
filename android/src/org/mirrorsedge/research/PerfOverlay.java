package org.mirrorsedge.research;

import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;
import android.widget.TextView;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.Locale;
import java.util.function.Supplier;

/**
 * Performance overlay over the game (launcher setting "Мониторинг"), refreshed once a second:
 * game FPS (guest swaps per second) and the longest frame of that second from the runtime,
 * GPU busy % when the kernel exposes it to apps (Adreno kgsl; often hidden by SELinux, then
 * omitted), battery temperature, Android thermal status/headroom and the game's resident
 * memory. Apps cannot read CPU/GPU temperature sensors on most phones.
 */
final class PerfOverlay {
    private static final String[] GPU_BUSY = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/class/kgsl/kgsl-3d0/devfreq/gpu_load",
    };
    private static final String[] THERMAL = {"норм", "лёгкий", "средний", "сильный", "крит.", "авар.", "выкл."};

    private final Context context;
    private final TextView view;
    private final Supplier<long[]> frameStats;  // {swaps so far, longest frame ns since last call}
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable tick = this::tick;
    private String gpuPath;
    private boolean gpuChecked, running;
    private long lastSwaps = -1, lastTime;

    PerfOverlay(Context context, TextView view, Supplier<long[]> frameStats) {
        this.context = context;
        this.view = view;
        this.frameStats = frameStats;
        view.setTypeface(Typeface.MONOSPACE);
        view.setTextSize(12);
        view.setTextColor(Color.WHITE);
        view.setBackgroundColor(0x88000000);
    }

    void start() { if (!running) { running = true; lastSwaps = -1; handler.post(tick); } }
    void stop() { running = false; handler.removeCallbacks(tick); }

    private void tick() {
        if (!running) return;
        long[] stats = frameStats.get();
        long now = System.nanoTime();
        SpannableStringBuilder text = new SpannableStringBuilder();
        if (stats != null && lastSwaps >= 0 && now > lastTime) {
            double fps = (stats[0] - lastSwaps) * 1e9 / (now - lastTime);
            int color = fps >= 28.5 ? 0xFF7CE38B : fps >= 24 ? 0xFFFFD166 : 0xFFFF6B6B;
            append(text, String.format(Locale.US, "FPS %4.1f", fps), color);
            double frameMs = stats[1] / 1e6;
            append(text, String.format(Locale.US, "  кадр≤%3.0f мс", frameMs), frameMs > 50 ? 0xFFFF6B6B : frameMs > 34 ? 0xFFFFD166 : Color.WHITE);
        } else {
            text.append("FPS …");
        }
        if (stats != null) { lastSwaps = stats[0]; lastTime = now; }
        int gpu = gpuBusy();
        if (gpu >= 0) text.append(String.format(Locale.US, "  GPU %3d%%", gpu));
        Intent battery = context.registerReceiver(null, new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        if (battery != null) text.append(String.format(Locale.US, "  бат %.1f°", battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) / 10f));
        PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
        if (power != null && Build.VERSION.SDK_INT >= 29) {
            int status = power.getCurrentThermalStatus();
            String name = status >= 0 && status < THERMAL.length ? THERMAL[status] : String.valueOf(status);
            String thermal = "  нагрев " + name;
            if (Build.VERSION.SDK_INT >= 30) {
                float headroom = power.getThermalHeadroom(0);
                if (!Float.isNaN(headroom)) thermal += String.format(Locale.US, " %.2f", headroom);
            }
            append(text, thermal, status >= 3 ? 0xFFFF6B6B : status >= 1 ? 0xFFFFD166 : Color.WHITE);
        }
        long rss = residentBytes();
        if (rss > 0) text.append(String.format(Locale.US, "  ОЗУ %.1f ГБ", rss / 1073741824.0));
        view.setText(text);
        handler.postDelayed(tick, 1000);
    }

    private static void append(SpannableStringBuilder text, String part, int color) {
        int start = text.length();
        text.append(part);
        text.setSpan(new ForegroundColorSpan(color), start, text.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
    }

    /** Adreno busy percent, or -1 when no readable source exists (checked once). */
    private int gpuBusy() {
        if (!gpuChecked) {
            gpuChecked = true;
            for (String path : GPU_BUSY) if (readFirstInt(path) >= 0) { gpuPath = path; break; }
        }
        return gpuPath == null ? -1 : Math.min(100, readFirstInt(gpuPath));
    }

    private static int readFirstInt(String path) {
        try (BufferedReader in = new BufferedReader(new FileReader(path))) {
            String line = in.readLine();
            if (line == null) return -1;
            String digits = line.trim().split("[^0-9]", 2)[0];
            return digits.isEmpty() ? -1 : Integer.parseInt(digits);
        } catch (Exception e) {
            return -1;
        }
    }

    /** Resident set of this (game) process from /proc/self/statm, in bytes. */
    private static long residentBytes() {
        try (BufferedReader in = new BufferedReader(new FileReader("/proc/self/statm"))) {
            String[] fields = in.readLine().trim().split("\\s+");
            return Long.parseLong(fields[1]) * android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE);
        } catch (Exception e) {
            return -1;
        }
    }
}
