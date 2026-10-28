package org.mirrorsedge.research;

import android.app.Activity;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.hardware.input.InputManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.*;
import android.widget.*;
import java.io.*;
import java.util.ArrayList;
import java.util.List;

/**
 * The game inside the app: libmeruntime.so (the same loader as the ADB executable) renders
 * through Vulkan straight into this SurfaceView. The library contains recompiled game code:
 * private builds carry it inside the APK, public builds load the user's own build from the
 * app's private files directory, copied there from the external files directory (see
 * RuntimeLibrary).
 *
 * External files directory (/sdcard/Android/data/org.mirrorsedge.research/files):
 *   libmeruntime.so, me_native_entry_sections_resources.bin, me_native_xex_headers.bin,
 *   me_game_readonly/ (extracted game files), me_saves/ (Xbox saves), game.log (output).
 * Tests: am start -n org.mirrorsedge.research/.GameActivity --es env "NAME=value NAME2=value"
 *   --ez external_input true: the app sends no input packets (automated route: mei_send).
 */
public class GameActivity extends Activity implements SurfaceHolder.Callback {
    private static native boolean nativeStart(String workDir, String[] args, String[] env);
    private static native int nativeExitCode();
    private static native void nativeSetSurface(Surface surface);
    private static native void nativeSetPaused(boolean paused);
    private static native long[] nativeFrameStats();
    /** Runtime frame counters for the overlay; null before the runtime loads or with an older runtime. */
    private static long[] frameStats() {
        if (!loaded) return null;
        try { return nativeFrameStats(); } catch (UnsatisfiedLinkError e) { return null; }
    }
    private PerfOverlay perfOverlay;  // launcher setting "Мониторинг"
    private static final String TAG = "MirrorsEdge";
    private static boolean loaded;

