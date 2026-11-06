package org.mirrorsedge.research;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.*;
import static org.mirrorsedge.research.LauncherUi.*;
import java.io.File;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.*;

/**
 * Start screen of the PC version, in the style of the Xbox launcher: play, settings that the game
 * has no good place for or that need a restart, achievements and a check of the files. Graphics
 * quality stays in the game (Options > Video): it works there and applies at once.
 * The settings live in the preferences file "pc"; PcGameActivity reads them at the start.
 */
public class PcLauncherActivity extends Activity {
    private static final String[] PAGES = {"Играть", "Настройки", "Достижения", "Файлы"};
    static final String[][] LANGUAGES = {{"int", "English"}, {"rus", "Русский"}, {"deu", "Deutsch"},
        {"fra", "Français"}, {"ita", "Italiano"}, {"esn", "Español"}, {"pol", "Polski"}, {"cze", "Čeština"},
        {"hun", "Magyar"}, {"por", "Português"}, {"jpn", "日本語"}, {"kor", "한국어"}, {"chs", "简体中文"}, {"cht", "繁體中文"}};
    static final int[] FPS = {30, 62, 90, 120};
    static final int[] CPUS = {1, 2, 4};
    static final String[] SCALES = {"2/3", "3/4", "Полное", "1/2"};
    private static final String[] TOUCH = {"Авто", "Всегда", "Нет"};
    /** ETdAchievements in the order of the game; the wording is ours, the PC game has no such texts. */
    private static final String[][] ACHIEVEMENTS = {
        {"1", "Глава 1", "Пройти главу 1"}, {"2", "Глава 2", "Пройти главу 2"}, {"3", "Глава 3", "Пройти главу 3"},
        {"4", "Глава 4", "Пройти главу 4"}, {"5", "Глава 5", "Пройти главу 5"}, {"6", "Глава 6", "Пройти главу 6"},
        {"7", "Глава 7", "Пройти главу 7"}, {"8", "Глава 8", "Пройти главу 8"}, {"9", "Глава 9", "Пройти главу 9"},
        {"26", "Вся история", "Пройти игру"}, {"27", "Сложный путь", "Пройти игру на сложном уровне"},
        {"11", "Обучение", "Пройти обучение"}, {"10", "На время", "Пройти испытание на время"},
        {"12", "Все участки", "Открыть все участки испытаний на время"},
        {"13", "Рейтинг 1", "Достичь первого рейтинга мастерства"}, {"14", "Рейтинг 2", "Достичь второго рейтинга мастерства"},
        {"15", "Рейтинг 3", "Достичь третьего рейтинга мастерства"},
        {"16", "Скоростной забег 1", "Пройти скоростной забег 1"}, {"17", "Скоростной забег 2", "Пройти скоростной забег 2"},
        {"18", "Скоростной забег 3", "Пройти скоростной забег 3"}, {"19", "Скоростной забег 4", "Пройти скоростной забег 4"},
        {"20", "Скоростной забег 5", "Пройти скоростной забег 5"}, {"21", "Скоростной забег 6", "Пройти скоростной забег 6"},
        {"22", "Скоростной забег 7", "Пройти скоростной забег 7"}, {"23", "Скоростной забег 8", "Пройти скоростной забег 8"},
        {"24", "Скоростной забег 9", "Пройти скоростной забег 9"}, {"25", "Скоростной забег 10", "Пройти скоростной забег 10"},
        {"28", "Мимо пуль", "Пройти уровень, не попав под выстрел"}, {"29", "Мягкая посадка", "Пройти уровень без тяжёлых приземлений"},
        {"30", "Без оружия", "Пройти уровень, не сделав ни выстрела"}, {"31", "Ни царапины", "Пройти игру без ранений от пуль"},
        {"32", "На полной скорости", "Долго держать полную скорость"}, {"33", "Удар с воздуха", "Победить врага ударом в прыжке"},
        {"34", "Обезоруживание", "Несколько раз обезоружить врагов"}, {"35", "Ближний бой", "Победить врагов в ближнем бою"},
        {"36", "Разворот", "Выполнить разворот-насмешку"}, {"37", "Сверху", "Приземлиться на голову врага"},
        {"38", "Связка 1", "Прыжок, подъём и подкат одной цепочкой"}, {"39", "Связка 2", "Бег по стене и прыжок с разворотом"},
        {"40", "Связка 3", "Бег по стене, поворот, прыжок и разворот"}, {"41", "Связка 4", "Бег по стене, прыжок, подъём и перекат"},
        {"42", "Все сумки", "Найти все спрятанные сумки"},
    };

    private final List<TextView> navItems = new ArrayList<>();
    private final List<View> pages = new ArrayList<>();
    private TextView playMeta, filesText, achCount;
    private LinearLayout achList;
    private LauncherUi.Btn play;
    private int navWidth;

