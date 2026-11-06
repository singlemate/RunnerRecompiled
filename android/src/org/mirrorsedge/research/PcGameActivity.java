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
 * Settings: the screen shown before the start (language, frame limit, FOV, guest CPUs, on-screen
 * controller), or for adb the intent extra "env", e.g. "ME_PC_FOV=100", which starts at once.
 */
public class PcGameActivity extends Activity implements SurfaceHolder.Callback {
    private static final String TAG = "me_pc";
    private static native boolean nativeStart(String library, String[] args, String[] env);
    private static native int nativeExitCode();
    private static native void nativeSetSurface(android.view.Surface surface);
    private static native void nativeSetForeground(boolean front);
    private static native void nativeKey(int vk, int scan, boolean extended, boolean down);
    private static native void nativeMouseMove(int x, int y);
    private static native void nativeMouseButton(int button, boolean down, int x, int y);
    private static native void nativePad(int index, boolean connected, int buttons, int lt, int rt, int lx, int ly, int rx, int ry);

    private static boolean started;
    private TextView status;
    private SurfaceView view;
    private final SparseIntArray padIndex = new SparseIntArray();
    private final int[] padButtons = new int[4];
    private final int[][] padAxes = new int[4][6];  // lt, rt, lx, ly, rx, ry
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
    private final InputManager.InputDeviceListener inputListener = new InputManager.InputDeviceListener() {
        public void onInputDeviceAdded(int id) { updateDevices(); }
        public void onInputDeviceChanged(int id) { updateDevices(); }
        public void onInputDeviceRemoved(int id) { updateDevices(); }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        FrameLayout root = new FrameLayout(this);
        view = new SurfaceView(this);
        view.getHolder().addCallback(this);
        root.addView(view);
        touchPad = new TouchPad(this, touchInput);
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
            message("Нет libmepcjni.so: " + e);
            return;
        }
        if (started || getIntent().hasExtra("env")) start();
        else showSettings(root);
    }

    // ---- settings screen (before the start) ----
    private static final String[][] LANGUAGES = {{"int", "English"}, {"rus", "Русский"}, {"deu", "Deutsch"},
        {"fra", "Français"}, {"ita", "Italiano"}, {"esn", "Español"}, {"pol", "Polski"}, {"cze", "Čeština"},
        {"hun", "Magyar"}, {"por", "Português"}, {"jpn", "日本語"}, {"kor", "한국어"}, {"chs", "简体中文"}, {"cht", "繁體中文"}};
    private static final int[] FPS = {30, 62, 90, 120, 144};
    private static final int[] CPUS = {1, 2, 4};
    // Share of the screen's pixels on each side: the same steps as the modes of the Android WSI
    // (pc/dxvk), rounded down to 8 (the game's buffers are aligned to 8; other sizes draw no world).
    private static final int[][] SCALES = {{2, 3}, {3, 4}, {1, 1}, {1, 2}};
    private static final String[] SCALE_NAMES = {"Разрешение: 2/3 экрана (рекомендуется)", "Разрешение: 3/4 экрана", "Разрешение: полное", "Разрешение: 1/2 экрана"};
    private static final String[] TOUCH = {"Экранный геймпад: авто", "Экранный геймпад: всегда", "Экранный геймпад: нет"};

    private android.widget.Spinner spinner(android.widget.LinearLayout box, String[] items, int selected) {
        android.widget.Spinner sp = new android.widget.Spinner(this);
        android.widget.ArrayAdapter<String> ad = new android.widget.ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        ad.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        sp.setAdapter(ad);
        sp.setSelection(Math.max(0, selected));
        box.addView(sp);
        return sp;
    }

    private static int indexOf(int[] a, int v) { for (int i = 0; i < a.length; i++) if (a[i] == v) return i; return -1; }

    private void showSettings(FrameLayout root) {
        android.content.SharedPreferences prefs = getSharedPreferences("pc", MODE_PRIVATE);
        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setBackgroundColor(0xFF101418);
        android.widget.LinearLayout box = new android.widget.LinearLayout(this);
        box.setOrientation(android.widget.LinearLayout.VERTICAL);
        box.setPadding(48, 32, 48, 32);
        scroll.addView(box);
        TextView title = new TextView(this);
        title.setText("Mirror's Edge — ПК-версия");
        title.setTextSize(22);
        title.setTextColor(0xFFFFFFFF);
        box.addView(title);
        TextView checks = new TextView(this);
        checks.setTextColor(0xFFB0BEC5);
        checks.setText(filesReport());
        box.addView(checks);

        String[] langNames = new String[LANGUAGES.length];
        int lang = 0;
        for (int i = 0; i < LANGUAGES.length; i++) {
            langNames[i] = "Язык: " + LANGUAGES[i][1];
            if (LANGUAGES[i][0].equals(prefs.getString("lang", "int"))) lang = i;
        }
        android.widget.Spinner langBox = spinner(box, langNames, lang);
        String[] fpsNames = new String[FPS.length];
        for (int i = 0; i < FPS.length; i++) fpsNames[i] = "Лимит кадров: " + FPS[i] + (FPS[i] == 62 ? " (как в игре)" : "");
        android.widget.Spinner fpsBox = spinner(box, fpsNames, indexOf(FPS, prefs.getInt("fps", 62)));
        String[] cpuNames = new String[CPUS.length];
        for (int i = 0; i < CPUS.length; i++) cpuNames[i] = "Ядер для игры: " + CPUS[i] + (CPUS[i] == 1 ? "" : " (отдельный поток отрисовки)");
        android.widget.Spinner cpuBox = spinner(box, cpuNames, indexOf(CPUS, prefs.getInt("cpus", 4)));
        android.widget.Spinner touchBox = spinner(box, TOUCH, prefs.getInt("touch", 0));
        android.widget.Spinner scaleBox = spinner(box, SCALE_NAMES, prefs.getInt("scale", 0));

        TextView fovText = new TextView(this);
        fovText.setTextColor(0xFFFFFFFF);
        box.addView(fovText);
        android.widget.SeekBar fov = new android.widget.SeekBar(this);
        fov.setMax(140 - 60);  // 0 = the game's own, 1..80 -> 61..140 degrees
        fov.setProgress(Math.max(0, prefs.getInt("fov", 0) - 60));
        android.widget.SeekBar.OnSeekBarChangeListener fovLabel = new android.widget.SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(android.widget.SeekBar b, int v, boolean user) {
                fovText.setText(v == 0 ? "Поле зрения: как в игре" : "Поле зрения: " + (60 + v) + "°");
            }
            public void onStartTrackingTouch(android.widget.SeekBar b) {}
            public void onStopTrackingTouch(android.widget.SeekBar b) {}
        };
        fov.setOnSeekBarChangeListener(fovLabel);
        fovLabel.onProgressChanged(fov, fov.getProgress(), false);
        box.addView(fov);

        TextView note = new TextView(this);
        note.setTextColor(0xFF90A4AE);
        note.setText("Язык, лимит кадров и разрешение записываются в TdEngine.ini игры; при самом первом запуске его ещё нет, "
            + "они применятся со второго запуска.");
        box.addView(note);
        android.widget.Button play = new android.widget.Button(this);
        play.setText("Играть");
        box.addView(play);
        play.setOnClickListener(v -> {
            int f = fov.getProgress();
            prefs.edit().putString("lang", LANGUAGES[langBox.getSelectedItemPosition()][0])
                .putInt("fps", FPS[fpsBox.getSelectedItemPosition()]).putInt("cpus", CPUS[cpuBox.getSelectedItemPosition()])
                .putInt("touch", touchBox.getSelectedItemPosition()).putInt("scale", scaleBox.getSelectedItemPosition()).putInt("fov", f == 0 ? 0 : 60 + f).commit();
            root.removeView(scroll);
            start();
            updateDevices();
            if (started) nativeSetForeground(true);
        });
        root.addView(scroll, new FrameLayout.LayoutParams(-1, -1));
    }

    private File pcDir() { return new File(getExternalFilesDir(null), "pc"); }

    private static String mark(boolean ok) { return ok ? "✓ " : "✗ "; }

    private String filesReport() {
        File pc = pcDir(), lib = new File(getApplicationInfo().nativeLibraryDir);
        return "Папка: " + pc
            + "\n" + mark(new File(lib, "libmepc.so").isFile()) + "libmepc.so (только личная сборка APK)"
            + "\n" + mark(new File(lib, "libdxvk_d3d9.so").isFile()) + "libdxvk_d3d9.so"
            + "\n" + mark(new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile()) + "MirrorsEdge/Binaries/MirrorsEdge.exe"
            + "\n" + mark(new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()) + "MirrorsEdge/TdGame/CookedPC"
            + "\n" + mark(new File(pc, "deps/msvcr80.dll").isFile() && new File(pc, "deps/msvcp80.dll").isFile()) + "deps/msvcr80.dll, msvcp80.dll"
            + "\n" + mark(new File(pc, "deps/rebased").isDirectory()) + "deps/rebased";
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
    private static void setIni(File ini, String section, String key, String value) {
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

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private void start() {
        if (started) return;
        File pc = pcDir();
        File game = new File(pc, "MirrorsEdge"), deps = new File(pc, "deps"), user = new File(pc, "user");
        File library = new File(getApplicationInfo().nativeLibraryDir, "libmepc.so");
        if (!library.isFile()) { message("Это APK без ПК-версии (нет libmepc.so)."); return; }
        if (!new File(game, "Binaries").isDirectory()) { message("Нет копии игры: " + game + "/Binaries"); return; }
        if (!new File(deps, "msvcr80.dll").isFile()) { message("Нет библиотек: " + deps); return; }
        user.mkdirs();
        List<String> env = new ArrayList<>();
        env.add("ME_PC_LOG_PATH=" + new File(pc, "pc-runtime.log"));
        if (!getIntent().hasExtra("env")) {
            android.content.SharedPreferences prefs = getSharedPreferences("pc", MODE_PRIVATE);
            File ini = new File(user, "other/C/Documents and Settings/Runner/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini");
            if (ini.isFile()) {
                setIni(ini, "Engine.Engine", "Language", prefs.getString("lang", "int"));
                setIni(ini, "Engine.GameEngine", "MaxSmoothedFrameRate", String.valueOf(prefs.getInt("fps", 62)));
                android.graphics.Point real = new android.graphics.Point();
                getWindowManager().getDefaultDisplay().getRealSize(real);
                int[] sc = SCALES[prefs.getInt("scale", 0)];
                int w = Math.max(real.x, real.y), h = Math.min(real.x, real.y);
                setIni(ini, "SystemSettings", "ResX", String.valueOf((w * sc[0] / sc[1]) & ~7));
                setIni(ini, "SystemSettings", "ResY", String.valueOf((h * sc[0] / sc[1]) & ~7));
            }
            if (prefs.getInt("fov", 0) > 0) env.add("ME_PC_FOV=" + prefs.getInt("fov", 0));
            env.add("ME_PC_CPUS=" + prefs.getInt("cpus", 4));
            int touch = prefs.getInt("touch", 0);
            if (touch > 0) env.add("ME_PC_TOUCH=" + (touch == 1 ? 1 : 0));
        }
        // adb tests: intent extra "ini" sets [SystemSettings] keys, e.g. "Bloom=False,MotionBlur=False";
        // the section is logged either way (the file belongs to the app, adb cannot read it).
        File systemIni = new File(user, "other/C/Documents and Settings/Runner/My Documents/EA Games/Mirror's Edge/TdGame/Config/TdEngine.ini");
        String iniExtra = getIntent().getStringExtra("ini");
        if (iniExtra != null && systemIni.isFile())
            for (String item : iniExtra.split(",")) {
                String[] kv = item.split("=", 2);
                if (kv.length == 2) setIni(systemIni, "SystemSettings", kv[0], kv[1]);
            }
        logSystemSettings(systemIni);
        String extra = getIntent().getStringExtra("env");
        if (extra != null) for (String item : extra.trim().split("\\s+")) if (item.contains("=")) env.add(item);
        for (String item : env) if (item.startsWith("ME_PC_TOUCH=")) touchMode = item.endsWith("=1") ? 1 : 0;
        String[] args = {new File(game, "Binaries").getPath(), deps.getPath(), user.getPath()};
        started = nativeStart(library.getPath(), args, env.toArray(new String[0]));
        if (!started) message("Не удалось запустить libmepc.so (см. logcat, тег me_pc_jni)");
        Log.i(TAG, "started " + started + " with " + env);
    }

    private void message(String text) {
        runOnUiThread(() -> { status.setText(text); status.setVisibility(View.VISIBLE); });
    }

    @Override protected void onResume() {
        super.onResume();
        hideSystemBars();
        ((InputManager) getSystemService(INPUT_SERVICE)).registerInputDeviceListener(inputListener, null);
        updateDevices();
        if (started) nativeSetForeground(true);
    }
    @Override protected void onPause() {
        super.onPause();
        ((InputManager) getSystemService(INPUT_SERVICE)).unregisterInputDeviceListener(inputListener);
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
        if (i < 0 && padIndex.size() < 4) { i = padIndex.size(); padIndex.put(id, i); }
        return i;
    }

    private interface InputEvent { int getDeviceId(); }

    /** Pad 0 also carries the on-screen controller (a physical pad wins on non-neutral axes). */
    private void sendPad(int i) {
        int[] a = padAxes[i];
        if (i != 0) { nativePad(i, true, padButtons[i], a[0], a[1], a[2], a[3], a[4], a[5]); return; }
        int[] t = touchState;
        int[] m = new int[6];
        for (int k = 0; k < 6; k++) m[k] = k < 2 ? Math.max(a[k], t[k + 1]) : (a[k] != 0 ? a[k] : t[k + 1]);
        nativePad(0, true, padButtons[0] | t[0], m[0], m[1], m[2], m[3], m[4], m[5]);
    }

    private static boolean fromPad(InputDevice d) {
        return d != null && ((d.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (d.getSources() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        if (!started) return super.dispatchKeyEvent(e);
        boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (e.getAction() != KeyEvent.ACTION_DOWN && e.getAction() != KeyEvent.ACTION_UP) return super.dispatchKeyEvent(e);
        int bit = xinputBit(e.getKeyCode());
        if (bit != 0 && fromPad(e.getDevice())) {
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
        if (fromPad(e.getDevice()) && e.getAction() == MotionEvent.ACTION_MOVE) {
            int i = pad(e::getDeviceId);
            if (i < 0) return true;
            int[] a = padAxes[i];
            float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
            float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
            a[0] = Math.round(lt * 255); a[1] = Math.round(rt * 255);
            a[2] = Math.round(e.getAxisValue(MotionEvent.AXIS_X) * 32767);
            a[3] = Math.round(-e.getAxisValue(MotionEvent.AXIS_Y) * 32767);  // XInput: up is positive
            a[4] = Math.round(e.getAxisValue(MotionEvent.AXIS_Z) * 32767);
            a[5] = Math.round(-e.getAxisValue(MotionEvent.AXIS_RZ) * 32767);
            float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
            int dpad = (hy < -0.5f ? 0x1 : 0) | (hy > 0.5f ? 0x2 : 0) | (hx < -0.5f ? 0x4 : 0) | (hx > 0.5f ? 0x8 : 0);
            padButtons[i] = (padButtons[i] & ~0xF) | dpad;
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
