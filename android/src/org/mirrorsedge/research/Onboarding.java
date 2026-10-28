package org.mirrorsedge.research;

import android.graphics.Color;
import android.view.Gravity;
import android.view.View;
import android.widget.*;
import java.util.List;
import static org.mirrorsedge.research.LauncherUi.*;

/**
 * First-run setup over the launcher: welcome, game language, game data (import), frame
 * limit, controls, done. Shown until finished once ("onboardingDone"); the current step is
 * saved on every move ("onboardingStep"), so closing the app mid-way resumes at the same
 * step. An interrupted import resumes too (finished files are skipped by the import).
 */
final class Onboarding {
    private static final int WELCOME = 0, LANGUAGE = 1, DATA = 2, HEAT = 3, GPU = 4, CONTROLS = 5, DONE = 6, STEPS = 7;
    private final LauncherActivity a;
    private final FrameLayout overlay;
    private final FrameLayout stepHost;
    private int step;
    // Data step views, updated from the launcher's refresh/import progress.
    private TextView dataStatus, dataProgress, driverStatus;
    private Bar dataBar;
    private Btn next, pick;

    static boolean needed(LauncherActivity a) { return !a.prefs().getBoolean("onboardingDone", false); }

    Onboarding(LauncherActivity a) {
        this.a = a;
        overlay = new FrameLayout(a);
        overlay.setBackgroundColor(BG);
        overlay.setClickable(true);  // keep touches off the launcher underneath
        overlay.setFitsSystemWindows(true);
        stepHost = new FrameLayout(a);
        overlay.addView(stepHost, new FrameLayout.LayoutParams(-1, -1));
        step = Math.max(0, Math.min(STEPS - 1, a.prefs().getInt("onboardingStep", 0)));
    }

    View view() { return overlay; }
    boolean visible() { return overlay.getParent() != null && overlay.getVisibility() == View.VISIBLE; }

    void show() { render(); }

    private void go(int to) {
        step = Math.max(0, Math.min(STEPS - 1, to));
        a.prefs().edit().putInt("onboardingStep", step).apply();
        render();
    }

    /** Back: previous step; false on the first step (the activity then closes). */
    boolean back() {
        if (step == WELCOME) return false;
        go(step - 1);
        return true;
    }

    private void finish() {
        a.prefs().edit().putBoolean("onboardingDone", true).remove("onboardingStep").apply();
        overlay.setVisibility(View.GONE);
        a.refresh();
    }