    private SharedPreferences prefs() { return getSharedPreferences("pc", MODE_PRIVATE); }
    private File pcDir() { return new File(getExternalFilesDir(null), "pc"); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        boolean wide = getResources().getConfiguration().screenWidthDp >= 600;
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(wide ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        root.setBackgroundColor(BG);
        root.setFitsSystemWindows(true);
        LinearLayout nav = new LinearLayout(this);
        nav.setOrientation(wide ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        nav.setBackgroundColor(0xEBF7F8F9);
        nav.setPadding(dp(this, 22), dp(this, 22), wide ? 0 : dp(this, 12), dp(this, wide ? 18 : 6));
        TextView brand = wordmark(this, 18);
        brand.setSingleLine(true);
        brand.setPadding(0, 0, dp(this, 18), dp(this, wide ? 22 : 0));
        nav.addView(brand);
        for (int i = 0; i < PAGES.length; ++i) {
            final int index = i;
            TextView item = text(this, PAGES[i], 15, NAV, false);
            item.setPadding(dp(this, 14), dp(this, 10), dp(this, 14), dp(this, 10));
            item.setClickable(true);
            item.setFocusable(true);
            item.setOnClickListener(v -> showPage(index));
            navItems.add(item);
            nav.addView(item);
        }
        navWidth = wide ? dp(this, 230) : 0;
        if (wide) root.addView(nav, new LinearLayout.LayoutParams(navWidth, -1));
        else {
            HorizontalScrollView strip = new HorizontalScrollView(this);
            strip.setHorizontalScrollBarEnabled(false);
            strip.addView(nav);
            root.addView(strip, new LinearLayout.LayoutParams(-1, -2));
        }
        FrameLayout content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(wide ? 0 : -1, wide ? -1 : 0, 1));
        pages.add(playPage());
        pages.add(settingsPage());
        pages.add(achievementsPage());
        pages.add(filesPage());
        for (View page : pages) content.addView(page, new FrameLayout.LayoutParams(-1, -1));
        setContentView(root);
        showPage(state == null ? 0 : state.getInt("page", 0));
    }

    private int currentPage;

    @Override protected void onSaveInstanceState(Bundle out) { super.onSaveInstanceState(out); out.putInt("page", currentPage); }

    @Override protected void onResume() { super.onResume(); refresh(); }

    private void showPage(int index) {
        currentPage = index;
        for (int i = 0; i < pages.size(); ++i) {
            boolean on = i == index;
            pages.get(i).setVisibility(on ? View.VISIBLE : View.GONE);
            TextView item = navItems.get(i);
            item.setTextColor(on ? INK : NAV);
            item.setTypeface(on ? MEDIUM : android.graphics.Typeface.DEFAULT);
            item.setBackgroundColor(on ? 0x99FFFFFF : 0);
        }
    }

    private FrameLayout page(int backdrop, View body, boolean scroll) {
        FrameLayout frame = new FrameLayout(this);
        frame.addView(new LauncherUi.Backdrop(this, backdrop, navWidth), new FrameLayout.LayoutParams(-1, -1));
        View inner = body;
        if (scroll) {
            ScrollView sv = new ScrollView(this);
            sv.setFillViewport(true);
            sv.addView(body);
            inner = sv;
        }
        frame.addView(inner, new FrameLayout.LayoutParams(-1, -1));
        return frame;
    }

    private LinearLayout column(int leftDp) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(this, leftDp), dp(this, 24), dp(this, 28), dp(this, 24));
        return c;
    }

    private LinearLayout.LayoutParams gap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.topMargin = dp(this, topDp);
        return p;
    }

    private LinearLayout label(String title, String hint) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.addView(text(this, title, 15, INK, false));
        l.addView(text(this, hint, 12, MUTED, false));
        return l;
    }

    private View playPage() {
        LinearLayout c = column(28);
        c.setGravity(Gravity.BOTTOM | Gravity.START);
        playMeta = text(this, "", 14, MUTED, false);
        play = cta(this, "▶  Играть", v -> launch());
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(this, 20), dp(this, 16), dp(this, 28), dp(this, 20));
        card.addView(kicker(this, "ПК-ВЕРСИЯ"));
        card.addView(text(this, "Mirror's Edge", 30, INK, true), gap(6));
        card.addView(playMeta, gap(6));
        card.addView(play, gap(16));
        c.addView(card, new LinearLayout.LayoutParams(-2, -2));
        return page(LauncherUi.Backdrop.CITY, c, false);
    }

    private void launch() {
        startActivity(new Intent(this, PcGameActivity.class));
    }

    private View settingsPage() {
        LinearLayout c = column(56);
        c.addView(kicker(this, "ИГРА"));
        String[] names = new String[LANGUAGES.length];
        int selected = 0;
        String current = prefs().getString("lang", defaultLanguage());
        for (int i = 0; i < LANGUAGES.length; ++i) {
            names[i] = LANGUAGES[i][1];
            if (LANGUAGES[i][0].equals(current)) selected = i;
        }
        Spinner language = new Spinner(this, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, names);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        language.setAdapter(adapter);
        language.setBackground(box(Color.WHITE, INK, 1, this));
        language.setPadding(dp(this, 6), dp(this, 6), dp(this, 6), dp(this, 6));
        language.setSelection(selected);
        language.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (view instanceof TextView) ((TextView) view).setTextColor(INK);
                prefs().edit().putString("lang", LANGUAGES[position][0]).apply();
                refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        c.addView(row(this, text(this, "Язык игры", 15, INK, false), language));

        String[] fpsNames = new String[FPS.length];
        for (int i = 0; i < FPS.length; ++i) fpsNames[i] = String.valueOf(FPS[i]);
        c.addView(row(this, label("Лимит кадров", "62 — как в игре, 30 — ровнее и холоднее"),
            segments(this, fpsNames, Math.max(0, indexOf(FPS, prefs().getInt("fps", 62))), i -> {
                prefs().edit().putInt("fps", FPS[i]).apply();
                refresh();
            })));

        final TextView fovLabel = text(this, "", 15, INK, false);
        LinearLayout fovText = new LinearLayout(this);
        fovText.setOrientation(LinearLayout.VERTICAL);
        fovText.addView(fovLabel);
        fovText.addView(text(this, "Шире — больше видно по краям, камера быстрее", 12, MUTED, false));
        SeekBar fov = new SeekBar(this);
        fov.setMax(140 - 60);
        fov.setProgress(Math.max(0, prefs().getInt("fov", 0) - 60));
        SeekBar.OnSeekBarChangeListener fovChange = new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int v, boolean user) {
                fovLabel.setText(v == 0 ? "Поле зрения: как в игре" : "Поле зрения: " + (60 + v) + "°");
                if (user) { prefs().edit().putInt("fov", v == 0 ? 0 : 60 + v).apply(); refresh(); }
            }
            public void onStartTrackingTouch(SeekBar b) {}
            public void onStopTrackingTouch(SeekBar b) {}
        };
        fov.setOnSeekBarChangeListener(fovChange);
        fovChange.onProgressChanged(fov, fov.getProgress(), false);
        c.addView(fovText, gap(16));
        c.addView(fov, new LinearLayout.LayoutParams(-1, -2));

        c.addView(kicker(this, "УПРАВЛЕНИЕ"), gap(22));
        c.addView(row(this, label("Экранный геймпад", "Авто — только без геймпада и клавиатуры"),
            segments(this, TOUCH, prefs().getInt("touch", 0), i -> prefs().edit().putInt("touch", i).apply())));

        c.addView(kicker(this, "ПРОИЗВОДИТЕЛЬНОСТЬ"), gap(22));
        c.addView(row(this, label("Начальное разрешение", "Доля экрана. Потом его можно менять в игре"),
            segments(this, SCALES, prefs().getInt("scale", 0), i -> prefs().edit().putInt("scale", i).apply())));
        String[] cpuNames = new String[CPUS.length];
        for (int i = 0; i < CPUS.length; ++i) cpuNames[i] = String.valueOf(CPUS[i]);
        c.addView(row(this, label("Ядра для игры", "4 — отдельный поток отрисовки, быстрее"),
            segments(this, cpuNames, Math.max(0, indexOf(CPUS, prefs().getInt("cpus", 4))), i -> prefs().edit().putInt("cpus", CPUS[i]).apply())));
        c.addView(text(this, "Качество графики, сглаживание и PhysX настраиваются в самой игре: Options → Video.\n"
            + "Язык и лимит кадров записываются в настройки игры, поэтому при самом первом запуске они применятся только со второго.",
            12, MUTED, false), gap(18));
        return page(LauncherUi.Backdrop.STRIPE, c, true);
    }

    private static int indexOf(int[] a, int v) { for (int i = 0; i < a.length; i++) if (a[i] == v) return i; return -1; }

    /** The system language when the game has it, English otherwise. */
    static String defaultLanguage() {
        String system = Locale.getDefault().getISO3Language();
        if ("rus".equals(system)) return "rus";
        if ("deu".equals(system)) return "deu";
        if ("fra".equals(system)) return "fra";
        if ("ita".equals(system)) return "ita";
        if ("spa".equals(system)) return "esn";
        return "int";
    }

    private View achievementsPage() {
        LinearLayout c = column(56);
        c.addView(kicker(this, "ДОСТИЖЕНИЯ"));
        achCount = text(this, "", 30, INK, true);
        c.addView(achCount, gap(6));
        achList = new LinearLayout(this);
        achList.setOrientation(LinearLayout.VERTICAL);
        c.addView(achList, new LinearLayout.LayoutParams(-1, -2));
        c.addView(text(this, "Логика достижений — из самой игры, а названия и описания написаны для этого приложения: "
            + "в ПК-версии их текстов нет.", 12, MUTED, false), gap(14));
        return page(LauncherUi.Backdrop.CORNER, c, true);
    }

    private Map<Integer, Long> unlocked() {
        Map<Integer, Long> out = new HashMap<>();
        File f = new File(pcDir(), "achievements.txt");
        if (!f.isFile()) return out;
        try {
            for (String line : Files.readAllLines(f.toPath())) {
                String[] p = line.trim().split("\\s+");
                if (p.length == 2) out.put(Integer.valueOf(p[0]), Long.valueOf(p[1]));
            }
        } catch (java.io.IOException | NumberFormatException ignored) { /* a torn last line: the rest stays */ }
        return out;
    }

    private void showAchievements() {
        Map<Integer, Long> got = unlocked();
        achCount.setText(got.size() + " из " + ACHIEVEMENTS.length);
        achList.removeAllViews();
        SimpleDateFormat date = new SimpleDateFormat("dd.MM.yyyy", Locale.getDefault());
        for (String[] a : ACHIEVEMENTS) {
            Long at = got.get(Integer.valueOf(a[0]));
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(dp(this, 14), dp(this, 9), dp(this, 14), dp(this, 9));
            card.setBackground(box(0xF2FFFFFF, at != null ? RED : PANEL_BORDER, 1, this));
            card.addView(text(this, a[1], 15, at != null ? INK : NAV, true));
            card.addView(text(this, at != null ? a[2] + " · получено " + date.format(new Date(at * 1000)) : a[2], 13, at != null ? MUTED : LOCKED, false));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
            p.bottomMargin = dp(this, 8);
            achList.addView(card, p);
        }
    }

    private View filesPage() {
        LinearLayout c = column(28);
        c.addView(kicker(this, "ФАЙЛЫ ИГРЫ"));
        filesText = text(this, "", 14, INK, false);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(box(0xE6FFFFFF, PANEL_BORDER, 1, this));
        panel.setPadding(dp(this, 16), dp(this, 12), dp(this, 16), dp(this, 12));
        panel.addView(filesText);
        c.addView(panel, gap(10));
        c.addView(text(this, "Файлы игры копируются с компьютера скриптом pc-device-setup.ps1. В приложение они не входят.", 12, MUTED, false), gap(12));
        return page(LauncherUi.Backdrop.HORIZON, c, true);
    }

    private boolean ready() {
        File pc = pcDir(), lib = new File(getApplicationInfo().nativeLibraryDir);
        return new File(lib, "libmepc.so").isFile() && new File(lib, "libdxvk_d3d9.so").isFile()
            && new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile()
            && new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()
            && new File(pc, "deps/msvcr80.dll").isFile() && new File(pc, "deps/msvcp80.dll").isFile()
            && new File(pc, "deps/rebased").isDirectory();
    }

    private static String mark(boolean ok) { return ok ? "✓  " : "✗  "; }

    private String filesReport() {
        File pc = pcDir(), lib = new File(getApplicationInfo().nativeLibraryDir);
        return "Папка: " + pc
            + "\n" + mark(new File(lib, "libmepc.so").isFile()) + "libmepc.so (только личная сборка)"
            + "\n" + mark(new File(lib, "libdxvk_d3d9.so").isFile()) + "libdxvk_d3d9.so"
            + "\n" + mark(new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile()) + "MirrorsEdge/Binaries/MirrorsEdge.exe"
            + "\n" + mark(new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()) + "MirrorsEdge/TdGame/CookedPC"
            + "\n" + mark(new File(pc, "deps/msvcr80.dll").isFile() && new File(pc, "deps/msvcp80.dll").isFile()) + "deps/msvcr80.dll, msvcp80.dll"
            + "\n" + mark(new File(pc, "deps/rebased").isDirectory()) + "deps/rebased";
    }

    private void refresh() {
        boolean ok = ready();
        play.setEnabled(ok);
        play.setAlpha(ok ? 1f : 0.4f);
        int fov = prefs().getInt("fov", 0);
        String lang = prefs().getString("lang", defaultLanguage());
        String name = lang;
        for (String[] l : LANGUAGES) if (l[0].equals(lang)) name = l[1];
        playMeta.setText(ok ? name + " · до " + prefs().getInt("fps", 62) + " кадров/с · " + (fov > 0 ? "поле зрения " + fov + "°" : "поле зрения как в игре")
            : "Не хватает файлов игры — смотрите раздел «Файлы»");
        filesText.setText(filesReport());
        showAchievements();
    }
}
