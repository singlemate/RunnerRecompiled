package org.mirrorsedge.research;

import android.content.SharedPreferences;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

/**
 * Per-gamepad layout on top of Android's standard gamepad mapping: which family it is (the game
 * shows Xbox buttons, so PlayStation and Nintendo pads get a button table once), which axes carry
 * the right stick (Z/RZ normally; RX/RY on pads whose Z/RZ are the triggers, e.g. older DualShock 4
 * layouts), and, for Nintendo pads, an optional A<->B / X<->Y swap for systems that report the
 * buttons by label instead of by position.
 */
final class Controller {
    enum Kind { XBOX, PLAYSTATION, NINTENDO, OTHER }

    final Kind kind;
    final String key;      // vendor:product, for per-pad preferences
    final int rightX, rightY;
    final boolean swapFace;
    final boolean analogTriggers;  // L2/R2 key events are then ignored: the axis carries the pull

    private Controller(Kind kind, String key, int rightX, int rightY, boolean swapFace, boolean analogTriggers) {
        this.kind = kind; this.key = key; this.rightX = rightX; this.rightY = rightY; this.swapFace = swapFace;
        this.analogTriggers = analogTriggers;
    }

    static Kind kind(InputDevice d) {
        switch (d.getVendorId()) {
            case 0x045E: return Kind.XBOX;          // Microsoft
            case 0x054C: return Kind.PLAYSTATION;   // Sony
            case 0x057E: return Kind.NINTENDO;      // Nintendo
            default: return Kind.OTHER;
        }
    }

    static boolean isGamepad(InputDevice d) {
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    static String key(InputDevice d) { return String.format("%04x:%04x", d.getVendorId(), d.getProductId()); }

    static Controller of(InputDevice d, SharedPreferences prefs) {
        if (d == null) return new Controller(Kind.OTHER, "", MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ, false, false);
        boolean hasZ = d.getMotionRange(MotionEvent.AXIS_Z) != null && d.getMotionRange(MotionEvent.AXIS_RZ) != null;
        boolean hasRx = d.getMotionRange(MotionEvent.AXIS_RX) != null && d.getMotionRange(MotionEvent.AXIS_RY) != null;
        boolean hasTriggerAxes = d.getMotionRange(MotionEvent.AXIS_LTRIGGER) != null || d.getMotionRange(MotionEvent.AXIS_BRAKE) != null;
        // Right stick on RX/RY when Z/RZ are missing, or when they must be the triggers.
        boolean rx = hasRx && (!hasZ || !hasTriggerAxes);
        String key = key(d);
        Kind kind = kind(d);
        boolean swap = kind == Kind.NINTENDO && prefs.getBoolean("swapFace." + key, false);
        return new Controller(kind, key, rx ? MotionEvent.AXIS_RX : MotionEvent.AXIS_Z, rx ? MotionEvent.AXIS_RY : MotionEvent.AXIS_RZ, swap,
            hasTriggerAxes);
    }

    /** Android key code as the game should see it (face button swap). */
    int remap(int code) {
        if (!swapFace) return code;
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: return KeyEvent.KEYCODE_BUTTON_B;
            case KeyEvent.KEYCODE_BUTTON_B: return KeyEvent.KEYCODE_BUTTON_A;
            case KeyEvent.KEYCODE_BUTTON_X: return KeyEvent.KEYCODE_BUTTON_Y;
            case KeyEvent.KEYCODE_BUTTON_Y: return KeyEvent.KEYCODE_BUTTON_X;
            default: return code;
        }
    }

    /** Button table shown once per pad: Xbox button in the game -> button on this pad. */
    static String[][] table(Kind kind) {
        if (kind == Kind.PLAYSTATION) return new String[][] {
            {"A", "✕ Крест"}, {"B", "○ Круг"}, {"X", "□ Квадрат"}, {"Y", "△ Треугольник"},
            {"LB / RB", "L1 / R1"}, {"LT / RT", "L2 / R2"}, {"Back (View)", "Share / Create"}, {"Start (Menu)", "Options"},
            {"Нажатие стиков", "L3 / R3"}};
        return new String[][] {   // Nintendo: by position, the labels differ from Xbox
            {"A (нижняя)", "B (нижняя)"}, {"B (правая)", "A (правая)"}, {"X (левая)", "Y (левая)"}, {"Y (верхняя)", "X (верхняя)"},
            {"LB / RB", "L / R"}, {"LT / RT", "ZL / ZR"}, {"Back (View)", "−"}, {"Start (Menu)", "+"},
            {"Нажатие стиков", "L3 / R3"}};
    }
}
