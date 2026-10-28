package org.mirrorsedge.research;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.*;
import static org.mirrorsedge.research.LauncherUi.*;
import java.io.*;
import java.text.SimpleDateFormat;
import java.util.*;

/**
 * Start screen: checks that the game data is in place, imports it from a folder the user
 * picks (Storage Access Framework, nothing leaves the device), backs up the Xbox saves
 * before every launch and starts GameActivity. The research stand stays reachable.
 *
 * Import folder layout (what the PC-side tooling produces; not part of the APK):
 *   libmeruntime.so, me_native_entry_sections_resources.bin, me_native_xex_headers.bin,
 *   me_game_readonly/ (inventory.txt + extracted game files), optional me_saves/.
 * Destination: the app's external files directory (owned by the app). Files that already
 * exist with the same size are skipped, so an interrupted import can be resumed. Existing
 * saves are never overwritten by an import.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_FOLDER = 20, PICK_ISO = 21, PICK_DRIVER = 22;
    private static final String[] BUNDLE = {"libmeruntime.so", "me_native_entry_sections_resources.bin", "me_native_xex_headers.bin"};
    private static final int KEEP_BACKUPS = 10;

    private TextView status, progress, playKicker, playTitle, playMeta, savesInfo;
    private LauncherUi.Btn play, importButton, moveButton, installButton;
    private LauncherUi.Bar progressBar;
    private volatile boolean importing;
    private Onboarding onboarding;
    boolean isImporting() { return importing; }
    // Navigation: one page visible at a time in the content area.
    private static final String[] PAGES = {"Играть", "Настройки", "Профиль", "Достижения", "Данные игры"};
    private final List<TextView> navItems = new ArrayList<>();
    private final List<View> pages = new ArrayList<>();
    private static final int[] FRAME_LIMITS = {0, 30, 60};
    // Languages the game ships (*_LOC_INT/DEU/FRA/ESN/ITA/RUS packages) with their Xbox
    // console language codes, passed to the runtime as ME_LANGUAGE (XConfig setting 9).
    static final String[] LANGUAGE_NAMES = {"English", "Русский", "Deutsch", "Français", "Español", "Italiano"};
    static final int[] LANGUAGE_CODES = {1, 12, 3, 4, 5, 6};
    private static final String[] LANGUAGE_LOCALES = {"en", "ru", "de", "fr", "es", "it"};

    private static int languageIndex(int code) {
        for (int i = 0; i < LANGUAGE_CODES.length; ++i) if (LANGUAGE_CODES[i] == code) return i;
        return 0;
    }

    /** The system language when the game has it, English otherwise. */
    static int defaultLanguage() {
        String system = Locale.getDefault().getLanguage();
        for (int i = 0; i < LANGUAGE_LOCALES.length; ++i) if (LANGUAGE_LOCALES[i].equals(system)) return LANGUAGE_CODES[i];
        return 1;
    }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(BG);
        getWindow().setNavigationBarColor(BG);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR | View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR);
        // Wide screens (tablet, unfolded Fold): navigation column on the left as in the
        // mockups. Narrow (folded phone): navigation strip on top.
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
        if (wide) {
            View spacer = new View(this);
            nav.addView(spacer, new LinearLayout.LayoutParams(1, 0, 1));
            TextView version = text(this, versionLine(), 12, MUTED, false);
            version.setPadding(dp(this, 14), 0, 0, 0);
            nav.addView(version);
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
        // progress/status are shared by the play and data pages, created before them.
        status = text(this, "", 15, INK, false);
        progress = text(this, "", 13, MUTED, false);
        progressBar = new LauncherUi.Bar(this);
        progressBar.setVisibility(View.GONE);
        pages.add(playPage());
        pages.add(settingsPage());
        pages.add(profilePage());
        pages.add(achievementsPage());
        pages.add(dataPage());
        for (View page : pages) content.addView(page, new FrameLayout.LayoutParams(-1, -1));
        FrameLayout host = new FrameLayout(this);
        host.addView(root, new FrameLayout.LayoutParams(-1, -1));
        if (Onboarding.needed(this)) {
            onboarding = new Onboarding(this);
            host.addView(onboarding.view(), new FrameLayout.LayoutParams(-1, -1));
            onboarding.show();
        }
        setContentView(host);
        showPage(state == null ? 0 : state.getInt("page", 0));
    }

    private int currentPage, navWidth;

    @Override protected void onSaveInstanceState(Bundle out) { super.onSaveInstanceState(out); out.putInt("page", currentPage); }

    private void showPage(int index) {
        currentPage = index;
        for (int i = 0; i < pages.size(); ++i) {
            boolean on = i == index;
            pages.get(i).setVisibility(on ? View.VISIBLE : View.GONE);
            TextView item = navItems.get(i);
            item.setTextColor(on ? INK : NAV);
            item.setTypeface(on ? MEDIUM : android.graphics.Typeface.DEFAULT);
            item.setBackground(on ? new SideMark(RED, dp(this, 3)) : null);
        }
    }

    /** Active nav item: translucent white fill with the red bar on the left. */
    private static final class SideMark extends android.graphics.drawable.Drawable {
        private final android.graphics.Paint paint = new android.graphics.Paint();
        private final int color, width;
        SideMark(int color, int width) { this.color = color; this.width = width; }
        @Override public void draw(android.graphics.Canvas c) {
            android.graphics.Rect b = getBounds();
            paint.setColor(0xB3FFFFFF); c.drawRect(b, paint);
            paint.setColor(color); c.drawRect(b.left, b.top, b.left + width, b.bottom, paint);
        }
        @Override public void setAlpha(int a) {}
        @Override public void setColorFilter(android.graphics.ColorFilter f) {}
        @Override public int getOpacity() { return android.graphics.PixelFormat.TRANSLUCENT; }
    }

    private String versionLine() {
        String version = "";
        try { version = "v" + getPackageManager().getPackageInfo(getPackageName(), 0).versionName + " · "; } catch (Exception ignored) {}
        String gpu = android.os.Build.VERSION.SDK_INT >= 31 ? android.os.Build.SOC_MODEL : android.os.Build.HARDWARE;
        return version + gpu;
    }

    /** Page frame: backdrop artwork under a padded column. */
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

    private static LinearLayout.LayoutParams gap(Context c, int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.topMargin = dp(c, topDp);
        return p;
    }

    private View playPage() {
        LinearLayout c = column(28);
        c.setGravity(Gravity.BOTTOM | Gravity.START);
        playKicker = kicker(this, "");
        playTitle = text(this, "", 30, INK, true);
        playMeta = text(this, "", 14, MUTED, false);
        play = cta(this, "▶  Играть", v -> launch());
        // Text sits on a light card so the route line behind never crosses it.
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(this, 20), dp(this, 16), dp(this, 28), dp(this, 20));
        card.addView(playKicker);
        card.addView(playTitle, gap(this, 6));
        card.addView(playMeta, gap(this, 6));
        card.addView(play, gap(this, 16));
        c.addView(card, new LinearLayout.LayoutParams(-2, -2));
        return page(LauncherUi.Backdrop.CITY, c, false);
    }

    private View settingsPage() {
        LinearLayout c = column(56);
        c.addView(kicker(this, "ИГРА"));
        Spinner language = new Spinner(this, Spinner.MODE_DROPDOWN);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, LANGUAGE_NAMES);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        language.setAdapter(adapter);
        language.setBackground(box(Color.WHITE, INK, 1, this));
        language.setPadding(dp(this, 6), dp(this, 6), dp(this, 6), dp(this, 6));
        language.setSelection(languageIndex(prefs().getInt("language", defaultLanguage())));
        language.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (view instanceof TextView) { ((TextView) view).setTextColor(INK); ((TextView) view).setPadding(dp(LauncherActivity.this, 12), 0, dp(LauncherActivity.this, 12), 0); }
                prefs().edit().putInt("language", LANGUAGE_CODES[position]).apply();
                loadAchievements();
                refresh();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        c.addView(row(this, text(this, "Язык игры", 15, INK, false), language));
        LinearLayout introLabel = new LinearLayout(this);
        introLabel.setOrientation(LinearLayout.VERTICAL);
        introLabel.addView(text(this, "Пропускать заставку", 15, INK, false));
        introLabel.addView(text(this, "Начальный ролик при запуске игры", 12, MUTED, false));
        c.addView(row(this, introLabel, new LauncherUi.Toggle(this, prefs().getBoolean("skipIntro", false),
            on -> prefs().edit().putBoolean("skipIntro", on).apply())));
        TextView heat = kicker(this, "ИЗОБРАЖЕНИЕ И НАГРЕВ");
        c.addView(heat, gap(this, 22));
        // Frame limit: 30 / 60 / none (stored as Hz, 0 = none).
        int[] limits = {30, 60, 0};
        int current = frameLimit(), selected = 2;
        for (int i = 0; i < limits.length; ++i) if (limits[i] == current) selected = i;
        LinearLayout limitLabel = new LinearLayout(this);
        limitLabel.setOrientation(LinearLayout.VERTICAL);
        limitLabel.addView(text(this, "Лимит кадров", 15, INK, false));
        limitLabel.addView(text(this, "30 — ровнее и холоднее", 12, MUTED, false));
        c.addView(row(this, limitLabel, segments(this, new String[] {"30", "60", "Без"}, selected, i -> {
            prefs().edit().putInt("frameLimit", limits[i]).apply();
            refresh();
        })));
        LinearLayout overlayLabel = new LinearLayout(this);
        overlayLabel.setOrientation(LinearLayout.VERTICAL);
        overlayLabel.addView(text(this, "Мониторинг", 15, INK, false));
        overlayLabel.addView(text(this, "FPS, самый долгий кадр, загрузка GPU, нагрев и память поверх игры", 12, MUTED, false));
        c.addView(row(this, overlayLabel, new LauncherUi.Toggle(this, prefs().getBoolean("perfOverlay", false),
            on -> prefs().edit().putBoolean("perfOverlay", on).apply())));
        c.addView(kicker(this, "ДРАЙВЕР GPU"), gap(this, 22));
        LinearLayout driverLabel = new LinearLayout(this);
        driverLabel.setOrientation(LinearLayout.VERTICAL);
        driverLabel.addView(text(this, "Драйвер Vulkan", 15, INK, false));
        driverInfo = text(this, "", 12, MUTED, false);
        driverLabel.addView(driverInfo);
        c.addView(row(this, driverLabel, ghost(this, "Свой драйвер (.zip)…", v -> pickDriver())));
        driverReset = ghost(this, "Вернуть системный драйвер", v -> { GpuDriver.useSystem(prefs()); updateDriverInfo(null); });
        c.addView(driverReset, gap(this, 10));
        LinearLayout sysmemLabel = new LinearLayout(this);
        sysmemLabel.setOrientation(LinearLayout.VERTICAL);
        sysmemLabel.addView(text(this, "Turnip без тайлового рендера", 15, INK, false));
        sysmemLabel.addView(text(this, "TU_DEBUG=sysmem — для Adreno 8xx, если с драйвером зависает или искажается картинка", 12, MUTED, false));
        c.addView(row(this, sysmemLabel, new LauncherUi.Toggle(this, prefs().getBoolean("gpuDriverSysmem", false),
            on -> prefs().edit().putBoolean("gpuDriverSysmem", on).apply())));
        updateDriverInfo(null);
        c.addView(kicker(this, "ДЛЯ РАЗРАБОТКИ"), gap(this, 22));
        LinearLayout internalLabel = new LinearLayout(this);
        internalLabel.setOrientation(LinearLayout.VERTICAL);
        internalLabel.addView(text(this, "Файлы игры во внутренней памяти", 15, INK, false));
        internalLabel.addView(text(this, "Без FUSE, для теста фризов", 12, MUTED, false));
        c.addView(row(this, internalLabel, new LauncherUi.Toggle(this, prefs().getBoolean("internalGame", false), on -> {
            prefs().edit().putBoolean("internalGame", on).apply();
            refresh();
        })));
        moveButton = ghost(this, "Скопировать файлы во внутреннюю память", v -> { showPage(4); copyGameInternal(); });
        c.addView(moveButton, gap(this, 12));
        c.addView(ghost(this, "Технический стенд", v -> startActivity(new Intent(this, MainActivity.class))), gap(this, 12));
        return page(LauncherUi.Backdrop.STRIPE, c, true);
    }

    private View profilePage() {
        LinearLayout c = column(28);
        c.setPadding(c.getPaddingLeft(), c.getPaddingTop(), dp(this, 170), c.getPaddingBottom());  // clear of the pale column
        c.addView(kicker(this, "ПРОФИЛЬ XBOX"));
        savesInfo = text(this, "", 15, INK, false);
        c.addView(savesInfo, gap(this, 10));
        TextView soon = text(this, "Переименование, создание и удаление профиля появятся в следующей версии.", 13, MUTED, false);
        c.addView(soon, gap(this, 14));
        return page(LauncherUi.Backdrop.CORNER, c, true);
    }

    private TextView achCount, achMeta;
    private LauncherUi.Bar achBar;
    private LinearLayout achList;
    private int achLoads;

    private View achievementsPage() {
        LinearLayout c = column(28);
        c.setPadding(c.getPaddingLeft(), c.getPaddingTop(), dp(this, 110), c.getPaddingBottom());  // clear of the pale column
        c.addView(kicker(this, "ДОСТИЖЕНИЯ"));
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.BOTTOM);
        achCount = text(this, "", 34, INK, true);
        achMeta = text(this, "", 14, MUTED, false);
        achMeta.setPadding(dp(this, 12), 0, 0, dp(this, 6));
        head.addView(achCount);
        head.addView(achMeta);
        c.addView(head, gap(this, 6));
        achBar = new LauncherUi.Bar(this);
        c.addView(achBar, new LinearLayout.LayoutParams(-1, -2));
        achList = new LinearLayout(this);
        achList.setOrientation(LinearLayout.VERTICAL);
        c.addView(achList, gap(this, 16));
        c.addView(text(this, "Названия, описания и иконки — из данных вашей копии игры. Полученные сохраняются в профиле рядом с сохранениями.", 12, MUTED, false), gap(this, 14));
        return page(LauncherUi.Backdrop.CORNER, c, true);
    }

    /** Reads the list (XDBF of the installed copy) and the profile's unlocks in the background. */
    private void loadAchievements() {
        final int load = ++achLoads;
        final int language = prefs().getInt("language", defaultLanguage());
        new Thread(() -> {
            List<Achievements.Item> items = null;
            String error = null;
            try {
                byte[] xdbf = Achievements.xdbfFromBundle(new File(work(), IsoInstall.BUNDLE));
                if (xdbf == null) error = "Список появится после установки игры из ISO (раздел «Данные игры»).";
                else {
                    items = Achievements.read(xdbf, language);
                    Achievements.applyUnlocks(items, new File(work(), "me_saves"));
                }
            } catch (IOException e) {
                error = "Не получилось прочитать достижения: " + e.getMessage();
            }
            final List<Achievements.Item> result = items;
            final String message = error;
            // Icons decoded here, off the UI thread.
            final Map<Integer, android.graphics.Bitmap> icons = new HashMap<>();
            if (result != null) for (Achievements.Item a : result)
                if (a.icon != null) {
                    android.graphics.Bitmap b = android.graphics.BitmapFactory.decodeByteArray(a.icon, 0, a.icon.length);
                    if (b != null) icons.put(a.id, b);
                }
            runOnUiThread(() -> { if (load == achLoads) showAchievements(result, icons, message); });
        }, "achievements").start();
    }

    private void showAchievements(List<Achievements.Item> items, Map<Integer, android.graphics.Bitmap> icons, String message) {
        achList.removeAllViews();
        if (items == null) {
            achCount.setText("—");
            achMeta.setText("");
            achBar.set(0);
            achList.addView(text(this, message, 15, INK, false));
            return;
        }
        int unlocked = 0, score = 0, total = 0;
        for (Achievements.Item a : items) { total += a.gamerscore; if (a.unlocked()) { unlocked++; score += a.gamerscore; } }
        achCount.setText(String.valueOf(unlocked));
        achMeta.setText(String.format(Locale.getDefault(), "из %d · %d из %d очков", items.size(), score, total));
        achBar.set(items.isEmpty() ? 0 : (float) unlocked / items.size());
        // Earned first (newest first), then the rest in the game's order.
        List<Achievements.Item> sorted = new ArrayList<>(items);
        Collections.sort(sorted, (x, y) -> x.unlocked() != y.unlocked() ? (x.unlocked() ? -1 : 1) : x.unlocked() ? Long.compare(y.unlockedAt, x.unlockedAt) : 0);
        LinearLayout row = null;
        for (int i = 0; i < sorted.size(); ++i) {
            if (i % 2 == 0) {
                row = new LinearLayout(this);
                LinearLayout.LayoutParams full = new LinearLayout.LayoutParams(-1, -2);
                full.topMargin = dp(this, i == 0 ? 0 : 8);
                achList.addView(row, full);
            }
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0, -1, 1);
            if (i % 2 == 1) p.leftMargin = dp(this, 8);
            row.addView(achievementCard(sorted.get(i), icons.get(sorted.get(i).id)), p);
        }
        if (sorted.size() % 2 == 1) row.addView(new View(this), new LinearLayout.LayoutParams(0, 1, 1));
    }

    private View achievementCard(Achievements.Item a, android.graphics.Bitmap icon) {
        boolean on = a.unlocked();
        LinearLayout card = new LinearLayout(this);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setPadding(dp(this, on ? 11 : 12), dp(this, 10), dp(this, 12), dp(this, 10));
        android.graphics.drawable.GradientDrawable bg = box(0xF2FFFFFF, PANEL_BORDER, 1, this);
        card.setBackground(on ? new android.graphics.drawable.LayerDrawable(new android.graphics.drawable.Drawable[] {bg, new SideMark(RED, dp(this, 3))}) : bg);
        ImageView image = new ImageView(this);
        image.setScaleType(ImageView.ScaleType.FIT_CENTER);
        if (icon != null) image.setImageBitmap(icon);
        if (!on) {
            android.graphics.ColorMatrix gray = new android.graphics.ColorMatrix();
            gray.setSaturation(0);
            image.setColorFilter(new android.graphics.ColorMatrixColorFilter(gray));
            image.setImageAlpha(120);
        }
        card.addView(image, new LinearLayout.LayoutParams(dp(this, 52), dp(this, 52)));
        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        texts.setPadding(dp(this, 12), 0, 0, 0);
        boolean hidden = !on && a.secret();
        texts.addView(text(this, hidden ? "Секретное достижение" : a.label, 15, on ? INK : NAV, true));
        String about = hidden ? "Откроется после получения" : (on || a.unachieved.isEmpty() ? a.description : a.unachieved);
        TextView description = text(this, about, 13, on ? MUTED : LOCKED, false);
        description.setMaxLines(2);
        texts.addView(description);
        String line = a.gamerscore + " G" + (on && a.unlockedAt > 0
            ? " · получено " + new SimpleDateFormat("dd.MM.yyyy", Locale.getDefault()).format(new Date(a.unlockedAt * 1000)) : "");
        texts.addView(text(this, line, 12, on ? RED : LOCKED, on));
        card.addView(texts, new LinearLayout.LayoutParams(0, -2, 1));
        return card;
    }

    private View dataPage() {
        LinearLayout c = column(28);
        c.addView(kicker(this, "ДАННЫЕ ИГРЫ"));
        c.addView(status, gap(this, 10));
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(box(0xE6FFFFFF, PANEL_BORDER, 1, this));
        panel.setPadding(dp(this, 14), dp(this, 12), dp(this, 14), dp(this, 12));
        panel.addView(progress);
        panel.addView(progressBar, gap(this, 8));
        c.addView(panel, gap(this, 14));
        installButton = cta(this, "Установить из ISO…", v -> pickIso());
        c.addView(installButton, gap(this, 14));
        c.addView(text(this, "Образ диска своей копии игры для Xbox 360. Файлы игры не входят в приложение и не покидают устройство.", 12, MUTED, false), gap(this, 8));
        importButton = ghost(this, "Импорт подготовленной папки…", v -> pickFolder());
        c.addView(importButton, gap(this, 12));
        c.addView(kicker(this, "ЛОГИ"), gap(this, 26));
        c.addView(text(this, "Архив последней сессии: FPS, температура, сцены. Без личных данных.", 13, MUTED, false), gap(this, 6));
        c.addView(ghost(this, "Отправить логи", v -> {
            progress.setText("Собираю архив…");
            new Thread(() -> {
                try {
                    File zip = SessionLog.bundle(this, work());
                    String saved = SessionLog.saveToDownloads(this, zip);
                    runOnUiThread(() -> {
                        progress.setText("Архив логов готов" + (saved != null ? " · копия: " + saved : ""));
                        SessionLog.share(this, zip);
                    });
                } catch (IOException e) {
                    runOnUiThread(() -> progress.setText("Не получилось собрать архив: " + e.getMessage()));
                }
            }, "logs-bundle").start();
        }), gap(this, 10));
        return page(LauncherUi.Backdrop.HORIZON, c, true);
    }

    @Override public void onResume() { super.onResume(); refresh(); loadAchievements(); }

    private File work() { return getExternalFilesDir(null); }
    android.content.SharedPreferences prefs() { return getSharedPreferences("launcher", MODE_PRIVATE); }
    /**
     * Frame limit in Hz, 0 = none. Default 30: above 30 FPS (Xenia "Unlock FPS" patch) the game
     * crashed in Stormdrain (lr 8249b888, bone attachment list, CHECKLIST). Settings saved before
     * that change are moved to 30 once; choosing "Без" again afterwards is kept.
     */
    int frameLimit() {
        if (!prefs().getBoolean("frameLimit30Default", false))
            prefs().edit().putInt("frameLimit", 30).putBoolean("frameLimit30Default", true).apply();
        return prefs().getInt("frameLimit", 30);
    }
    /** Extracted game files: app-internal storage (ext4) or the external files directory (FUSE). */
    private File gameRoot() {
        return prefs().getBoolean("internalGame", false) ? new File(getFilesDir(), "me_game_readonly")
                                                         : new File(work(), "me_game_readonly");
    }

    /** Missing pieces, empty when the game can start. */
    List<String> missing() {
        List<String> missing = new ArrayList<>();
        File work = work();
        if (!RuntimeLibrary.bundled(this) && !new File(work, RuntimeLibrary.NAME).isFile())
            missing.add("libmeruntime.so (собирается самостоятельно, см. README)");
        if (!new File(work, IsoInstall.BUNDLE).isFile() || !new File(work, IsoInstall.HEADERS).isFile()
            || !new File(gameRoot(), "inventory.txt").isFile())
            missing.add(prefs().getBoolean("internalGame", false) && new File(work, IsoInstall.BUNDLE).isFile()
                ? "файлы игры во внутренней памяти (нажми «Скопировать…»)" : "данные игры (установка из ISO)");
        return missing;
    }

    void refresh() {
        if (onboarding != null) onboarding.updateData();
        if (importing) return;
        List<String> missing = missing();
        File saves = new File(work(), "me_saves");
        boolean hasSaves = saves.isDirectory() && count(saves) > 0;
        String savesText = hasSaves ? "сохранения Xbox: есть" : "сохранений Xbox нет (игра создаст новые)";
        if (missing.isEmpty()) status.setText("Данные игры на месте · " + savesText + ".\nПеред каждым запуском делается копия сохранений.");
        else status.setText("Не хватает: " + String.join(", ", missing) + ".\nНажми «Установить из ISO…» и выбери образ своей копии игры.");
        if (progress.getText().length() == 0) progress.setText(missing.isEmpty() ? "Всё готово к запуску" : "Ожидает импорта");
        // Play page: what will start and with which settings.
        int limit = frameLimit();
        String settings = (limit > 0 ? limit + " к/с" : "без лимита кадров") + " · " + LANGUAGE_NAMES[languageIndex(prefs().getInt("language", defaultLanguage()))];
        if (!missing.isEmpty()) {
            playKicker.setText("НУЖНЫ ДАННЫЕ ИГРЫ");
            playTitle.setText("Подключите свою копию");
            playMeta.setText("Раздел «Данные игры» → импорт");
        } else if (hasSaves) {
            playKicker.setText("ПРОДОЛЖИТЬ");
            playTitle.setText("Сохранение " + new SimpleDateFormat("dd.MM · HH:mm", Locale.getDefault()).format(new Date(newest(saves))));
            playMeta.setText(settings);
        } else {
            playKicker.setText("НОВАЯ ИГРА");
            playTitle.setText("Всё готово");
            playMeta.setText(settings);
        }
        File backups = new File(work(), "me_saves_backups");
        File[] list = backups.listFiles(File::isDirectory);
        savesInfo.setText((hasSaves ? "Сохранения Xbox есть, последнее изменение " + new SimpleDateFormat("dd.MM.yyyy HH:mm", Locale.getDefault()).format(new Date(newest(saves)))
                                    : "Сохранений Xbox пока нет — игра создаст их сама.")
            + "\nРезервных копий: " + (list == null ? 0 : list.length) + " из " + KEEP_BACKUPS + " (делаются перед каждым запуском).");
        play.setEnabled(missing.isEmpty());
        importButton.setEnabled(true);
        installButton.setEnabled(true);
        installButton.setText(pendingIso() != null ? "Продолжить установку" : "Установить из ISO…");
        installButton.setOnClickListener(v -> { Uri pending = pendingIso(); if (pending != null) installIso(pending); else pickIso(); });
        boolean internal = prefs().getBoolean("internalGame", false);
        boolean externalReady = new File(work(), "me_game_readonly/inventory.txt").isFile();
        moveButton.setVisibility(internal ? View.VISIBLE : View.GONE);
        moveButton.setEnabled(externalReady);
    }

    /** Newest modification time in a tree. */
    private static long newest(File f) {
        if (f.isFile()) return f.lastModified();
        long t = 0;
        File[] list = f.listFiles();
        if (list != null) for (File c : list) t = Math.max(t, newest(c));
        return t;
    }

    private static int count(File dir) {
        int n = 0;
        File[] list = dir.listFiles();
        if (list != null) for (File f : list) n += f.isDirectory() ? count(f) : 1;
        return n;
    }

    void launch() {
        List<String> missing = missing();
        if (!missing.isEmpty()) { refresh(); return; }
        try {
            String backup = backupSaves();
            if (backup != null) progress.setText("Копия сохранений: " + backup);
        } catch (IOException e) {
            new AlertDialog.Builder(this).setMessage("Не удалось сделать копию сохранений (" + e.getMessage() + "). Запускать без копии нельзя: сохранения Xbox обязательны.")
                .setPositiveButton("OK", null).show();
            return;
        }
        Intent intent = new Intent(this, GameActivity.class);
        intent.putExtra("game_root", gameRoot().getAbsolutePath());
        int limit = frameLimit();
        String env = "ME_LANGUAGE=" + prefs().getInt("language", defaultLanguage());
        if (prefs().getBoolean("skipIntro", false)) env += " ME_SKIP_INTRO=1";
        env += " ME_FRAME_LIMIT=" + limit;  // 0 = no limit (the runtime defaults to 30)
        env += GpuDriver.env(this, prefs());
        intent.putExtra("env", env);
        intent.putExtra("perf_overlay", prefs().getBoolean("perfOverlay", false));
        startActivity(intent);
    }

    /** Copies the imported game files (external, FUSE) to internal storage; same-size files are skipped. */
    private void copyGameInternal() {
        File from = new File(work(), "me_game_readonly"), to = new File(getFilesDir(), "me_game_readonly");
        long need = size(from), free = getFilesDir().getUsableSpace();
        if (need > free) { progress.setText(String.format(Locale.US, "Мало места: нужно %.1f ГБ, свободно %.1f ГБ", need / 1e9, free / 1e9)); return; }
        importing = true;
        play.setEnabled(false); importButton.setEnabled(false); moveButton.setEnabled(false);
        status.setText("Копирую файлы игры во внутреннюю память… Не закрывай приложение.");
        new Thread(() -> {
            String error = null;
            bytesDone = 0; bytesTotal = need; filesCopied = filesSkipped = 0;
            try { copyTreeCounting(from, to); } catch (IOException e) { error = e.getMessage(); }
            final String message = error;
            runOnUiThread(() -> {
                importing = false;
                refresh();
                progress.setText(message == null
                    ? String.format(Locale.US, "Скопировано во внутреннюю память: %d файлов, уже было %d. Копию на /sdcard можно оставить.", filesCopied, filesSkipped)
                    : "Копирование не завершено: " + message + ". Можно запустить ещё раз.");
            });
        }, "game-copy").start();
    }

    private static long size(File f) {
        if (f.isFile()) return f.length();
        long n = 0;
        File[] list = f.listFiles();
        if (list != null) for (File c : list) n += size(c);
        return n;
    }

    private void copyTreeCounting(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to);
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from);
            for (File f : list) copyTreeCounting(f, new File(to, f.getName()));
            return;
        }
        if (to.isFile() && to.length() == from.length()) { filesSkipped++; bytesDone += from.length(); report(false); return; }
        try (InputStream in = new FileInputStream(from)) { write(new CountingStream(in), to); }
        filesCopied++;
        report(false);
    }

    /** Copies me_saves to me_saves_backups/<time>; keeps the newest KEEP_BACKUPS. Null if there is nothing to back up. */
    private String backupSaves() throws IOException {
        File saves = new File(work(), "me_saves");
        if (!saves.isDirectory() || count(saves) == 0) return null;
        File root = new File(work(), "me_saves_backups");
        String name = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File target = new File(root, name);
        copyTree(saves, target);
        File[] old = root.listFiles(File::isDirectory);
        if (old != null && old.length > KEEP_BACKUPS) {
            Arrays.sort(old, (a, b) -> a.getName().compareTo(b.getName()));
            for (int i = 0; i < old.length - KEEP_BACKUPS; ++i) deleteTree(old[i]);
        }
        return "me_saves_backups/" + name;
    }

    private static void copyTree(File from, File to) throws IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs()) throw new IOException("mkdir " + to);
            File[] list = from.listFiles();
            if (list == null) throw new IOException("list " + from);
            for (File f : list) copyTree(f, new File(to, f.getName()));
        } else {
            try (InputStream in = new FileInputStream(from)) { write(in, to); }
        }
    }

    private static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteTree(c);
        f.delete();
    }

    /** Writes through a temporary file so a cut-off copy never looks complete. */
    private static long write(InputStream in, File to) throws IOException {
        File temp = new File(to.getPath() + ".part");
        long total = 0;
        try (OutputStream out = new FileOutputStream(temp)) {
            byte[] buffer = new byte[1 << 20];
            for (int n; (n = in.read(buffer)) > 0; ) { out.write(buffer, 0, n); total += n; }
        }
        if (to.exists() && !to.delete()) throw new IOException("replace " + to);
        if (!temp.renameTo(to)) throw new IOException("rename " + temp);
        return total;
    }

    void pickFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
    }

    /** System file picker for the disc image (any type: .iso has no reliable MIME type). */
    void pickIso() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, PICK_ISO);
    }

    private TextView driverInfo;
    private View driverReset;

    /** System file picker for a GPU driver package (.zip, AdrenoTools format). */
    void pickDriver() {
        Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        startActivityForResult(pick, PICK_DRIVER);
    }

    /** Shows the chosen driver in the settings; message = import result or error, if any. */
    void updateDriverInfo(String message) {
        GpuDriver.Info info = GpuDriver.current(this, prefs());
        String state = info == null ? "Системный драйвер устройства" : "Свой: " + info.label();
        if (driverInfo != null) driverInfo.setText(message == null ? state : message + "\n" + state);
        if (driverReset != null) driverReset.setVisibility(info == null ? View.GONE : View.VISIBLE);
        if (onboarding != null) onboarding.driverChanged(message);
    }

    private void installDriver(Uri uri) {
        new Thread(() -> {
            String message;
            try {
                GpuDriver.Info info = GpuDriver.install(this, prefs(), uri);
                message = "Драйвер установлен: " + info.label();
            } catch (IOException | RuntimeException e) {
                message = "Драйвер не установлен: " + e.getMessage();
            }
            final String text = message;
            runOnUiThread(() -> updateDriverInfo(text));
        }, "gpu-driver-import").start();
    }

    /** An install that was interrupted (app closed) and can be continued from the same file. */
    Uri pendingIso() {
        String s = prefs().getString("isoUri", null);
        return s == null || missing().isEmpty() ? null : Uri.parse(s);
    }

    /** Installs from the picked image in the background; finished files are skipped on a retry. */
    void installIso(Uri uri) {
        importing = true;
        play.setEnabled(false); importButton.setEnabled(false); installButton.setEnabled(false);
        status.setText("Установка из образа… Можно свернуть приложение, но не закрывайте его.");
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (onboarding != null) onboarding.updateData();
        new Thread(() -> {
            String error = null;
            try (android.os.ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                if (pfd == null) throw new IOException("файл не открывается");
                try (FileInputStream in = new FileInputStream(pfd.getFileDescriptor())) {
                    IsoInstall install = new IsoInstall(in.getChannel());
                    install.scan();
                    long present = 0;
                    File root = new File(work(), "me_game_readonly");
                    for (IsoInstall.Entry e : install.entries) {
                        File f = new File(root, e.path.toLowerCase(Locale.ROOT));
                        if (!e.dir && f.isFile() && f.length() == e.size) present += e.size;
                    }
                    long need = install.requiredBytes() - present, free = work().getUsableSpace();
                    if (need > free) throw new IOException(String.format(Locale.US, "мало места: нужно ещё %.1f ГБ, свободно %.1f ГБ", need / 1e9, free / 1e9));
                    install.install(work(), this::installReport);
                }
            } catch (IOException | RuntimeException e) {
                error = e.getMessage();
            }
            final String message = error;
            runOnUiThread(() -> {
                importing = false;
                getWindow().clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                if (message == null) prefs().edit().remove("isoUri").apply();
                refresh();
                progress.setText(message == null ? "Установка завершена" : "Установка не завершена: " + message);
                if (onboarding != null) onboarding.importFinished(progress.getText().toString());
            });
        }, "iso-install").start();
    }

    private void installReport(String stage, long done, long total) {
        long now = System.currentTimeMillis();
        if (now - lastUpdate < 300 && done < total) return;
        lastUpdate = now;
        final String text = total > 1 ? String.format(Locale.US, "%s · %.2f / %.2f ГБ", stage, done / 1e9, total / 1e9) : stage + "…";
        final float fraction = total > 1 ? (float) done / total : 0;
        runOnUiThread(() -> {
            progress.setText(text); progressBar.setVisibility(View.VISIBLE); progressBar.set(fraction);
            if (onboarding != null) onboarding.progress(text, fraction);
        });
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request == PICK_DRIVER) {
            if (result == RESULT_OK && data != null && data.getData() != null) installDriver(data.getData());
            return;
        }
        if (request == PICK_ISO) {
            if (result != RESULT_OK || data == null || data.getData() == null) return;
            Uri uri = data.getData();
            try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); } catch (SecurityException ignored) {}
            prefs().edit().putString("isoUri", uri.toString()).apply();  // to continue after the app was closed
            installIso(uri);
            return;
        }
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        importing = true;
        play.setEnabled(false);
        importButton.setEnabled(false);
        status.setText("Импорт… Не закрывай приложение.");
        if (onboarding != null) onboarding.updateData();
        new Thread(() -> runImport(tree), "game-import").start();
    }

    @Override public void onBackPressed() {
        if (onboarding != null && onboarding.visible() && onboarding.back()) return;
        super.onBackPressed();
    }

    /** One entry of the picked folder. */
    private static final class Entry {
        final String id, name; final boolean dir; final long size;
        Entry(String id, String name, boolean dir, long size) { this.id = id; this.name = name; this.dir = dir; this.size = size; }
    }

    private List<Entry> children(Uri tree, String parentId) {
        List<Entry> result = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = getContentResolver().query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext())
                result.add(new Entry(c.getString(0), c.getString(1), DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? -1 : c.getLong(3)));
        }
        return result;
    }

    private long bytesDone, bytesTotal, filesCopied, filesSkipped;
    private long lastUpdate;

    private void runImport(Uri tree) {
        String error = null;
        bytesDone = bytesTotal = filesCopied = filesSkipped = 0;
        try {
            String rootId = DocumentsContract.getTreeDocumentId(tree);
            Map<String, Entry> top = new HashMap<>();
            for (Entry e : children(tree, rootId)) top.put(e.name, e);
            List<String> absent = new ArrayList<>();
            for (String name : BUNDLE) if (!top.containsKey(name)) absent.add(name);
            if (!top.containsKey("me_game_readonly")) absent.add("me_game_readonly/");
            if (!absent.isEmpty() && missingAfterImport(absent)) throw new IOException("в папке нет: " + String.join(", ", absent));
            // Plan: (source entry, destination file) pairs, sizes for the progress line.
            List<Entry> sources = new ArrayList<>();
            List<File> targets = new ArrayList<>();
            File work = work();
            for (String name : BUNDLE) if (top.containsKey(name)) { sources.add(top.get(name)); targets.add(new File(work, name)); }
            if (top.containsKey("me_game_readonly")) collect(tree, top.get("me_game_readonly"), new File(work, "me_game_readonly"), sources, targets);
            File saves = new File(work, "me_saves");
            boolean importSaves = top.containsKey("me_saves") && (!saves.isDirectory() || count(saves) == 0);
            if (importSaves) collect(tree, top.get("me_saves"), saves, sources, targets);
            for (Entry e : sources) bytesTotal += Math.max(0, e.size);
            ContentResolver resolver = getContentResolver();
            for (int i = 0; i < sources.size(); ++i) {
                Entry e = sources.get(i); File to = targets.get(i);
                if (to.isFile() && e.size >= 0 && to.length() == e.size) { filesSkipped++; bytesDone += e.size; continue; }
                File parent = to.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent);
                try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.id))) {
                    if (in == null) throw new IOException("open " + e.name);
                    write(new CountingStream(in), to);  // bytesDone is counted while reading
                }
                filesCopied++;
                report(false);
            }
            if (top.containsKey("me_saves") && !importSaves) filesSkipped++;  // existing saves kept
        } catch (Exception e) {
            error = e.getMessage();
        }
        final String message = error;
        runOnUiThread(() -> {
            importing = false;
            refresh();
            progress.setText(message == null
                ? String.format(Locale.US, "Импорт завершён: скопировано %d, уже было %d, %.1f ГБ.", filesCopied, filesSkipped, bytesDone / 1e9)
                : "Импорт не завершён: " + message + ". Можно запустить ещё раз — готовые файлы пропускаются.");
            if (onboarding != null) onboarding.importFinished(progress.getText().toString());
        });
    }

    /** True if some of the absent pieces are not already in place either. */
    private boolean missingAfterImport(List<String> absent) {
        File work = work();
        for (String name : absent) {
            File f = name.endsWith("/") ? new File(work, name + "inventory.txt") : new File(work, name);
            if (!f.isFile()) return true;
        }
        return false;
    }

    private void collect(Uri tree, Entry dir, File target, List<Entry> sources, List<File> targets) {
        for (Entry e : children(tree, dir.id)) {
            if (e.dir) collect(tree, e, new File(target, e.name), sources, targets);
            else { sources.add(e); targets.add(new File(target, e.name)); }
        }
    }

    private void report(boolean force) {
        long now = System.currentTimeMillis();
        if (!force && now - lastUpdate < 300) return;
        lastUpdate = now;
        final String text = String.format(Locale.US, "%.2f / %.2f ГБ · файлов %d (пропущено %d)", bytesDone / 1e9, bytesTotal / 1e9, filesCopied, filesSkipped);
        final float fraction = bytesTotal > 0 ? (float) bytesDone / bytesTotal : 0;
        runOnUiThread(() -> {
            progress.setText(text); progressBar.setVisibility(View.VISIBLE); progressBar.set(fraction);
            if (onboarding != null) onboarding.progress(text, fraction);
        });
    }

    /** Counts bytes into bytesDone as they are read, for the progress line. */
    private final class CountingStream extends FilterInputStream {
        CountingStream(InputStream in) { super(in); }
        @Override public int read(byte[] b, int off, int len) throws IOException {
            int n = super.read(b, off, len);
            if (n > 0) { bytesDone += n; report(false); }
            return n;
        }
    }
}