    private final GuestInput guestInput = new GuestInput();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status;
    private LinearLayout inputBar;
    private TouchPad touchPad;
    // Pause menu over the game (Back, holding View/Select, View+Start, F1, corner button).
    private FrameLayout pauseMenu;
    private TextView menuStatus;
    private boolean menuOpen;
    private boolean selectHeld;
    private final Runnable selectHold = () -> { if (selectHeld) openMenu(); };
    private SessionLog sessionLog;
    // Gamepad layouts by device id (Controller), the notice card over the game and the
    // Nintendo face-button swap in the pause menu.
    private final android.util.SparseArray<Controller> pads = new android.util.SparseArray<>();
    private SharedPreferences padPrefs;
    private TextView notice, swapButton;
    private final Runnable hideNotice = () -> notice.setVisibility(View.GONE);
    private int lastSmallestWidth;
    private InputManager inputs;
    private final InputManager.InputDeviceListener inputListener = new InputManager.InputDeviceListener() {
        public void onInputDeviceAdded(int id) { updateDevices(); }
        public void onInputDeviceChanged(int id) { guestInput.clear(); updateDevices(); }
        public void onInputDeviceRemoved(int id) { guestInput.clear(); updateDevices(); }
    };

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        SurfaceView surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(-1, -1));
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setShadowLayer(3, 1, 1, Color.BLACK);
        status.setPadding(24, 16, 24, 16);
        root.addView(status, new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START));
        inputBar = new LinearLayout(this);
        inputBar.setOrientation(LinearLayout.HORIZONTAL);
        addInputButton("START", 0x10);
        addInputButton("A", 0x1000);
        addInputButton("B", 0x2000);
        root.addView(inputBar, new FrameLayout.LayoutParams(-1, -2, Gravity.BOTTOM));
        touchPad = new TouchPad(this, guestInput);
        touchPad.setVisibility(View.GONE);  // updateDevices() shows it when no gamepad is connected
        root.addView(touchPad, new FrameLayout.LayoutParams(-1, -1));
        TextView corner = new TextView(this);
        corner.setText("\u2261");
        corner.setTextSize(22);
        corner.setTextColor(Color.WHITE);
        corner.setGravity(Gravity.CENTER);
        corner.setBackgroundColor(0x55000000);
        corner.setOnClickListener(v -> openMenu());
        FrameLayout.LayoutParams cornerParams = new FrameLayout.LayoutParams(dp(40), dp(40), Gravity.TOP | Gravity.END);
        cornerParams.setMargins(0, dp(10), dp(10), 0);
        root.addView(corner, cornerParams);
        notice = new TextView(this);
        notice.setTextColor(Color.WHITE);
        notice.setTextSize(14);
        notice.setBackgroundColor(0xE0141414);
        notice.setPadding(dp(18), dp(14), dp(18), dp(14));
        notice.setVisibility(View.GONE);
        notice.setOnClickListener(v -> notice.setVisibility(View.GONE));
        FrameLayout.LayoutParams noticeParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        noticeParams.setMargins(dp(16), dp(16), dp(16), 0);
        root.addView(notice, noticeParams);
        if (getIntent().getBooleanExtra("perf_overlay", false)) {
            TextView stats = new TextView(this);
            stats.setPadding(dp(8), dp(3), dp(8), dp(3));
            FrameLayout.LayoutParams statsParams = new FrameLayout.LayoutParams(-2, -2, Gravity.TOP | Gravity.START);
            statsParams.setMargins(dp(6), dp(6), 0, 0);
            root.addView(stats, statsParams);
            perfOverlay = new PerfOverlay(this, stats, GameActivity::frameStats);
        }
        padPrefs = getSharedPreferences("controllers", MODE_PRIVATE);
        pauseMenu = buildPauseMenu();
        root.addView(pauseMenu, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        hideSystemBars();
        inputs = (InputManager) getSystemService(INPUT_SERVICE);
        guestInput.player(LocalPlayer.load(this));
        startRuntime();
        lastSmallestWidth = getResources().getConfiguration().smallestScreenWidthDp;
        handler.postDelayed(this::checkFolded, 1500);
    }

    /** Short card over the game; tap hides it. */
    private void showNotice(CharSequence text, long millis) {
        notice.setText(text);
        notice.setVisibility(View.VISIBLE);
        handler.removeCallbacks(hideNotice);
        handler.postDelayed(hideNotice, millis);
    }

    // Foldables: the game is laid out for the inner screen. A hinge sensor plus a small screen is
    // taken as "folded" (assumption, CHECKLIST); shown at start and on every fold, never blocking.
    private boolean foldable() {
        return getPackageManager().hasSystemFeature("android.hardware.sensor.hinge_angle");
    }
    private void checkFolded() {
        if (foldable() && getResources().getConfiguration().smallestScreenWidthDp < 600)
            showNotice("Для лучшего игрового опыта рекомендуем полностью разложить телефон", 7000);
    }
    @Override public void onConfigurationChanged(Configuration config) {
        super.onConfigurationChanged(config);
        int sw = config.smallestScreenWidthDp;
        if (sw < 600 && lastSmallestWidth >= 600) checkFolded();
        else if (sw >= 600 && lastSmallestWidth < 600 && notice.getVisibility() == View.VISIBLE) notice.setVisibility(View.GONE);
        lastSmallestWidth = sw;
        hideSystemBars();
    }

    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }

    /** Left panel over the dimmed game: continue, send logs, back to the launcher. */
    private FrameLayout buildPauseMenu() {
        FrameLayout overlay = new FrameLayout(this);
        overlay.setBackgroundColor(0x99000000);
        overlay.setVisibility(View.GONE);
        overlay.setClickable(true);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackgroundColor(0xFFF7F8F9);
        panel.setPadding(dp(22), dp(22), dp(22), dp(22));
        TextView brand = new TextView(this);
        brand.setText("RUNNER/RECOMP");
        brand.setTextColor(0xFF141414);
        brand.setTextSize(16);
        brand.setLetterSpacing(0.06f);
        panel.addView(brand);
        TextView paused = new TextView(this);
        paused.setText("ПАУЗА");
        paused.setTextColor(0xFF6A7680);
        paused.setLetterSpacing(0.12f);
        paused.setPadding(0, dp(6), 0, dp(18));
        panel.addView(paused);
        panel.addView(menuButton("Продолжить", true, v -> closeMenu()));
        View spacer = new View(this);
        panel.addView(spacer, new LinearLayout.LayoutParams(1, 0, 1));
        menuStatus = new TextView(this);
        menuStatus.setTextColor(0xFF6A7680);
        menuStatus.setPadding(0, 0, 0, dp(8));
        panel.addView(menuStatus);
        swapButton = menuButton("", false, v -> toggleSwap());
        swapButton.setVisibility(View.GONE);
        panel.addView(swapButton);
        panel.addView(menuButton("Отправить логи", false, v -> sendLogs()));
        panel.addView(menuButton("Выйти в лаунчер", false, v -> finish()));
        overlay.addView(panel, new FrameLayout.LayoutParams(dp(260), -1, Gravity.START));
        return overlay;
    }

    private TextView menuButton(String text, boolean primary, View.OnClickListener listener) {
        TextView button = new TextView(this);
        button.setText(text);
        button.setTextSize(16);
        button.setPadding(dp(14), dp(12), dp(14), dp(12));
        button.setTextColor(primary ? Color.WHITE : 0xFF141414);
        button.setBackgroundColor(primary ? 0xFFE24B4A : 0x00000000);
        button.setFocusable(true);
        button.setOnClickListener(listener);
        return button;
    }

    private void openMenu() {
        if (menuOpen) return;
        menuOpen = true;
        guestInput.clear();
        if (loaded) nativeSetPaused(true);
        menuStatus.setText("");
        pauseMenu.setVisibility(View.VISIBLE);
        updateDevices();  // hides the on-screen controller
    }

    private void closeMenu() {
        if (!menuOpen) return;
        menuOpen = false;
        pauseMenu.setVisibility(View.GONE);
        updateDevices();  // brings the on-screen controller back when there is no gamepad
        guestInput.clear();
        if (loaded) nativeSetPaused(false);
        hideSystemBars();
    }

    private void sendLogs() {
        menuStatus.setText("Собираю архив…");
        new Thread(() -> {
            try {
                if (sessionLog != null) sessionLog.stop();
                File zip = SessionLog.bundle(this, getExternalFilesDir(null));
                String saved = SessionLog.saveToDownloads(this, zip);
                runOnUiThread(() -> {
                    menuStatus.setText(String.format(java.util.Locale.US, "Архив готов · %.1f МБ", zip.length() / 1e6)
                        + (saved != null ? "\nКопия: " + saved : ""));
                    // This process's own provider: the launcher process may be frozen meanwhile.
                    SessionLog.share(this, zip, GameLogsProvider.AUTHORITY);
                });
            } catch (IOException e) {
                runOnUiThread(() -> menuStatus.setText("Не получилось собрать архив: " + e.getMessage()));
            }
        }, "logs-bundle").start();
    }

    @Override public void onBackPressed() {
        if (menuOpen) closeMenu(); else openMenu();
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private void startRuntime() {
        File work = getExternalFilesDir(null);
        File staged = RuntimeLibrary.staged(this), library = new File(getFilesDir(), RuntimeLibrary.NAME);
        final boolean bundled = RuntimeLibrary.bundled(this);
        try {
            if (!loaded) {
                // Public builds: a newer staged copy replaces the private one (dlopen cannot
                // use the noexec external storage directly).
                if (!bundled && staged.isFile() && (!library.isFile() || staged.lastModified() != library.lastModified()
                        || staged.length() != library.length())) {
                    copy(staged, library);
                    library.setLastModified(staged.lastModified());
                }
                if (!bundled && !library.isFile()) { message("Нет libmeruntime.so в " + work); return; }
                for (String name : new String[] {"me_native_entry_sections_resources.bin", "me_native_xex_headers.bin"})
                    if (!new File(work, name).isFile()) { message("Нет " + name + " в " + work); return; }
                if (!new File(gameRoot(work), "inventory.txt").isFile()) { message("Нет файлов игры в " + gameRoot(work)); return; }
                if (bundled) System.loadLibrary("meruntime");
                else System.load(library.getAbsolutePath());
                loaded = true;
            }
        } catch (Throwable e) {
            message("Ошибка загрузки runtime: " + e);
            Log.e(TAG, "load", e);
            return;
        }
        new File(work, "me_saves").mkdirs();
        List<String> env = new ArrayList<>();
        env.add("ME_DISPLAY=1");
        env.add("ME_PREVIEW=0");  // no readback preview: the frame goes straight to the Surface
        env.add("ME_GUEST_INPUT=1");
        env.add("ME_DIAGNOSTIC_SECONDS=0");
        env.add("ME_GAME_ROOT=" + gameRoot(work));
        env.add("ME_SAVE_ROOT=" + new File(work, "me_saves"));
        env.add("ME_FRAME_DUMP=" + new File(work, "me_guest_frame.ppm"));
        env.add("ME_CACHE_ROOT=" + new File(work, "cache"));  // shader/pipeline storage + driver pipeline cache
        env.add("ME_NATIVE_LIB_DIR=" + getApplicationInfo().nativeLibraryDir);  // custom Vulkan driver (ME_TURNIP)
        String extra = getIntent().getStringExtra("env");  // tests and launcher options, applied last
        if (extra != null) for (String item : extra.trim().split("\\s+")) if (item.contains("=")) {
            env.add(item);
            if (item.startsWith("ME_FRAME_LIMIT_FILE=")) listenFrameLimit(new File(item.substring(20)));
        }
        sessionLog = new SessionLog(this, work);
        sessionLog.start(extra == null ? "" : extra);  // also keeps the previous session's game.log
        String[] args = {"me_native_entry_sections_resources.bin", "me_native_xex_headers.bin", "--gpu-explore-pool-scan"};
        if (nativeStart(work.getAbsolutePath(), args, env.toArray(new String[0]))) {
            message("Запуск…");
            handler.postDelayed(() -> message(""), 6000);
        }
        watchExit();
    }

    // Diagnostics only (tests pass ME_FRAME_LIMIT_FILE): files adb writes are unreadable for the
    // app, so adb sends `am broadcast -a org.mirrorsedge.research.FRAME_LIMIT --ei hz 30` and the
    // app writes the file the runtime re-reads (frame pacing A/B without a restart).
    private void listenFrameLimit(File file) {
        android.content.BroadcastReceiver receiver = new android.content.BroadcastReceiver() {
            @Override public void onReceive(android.content.Context context, android.content.Intent intent) {
                // --ei display 120: display refresh mode and the presenter's cap (file + ".display").
                // --ei shrink 0|1: ME_RENDER_AREA_SHRINK (file + ".shrink").
                // --ei hds 0|1: ME_HOST_DEPTH_STORE (file + ".hds").
                String extraName = intent.hasExtra("display") ? "display" : intent.hasExtra("shrink") ? "shrink"
                    : intent.hasExtra("hds") ? "hds" : intent.hasExtra("ablate") ? "ablate"
                    : intent.hasExtra("hoist") ? "hoist" : "hz";
                File target = extraName.equals("hz") ? file : new File(file.getPath() + "." + extraName);
                int value = intent.getIntExtra(extraName, 0);
                try (FileOutputStream out = new FileOutputStream(target)) {
                    out.write(Integer.toString(value).getBytes());
                } catch (IOException e) { Log.e(TAG, "frame limit file", e); }
                if (intent.hasExtra("display")) preferRefreshRate(value);
            }
        };
        android.content.IntentFilter filter = new android.content.IntentFilter("org.mirrorsedge.research.FRAME_LIMIT");
        if (android.os.Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, filter, 2 /* RECEIVER_EXPORTED */);
        else registerReceiver(receiver, filter);
    }

    /** Display mode with the current resolution and the refresh rate closest to hz (0 = system choice). */
    private void preferRefreshRate(int hz) {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode current = display.getMode(), best = null;
        if (hz > 0) for (Display.Mode m : display.getSupportedModes()) {
            if (m.getPhysicalWidth() != current.getPhysicalWidth() || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
            if (best == null || Math.abs(m.getRefreshRate() - hz) < Math.abs(best.getRefreshRate() - hz)) best = m;
        }
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredDisplayModeId = best == null ? 0 : best.getModeId();
        getWindow().setAttributes(params);
        Log.i(TAG, "display mode " + (best == null ? "system" : best.getRefreshRate() + " Hz"));
    }

    /** Game files: the launcher's choice (internal storage or the external files directory). */
    private File gameRoot(File work) {
        String root = getIntent().getStringExtra("game_root");
        return root != null ? new File(root) : new File(work, "me_game_readonly");
    }

    private void watchExit() {
        handler.postDelayed(() -> {
            int code = nativeExitCode();
            if (code >= 0) message("Runtime остановился (код " + code + "), см. game.log");
            else watchExit();
        }, 2000);
    }

    private static void copy(File from, File to) throws IOException {
        File temp = new File(to.getPath() + ".tmp");
        try (InputStream in = new FileInputStream(from); OutputStream out = new FileOutputStream(temp)) {
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
        }
        if (!temp.renameTo(to)) throw new IOException("rename " + temp);
    }

    private void message(String text) {
        runOnUiThread(() -> { status.setText(text); status.setVisibility(text.isEmpty() ? View.GONE : View.VISIBLE); });
    }

    @Override public void surfaceCreated(SurfaceHolder holder) {}
    @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (loaded) nativeSetSurface(holder.getSurface());
    }
    @Override public void surfaceDestroyed(SurfaceHolder holder) {
        if (loaded) nativeSetSurface(null);  // returns once the presenter released the window
    }

    // Virtual buttons until the full touch layout exists (CHECKLIST section C).
    private void addInputButton(String text, int bit) {
        Button button = new Button(this);
        button.setText(text);
        button.setAlpha(0.6f);
        button.setOnTouchListener((v, e) -> {
            int action = e.getActionMasked();
            if (action == MotionEvent.ACTION_DOWN) guestInput.button(bit, true, true);
            else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) guestInput.button(bit, false, true);
            return true;
        });
        inputBar.addView(button, new LinearLayout.LayoutParams(0, -2, 1));
    }

    private void updateDevices() {
        boolean external = false;
        Controller nintendo = null;
        pads.clear();
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            boolean pad = Controller.isGamepad(d);
            external |= pad || d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
            if (!pad) continue;
            Controller c = Controller.of(d, padPrefs);
            pads.put(id, c);
            if (c.kind == Controller.Kind.NINTENDO) nintendo = c;
            announce(d, c);
        }
        swapButton.setTag(nintendo == null ? null : nintendo.key);
        swapButton.setVisibility(nintendo == null ? View.GONE : View.VISIBLE);
        if (nintendo != null) swapButton.setText(nintendo.swapFace ? "Кнопки Nintendo: по надписям" : "Кнопки Nintendo: по положению");
        // The plain START/A/B strip is retired; the on-screen controller (TouchPad) is shown
        // only while no gamepad or keyboard is connected, and never over the pause menu.
        inputBar.setVisibility(View.GONE);
        boolean show = !external && !menuOpen && !getIntent().getBooleanExtra("external_input", false);
        if (!show && touchPad.getVisibility() == View.VISIBLE) touchPad.release();
        touchPad.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    /** Once per pad model: the game shows Xbox buttons, so PlayStation / Nintendo pads get the table. */
    private void announce(InputDevice d, Controller c) {
        if (c.kind != Controller.Kind.PLAYSTATION && c.kind != Controller.Kind.NINTENDO) return;
        if (padPrefs.getBoolean("shown." + c.key, false)) return;
        padPrefs.edit().putBoolean("shown." + c.key, true).apply();
        StringBuilder text = new StringBuilder(d.getName()).append("\nВ игре кнопки Xbox. Соответствие:\n");
        for (String[] row : Controller.table(c.kind)) text.append(row[0]).append("  →  ").append(row[1]).append('\n');
        if (c.kind == Controller.Kind.NINTENDO) text.append("Если A и B перепутаны — пауза → «Кнопки Nintendo».");
        else text.setLength(text.length() - 1);
        showNotice(text, 12000);
    }

    private void toggleSwap() {
        Object key = swapButton.getTag();
        if (key == null) return;
        String pref = "swapFace." + key;
        padPrefs.edit().putBoolean(pref, !padPrefs.getBoolean(pref, false)).apply();
        updateDevices();
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        Controller pad = pads.get(e.getDeviceId());
        final int code = pad == null ? e.getKeyCode() : pad.remap(e.getKeyCode());
        final boolean down = e.getAction() == KeyEvent.ACTION_DOWN;
        if (code == KeyEvent.KEYCODE_F1 && down) { if (menuOpen) closeMenu(); else openMenu(); return true; }
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) {
            // Short press goes to the game; holding it ~0.8 s opens the menu.
            if (down && e.getRepeatCount() == 0) { selectHeld = true; handler.postDelayed(selectHold, 800); }
            if (!down) { selectHeld = false; handler.removeCallbacks(selectHold); }
        }
        if (code == KeyEvent.KEYCODE_BUTTON_START && down && selectHeld) { selectHeld = false; handler.removeCallbacks(selectHold); openMenu(); return true; }
        if (menuOpen) {
            if (down && (code == KeyEvent.KEYCODE_BUTTON_B || code == KeyEvent.KEYCODE_BUTTON_START)) { closeMenu(); return true; }
            return super.dispatchKeyEvent(e);  // D-pad / A navigate the menu
        }
        if (GuestInput.isTriggerKey(code)) {
            if (pad == null || !pad.analogTriggers) guestInput.triggerKey(code == KeyEvent.KEYCODE_BUTTON_L2, down);
            return true;
        }
        int bit = GuestInput.keyBit(code);
        if (bit != 0) { guestInput.button(bit, e.getAction() == KeyEvent.ACTION_DOWN, false); return true; }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (menuOpen) return super.dispatchGenericMotionEvent(e);
        Controller pad = pads.get(e.getDeviceId());
        boolean used = pad == null ? guestInput.motion(e) : guestInput.motion(e, pad.rightX, pad.rightY);
        return used || super.dispatchGenericMotionEvent(e);
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars(); else guestInput.clear();
    }
    // Not visible (home, another app, screen off): stop the guest and its audio. Resumed on
    // onStart; onPause alone (dialogs, multi-window focus) keeps the game running.
    @Override public void onStop() {
        if (loaded) nativeSetPaused(true);
        if (perfOverlay != null) perfOverlay.stop();
        super.onStop();
    }
    @Override public void onStart() {
        super.onStart();
        if (loaded && !menuOpen) nativeSetPaused(false);  // the pause menu keeps the game paused
        if (perfOverlay != null) perfOverlay.start();
    }
    @Override public void onResume() {
        super.onResume();
        if (!getIntent().getBooleanExtra("external_input", false)) guestInput.start();
        inputs.registerInputDeviceListener(inputListener, null);
        updateDevices();
    }
    @Override public void onPause() {
        guestInput.stop();
        inputs.unregisterInputDeviceListener(inputListener);
        super.onPause();
    }
    @Override public void onDestroy() {
        if (sessionLog != null) sessionLog.stop();
        super.onDestroy();
        // The runtime cannot be stopped or restarted inside a process (and would keep
        // playing audio): leaving the game ends the process.
        if (isFinishing() && loaded) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
