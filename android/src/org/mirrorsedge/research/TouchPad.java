package org.mirrorsedge.research;

import android.content.Context;
import android.graphics.*;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * On-screen controller for playing without a gamepad (mockup runner_apk_touch_controls_v2,
 * adjusted with the user): large LB/LT at the left thumb, RB/RT at the right, A/B/X/Y on
 * the right above the thumb area, small BACK/START at the top. Both sticks float: a finger
 * on the free left half becomes the run stick and on the free right half the camera stick,
 * centred where the finger landed; released, they return (dimmed) to their rest places.
 * Multi-touch: every pointer owns one control until it lifts. Layout units are the
 * mockup's 640x400 frame, scaled by the view height and anchored left/right.
 */
final class TouchPad extends View {
    private static final int LT = -3, RT = -4;  // triggers (analog axes, sent fully pressed)

    /** A button: rectangle or circle in mockup units, the guest bit (or trigger id) and labels. */
    private static final class Key {
        final float left, top, w, h; final boolean fromRight, fromBottom, round, accent, centred;
        final int bit; final String label, hint;
        boolean down;
        final RectF rect = new RectF();
        Key(float left, float top, float w, float h, boolean fromRight, boolean fromBottom, boolean round, boolean accent, boolean centred, int bit, String label, String hint) {
            this.left = left; this.top = top; this.w = w; this.h = h; this.fromRight = fromRight; this.fromBottom = fromBottom;
            this.round = round; this.accent = accent; this.centred = centred; this.bit = bit; this.label = label; this.hint = hint;
        }
    }

    /** A floating stick: rest place, current base and knob. */
    private static final class Stick {
        final boolean right; final String label;
        float restX, restY, radius, baseX, baseY, knobX, knobY;
        int pointer = -1;
        Stick(boolean right, String label) { this.right = right; this.label = label; }
        boolean held() { return pointer >= 0; }
        void rest() { pointer = -1; baseX = knobX = restX; baseY = knobY = restY; }
    }

    private final GuestInput input;
    private final Key[] keys = {
        new Key(22, 44, 118, 52, false, false, false, true, false, 0x100, "LB", "вверх · прыжок"),
        new Key(150, 44, 96, 52, false, false, false, false, false, LT, "LT", "вниз · подкат"),
        new Key(22, 44, 96, 52, true, false, false, false, false, 0x200, "RB", "разворот"),
        new Key(128, 44, 96, 52, true, false, false, false, false, RT, "RT", "атака"),
        // A/B/X/Y above the camera stick, leaving the lower right free for it (user request).
        new Key(64, 284, 40, 40, true, true, true, false, false, 0x8000, "Y", null),
        new Key(18, 242, 40, 40, true, true, true, false, false, 0x2000, "B", null),
        new Key(110, 242, 40, 40, true, true, true, false, false, 0x4000, "X", null),
        new Key(64, 200, 40, 40, true, true, true, false, false, 0x1000, "A", null),
        new Key(-60, 8, 52, 22, false, false, false, false, true, 0x20, "BACK", null),   // left = offset from the centre
        new Key(8, 8, 52, 22, false, false, false, false, true, 0x10, "START", null),
    };
    private final Stick run = new Stick(false, "Бег"), look = new Stick(true, "Обзор");
    private final SparseArray<Key> owners = new SparseArray<>();  // pointer id -> button
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private float scale = 1, density;

    TouchPad(Context context, GuestInput input) {
        super(context);
        this.input = input;
        density = context.getResources().getDisplayMetrics().density;
        stroke.setStyle(Paint.Style.STROKE);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        setClickable(true);
    }

    @Override protected void onSizeChanged(int w, int h, int ow, int oh) {
        // Mockup proportions at 80 %: the mockup frame is a phone-sized preview; on a
        // tablet the full size covers too much of the picture.
        scale = h / 400f * 0.8f;
        for (Key k : keys) {
            float x = k.fromRight ? w - (k.left + k.w) * scale : k.centred ? w / 2f + k.left * scale : k.left * scale;
            float y = k.fromBottom ? h - (k.top + k.h) * scale : k.top * scale;
            k.rect.set(x, y, x + k.w * scale, y + k.h * scale);
        }
        run.radius = look.radius = 50 * scale;
        run.restX = (30 + 50) * scale;
        run.restY = h - (26 + 50) * scale;
        look.restX = w - (24 + 50) * scale;  // close to the right edge
        look.restY = h - (26 + 50) * scale;
        run.rest();
        look.rest();
        stroke.setStrokeWidth(Math.max(2, 1.2f * scale));
    }