    private void render() {
        stepHost.removeAllViews();
        dataStatus = dataProgress = driverStatus = null;
        dataBar = null;
        stepHost.addView(new Backdrop(a, step == WELCOME || step == DONE ? Backdrop.CITY : Backdrop.HORIZON, 0), new FrameLayout.LayoutParams(-1, -1));
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(a, 32), dp(a, 28), dp(a, 32), dp(a, 24));
        ScrollView scroll = new ScrollView(a);
        scroll.addView(card);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(dp(a, 620), a.getResources().getDisplayMetrics().widthPixels - dp(a, 32)), -2, Gravity.CENTER_VERTICAL | Gravity.START);
        lp.leftMargin = dp(a, 16) + Math.max(0, (a.getResources().getDisplayMetrics().widthPixels - dp(a, 620)) / 8);
        stepHost.addView(scroll, lp);

        LinearLayout head = new LinearLayout(a);
        head.setGravity(Gravity.CENTER_VERTICAL);
        head.addView(wordmark(a, 15));
        View fill = new View(a);
        head.addView(fill, new LinearLayout.LayoutParams(0, 1, 1));
        if (step != WELCOME && step != DONE) head.addView(text(a, "Шаг " + step + " из " + (STEPS - 2), 12, MUTED, false));
        card.addView(head);
        card.addView(dots(), gap(14));

        Btn primary;
        switch (step) {
            case WELCOME:
                card.addView(kicker(a, "ДОБРО ПОЖАЛОВАТЬ"), gap(22));
                card.addView(title("Настроим всё за пару минут"), gap(6));
                card.addView(body("Приложение запускает версию игры для Xbox 360 прямо на Android. Файлы игры в приложение не входят — понадобится ваша копия.\n\nВсё, что выберете сейчас, можно поменять потом в разделе «Настройки»."), gap(12));
                primary = cta(a, "Начать", v -> go(LANGUAGE));
                break;
            case LANGUAGE:
                card.addView(kicker(a, "ЯЗЫК ИГРЫ"), gap(22));
                card.addView(title("На каком языке играть?"), gap(6));
                card.addView(body("Тексты, субтитры и озвучка — из вашей копии игры."), gap(8));
                card.addView(languageList(), gap(14));
                primary = cta(a, "Далее", v -> go(DATA));
                break;
            case DATA:
                card.addView(kicker(a, "ДАННЫЕ ИГРЫ"), gap(22));
                card.addView(title("Подключите свою копию"), gap(6));
                card.addView(body("Выберите файл образа диска (ISO) своей копии игры для Xbox 360. Приложение проверит версию и распакует данные в свою память — около 6 ГБ, файлы не покидают устройство. Если установку прервать, она продолжится с того же места."), gap(8));
                LinearLayout panel = new LinearLayout(a);
                panel.setOrientation(LinearLayout.VERTICAL);
                panel.setBackground(box(0xFFFFFFFF, PANEL_BORDER, 1, a));
                panel.setPadding(dp(a, 14), dp(a, 12), dp(a, 14), dp(a, 12));
                dataStatus = text(a, "", 14, INK, false);
                dataProgress = text(a, "", 13, MUTED, false);
                dataBar = new Bar(a);
                dataBar.setVisibility(View.GONE);
                dataProgress.setVisibility(View.GONE);
                panel.addView(dataStatus);
                panel.addView(dataProgress, gap(4));
                panel.addView(dataBar, gap(8));
                card.addView(panel, gap(14));
                pick = ghost(a, "Выбрать файл ISO…", v -> { android.net.Uri pending = a.pendingIso(); if (pending != null) a.installIso(pending); else a.pickIso(); });
                card.addView(pick, wrap(12));
                primary = next = cta(a, "Далее", v -> go(HEAT));
                updateData();
                break;
            case HEAT:
                card.addView(kicker(a, "ИЗОБРАЖЕНИЕ И НАГРЕВ"), gap(22));
                card.addView(title("Лимит кадров"), gap(6));
                card.addView(body("30 к/с — ровная картинка, устройство меньше греется и дольше держит заряд. Без лимита кадров больше, но телефон нагревается и со временем сбрасывает частоты."), gap(8));
                if (!a.prefs().contains("frameLimit")) a.prefs().edit().putInt("frameLimit", 30).putBoolean("frameLimit30Default", true).apply();
                int[] limits = {30, 60, 0};
                int current = a.prefs().getInt("frameLimit", 30), selected = 0;
                for (int i = 0; i < limits.length; ++i) if (limits[i] == current) selected = i;
                LinearLayout seg = segments(a, new String[] {"30 · рекомендуем", "60", "Без лимита"}, selected,
                    i -> a.prefs().edit().putInt("frameLimit", limits[i]).putBoolean("frameLimit30Default", true).apply());
                card.addView(seg, wrap(16));
                primary = cta(a, "Далее", v -> go(GPU));
                break;
            case GPU:
                card.addView(kicker(a, "ДРАЙВЕР GPU"), gap(22));
                card.addView(title("Драйвер видеокарты"), gap(6));
                card.addView(body("Обычно подходит драйвер устройства — шаг можно пропустить. На Snapdragon (Adreno) можно подложить свой драйвер Vulkan, например Mesa Turnip: файл .zip в формате AdrenoTools (meta.json и библиотека внутри). Если со своим драйвером игра не запускается, верните системный в «Настройках»."), gap(8));
                LinearLayout driverPanel = new LinearLayout(a);
                driverPanel.setOrientation(LinearLayout.VERTICAL);
                driverPanel.setBackground(box(0xFFFFFFFF, PANEL_BORDER, 1, a));
                driverPanel.setPadding(dp(a, 14), dp(a, 12), dp(a, 14), dp(a, 12));
                driverStatus = text(a, "", 14, INK, false);
                driverPanel.addView(driverStatus);
                card.addView(driverPanel, gap(14));
                card.addView(ghost(a, "Выбрать драйвер (.zip)…", v -> a.pickDriver()), wrap(12));
                driverChanged(null);
                primary = cta(a, "Далее", v -> go(CONTROLS));
                break;
            case CONTROLS:
                card.addView(kicker(a, "УПРАВЛЕНИЕ"), gap(22));
                card.addView(title("Геймпад и пауза"), gap(6));
                card.addView(body("Лучше всего играть с геймпадом Xbox или любым Bluetooth-геймпадом — подключите его в настройках Android.\n\nМеню паузы во время игры: кнопка «Назад», долгое нажатие View (Select), View + Start или значок ≡ в углу экрана. Оттуда можно продолжить, выйти в лаунчер или отправить логи."), gap(8));
                primary = cta(a, "Далее", v -> go(DONE));
                break;
            default:
                card.addView(kicker(a, "ГОТОВО"), gap(22));
                card.addView(title("Можно играть"), gap(6));
                boolean ready = a.missing().isEmpty();
                card.addView(body(ready
                    ? "Перед каждым запуском приложение делает копию сохранений Xbox. Язык, лимит кадров и остальное — в разделе «Настройки»."
                    : "Данные игры ещё не подключены — это можно сделать в разделе «Данные игры». Остальные настройки сохранены."), gap(8));
                primary = cta(a, ready ? "▶  Играть" : "Перейти в лаунчер", v -> { finish(); if (a.missing().isEmpty()) a.launch(); });
                break;
        }
        LinearLayout buttons = new LinearLayout(a);
        buttons.setGravity(Gravity.CENTER_VERTICAL);
        buttons.addView(primary);
        if (step != WELCOME) {
            TextView backLink = text(a, "Назад", 14, MUTED, false);
            backLink.setPadding(dp(a, 20), dp(a, 10), dp(a, 12), dp(a, 10));
            backLink.setOnClickListener(v -> back());
            buttons.addView(backLink);
        }
        if (step == DONE && a.missing().isEmpty()) {
            View spacer = new View(a);
            buttons.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1));
            TextView launcher = text(a, "В лаунчер", 14, MUTED, false);
            launcher.setPadding(dp(a, 12), dp(a, 10), 0, dp(a, 10));
            launcher.setOnClickListener(v -> finish());
            buttons.addView(launcher);
        }
        if (step == DATA) {
            View spacer = new View(a);
            buttons.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1));
            TextView later = text(a, "Сделаю позже", 14, MUTED, false);
            later.setPadding(dp(a, 12), dp(a, 10), 0, dp(a, 10));
            later.setOnClickListener(v -> go(HEAT));
            buttons.addView(later);
        }
        card.addView(buttons, gap(24));
    }

    /** Called by the launcher on refresh and import progress. */
    void updateData() {
        if (dataStatus == null) return;
        List<String> missing = a.missing();
        if (a.isImporting()) {
            dataStatus.setText("Установка… Можно свернуть приложение, но не закрывайте его.");
            next.setEnabled(false);
            pick.setEnabled(false);
            return;
        }
        pick.setEnabled(true);
        pick.setText(a.pendingIso() != null ? "Продолжить установку" : missing.isEmpty() ? "Установить заново из ISO…" : "Выбрать файл ISO…");
        next.setEnabled(missing.isEmpty());
        dataStatus.setText(missing.isEmpty() ? "✓ Данные игры на месте" : "Не хватает: " + String.join(", ", missing));
        dataStatus.setTextColor(missing.isEmpty() ? OK : INK);
    }

    /** Called by the launcher after a driver import (message) or a reset (null). */
    void driverChanged(String message) {
        if (driverStatus == null) return;
        GpuDriver.Info info = GpuDriver.current(a, a.prefs());
        String state = info == null ? "Сейчас: системный драйвер устройства" : "✓ Свой драйвер: " + info.label();
        driverStatus.setText(message == null ? state : message + "\n" + state);
        driverStatus.setTextColor(info != null ? OK : INK);
    }

    void progress(String text, float fraction) {
        if (dataProgress == null) return;
        dataProgress.setText(text);
        dataProgress.setVisibility(View.VISIBLE);
        dataBar.setVisibility(View.VISIBLE);
        dataBar.set(fraction);
    }

    void importFinished(String message) {
        if (dataProgress != null) { dataProgress.setText(message); dataProgress.setVisibility(View.VISIBLE); }
        updateData();
    }

    private LinearLayout languageList() {
        LinearLayout grid = new LinearLayout(a);
        grid.setOrientation(LinearLayout.VERTICAL);
        int current = a.prefs().getInt("language", LauncherActivity.defaultLanguage());
        if (!a.prefs().contains("language")) a.prefs().edit().putInt("language", current).apply();
        TextView[] items = new TextView[LauncherActivity.LANGUAGE_NAMES.length];
        LinearLayout row = null;
        for (int i = 0; i < items.length; ++i) {
            if (i % 3 == 0) { row = new LinearLayout(a); grid.addView(row, gap(i == 0 ? 0 : 8)); }
            final int index = i;
            TextView t = text(a, LauncherActivity.LANGUAGE_NAMES[i], 15, INK, false);
            t.setGravity(Gravity.CENTER);
            t.setPadding(dp(a, 8), dp(a, 10), dp(a, 8), dp(a, 10));
            items[i] = t;
            t.setOnClickListener(v -> {
                a.prefs().edit().putInt("language", LauncherActivity.LANGUAGE_CODES[index]).apply();
                for (int k = 0; k < items.length; ++k) paintChoice(items[k], k == index);
            });
            paintChoice(t, LauncherActivity.LANGUAGE_CODES[i] == current);
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, -2, 1);
            if (i % 3 != 0) p.leftMargin = dp(a, 8);
            row.addView(t, p);
        }
        return grid;
    }

    private void paintChoice(TextView t, boolean on) {
        t.setBackground(box(on ? INK : Color.WHITE, INK, 1, a));
        t.setTextColor(on ? Color.WHITE : INK);
    }

    /** Step dots: done and current in red, rest grey. */
    private LinearLayout dots() {
        LinearLayout row = new LinearLayout(a);
        for (int i = 0; i < STEPS; ++i) {
            View d = new View(a);
            d.setBackgroundColor(i <= step ? RED : PANEL_BORDER);
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, dp(a, 3), 1);
            if (i > 0) p.leftMargin = dp(a, 4);
            row.addView(d, p);
        }
        return row;
    }

    private TextView title(String s) { return text(a, s, 26, INK, true); }
    private TextView body(String s) { TextView t = text(a, s, 15, 0xFF3A444C, false); t.setLineSpacing(0, 1.15f); return t; }
    private LinearLayout.LayoutParams wrap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.topMargin = dp(a, topDp);
        return p;
    }
    private LinearLayout.LayoutParams gap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
        p.topMargin = dp(a, topDp);
        return p;
    }
}
