package org.mirrorsedge.research;

import android.app.Activity;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.os.Bundle;
import android.util.Log;
import android.util.SparseIntArray;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The PC version of the game (statically recompiled from the GOG PC release) on Android:
 * libmepc.so with the runtime's Android host and DXVK Native (libdxvk_d3d9.so) for Direct3D 9.
 * Private builds only carry those two libraries (android/build.ps1 -PcRuntime -PcDxvk); the game
 * files are the user's own copy on the device, never in the APK:
 *   files/pc/MirrorsEdge   the GOG install (Binaries, Engine, TdGame)
 *   files/pc/deps          msvcr80.dll, msvcp80.dll, rebased/ (the DLLs the build lifted)
 *   files/pc/user          the guest's writable files (configs, saves)
 * Input: keyboard keys as Windows virtual keys, the pointer as a mouse, pads as XInput. Without a
 * pad or keyboard the on-screen controller (TouchPad) is shown and is XInput pad 0.
 * Settings come from PcLauncherActivity; with the intent extra "env" (adb tests, e.g.
 * "ME_PC_FOV=100") they are left out.
 */
public class PcGameActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "me_pc";
    private static native boolean nativeStart(String library, String[] args, String[] env);
    private static native int nativeExitCode();
    private static native void nativeSetSurface(android.view.Surface surface);
    private static native void nativeSetForeground(boolean front);
    private static native void nativeKey(int vk, int scan, boolean extended, boolean down);
    private static native void nativeMouseMove(int x, int y);
    private static native void nativeTurn(int degrees);
    private static native int nativeRumble(int index);
    private static native void nativeSetFov(int degrees);
    private static native int nativeCpuMask(int cpus);
    private static native void nativeAudioRoute(int mainDevice, int padDevice);

    private final android.media.AudioDeviceCallback audioDevices = new android.media.AudioDeviceCallback() {
        @Override public void onAudioDevicesAdded(android.media.AudioDeviceInfo[] added) { updateAudioRoute(); }
        @Override public void onAudioDevicesRemoved(android.media.AudioDeviceInfo[] removed) { updateAudioRoute(); }
    };

    /** A DualSense or DualShock on USB is an audio device: the radio voice goes to its speaker, the rest stays here. */
    private void updateAudioRoute() {
        if (!started) return;
        android.media.AudioManager am = (android.media.AudioManager) getSystemService(AUDIO_SERVICE);
        int pad = 0, speaker = 0, phones = 0;
        for (android.media.AudioDeviceInfo d : am.getDevices(android.media.AudioManager.GET_DEVICES_OUTPUTS)) {
            switch (d.getType()) {
                case android.media.AudioDeviceInfo.TYPE_USB_DEVICE:
                case android.media.AudioDeviceInfo.TYPE_USB_HEADSET:
                    if (String.valueOf(d.getProductName()).matches("(?i).*(dualsense|dualshock|wireless controller).*")) pad = d.getId();
                    break;
                case android.media.AudioDeviceInfo.TYPE_BUILTIN_SPEAKER: speaker = d.getId(); break;
                case android.media.AudioDeviceInfo.TYPE_BLUETOOTH_A2DP:
                case android.media.AudioDeviceInfo.TYPE_WIRED_HEADPHONES:
                case android.media.AudioDeviceInfo.TYPE_WIRED_HEADSET: phones = d.getId(); break;
                default: break;
            }
        }
        if (!getSharedPreferences("controllers", MODE_PRIVATE).getBoolean("padSpeaker", false)) pad = 0;  // off: Android offers no output on a DualSense (CHECKLIST 108)
        Log.i(TAG, "audio route: pad " + pad + " speaker " + speaker + " phones " + phones);
        nativeAudioRoute(pad != 0 ? (phones != 0 ? phones : speaker) : 0, pad);
    }

    /**
     * The CPUs to run on: without the fastest cluster when at least four cores of the next one remain
     * (Snapdragon 8 Elite: 6 of 3.53 GHz, without the 2 of 4.32 GHz). There the game kept 62 fps and drew
     * about 2.5 W less when hot. 0 leaves everything as it is.
     */
    private static int economyMask() {
        int[] freq = new int[16];
        int top = 0;
        for (int i = 0; i < freq.length; i++) {
            try (java.io.BufferedReader r = new java.io.BufferedReader(new java.io.FileReader("/sys/devices/system/cpu/cpu" + i + "/cpufreq/cpuinfo_max_freq"))) {
                freq[i] = Integer.parseInt(r.readLine().trim());
                top = Math.max(top, freq[i]);
            } catch (Exception e) {
                freq[i] = 0;
            }
        }
        int next = 0, mask = 0, nextCount = 0;
        for (int f : freq) if (f < top) next = Math.max(next, f);
        for (int i = 0; i < freq.length; i++) {
            if (freq[i] == next && next > 0) nextCount++;
            if (freq[i] > 0 && freq[i] < top) mask |= 1 << i;
        }
        return nextCount >= 4 ? mask : 0;
    }

    /** Diagnostics, env ME_PC_AB=delay/phase/ff,3f (seconds, hex CPU masks): alternates the masks and logs battery power. */
    private void powerLog(String spec) {
        String[] part = spec.split("/");
        int delay = Integer.parseInt(part[0]), phase = Integer.parseInt(part[1]);
        String[] masks = part[2].split(",");
        android.os.BatteryManager battery = (android.os.BatteryManager) getSystemService(BATTERY_SERVICE);
        new Thread(() -> {
            try {
                Thread.sleep(delay * 1000L);
                for (int s = 0; ; s++) {
                    String mask = masks[(s / phase) % masks.length];
                    if (s % phase == 0) {
                        String[] t = mask.split("_");
                        Log.i(TAG, "ab threads=" + nativeCpuMask(Integer.parseInt(t[0], 16)));
                        if (t.length > 1 && android.os.Build.VERSION.SDK_INT >= 30)
                            view.getHolder().getSurface().setFrameRate(Float.parseFloat(t[1]), android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                    }
                    int mv = registerReceiver(null, new android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED)).getIntExtra(android.os.BatteryManager.EXTRA_VOLTAGE, 0);
                    Log.i(TAG, "ab mask=" + mask + " phase=" + (s / phase) + " ua=" + battery.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CURRENT_NOW) + " mv=" + mv);
                    Thread.sleep(1000);
                }
            } catch (InterruptedException ignored) { }
        }, "power-log").start();
    }
    private static native void nativeMouseButton(int button, boolean down, int x, int y);
    private static native void nativePad(int index, boolean connected, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);

    private static boolean started;
    private TextView status;
    private SurfaceView view;
    private final SparseIntArray padIndex = new SparseIntArray();
    private final int[] padButtons = new int[4];
    private final int[][] padAxes = new int[4][6];  // lt, rt, lx, ly, rx, ry
    private final android.util.SparseArray<Controller> pads = new android.util.SparseArray<>();
    private android.content.SharedPreferences padPrefs;
    // On-screen controller: TouchPad writes into a GuestInput that is never started (no loopback
    // sender); the state is polled every 16 ms and merged into pad 0.
    private final GuestInput touchInput = new GuestInput();
    private TouchPad touchPad;
    private static int touchMode = -1;  // ME_PC_TOUCH: -1 auto, 1 always, 0 never
    private int[] touchState = new int[7];  // buttons, lt, rt, lx, ly, rx, ry
    private int holdBits, holdLt, holdRt;
    private long holdUntil;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable touchPoll = new Runnable() {
        public void run() {
            int[] s = touchInput.takeTouch();
            // A quick tap is held for at least 100 ms: the game reads XInput once a frame (30 ms at
            // 33 fps on the tablet), and a press seen by one 16 ms poll only was missed.
            long now = android.os.SystemClock.uptimeMillis();
            if (now >= holdUntil) holdBits = holdLt = holdRt = 0;
            int fresh = s[0] & ~touchState[0];
            if (fresh != 0 || (s[1] > 0 && touchState[1] == 0) || (s[2] > 0 && touchState[2] == 0)) {
                holdBits |= fresh;
                if (s[1] > 0) holdLt = 255;
                if (s[2] > 0) holdRt = 255;
                holdUntil = now + 100;
            }
            s[0] |= holdBits;
            s[1] = Math.max(s[1], holdLt);
            s[2] = Math.max(s[2], holdRt);
            if (started && !java.util.Arrays.equals(s, touchState)) { touchState = s; sendPad(0); }
            if (touchPad.getVisibility() == View.VISIBLE) handler.postDelayed(this, 16);
        }
    };
    private final int[] rumbleLast = new int[4];
    private final long[] rumbleSent = new long[4];
    /** The game sets motor levels and leaves them on; a one-shot is renewed while they stay non-zero. */
    private final Runnable rumblePoll = new Runnable() {
        public void run() {
            long now = android.os.SystemClock.uptimeMillis();
            for (int k = 0; started && k < padIndex.size(); k++) {
                int i = padIndex.valueAt(k), level = nativeRumble(i);
                if (level == rumbleLast[i] && (level == 0 || now - rumbleSent[i] < 600)) continue;
                rumbleLast[i] = level;
                rumbleSent[i] = now;
                applyRumble(padIndex.keyAt(k), level);
            }
            handler.postDelayed(this, 33);
        }
    };

    private static int amplitude(int level) { return Math.max(1, Math.min(255, level * 255 / 65535)); }

    private void applyRumble(int deviceId, int level) {
        InputDevice d = InputDevice.getDevice(deviceId);
        if (d == null) return;
        int left = level >>> 16, right = level & 0xFFFF;
        try {
            if (android.os.Build.VERSION.SDK_INT >= 31) {
                android.os.VibratorManager m = d.getVibratorManager();
                int[] ids = m.getVibratorIds();
                if (ids.length == 0) return;
                if (level == 0) { m.cancel(); return; }
                android.os.CombinedVibration.ParallelCombination all = android.os.CombinedVibration.startParallel();
                boolean any = false;
                for (int k = 0; k < ids.length; k++) {
                    int value = ids.length == 1 ? Math.max(left, right) : k == 0 ? left : k == 1 ? right : 0;
                    if (value == 0) continue;
                    all.addVibrator(ids[k], android.os.VibrationEffect.createOneShot(1000, amplitude(value)));
                    any = true;
                }
                if (any) m.vibrate(all.combine()); else m.cancel();
            } else {
                android.os.Vibrator v = d.getVibrator();
                if (!v.hasVibrator()) return;
                if (level == 0) v.cancel();
                else v.vibrate(android.os.VibrationEffect.createOneShot(1000, amplitude(Math.max(left, right))));
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "rumble: " + e);
        }
    }

    private final InputManager.InputDeviceListener inputListener = new InputManager.InputDeviceListener() {
        public void onInputDeviceAdded(int id) { updateDevices(); }
        public void onInputDeviceChanged(int id) { updateDevices(); }
        public void onInputDeviceRemoved(int id) { releasePad(id); updateDevices(); }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        FrameLayout root = new FrameLayout(this);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        root.addView(view);
        touchPad = new TouchPad(this, touchInput);
        touchPad.enableTurn(PcGameActivity::nativeTurn);
        touchPad.setVisibility(View.GONE);  // updateDevices() shows it when no pad or keyboard is connected
        root.addView(touchPad, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this);
        status.setTextColor(0xFFFFFFFF);
        status.setBackgroundColor(0xA0000000);
        status.setPadding(24, 24, 24, 24);
        status.setVisibility(View.GONE);
        root.addView(status, new FrameLayout.LayoutParams(-2, -2));
        setContentView(root);
        hideSystemBars();
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        try {
            System.loadLibrary("mepcjni");
        } catch (Throwable e) {
            message(Tr.t("Нет libmepcjni.so: ") + e);
            return;
        }
        start();
    }

    // Share of the screen's pixels on each side: the same steps as the modes of the Android WSI
    // (pc/dxvk), rounded down to 8 (the game's buffers are aligned to 8; other sizes draw no world).
    private static final int[][] SCALES = {{2, 3}, {3, 4}, {1, 1}, {1, 2}};

    private File pcDir() { return new File(getExternalFilesDir(null), "pc"); }

    /** This screen at the chosen scale, in landscape; the swapchain modes are aligned to 8. */
    static int[] scaledSize(android.app.Activity a, int scale) {
        android.graphics.Point real = new android.graphics.Point();
        a.getWindowManager().getDefaultDisplay().getRealSize(real);
        int[] sc = SCALES[scale];
        return new int[] {(Math.max(real.x, real.y) * sc[0] / sc[1]) & ~7, (Math.min(real.x, real.y) * sc[0] / sc[1]) & ~7};
    }

    private static void logSystemSettings(File ini) {
        if (!ini.isFile()) return;
        try {
            byte[] raw = Files.readAllBytes(ini.toPath());
            boolean utf16 = raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xFF) == 0xFE;
            String text = new String(raw, utf16 ? 2 : 0, raw.length - (utf16 ? 2 : 0), utf16 ? StandardCharsets.UTF_16LE : StandardCharsets.UTF_8);
            int a = text.indexOf("[SystemSettings]");
            if (a < 0) return;
            int b = text.indexOf("\n[", a + 1);
            for (String line : text.substring(a, b < 0 ? text.length() : b).split("\\r?\\n")) Log.i(TAG, "ini " + line);
        } catch (java.io.IOException e) {
            Log.w(TAG, "ini " + ini + ": " + e);
        }
    }

    /** Sets key=value in one section of a UE3 ini (ASCII/UTF-8, or UTF-16LE with a BOM). */
    /** The game's own settings file, under the app's user folder. */
    static File systemIni(File user) {
        return new File(user, "other/C/Documents and Settings/Runner/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini");
    }

    static void setIni(File ini, String section, String key, String value) {
        try {
            byte[] raw = Files.readAllBytes(ini.toPath());
            boolean utf16 = raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xFF) == 0xFE;
            Charset cs = utf16 ? StandardCharsets.UTF_16LE : StandardCharsets.UTF_8;
            String text = new String(raw, utf16 ? 2 : 0, raw.length - (utf16 ? 2 : 0), cs);
            String nl = text.contains("\r\n") ? "\r\n" : "\n";
            List<String> lines = new ArrayList<>(java.util.Arrays.asList(text.split("\\r?\\n", -1)));
            int start = -1, end = lines.size();
            for (int i = 0; i < lines.size(); i++) {
                String l = lines.get(i).trim();
                if (start < 0 && l.equalsIgnoreCase("[" + section + "]")) start = i;
                else if (start >= 0 && l.startsWith("[")) { end = i; break; }
            }
            if (start < 0) return;  // no such section: leave the file alone
            boolean set = false;
            for (int i = start + 1; i < end; i++)
                if (lines.get(i).regionMatches(true, 0, key + "=", 0, key.length() + 1)) { lines.set(i, key + "=" + value); set = true; }
            if (!set) lines.add(start + 1, key + "=" + value);
            byte[] body = String.join(nl, lines).getBytes(cs);
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            if (utf16) { out.write(0xFF); out.write(0xFE); }
            out.write(body, 0, body.length);
            Files.write(ini.toPath(), out.toByteArray());
        } catch (java.io.IOException e) {
            Log.w(TAG, "ini " + ini + ": " + e);
        }
    }

    /**
     * First run only: the game writes its settings within seconds, with its lowest preset, and keeps those
     * in memory. Once the file is there the preset goes into it and the game starts again to read it.
     */
    private void watchFirstRun(File ini, File pc, int resX, int resY) {
        Thread t = new Thread(() -> {
            try {
                for (int i = 0; i < 600; i++) {
                    Thread.sleep(100);
                    if (!ini.isFile()) continue;
                    String text = new String(Files.readAllBytes(ini.toPath()), StandardCharsets.ISO_8859_1).replace("\0", "");
                    if (!text.contains("CompatLevelComposite") || !text.contains("ResX=")) continue;
                    Thread.sleep(1000);  // the game may still be finishing this write
                    for (String item : PcSetup.firstRunPreset(pc, resX, resY)) {
                        String[] kv = item.split("=", 2);
                        setIni(ini, "SystemSettings", kv[0], kv[1]);
                    }
                    startActivity(new android.content.Intent(this, PcGameActivity.class)
                        .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK | android.content.Intent.FLAG_ACTIVITY_CLEAR_TASK));
                    Runtime.getRuntime().exit(0);
                }
            } catch (Exception e) {
                Log.w(TAG, "first-run preset: " + e);
            }
        }, "first-run-preset");
        t.setDaemon(true);
        t.start();
    }

    /** Why the last run's DXVK failed, in logcat: its log file cannot be read without the app on most phones. */
    private static void logPreviousDxvk(File log) {
        if (!log.isFile()) return;
        try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(new java.io.FileInputStream(log), StandardCharsets.ISO_8859_1))) {
            int read = 0, shown = 0;
            for (String line; (line = in.readLine()) != null && read < 3000 && shown < 60; read++)
                if (line.matches(".*(Skipping|No adapters|Found device|err:|Failed|not supported|capable).*")) {
                    Log.i(TAG, "previous dxvk: " + line);
                    shown++;
                }
        } catch (java.io.IOException e) {
            Log.w(TAG, "previous dxvk: " + e);
        }
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private void start() {
        if (started) {
            // Back to a game that is already running: its process keeps the settings it started with,
            // the field of view is the one the launcher can change on the fly.
            int fov = getIntent().getIntExtra("fov", 0);
            if (fov > 0) nativeSetFov(fov);
            return;
        }
        File pc = pcDir();
        logPreviousDxvk(new File(pc, "app_process64_d3d9.log"));  // before the new run truncates it
        File game = new File(pc, "MirrorsEdge"), deps = new File(pc, "deps"), user = new File(pc, "user");
        File library = new File(getApplicationInfo().nativeLibraryDir, "libmepc.so");
        if (!library.isFile()) { message(Tr.t("Это APK без ПК-версии (нет libmepc.so).")); return; }
        if (!new File(game, "Binaries").isDirectory()) { message(Tr.t("Нет копии игры: ") + game + "/Binaries"); return; }
        if (!new File(deps, "msvcr80.dll").isFile()) { message(Tr.t("Нет библиотек: ") + deps); return; }
        user.mkdirs();
        List<String> env = new ArrayList<>();
        env.add("ME_PC_LOG_PATH=" + new File(pc, "pc-runtime.log"));
        env.add("ME_PC_ACH_PATH=" + new File(pc, "achievements.txt"));
        if (!getIntent().hasExtra("env")) {
            android.content.SharedPreferences prefs = getSharedPreferences("pc", MODE_PRIVATE);
            File ini = new File(user, "other/C/Documents and Settings/Runner/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini");
            if (!ini.isFile()) {
                // First run: the game writes this file itself; the runtime changes these keys in it.
                android.graphics.Point real = new android.graphics.Point();
                getWindowManager().getDefaultDisplay().getRealSize(real);
                int scale = prefs.getInt("scale", 0);
                int[] sc = SCALES[scale];
                int w = Math.max(real.x, real.y), h = Math.min(real.x, real.y);
                env.add("ME_PC_INI_SET=Engine.Engine|Language=" + prefs.getString("lang", PcLauncherActivity.defaultLanguage())
                    + ";Engine.GameEngine|MaxSmoothedFrameRate=" + prefs.getInt("fps", 62)
                    + ";SystemSettings|ResX=" + ((w * sc[0] / sc[1]) & ~7) + ";SystemSettings|ResY=" + ((h * sc[0] / sc[1]) & ~7));
                final int presetX = (w * sc[0] / sc[1]) & ~7, presetY = (h * sc[0] / sc[1]) & ~7;
                watchFirstRun(ini, pc, presetX, presetY);
            } else {
                setIni(ini, "Engine.Engine", "Language", prefs.getString("lang", PcLauncherActivity.defaultLanguage()));
                setIni(ini, "Engine.GameEngine", "MaxSmoothedFrameRate", String.valueOf(prefs.getInt("fps", 62)));
                // Only when the choice here changed: the game's own Options > Video keeps its resolution.
                int scale = prefs.getInt("scale", 0);
                if (prefs.getInt("scaleApplied", -1) != scale) {
                    android.graphics.Point real = new android.graphics.Point();
                    getWindowManager().getDefaultDisplay().getRealSize(real);
                    int[] sc = SCALES[scale];
                    int w = Math.max(real.x, real.y), h = Math.min(real.x, real.y);
                    setIni(ini, "SystemSettings", "ResX", String.valueOf((w * sc[0] / sc[1]) & ~7));
                    setIni(ini, "SystemSettings", "ResY", String.valueOf((h * sc[0] / sc[1]) & ~7));
                    prefs.edit().putInt("scaleApplied", scale).commit();
                }
            }
            if (prefs.getInt("fov", 0) > 0) env.add("ME_PC_FOV=" + prefs.getInt("fov", 0));
            env.add("ME_PC_CPUS=" + prefs.getInt("cpus", 4));
            env.add("ME_PC_FILL_SCREEN=1");
            int upscaler = prefs.getInt("upscaler", 0);
            if (upscaler > 0) env.add("ME_PC_UPSCALER=" + (upscaler == 1 ? "fsr" : "sgsr"));
            GpuDriver.Info gpu = GpuDriver.current(this, prefs);
            env.add("ME_PC_PIPELINE_CACHE=" + new File(pc, "pipeline-" + (gpu == null ? "system" : gpu.folder) + ".cache"));
            String driver = GpuDriver.env(this, prefs).trim();
            if (!driver.isEmpty()) {
                env.addAll(java.util.Arrays.asList(driver.split(" ")));
                env.add("ME_NATIVE_LIB_DIR=" + getApplicationInfo().nativeLibraryDir);
            }
            if (prefs.getInt("intro", 0) == 1) env.add("ME_PC_NO_INTRO=1");
            // Button strings are read at start: the pad connected now decides them.
            for (int id : InputDevice.getDeviceIds()) {
                InputDevice d = InputDevice.getDevice(id);
                if (d == null || d.isVirtual() || !Controller.isGamepad(d)) continue;
                Controller c = Controller.of(d, getSharedPreferences("controllers", MODE_PRIVATE));
                if (c.kind == Controller.Kind.PLAYSTATION) { env.add("ME_PC_PAD_GLYPHS=ps"); break; }
                // Swapped by label, the Xbox letters already match the buttons.
                if (c.kind == Controller.Kind.NINTENDO) { if (!c.swapFace) env.add("ME_PC_PAD_GLYPHS=nintendo"); break; }
            }
            int touch = prefs.getInt("touch", 0);
            if (touch > 0) env.add("ME_PC_TOUCH=" + (touch == 1 ? 1 : 0));
        }
        // adb tests: intent extra "ini" sets [SystemSettings] keys, e.g. "Bloom=False,MotionBlur=False";
        // the section is logged either way (the file belongs to the app, adb cannot read it).
        File systemIni = systemIni(user);
        String iniExtra = getIntent().getStringExtra("ini");
        if (iniExtra != null && systemIni.isFile())
            for (String item : iniExtra.split(",(?![^()]*\\))")) {  // commas inside (...) belong to the value
                String[] kv = item.split("=", 2);
                if (kv.length == 2) setIni(systemIni, "SystemSettings", kv[0], kv[1]);
            }
        logSystemSettings(systemIni);
        String extra = getIntent().getStringExtra("env");
        if (extra != null) for (String item : extra.trim().split("\\s+")) if (item.contains("=")) env.add(item);
        for (String item : env) if (item.startsWith("ME_PC_TOUCH=")) touchMode = item.endsWith("=1") ? 1 : 0;
        for (String item : env) if (item.startsWith("ME_PC_AB=")) powerLog(item.substring(9));
        String[] args = {new File(game, "Binaries").getPath(), deps.getPath(), user.getPath()};
        started = nativeStart(library.getPath(), args, env.toArray(new String[0]));
        updateAudioRoute();
        int cpus = started ? economyMask() : 0;
        if (cpus != 0) Log.i(TAG, "cpu mask " + Integer.toHexString(cpus) + ", threads " + nativeCpuMask(cpus));
        if (!started) message(Tr.t("Не удалось запустить libmepc.so (см. logcat, тег me_pc_jni)"));
        Log.i(TAG, "started " + started + " with " + env);
    }

    private void message(String text) {
        runOnUiThread(() -> { status.setText(text); status.setVisibility(View.VISIBLE); });
    }

    @Override protected void onResume() {
        super.onResume();
        hideSystemBars();
        ((InputManager) getSystemService(INPUT_SERVICE)).registerInputDeviceListener(inputListener, null);
        ((android.media.AudioManager) getSystemService(AUDIO_SERVICE)).registerAudioDeviceCallback(audioDevices, handler);
        handler.removeCallbacks(rumblePoll);
        handler.post(rumblePoll);
        updateDevices();
        if (started) nativeSetForeground(true);
    }
    @Override protected void onPause() {
        super.onPause();
        ((InputManager) getSystemService(INPUT_SERVICE)).unregisterInputDeviceListener(inputListener);
        ((android.media.AudioManager) getSystemService(AUDIO_SERVICE)).unregisterAudioDeviceCallback(audioDevices);
        handler.removeCallbacks(rumblePoll);
        for (int k = 0; k < padIndex.size(); k++) { rumbleLast[padIndex.valueAt(k)] = 0; applyRumble(padIndex.keyAt(k), 0); }
        handler.removeCallbacks(touchPoll);
        if (touchPad.getVisibility() == View.VISIBLE) touchPad.release();
        if (started) nativeSetForeground(false);
    }

    /** The on-screen controller only while no gamepad or keyboard is connected (or env ME_PC_TOUCH=0/1). */
    private void updateDevices() {
        boolean external = false;
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            external |= fromPad(d) || d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
        }
        if (touchOverride) external = false;  // tapped the screen: see onTouchEvent
        if (touchMode == 1) external = false;
        if (touchMode == 0) external = true;
        boolean show = started && !external;
        if (!show && touchPad.getVisibility() == View.VISIBLE) touchPad.release();
        touchPad.setVisibility(show ? View.VISIBLE : View.GONE);
        handler.removeCallbacks(touchPoll);
        if (show) handler.post(touchPoll);
        else if (started) { touchState = new int[7]; sendPad(0); }
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { nativeSetSurfaceSafe(holder.getSurface()); }
    @Override public void surfaceDestroyed(SurfaceHolder holder) { nativeSetSurfaceSafe(null); }

    private void nativeSetSurfaceSafe(android.view.Surface s) {
        try { nativeSetSurface(s); } catch (UnsatisfiedLinkError ignored) { }
    }

    // ---- keyboard: Android key codes -> Windows virtual key and scan code (set 1) ----
    private static final int[][] KEYS = {
        // android keycode, vk, scan, extended
        {KeyEvent.KEYCODE_SPACE, 0x20, 0x39, 0}, {KeyEvent.KEYCODE_ENTER, 0x0D, 0x1C, 0},
        {KeyEvent.KEYCODE_ESCAPE, 0x1B, 0x01, 0}, {KeyEvent.KEYCODE_TAB, 0x09, 0x0F, 0},
        {KeyEvent.KEYCODE_DEL, 0x08, 0x0E, 0}, {KeyEvent.KEYCODE_SHIFT_LEFT, 0x10, 0x2A, 0},
        {KeyEvent.KEYCODE_SHIFT_RIGHT, 0x10, 0x36, 0}, {KeyEvent.KEYCODE_CTRL_LEFT, 0x11, 0x1D, 0},
        {KeyEvent.KEYCODE_CTRL_RIGHT, 0x11, 0x1D, 1}, {KeyEvent.KEYCODE_ALT_LEFT, 0x12, 0x38, 0},
        {KeyEvent.KEYCODE_ALT_RIGHT, 0x12, 0x38, 1}, {KeyEvent.KEYCODE_DPAD_UP, 0x26, 0x48, 1},
        {KeyEvent.KEYCODE_DPAD_DOWN, 0x28, 0x50, 1}, {KeyEvent.KEYCODE_DPAD_LEFT, 0x25, 0x4B, 1},
        {KeyEvent.KEYCODE_DPAD_RIGHT, 0x27, 0x4D, 1},
    };
    private static final int[] LETTER_SCAN = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
        0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};

    private static int[] windowsKey(int code) {
        if (code >= KeyEvent.KEYCODE_A && code <= KeyEvent.KEYCODE_Z) {
            int i = code - KeyEvent.KEYCODE_A;
            return new int[] {0x41 + i, LETTER_SCAN[i], 0};
        }
        if (code >= KeyEvent.KEYCODE_1 && code <= KeyEvent.KEYCODE_9) return new int[] {0x31 + code - KeyEvent.KEYCODE_1, 0x02 + code - KeyEvent.KEYCODE_1, 0};
        if (code == KeyEvent.KEYCODE_0) return new int[] {0x30, 0x0B, 0};
        if (code >= KeyEvent.KEYCODE_F1 && code <= KeyEvent.KEYCODE_F10) return new int[] {0x70 + code - KeyEvent.KEYCODE_F1, 0x3B + code - KeyEvent.KEYCODE_F1, 0};
        for (int[] k : KEYS) if (k[0] == code) return new int[] {k[1], k[2], k[3]};
        return null;
    }

    // ---- pads: Android gamepad buttons -> XInput bits ----
    private static int xinputBit(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_B: return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X: return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_Y: return 0x8000;
            case KeyEvent.KEYCODE_BUTTON_L1: return 0x0100;
            case KeyEvent.KEYCODE_BUTTON_R1: return 0x0200;
            case KeyEvent.KEYCODE_BUTTON_START: return 0x0010;
            case KeyEvent.KEYCODE_BUTTON_SELECT: return 0x0020;
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return 0x0040;
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return 0x0080;
            default: return 0;
        }
    }

    private int pad(InputEvent e) {
        int id = e.getDeviceId(), i = padIndex.get(id, -1);
        if (i >= 0) return i;
        for (int slot = 0; slot < 4; slot++) {  // the lowest free one: the game itself only reads the first
            if (padIndex.indexOfValue(slot) >= 0) continue;
            padIndex.put(id, slot);
            return slot;
        }
        return -1;
    }

    /** A pad that went away frees its slot, and nothing it was holding stays pressed. */
    private void releasePad(int id) {
        int i = padIndex.get(id, -1);
        pads.remove(id);
        if (i < 0) return;
        padIndex.delete(id);
        padButtons[i] = 0;
        java.util.Arrays.fill(padAxes[i], 0);
        rumbleLast[i] = 0;
        if (started) sendPad(i);
    }

    /** Set by a tap, cleared by the next real input from a pad. */
    private boolean touchOverride;

    private void padUsed() {
        if (!touchOverride) return;
        touchOverride = false;
        updateDevices();
    }

    private interface InputEvent { int getDeviceId(); }

    /** Pad 0 also carries the on-screen controller (a physical pad wins on non-neutral axes). */
    private void sendPad(int i) {
        int[] a = padAxes[i];
        if (i != 0) { nativePad(i, true, padButtons[i], a[0], a[1], a[2], a[3], a[4], a[5]); return; }
        int[] t = touchState;
        int[] m = new int[6];
        for (int k = 0; k < 6; k++) m[k] = k < 2 ? Math.max(a[k], t[k + 1]) : (Math.abs(a[k]) >= Math.abs(t[k + 1]) ? a[k] : t[k + 1]);  // a resting pad is never exactly at 0
        nativePad(0, true, padButtons[0] | t[0], m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    /** The layout of this pad; the first time a PlayStation or Nintendo model is seen, its button table. */
    private Controller controller(InputDevice d) {
        Controller c = pads.get(d.getId());
        if (c != null) return c;
        if (padPrefs == null) padPrefs = getSharedPreferences("controllers", MODE_PRIVATE);
        c = Controller.of(d, padPrefs);
        pads.put(d.getId(), c);
        if ((c.kind == Controller.Kind.PLAYSTATION || c.kind == Controller.Kind.NINTENDO) && !padPrefs.getBoolean("shown." + c.key, false)) {
            padPrefs.edit().putBoolean("shown." + c.key, true).apply();
            StringBuilder text = new StringBuilder(d.getName()).append('\n');
            // The game itself names this pad's buttons (ME_PC_PAD_GLYPHS).
            text.append(Tr.t("Крестовина влево/вправо — поворот на 90°."));
            android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_LONG).show();
        }
        return c;
    }

    private static boolean fromPad(InputDevice d) {
        return d != null && ((d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (d.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started) return super.dispatchKeyEvent(e);
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return super.dispatchKeyEvent(e);
        Controller c = fromPad(e.getDevice()) ? controller(e.getDevice()) : null;
        if (c != null && down) padUsed();
        int code = c == null ? e.getKeyCode() : c.remap(e.getKeyCode());
        if (c != null && GuestInput.isTriggerKey(code)) {  // pads without analog triggers, e.g. Switch Pro
            int i = c.analogTriggers ? -1 : pad(e::getDeviceId);
            if (i >= 0) { padAxes[i][code == KeyEvent.KEYCODE_BUTTON_L2 ? 0 : 1] = down ? 255 : 0; sendPad(i); }
            return true;
        }
        int bit = xinputBit(code);
        if (bit != 0 && c != null) {
            int i = pad(e::getDeviceId);
            if (i >= 0) { padButtons[i] = down ? padButtons[i] | bit : padButtons[i] & ~bit; sendPad(i); }
            return true;
        }
        if (e.getKeyCode() == KeyEvent.KEYCODE_BACK) { if (down) nativeKey(0x1B, 0x01, false, true); else nativeKey(0x1B, 0x01, false, false); return true; }
        int[] k = windowsKey(e.getKeyCode());
        if (k == null) return super.dispatchKeyEvent(e);
        if (down && e.getRepeatCount() > 0) return true;  // the guest repeats on its own
        nativeKey(k[0], k[1], k[2] != 0, down);
        return true;
    }

    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (!started) return super.dispatchGenericMotionEvent(e);
        // The source check: a DualSense's touchpad comes from the same device as its sticks.
        if (fromPad(e.getDevice()) && e.isFromSource(InputDevice.SOURCE_JOYSTICK) && e.getAction() == MotionEvent.ACTION_MOVE) {
            int i = pad(e::getDeviceId);
            if (i < 0) return true;
            int[] a = padAxes[i];
            Controller c = controller(e.getDevice());
            if (c.analogTriggers) {
                float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
                float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
                a[0] = Math.round(lt * 255); a[1] = Math.round(rt * 255);
            }
            a[2] = Math.round(e.getAxisValue(MotionEvent.AXIS_X) * 32767);
            a[3] = Math.round(-e.getAxisValue(MotionEvent.AXIS_Y) * 32767);  // XInput: up is positive
            a[4] = Math.round(e.getAxisValue(c.rightX) * 32767);
            a[5] = Math.round(-e.getAxisValue(c.rightY) * 32767);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int dpad = (hy < -0.5f ? 0x1 : 0) | (hy > 0.5f ? 0x2 : 0) | (hx < -0.5f ? 0x4 : 0) | (hx > 0.5f ? 0x8 : 0);
            // D-pad left and right, free in play: the quick turn (they still reach the menus).
            int pressed = dpad & ~padButtons[i];
            if ((pressed & 0x4) != 0) nativeTurn(-90);
            if ((pressed & 0x8) != 0) nativeTurn(90);
            padButtons[i] = (padButtons[i] & ~0xF) | dpad;
            if (dpad != 0 || Math.max(a[0], a[1]) > 127 || Math.abs(a[2]) > 16000 || Math.abs(a[3]) > 16000
                || Math.abs(a[4]) > 16000 || Math.abs(a[5]) > 16000) padUsed();
            sendPad(i);
            return true;
        }
        if (e.isFromSource(InputDevice.SOURCE_MOUSE) && e.getAction() == MotionEvent.ACTION_HOVER_MOVE) {
            nativeMouseMove((int) e.getX(), (int) e.getY());
            return true;
        }
        return super.dispatchGenericMotionEvent(e);
    }

    /** Touch and mouse buttons: the pointer is the guest's mouse (left button while touching). */
    @Override public boolean onTouchEvent(MotionEvent e) {
        if (!started) return super.onTouchEvent(e);
        // A pad can be listed long after it stopped working: a tap brings the on-screen controls back.
        if (e.getActionMasked() == MotionEvent.ACTION_DOWN && e.isFromSource(InputDevice.SOURCE_TOUCHSCREEN)
            && touchMode != 0 && touchPad.getVisibility() != View.VISIBLE) {
            touchOverride = true;
            updateDevices();
            return true;
        }
        int x = (int) e.getX(), y = (int) e.getY();
        int button = e.isFromSource(InputDevice.SOURCE_MOUSE) && (e.getButtonState() & MotionEvent.BUTTON_SECONDARY) != 0 ? 1 : 0;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: nativeMouseMove(x, y); nativeMouseButton(button, true, x, y); return true;
            case MotionEvent.ACTION_MOVE: nativeMouseMove(x, y); return true;
            case MotionEvent.ACTION_UP: case MotionEvent.ACTION_CANCEL: nativeMouseButton(button, false, x, y); return true;
            default: return true;
        }
    }
}
