package org.mirrorsedge.research;

import android.app.Activity;
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
 * through Vulkan straight into this SurfaceView. The library contains generated game code
 * and is never packaged in the APK; it is loaded from the app's private files directory,
 * copied there from the external files directory (where adb or the launcher puts it).
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
    private static final String TAG = "MirrorsEdge";
    private static boolean loaded;

    private final GuestInput guestInput = new GuestInput();
    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView status;
    private LinearLayout inputBar;
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
        setContentView(root);
        hideSystemBars();
        inputs = (InputManager) getSystemService(INPUT_SERVICE);
        guestInput.player(LocalPlayer.load(this));
        startRuntime();
    }

    private void hideSystemBars() {
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
    }

    private void startRuntime() {
        File work = getExternalFilesDir(null);
        File staged = new File(work, "libmeruntime.so"), library = new File(getFilesDir(), "libmeruntime.so");
        try {
            if (!loaded) {
                // A newer staged copy replaces the private one (dlopen cannot use the
                // noexec external storage directly).
                if (staged.isFile() && (!library.isFile() || staged.lastModified() != library.lastModified()
                        || staged.length() != library.length())) {
                    copy(staged, library);
                    library.setLastModified(staged.lastModified());
                }
                if (!library.isFile()) { message("Нет libmeruntime.so в " + work); return; }
                for (String name : new String[] {"me_native_entry_sections_resources.bin", "me_native_xex_headers.bin"})
                    if (!new File(work, name).isFile()) { message("Нет " + name + " в " + work); return; }
                if (!new File(work, "me_game_readonly/inventory.txt").isFile()) { message("Нет файлов игры (me_game_readonly) в " + work); return; }
                System.load(library.getAbsolutePath());
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
        env.add("ME_GAME_ROOT=" + new File(work, "me_game_readonly"));
        env.add("ME_SAVE_ROOT=" + new File(work, "me_saves"));
        env.add("ME_FRAME_DUMP=" + new File(work, "me_guest_frame.ppm"));
        String extra = getIntent().getStringExtra("env");  // tests and launcher options, applied last
        if (extra != null) for (String item : extra.trim().split("\\s+")) if (item.contains("=")) env.add(item);
        String[] args = {"me_native_entry_sections_resources.bin", "me_native_xex_headers.bin", "--gpu-explore-pool-scan"};
        if (nativeStart(work.getAbsolutePath(), args, env.toArray(new String[0]))) {
            message("Запуск…");
            handler.postDelayed(() -> message(""), 6000);
        }
        watchExit();
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
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int sources = d.getSources();
            external |= (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
        }
        inputBar.setVisibility(external ? View.GONE : View.VISIBLE);
    }

    @Override public boolean dispatchKeyEvent(KeyEvent e) {
        int bit = GuestInput.keyBit(e.getKeyCode());
        if (bit != 0) { guestInput.button(bit, e.getAction() == KeyEvent.ACTION_DOWN, false); return true; }
        return super.dispatchKeyEvent(e);
    }
    @Override public boolean dispatchGenericMotionEvent(MotionEvent e) {
        return guestInput.motion(e) || super.dispatchGenericMotionEvent(e);
    }
    @Override public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars(); else guestInput.clear();
    }
    // Not visible (home, another app, screen off): stop the guest and its audio. Resumed on
    // onStart; onPause alone (dialogs, multi-window focus) keeps the game running.
    @Override public void onStop() {
        if (loaded) nativeSetPaused(true);
        super.onStop();
    }
    @Override public void onStart() {
        super.onStart();
        if (loaded) nativeSetPaused(false);
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
        super.onDestroy();
        // The runtime cannot be stopped or restarted inside a process (and would keep
        // playing audio): leaving the game ends the process.
        if (isFinishing() && loaded) android.os.Process.killProcess(android.os.Process.myPid());
    }
}