    /** Lifts everything (menu opened, controller hidden). */
    void release() {
        owners.clear();
        for (Key k : keys) if (k.down) press(k, false);
        run.rest(); input.touchStick(0, 0);
        look.rest(); input.touchRightStick(0, 0);
        invalidate();
    }

    private void press(Key k, boolean down) {
        k.down = down;
        if (k.bit == LT || k.bit == RT) input.touchTrigger(k.bit == LT, down);
        else input.button(k.bit, down, true);
    }

    private Key hit(float x, float y) {
        float slop = 6 * density;
        for (Key k : keys) if (x >= k.rect.left - slop && x <= k.rect.right + slop && y >= k.rect.top - slop && y <= k.rect.bottom + slop) return k;
        return null;
    }

    private void send(Stick s) {
        float dx = (s.knobX - s.baseX) / s.radius, dy = (s.knobY - s.baseY) / s.radius;
        if (s.right) input.touchRightStick(dx, dy); else input.touchStick(dx, dy);
    }

    private void grab(Stick s, int pointer, float x, float y) {
        s.pointer = pointer;
        // The base comes under the finger, kept fully on screen.
        s.baseX = Math.min(getWidth() - s.radius, Math.max(s.radius, x));
        s.baseY = Math.min(getHeight() - s.radius, Math.max(s.radius, y));
        drag(s, x, y);
    }

    private void drag(Stick s, float x, float y) {
        float dx = x - s.baseX, dy = y - s.baseY, d = (float) Math.hypot(dx, dy);
        if (d > s.radius) { dx *= s.radius / d; dy *= s.radius / d; }
        s.knobX = s.baseX + dx; s.knobY = s.baseY + dy;
        send(s);
    }

    @Override public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex(), id = e.getPointerId(i);
                float x = e.getX(i), y = e.getY(i);
                Key k = hit(x, y);
                Stick s = x < getWidth() / 2f ? run : look;
                if (k != null) { owners.put(id, k); press(k, true); }
                else if (!s.held()) grab(s, id, x, y);
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); ++i) {
                    int id = e.getPointerId(i);
                    if (run.pointer == id) drag(run, e.getX(i), e.getY(i));
                    else if (look.pointer == id) drag(look, e.getX(i), e.getY(i));
                }
                invalidate();
                return true;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                int id = e.getPointerId(e.getActionIndex());
                Key k = owners.get(id);
                owners.remove(id);
                if (k != null) press(k, false);
                if (run.pointer == id) { run.rest(); input.touchStick(0, 0); }
                if (look.pointer == id) { look.rest(); input.touchRightStick(0, 0); }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_CANCEL:
                release();
                return true;
        }
        return super.onTouchEvent(e);
    }

    private void drawStick(Canvas c, Stick s) {
        boolean held = s.held();
        fill.setColor(held ? 0x38141414 : 0x1F141414);
        stroke.setColor(held ? 0xE6FFFFFF : 0x80FFFFFF);
        c.drawCircle(s.baseX, s.baseY, s.radius, fill);
        c.drawCircle(s.baseX, s.baseY, s.radius, stroke);
        fill.setColor(held ? 0xFFE24B4A : 0x99FFFFFF);
        c.drawCircle(s.knobX, s.knobY, 18 * scale, fill);
        if (!held) {
            text.setTextSize(11 * scale);
            text.setAlpha(200);
            c.drawText(s.label, s.restX, s.restY - s.radius - 8 * scale, text);
            text.setAlpha(255);
        }
    }

    @Override protected void onDraw(Canvas c) {
        drawStick(c, run);
        drawStick(c, look);
        stroke.setColor(0xE6FFFFFF);
        for (Key k : keys) {
            int base = k.accent ? 0xD1E24B4A : 0x47141414;
            fill.setColor(k.down ? 0xF2E24B4A : base);
            if (k.round) {
                c.drawCircle(k.rect.centerX(), k.rect.centerY(), k.rect.width() / 2, fill);
                c.drawCircle(k.rect.centerX(), k.rect.centerY(), k.rect.width() / 2, stroke);
            } else {
                c.drawRect(k.rect, fill);
                c.drawRect(k.rect, stroke);
            }
            if (k.hint != null) {
                text.setTextSize(14 * scale);
                c.drawText(k.label, k.rect.centerX(), k.rect.centerY() - 2 * scale, text);
                text.setTextSize(10 * scale);
                c.drawText(k.hint, k.rect.centerX(), k.rect.centerY() + 12 * scale, text);
            } else {
                text.setTextSize((k.round ? 14 : 10) * scale);
                c.drawText(k.label, k.rect.centerX(), k.rect.centerY() + (k.round ? 5 : 3.5f) * scale, text);
            }
        }
    }
}
