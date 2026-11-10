package org.mirrorsedge.research;

import android.content.Context;
import android.graphics.*;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Launcher look (stylized mockups v2): light background, white geometric city, red route
 * line, RUNNER/RECOMP wordmark. Original artwork only, no game logos or assets.
 */
final class LauncherUi {
    static final int BG = 0xFFF7F8F9, INK = 0xFF141414, MUTED = 0xFF6A7680, NAV = 0xFF55606A,
        RED = 0xFFE24B4A, LINE = 0xFFE8ECEF, PANEL_BORDER = 0xFFDFE5EA, LOCKED = 0xFF9AA4AC, OK = 0xFF3B6D11;
    static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    private LauncherUi() {}

    static int dp(Context c, float v) { return Math.round(v * c.getResources().getDisplayMetrics().density); }

    static TextView text(Context c, CharSequence s, float sp, int color, boolean medium) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(color);
        if (medium) t.setTypeface(MEDIUM);
        return t;
    }

    /** RUNNER/RECOMP with a red slash. */
    static TextView wordmark(Context c, float sp) {
        android.text.SpannableString s = new android.text.SpannableString("RUNNER/RECOMP");
        s.setSpan(new android.text.style.ForegroundColorSpan(RED), 6, 7, 0);
        TextView t = text(c, s, sp, INK, true);
        t.setLetterSpacing(0.06f);
        return t;
    }

    /** Small red caps section label ("ИГРА", "ПРОДОЛЖИТЬ"). */
    static TextView kicker(Context c, String s) {
        TextView t = text(c, s, 11, RED, true);
        t.setLetterSpacing(0.12f);
        return t;
    }

    static GradientDrawable box(int fill, int stroke, int strokeDp, Context c) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        if (stroke != 0) d.setStroke(dp(c, strokeDp), stroke);
        return d;
    }

    /** Text button that dims while disabled. */
    static class Btn extends TextView {
        Btn(Context c) { super(c); setGravity(Gravity.CENTER_VERTICAL); setClickable(true); setFocusable(true); }
        @Override public void setEnabled(boolean on) { super.setEnabled(on); setAlpha(on ? 1f : 0.4f); }
    }

    /** Red call to action with the slanted right edge. */
    static Btn cta(Context c, String s, View.OnClickListener l) {
        Btn b = new Btn(c);
        b.setText(s);
        b.setTextSize(16);
        b.setTypeface(MEDIUM);
        b.setTextColor(Color.WHITE);
        b.setPadding(dp(c, 26), dp(c, 13), dp(c, 40), dp(c, 13));
        b.setBackground(new Slant(RED));
        b.setOnClickListener(l);
        return b;
    }

    /** Outlined secondary button. */
    static Btn ghost(Context c, String s, View.OnClickListener l) {
        Btn b = new Btn(c);
        b.setText(s);
        b.setTextSize(14);
        b.setTextColor(INK);
        b.setPadding(dp(c, 14), dp(c, 8), dp(c, 14), dp(c, 8));
        b.setBackground(box(Color.WHITE, INK, 1, c));
        b.setOnClickListener(l);
        return b;
    }

    /** Settings row: label left, control right, thin divider below. */
    static LinearLayout row(Context c, View label, View control) {
        LinearLayout r = new LinearLayout(c);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(0, dp(c, 10), 0, dp(c, 10));
        r.addView(label, new LinearLayout.LayoutParams(0, -2, 1));
        r.addView(control);
        r.setBackground(new Underline(LINE, dp(c, 1)));
        return r;
    }

    /** Segmented control (frame limit): black frame, selected segment filled. */
    static LinearLayout segments(Context c, String[] labels, int selected, java.util.function.IntConsumer onPick) {
        LinearLayout g = new LinearLayout(c);
        g.setBackground(box(Color.TRANSPARENT, INK, 1, c));
        g.setPadding(dp(c, 1), dp(c, 1), dp(c, 1), dp(c, 1));
        TextView[] items = new TextView[labels.length];
        for (int i = 0; i < labels.length; ++i) {
            final int index = i;
            TextView t = text(c, labels[i], 14, INK, false);
            t.setPadding(dp(c, 14), dp(c, 6), dp(c, 14), dp(c, 6));
            t.setClickable(true);
            t.setFocusable(true);
            items[i] = t;
            t.setOnClickListener(v -> {
                for (int k = 0; k < items.length; ++k) paintSegment(items[k], k == index);
                onPick.accept(index);
            });
            paintSegment(t, i == selected);
            g.addView(t);
        }
        return g;
    }

    private static void paintSegment(TextView t, boolean on) {
        t.setBackgroundColor(on ? INK : Color.TRANSPARENT);
        t.setTextColor(on ? Color.WHITE : INK);
    }

    /** Square toggle (red when on). */
    static final class Toggle extends View {
        private boolean on;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private java.util.function.Consumer<Boolean> listener;
        Toggle(Context c, boolean on, java.util.function.Consumer<Boolean> listener) {
            super(c);
            this.on = on;
            this.listener = listener;
            setClickable(true);
            setFocusable(true);
            setOnClickListener(v -> { this.on = !this.on; invalidate(); this.listener.accept(this.on); });
        }
        @Override protected void onMeasure(int w, int h) { setMeasuredDimension(dp(getContext(), 40), dp(getContext(), 22)); }
        @Override protected void onDraw(Canvas canvas) {
            float w = getWidth(), h = getHeight(), pad = dp(getContext(), 3);
            paint.setColor(on ? RED : 0xFFCFD6DB);
            canvas.drawRect(0, 0, w, h, paint);
            paint.setColor(Color.WHITE);
            float knob = h - 2 * pad, x = on ? w - pad - knob : pad;
            canvas.drawRect(x, pad, x + knob, pad + knob, paint);
        }
    }

    /** Polygon (0,0)-(w,0)-(0.94w,h)-(0,h). */
    static final class Slant extends Drawable {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Slant(int color) { paint.setColor(color); }
        @Override public void draw(Canvas canvas) {
            Rect b = getBounds();
            Path p = new Path();
            p.moveTo(b.left, b.top);
            p.lineTo(b.right, b.top);
            p.lineTo(b.left + b.width() * 0.92f, b.bottom);
            p.lineTo(b.left, b.bottom);
            p.close();
            canvas.drawPath(p, paint);
        }
        @Override public void setAlpha(int a) { paint.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { paint.setColorFilter(f); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** Bottom divider line. */
    static final class Underline extends Drawable {
        private final Paint paint = new Paint();
        private final int height;
        Underline(int color, int height) { paint.setColor(color); this.height = height; }
        @Override public void draw(Canvas canvas) { Rect b = getBounds(); canvas.drawRect(b.left, b.bottom - height, b.right, b.bottom, paint); }
        @Override public void setAlpha(int a) { paint.setAlpha(a); }
        @Override public void setColorFilter(ColorFilter f) { paint.setColorFilter(f); }
        @Override public int getOpacity() { return PixelFormat.TRANSLUCENT; }
    }

    /** Thin progress bar (red fill on light track). */
    static final class Bar extends View {
        private float value;
        private final Paint paint = new Paint();
        Bar(Context c) { super(c); }
        void set(float v) { value = Math.max(0, Math.min(1, v)); invalidate(); }
        @Override protected void onMeasure(int w, int h) { setMeasuredDimension(MeasureSpec.getSize(w), dp(getContext(), 4)); }
        @Override protected void onDraw(Canvas canvas) {
            paint.setColor(PANEL_BORDER);
            canvas.drawRect(0, 0, getWidth(), getHeight(), paint);
            paint.setColor(RED);
            canvas.drawRect(0, 0, getWidth() * value, getHeight(), paint);
        }
    }

    /**
     * Page backgrounds, drawn from the mockup's 640x400 artwork scaled to cover the view.
     * CITY: sky, white towers, red route diagonal (play page). STRIPE: single red diagonal
     * (settings). CORNER: pale column with a red corner (lists). HORIZON: low red line (data).
     */
    static final class Backdrop extends View {
        static final int CITY = 0, STRIPE = 1, CORNER = 2, HORIZON = 3;
        private final int mode;
        private final int extendLeft;  // nav column width: the artwork is laid out for the whole screen
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        Backdrop(Context c, int mode, int extendLeft) { super(c); this.mode = mode; this.extendLeft = extendLeft; }

        private void rect(Canvas c, float x, float y, float w, float h, int color) { paint.setColor(color); c.drawRect(x, y, x + w, y + h, paint); }
        private void poly(Canvas c, int color, float... pts) {
            Path p = new Path();
            p.moveTo(pts[0], pts[1]);
            for (int i = 2; i < pts.length; i += 2) p.lineTo(pts[i], pts[i + 1]);
            p.close();
            paint.setColor(color);
            c.drawPath(p, paint);
        }

        @Override protected void onDraw(Canvas canvas) {
            float w = getWidth() + extendLeft, h = getHeight();
            // Cover the whole screen (nav column included, as in the mockups), anchored
            // right and bottom; this view shows the part right of the nav.
            float s = Math.max(w / 640f, h / 400f);
            canvas.save();
            // The city stands on the bottom edge; the flat pages keep their accents at the top.
            canvas.translate(w - 640 * s - extendLeft, mode == CITY || mode == STRIPE ? h - 400 * s : 0);
            canvas.scale(s, s);
            switch (mode) {
                case CITY:
                    rect(canvas, -2000, -2000, 4640, 4400, 0xFFDCEBF5);
                    rect(canvas, 300, 60, 70, 940, Color.WHITE); rect(canvas, 370, 60, 26, 940, 0xFFC9D7E1);
                    rect(canvas, 420, 120, 90, 880, Color.WHITE); rect(canvas, 510, 120, 30, 880, 0xFFC3D2DC);
                    rect(canvas, 545, 30, 60, 970, Color.WHITE); rect(canvas, 605, 30, 35, 970, 0xFFCBD9E3);
                    rect(canvas, 230, 190, 70, 810, 0xFFF2F5F7); rect(canvas, 250, 170, 18, 20, RED);
                    rect(canvas, 190, 300, 450, 700, Color.WHITE);
                    poly(canvas, RED, 200, 330, 330, 250, 470, 220, 640, 150, 640, 165, 472, 236, 334, 266, 206, 342);
                    break;
                case STRIPE:
                    rect(canvas, -2000, -2000, 4640, 4400, BG);
                    // Route line low in the empty bottom-right corner, clear of the settings rows.
                    poly(canvas, RED, 360, 400, 640, 268, 640, 278, 382, 400);
                    break;
                case CORNER:
                    rect(canvas, -2000, -2000, 4640, 4400, BG);
                    rect(canvas, 560, -400, 80, 1200, 0xFFEEF2F5);
                    poly(canvas, RED, 560, 0, 640, 0, 640, 30);
                    break;
                default:
                    rect(canvas, -2000, -2000, 4640, 4400, BG);
                    rect(canvas, 470, 40, 50, 220, 0xFFE6EDF2); rect(canvas, 530, 90, 80, 180, 0xFFEDF2F5);
                    poly(canvas, RED, -640, 502, -640, 490, 640, 330, 640, 342);
                    break;
            }
            canvas.restore();
        }
    }
}
