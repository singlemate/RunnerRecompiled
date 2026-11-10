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
 * Start screen: play, settings that the game
 * has no good place for or that need a restart, achievements and a check of the files. Graphics
 * quality stays in the game (Options > Video): it works there and applies at once.
 * The settings live in the preferences file "pc"; PcGameActivity reads them at the start.
 */
public class PcLauncherActivity extends Activity {
    private static final String[] PAGES = {Tr.t("Играть"), Tr.t("Настройки"), Tr.t("Достижения"), Tr.t("Файлы")};
    static final String[][] LANGUAGES = {{"int", "English"}, {"rus", "Русский"}, {"deu", "Deutsch"},
        {"fra", "Français"}, {"ita", "Italiano"}, {"esn", "Español"}, {"pol", "Polski"}, {"cze", "Čeština"},
        {"hun", "Magyar"}, {"por", "Português"}, {"jpn", "日本語"}, {"kor", "한국어"}, {"chs", "简体中文"}, {"cht", "繁體中文"}};
    static final int[] FPS = {30, 62, 90, 120};
    static final int[] CPUS = {1, 2, 4};
    static final String[] SCALES = {"2/3", "3/4", Tr.t("Полное"), "1/2"};
    private static final String[] TOUCH = {Tr.t("Авто"), Tr.t("Всегда"), Tr.t("Нет")};
    /** ETdAchievements in the order of the game; the wording is ours, the PC game has no such texts. */
    private static final String[][] ACHIEVEMENTS = {
        {"1", Tr.t("Глава 1"), Tr.t("Пройти главу 1")}, {"2", Tr.t("Глава 2"), Tr.t("Пройти главу 2")}, {"3", Tr.t("Глава 3"), Tr.t("Пройти главу 3")},
        {"4", Tr.t("Глава 4"), Tr.t("Пройти главу 4")}, {"5", Tr.t("Глава 5"), Tr.t("Пройти главу 5")}, {"6", Tr.t("Глава 6"), Tr.t("Пройти главу 6")},
        {"7", Tr.t("Глава 7"), Tr.t("Пройти главу 7")}, {"8", Tr.t("Глава 8"), Tr.t("Пройти главу 8")}, {"9", Tr.t("Глава 9"), Tr.t("Пройти главу 9")},
        {"26", Tr.t("Вся история"), Tr.t("Пройти игру")}, {"27", Tr.t("Сложный путь"), Tr.t("Пройти игру на сложном уровне")},
        {"11", Tr.t("Обучение"), Tr.t("Пройти обучение")}, {"10", Tr.t("На время"), Tr.t("Пройти испытание на время")},
        {"12", Tr.t("Все участки"), Tr.t("Открыть все участки испытаний на время")},
        {"13", Tr.t("Рейтинг 1"), Tr.t("Достичь первого рейтинга мастерства")}, {"14", Tr.t("Рейтинг 2"), Tr.t("Достичь второго рейтинга мастерства")},
        {"15", Tr.t("Рейтинг 3"), Tr.t("Достичь третьего рейтинга мастерства")},
        {"16", Tr.t("Скоростной забег 1"), Tr.t("Пройти скоростной забег 1")}, {"17", Tr.t("Скоростной забег 2"), Tr.t("Пройти скоростной забег 2")},
        {"18", Tr.t("Скоростной забег 3"), Tr.t("Пройти скоростной забег 3")}, {"19", Tr.t("Скоростной забег 4"), Tr.t("Пройти скоростной забег 4")},
        {"20", Tr.t("Скоростной забег 5"), Tr.t("Пройти скоростной забег 5")}, {"21", Tr.t("Скоростной забег 6"), Tr.t("Пройти скоростной забег 6")},
        {"22", Tr.t("Скоростной забег 7"), Tr.t("Пройти скоростной забег 7")}, {"23", Tr.t("Скоростной забег 8"), Tr.t("Пройти скоростной забег 8")},
        {"24", Tr.t("Скоростной забег 9"), Tr.t("Пройти скоростной забег 9")}, {"25", Tr.t("Скоростной забег 10"), Tr.t("Пройти скоростной забег 10")},
        {"28", Tr.t("Мимо пуль"), Tr.t("Пройти уровень, не попав под выстрел")}, {"29", Tr.t("Мягкая посадка"), Tr.t("Пройти уровень без тяжёлых приземлений")},
        {"30", Tr.t("Без оружия"), Tr.t("Пройти уровень, не сделав ни выстрела")}, {"31", Tr.t("Ни царапины"), Tr.t("Пройти игру без ранений от пуль")},
        {"32", Tr.t("На полной скорости"), Tr.t("Долго держать полную скорость")}, {"33", Tr.t("Удар с воздуха"), Tr.t("Победить врага ударом в прыжке")},
        {"34", Tr.t("Обезоруживание"), Tr.t("Несколько раз обезоружить врагов")}, {"35", Tr.t("Ближний бой"), Tr.t("Победить врагов в ближнем бою")},
        {"36", Tr.t("Разворот"), Tr.t("Выполнить разворот-насмешку")}, {"37", Tr.t("Сверху"), Tr.t("Приземлиться на голову врага")},
        {"38", Tr.t("Связка 1"), Tr.t("Прыжок, подъём и подкат одной цепочкой")}, {"39", Tr.t("Связка 2"), Tr.t("Бег по стене и прыжок с разворотом")},
        {"40", Tr.t("Связка 3"), Tr.t("Бег по стене, поворот, прыжок и разворот")}, {"41", Tr.t("Связка 4"), Tr.t("Бег по стене, прыжок, подъём и перекат")},
        {"42", Tr.t("Все сумки"), Tr.t("Найти все спрятанные сумки")},
    };

    private final List<TextView> navItems = new ArrayList<>();
    private final List<View> pages = new ArrayList<>();
    private TextView playMeta, filesText, achCount;
    private LinearLayout achList;
    private LauncherUi.Btn play;
    private int navWidth;

    SharedPreferences prefs() { return getSharedPreferences("pc", MODE_PRIVATE); }
    File pcDir() { return new File(getExternalFilesDir(null), "pc"); }

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
        FrameLayout host = new FrameLayout(this);
        host.addView(root, new FrameLayout.LayoutParams(-1, -1));
        setup = new PcSetup(this);
        host.addView(setup.view(), new FrameLayout.LayoutParams(-1, -1));
        setContentView(host);
        if (state == null && getIntent().getBooleanExtra("receiveFromPc", false)) {
            // Started by RunnerBuilder (adb shell am start): take the game files from the computer.
            setup.showData();
            setup.receive(getIntent().getBooleanExtra("receiveTest", false));
        } else if (!prefs().getBoolean("setupDone", false) && !PcSetup.filesReady(pcDir())) setup.show(0);
        checkForUpdate();
        showPage(state == null ? 0 : state.getInt("page", 0));
    }

    private int currentPage;
    private PcSetup setup;

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_DRIVER) {
            if (result == RESULT_OK && data != null && data.getData() != null) installDriver(data.getData());
            return;
        }
        if ((request == PcSetup.PICK_GAME || request == PcSetup.PICK_ZIP) && result == RESULT_OK && data != null && data.getData() != null)
            setup.picked(data.getData(), request == PcSetup.PICK_ZIP);
    }

    /** Public file with the newest version number; the APK itself is handed over privately. */
    private static final String UPDATE_URL = "https://raw.githubusercontent.com/singlemate/RunnerRecompiled/main/version-pc.json";

    /** At most once a day, and one notice per version. Any failure is silent: this is only a hint. */
    private void checkForUpdate() {
        String test = getIntent().getStringExtra("updateJson");
        long now = System.currentTimeMillis();
        if (test == null && now - prefs().getLong("updateCheckedAt", 0) < 24 * 3600 * 1000L) return;
        prefs().edit().putLong("updateCheckedAt", now).apply();
        new Thread(() -> {
            try {
                String text = test != null ? test : readUpdateFile();
                org.json.JSONObject latest = new org.json.JSONObject(text);
                int code = latest.getInt("versionCode");
                String name = latest.getString("versionName"), notes = latest.optString("notes", "");
                android.content.pm.PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
                if (code <= info.versionCode || prefs().getInt("updateNotified", 0) >= code) return;
                runOnUiThread(() -> {
                    if (isFinishing()) return;
                    prefs().edit().putInt("updateNotified", code).apply();
                    new android.app.AlertDialog.Builder(this)
                        .setTitle(Tr.t("Доступна версия ") + name)
                        .setMessage(Tr.t("Установлена версия ") + info.versionName + "."
                            + (notes.isEmpty() ? "" : "\n\n" + notes)
                            + Tr.t("\n\nСоберите новый APK через RunnerBuilder. Он ставится поверх, файлы игры и сохранения остаются."))
                        .setPositiveButton(Tr.t("Понятно"), null)
                        .show();
                });
            } catch (Exception e) {
                android.util.Log.i("me_pc", "update check: " + e);
            }
        }, "update-check").start();
    }

    private static String readUpdateFile() throws java.io.IOException {
        java.net.HttpURLConnection c = (java.net.HttpURLConnection) new java.net.URL(UPDATE_URL).openConnection();
        c.setConnectTimeout(8000);
        c.setReadTimeout(8000);
        try (java.io.InputStream in = c.getInputStream(); java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream()) {
            byte[] buf = new byte[4096];
            int total = 0;
            for (int n; (n = in.read(buf)) > 0 && total < 65536; total += n) out.write(buf, 0, n);
            return out.toString("UTF-8");
        } finally {
            c.disconnect();
        }
    }

    private static final int PICK_DRIVER = 40;
    private TextView driverInfo;
    private View driverReset;

    /** System file picker for a GPU driver package (.zip, AdrenoTools format). */
    private void pickDriver() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, PICK_DRIVER);
    }

    /** Shows the chosen driver in the settings; message = import result or error, if any. */
    /** key in [SystemSettings], label. The values live in the game's own file, so the game's menu and these rows agree. */
    private static final String[][] GRAPHICS = {
        {"DynamicShadows", "Динамические тени"}, {"Bloom", "Свечение"}, {"DepthOfField", "Глубина резкости"},
        {"TdMotionBlur", "Размытие в движении"}, {"LensFlares", "Блики"}, {"Distortion", "Искажения"},
        {"TdSunHaze", "Дымка от солнца"}, {"StaticDecals", "Статические декали"}, {"DynamicDecals", "Динамические декали"},
    };
    private static final String[] ECONOMY = {"DepthOfField", "TdMotionBlur", "LensFlares", "Distortion", "TdSunHaze"};
    private static final int[] ANISOTROPY = {1, 2, 4, 8, 16};

    private static Map<String, String> iniSection(File ini, String section) {
        Map<String, String> values = new HashMap<>();
        try {
            byte[] raw = Files.readAllBytes(ini.toPath());
            boolean utf16 = raw.length >= 2 && (raw[0] & 0xFF) == 0xFF && (raw[1] & 0xFF) == 0xFE;
            String text = new String(raw, utf16 ? 2 : 0, raw.length - (utf16 ? 2 : 0),
                utf16 ? java.nio.charset.StandardCharsets.UTF_16LE : java.nio.charset.StandardCharsets.UTF_8);
            boolean inside = false;
            for (String line : text.split("\\r?\\n")) {
                String l = line.trim();
                if (l.startsWith("[")) inside = l.equalsIgnoreCase("[" + section + "]");
                else if (inside && l.indexOf('=') > 0) values.put(l.substring(0, l.indexOf('=')), l.substring(l.indexOf('=') + 1));
            }
        } catch (java.io.IOException e) {
            android.util.Log.i("me_pc", "settings: " + e);
        }
        return values;
    }

    private void addGraphics(LinearLayout c) {
        c.addView(kicker(this, Tr.t("ГРАФИКА")), gap(22));
        File ini = PcGameActivity.systemIni(new File(pcDir(), "user"));
        if (!ini.isFile()) {
            c.addView(text(this, Tr.t("Появятся после первого запуска игры."), 12, MUTED, false), gap(4));
            return;
        }
        Map<String, String> now = iniSection(ini, "SystemSettings");
        c.addView(row(this, label(Tr.t("Режим экономии"), Tr.t("Выключает самые тяжёлые эффекты, чтобы планшет меньше грелся")),
            ghost(this, Tr.t("Применить"), v -> {
                for (String key : ECONOMY) PcGameActivity.setIni(ini, "SystemSettings", key, "False");
                recreate();
            })));
        for (String[] g : GRAPHICS) {
            String key = g[0];
            if (!now.containsKey(key)) continue;
            c.addView(row(this, label(Tr.t(g[1]), ""), segments(this, new String[] {Tr.t("Выкл"), Tr.t("Вкл")},
                "true".equalsIgnoreCase(now.get(key)) ? 1 : 0, i -> PcGameActivity.setIni(ini, "SystemSettings", key, i == 1 ? "True" : "False"))));
        }
        if (now.containsKey("MaxAnisotropy")) {
            int current = 0;
            String[] names = new String[ANISOTROPY.length];
            for (int i = 0; i < names.length; i++) {
                names[i] = ANISOTROPY[i] + "×";
                if (String.valueOf(ANISOTROPY[i]).equals(now.get("MaxAnisotropy"))) current = i;
            }
            c.addView(row(this, label(Tr.t("Анизотропная фильтрация"), Tr.t("Чёткость текстур под углом")),
                segments(this, names, current, i -> PcGameActivity.setIni(ini, "SystemSettings", "MaxAnisotropy", String.valueOf(ANISOTROPY[i])))));
        }
    }

    /** The issue form with what the app already knows filled in (its fields are named in the form's ids). */
    private void reportBug(View ignored) {
        GpuDriver.Info driver = GpuDriver.current(this, prefs());
        String chip = android.os.Build.VERSION.SDK_INT >= 31
            ? android.os.Build.SOC_MANUFACTURER + " " + android.os.Build.SOC_MODEL : android.os.Build.HARDWARE;
        String settings = "lang " + prefs().getString("lang", defaultLanguage()) + ", fps " + prefs().getInt("fps", 62)
            + ", fov " + prefs().getInt("fov", 0) + ", scale " + prefs().getInt("scale", 0)
            + ", cpus " + prefs().getInt("cpus", 4) + ", upscaler " + prefs().getInt("upscaler", 0);
        android.net.Uri uri = android.net.Uri.parse("https://github.com/singlemate/RunnerRecompiled/issues/new").buildUpon()
            .appendQueryParameter("template", "bug_report.yml")
            .appendQueryParameter("version", SessionLog.version(this))
            .appendQueryParameter("device", android.os.Build.MANUFACTURER + " " + android.os.Build.MODEL)
            .appendQueryParameter("chip", chip)
            .appendQueryParameter("android", android.os.Build.VERSION.RELEASE + " (API " + android.os.Build.VERSION.SDK_INT + ")")
            .appendQueryParameter("driver", driver == null ? "System driver" : driver.label())
            .appendQueryParameter("settings", settings)
            .build();
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, uri));
        } catch (android.content.ActivityNotFoundException e) {
            android.widget.Toast.makeText(this, uri.toString(), android.widget.Toast.LENGTH_LONG).show();
        }
    }

    private void updateDriverInfo(String message) {
        GpuDriver.Info info = GpuDriver.current(this, prefs());
        String state = info == null ? Tr.t("Системный драйвер устройства") : Tr.t("Свой: ") + info.label();
        if (driverInfo != null) driverInfo.setText(message == null ? state : message + "\n" + state);
        if (driverReset != null) driverReset.setVisibility(info == null ? View.GONE : View.VISIBLE);
    }

    private void installDriver(android.net.Uri uri) {
        new Thread(() -> {
            String message;
            try {
                message = Tr.t("Драйвер установлен: ") + GpuDriver.install(this, prefs(), uri).label();
            } catch (java.io.IOException | RuntimeException e) {
                message = Tr.t("Драйвер не установлен: ") + e.getMessage();
            }
            final String text = message;
            runOnUiThread(() -> updateDriverInfo(text));
        }, "gpu-driver-import").start();
    }

    @Override public void onBackPressed() {
        if (setup.visible() && setup.back()) return;
        super.onBackPressed();
    }

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
        play = cta(this, Tr.t("▶  Играть"), v -> launch());
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(this, 20), dp(this, 16), dp(this, 28), dp(this, 20));
        card.addView(kicker(this, Tr.t("ПК-ВЕРСИЯ")));
        card.addView(text(this, "Mirror's Edge", 30, INK, true), gap(6));
        card.addView(playMeta, gap(6));
        card.addView(play, gap(16));
        c.addView(card, new LinearLayout.LayoutParams(-2, -2));
        return page(LauncherUi.Backdrop.CITY, c, false);
    }

    private void launch() {
        PcSetup.refreshBundled(this, pcDir());
        startActivity(new Intent(this, PcGameActivity.class).putExtra("fov", prefs().getInt("fov", 0)));
    }

    /** The language list, saved as it changes; also on the first-run screen. */
    Spinner languageBox() {
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
                String chosen = LANGUAGES[position][0];
                if (!chosen.equals(prefs().getString("lang", defaultLanguage()))) {
                    // The menus follow this language, and their tables are read once per process.
                    prefs().edit().putString("lang", chosen).commit();
                    startActivity(new Intent(PcLauncherActivity.this, PcLauncherActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                    Runtime.getRuntime().exit(0);
                }
                prefs().edit().putString("lang", chosen).apply();
                refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        return language;
    }

    private View settingsPage() {
        LinearLayout c = column(56);
        c.addView(kicker(this, Tr.t("ИГРА")));
        c.addView(row(this, text(this, Tr.t("Язык игры"), 15, INK, false), languageBox()));

        String[] fpsNames = new String[FPS.length];
        for (int i = 0; i < FPS.length; ++i) fpsNames[i] = String.valueOf(FPS[i]);
        c.addView(row(this, label(Tr.t("Лимит кадров"), Tr.t("62 — как в игре, 30 — ровнее и холоднее")),
            segments(this, fpsNames, Math.max(0, indexOf(FPS, prefs().getInt("fps", 62))), i -> {
                prefs().edit().putInt("fps", FPS[i]).apply();
                refresh();
            })));

        final TextView fovLabel = text(this, "", 15, INK, false);
        LinearLayout fovText = new LinearLayout(this);
        fovText.setOrientation(LinearLayout.VERTICAL);
        fovText.addView(fovLabel);
        fovText.addView(text(this, Tr.t("Шире — больше видно по краям"), 12, MUTED, false));
        SeekBar fov = new SeekBar(this);
        fov.setMax(140 - 60);
        fov.setProgress(Math.max(0, prefs().getInt("fov", 0) - 60));
        SeekBar.OnSeekBarChangeListener fovChange = new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar b, int v, boolean user) {
                fovLabel.setText(v == 0 ? Tr.t("Поле зрения: как в игре") : Tr.t("Поле зрения: ") + (60 + v) + "°");
                if (user) { prefs().edit().putInt("fov", v == 0 ? 0 : 60 + v).apply(); refresh(); }
            }
            public void onStartTrackingTouch(SeekBar b) {}
            public void onStopTrackingTouch(SeekBar b) {}
        };
        fov.setOnSeekBarChangeListener(fovChange);
        fovChange.onProgressChanged(fov, fov.getProgress(), false);
        c.addView(fovText, gap(16));
        c.addView(fov, new LinearLayout.LayoutParams(-1, -2));
        c.addView(row(this, label(Tr.t("Заставка при запуске"), Tr.t("Логотипы перед главным меню")),
            segments(this, new String[] {Tr.t("Показывать"), Tr.t("Пропускать")}, prefs().getInt("intro", 0), i -> prefs().edit().putInt("intro", i).apply())));

        c.addView(kicker(this, Tr.t("УПРАВЛЕНИЕ")), gap(22));
        android.content.SharedPreferences pads = getSharedPreferences("controllers", MODE_PRIVATE);
        c.addView(row(this, label(Tr.t("Кнопки Nintendo"), Tr.t("Если A и B на геймпаде перепутаны, выберите «По надписям»")),
            segments(this, new String[] {Tr.t("По положению"), Tr.t("По надписям")}, pads.getBoolean("swapFaceAll", false) ? 1 : 0,
                i -> pads.edit().putBoolean("swapFaceAll", i == 1).apply())));
        c.addView(row(this, label(Tr.t("Экранный геймпад"), Tr.t("Авто — только без геймпада и клавиатуры")),
            segments(this, TOUCH, prefs().getInt("touch", 0), i -> prefs().edit().putInt("touch", i).apply())));

        c.addView(kicker(this, Tr.t("ПРОИЗВОДИТЕЛЬНОСТЬ")), gap(22));
        c.addView(row(this, label(Tr.t("Начальное разрешение"), Tr.t("Доля экрана. Потом его можно менять в игре")),
            segments(this, SCALES, prefs().getInt("scale", 0), i -> prefs().edit().putInt("scale", i).apply())));
        c.addView(row(this, label(Tr.t("Апскейлер"), Tr.t("Как растянуть картинку на экран. Только если разрешение ниже полного")),
            segments(this, new String[] {Tr.t("Выкл"), "FSR", "SGSR"}, prefs().getInt("upscaler", 0), i -> prefs().edit().putInt("upscaler", i).apply())));
        String[] cpuNames = new String[CPUS.length];
        for (int i = 0; i < CPUS.length; ++i) cpuNames[i] = String.valueOf(CPUS[i]);
        c.addView(row(this, label(Tr.t("Ядра для игры"), Tr.t("4 — отдельный поток отрисовки, быстрее")),
            segments(this, cpuNames, Math.max(0, indexOf(CPUS, prefs().getInt("cpus", 4))), i -> prefs().edit().putInt("cpus", CPUS[i]).apply())));
        addGraphics(c);

        c.addView(kicker(this, Tr.t("ДРАЙВЕР ГРАФИКИ")), gap(22));
        LinearLayout driverLabel = new LinearLayout(this);
        driverLabel.setOrientation(LinearLayout.VERTICAL);
        driverLabel.addView(text(this, Tr.t("Драйвер Vulkan"), 15, INK, false));
        driverInfo = text(this, "", 12, MUTED, false);
        driverLabel.addView(driverInfo);
        c.addView(row(this, driverLabel, ghost(this, Tr.t("Свой драйвер (.zip)…"), v -> pickDriver())));
        driverReset = ghost(this, Tr.t("Вернуть системный драйвер"), v -> { GpuDriver.useSystem(prefs()); updateDriverInfo(null); });
        c.addView(driverReset, gap(10));
        c.addView(row(this, label(Tr.t("Turnip без тайлового рендера"), Tr.t("TU_DEBUG=sysmem: если со своим драйвером зависает или искажается картинка")),
            new LauncherUi.Toggle(this, prefs().getBoolean("gpuDriverSysmem", false), on -> prefs().edit().putBoolean("gpuDriverSysmem", on).apply())));
        c.addView(text(this, Tr.t("Нужен пакет AdrenoTools: ZIP с meta.json и библиотекой драйвера. Если игра перестала запускаться, верните системный драйвер."),
            12, MUTED, false), gap(8));
        updateDriverInfo(null);
        c.addView(kicker(this, Tr.t("ПОДДЕРЖКА")), gap(22));
        c.addView(row(this, label(Tr.t("Сообщить о баге"), Tr.t("Откроет форму на GitHub, данные устройства уже подставлены")),
            ghost(this, Tr.t("Открыть форму…"), this::reportBug)));
        c.addView(row(this, label(Tr.t("Логи для разработчика"), Tr.t("Модель устройства, настройки и журналы игры")),
            ghost(this, Tr.t("Отправить логи…"), this::sendLogs)));
        c.addView(text(this, Tr.t("Качество графики, сглаживание и PhysX настраиваются в самой игре: Options → Video.\n")
            + Tr.t("При первом запуске выставляются средние настройки с высокими текстурами; дальше они ваши и приложение их не меняет."),
            12, MUTED, false), gap(18));
        c.addView(text(this, Tr.t("Версия ") + SessionLog.version(this), 12, MUTED, false), gap(8));
        return page(LauncherUi.Backdrop.STRIPE, c, true);
    }

    /** Zips the logs off the UI thread and opens the share sheet (Telegram etc.); a copy stays in Downloads. */
    private void sendLogs(View button) {
        button.setEnabled(false);
        Toast.makeText(this, Tr.t("Собираю логи…"), Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            try {
                File zip = SessionLog.bundlePc(this, pcDir(), new TreeMap<>(prefs().getAll()).toString());
                SessionLog.saveToDownloads(this, zip);
                runOnUiThread(() -> {
                    button.setEnabled(true);
                    SessionLog.share(this, zip);
                });
            } catch (java.io.IOException e) {
                runOnUiThread(() -> {
                    button.setEnabled(true);
                    Toast.makeText(this, Tr.t("Не получилось собрать логи: ") + e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        }, "pc-logs").start();
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
        c.addView(kicker(this, Tr.t("ДОСТИЖЕНИЯ")));
        achCount = text(this, "", 30, INK, true);
        c.addView(achCount, gap(6));
        achList = new LinearLayout(this);
        achList.setOrientation(LinearLayout.VERTICAL);
        c.addView(achList, new LinearLayout.LayoutParams(-1, -2));
        c.addView(text(this, Tr.t("Логика достижений — из самой игры, а названия и описания написаны для этого приложения: ")
            + Tr.t("в ПК-версии их текстов нет."), 12, MUTED, false), gap(14));
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
        achCount.setText(got.size() + Tr.t(" из ") + ACHIEVEMENTS.length);
        achList.removeAllViews();
        SimpleDateFormat date = new SimpleDateFormat("dd.MM.yyyy", Locale.getDefault());
        for (String[] a : ACHIEVEMENTS) {
            Long at = got.get(Integer.valueOf(a[0]));
            LinearLayout card = new LinearLayout(this);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setPadding(dp(this, 14), dp(this, 9), dp(this, 14), dp(this, 9));
            card.setBackground(box(0xF2FFFFFF, at != null ? RED : PANEL_BORDER, 1, this));
            card.addView(text(this, a[1], 15, at != null ? INK : NAV, true));
            card.addView(text(this, at != null ? a[2] + Tr.t(" · получено ") + date.format(new Date(at * 1000)) : a[2], 13, at != null ? MUTED : LOCKED, false));
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-1, -2);
            p.bottomMargin = dp(this, 8);
            achList.addView(card, p);
        }
    }

    private View filesPage() {
        LinearLayout c = column(28);
        c.addView(kicker(this, Tr.t("ФАЙЛЫ ИГРЫ")));
        filesText = text(this, "", 14, INK, false);
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(box(0xE6FFFFFF, PANEL_BORDER, 1, this));
        panel.setPadding(dp(this, 16), dp(this, 12), dp(this, 16), dp(this, 12));
        panel.addView(filesText);
        c.addView(panel, gap(10));
        c.addView(ghost(this, Tr.t("Установить или обновить файлы…"), v -> setup.showData()), gap(14));
        c.addView(text(this, Tr.t("Файлы игры берутся из вашей копии GOG. В приложение они не входят."), 12, MUTED, false), gap(12));
        return page(LauncherUi.Backdrop.HORIZON, c, true);
    }

    private boolean ready() {
        File lib = new File(getApplicationInfo().nativeLibraryDir);
        return new File(lib, "libmepc.so").isFile() && new File(lib, "libdxvk_d3d9.so").isFile() && PcSetup.filesReady(pcDir());
    }

    private static String mark(boolean ok) { return ok ? "✓  " : "✗  "; }

    private String filesReport() {
        File pc = pcDir(), lib = new File(getApplicationInfo().nativeLibraryDir);
        return Tr.t("Папка: ") + pc
            + "\n" + mark(new File(lib, "libmepc.so").isFile()) + Tr.t("libmepc.so (только личная сборка)")
            + "\n" + mark(new File(lib, "libdxvk_d3d9.so").isFile()) + "libdxvk_d3d9.so"
            + "\n" + mark(new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile()) + "MirrorsEdge/Binaries/MirrorsEdge.exe"
            + "\n" + mark(new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()) + "MirrorsEdge/TdGame/CookedPC"
            + "\n" + mark(PcSetup.filesReady(pc)) + Tr.t("библиотеки и все файлы игры");
    }

    void refresh() {
        boolean ok = ready();
        play.setEnabled(ok);
        play.setAlpha(ok ? 1f : 0.4f);
        int fov = prefs().getInt("fov", 0);
        String lang = prefs().getString("lang", defaultLanguage());
        String name = lang;
        for (String[] l : LANGUAGES) if (l[0].equals(lang)) name = l[1];
        playMeta.setText(ok ? name + Tr.t(" · до ") + prefs().getInt("fps", 62) + Tr.t(" кадров/с · ") + (fov > 0 ? Tr.t("поле зрения ") + fov + "°" : Tr.t("поле зрения как в игре"))
            : Tr.t("Не хватает файлов игры — смотрите раздел «Файлы»"));
        filesText.setText(filesReport());
        if (setup.isBusy()) play.setEnabled(false);
        showAchievements();
    }
}
