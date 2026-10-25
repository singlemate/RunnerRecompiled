package org.mirrorsedge.research;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.widget.*;
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
    private static final int PICK_FOLDER = 20;
    private static final String[] BUNDLE = {"libmeruntime.so", "me_native_entry_sections_resources.bin", "me_native_xex_headers.bin"};
    private static final int KEEP_BACKUPS = 10;

    private TextView status, progress;
    private Button play, importButton;
    private volatile boolean importing;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff111820);
        root.setPadding(48, 32, 48, 32);
        root.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView title = new TextView(this);
        title.setText("Mirror’s Edge");
        title.setTextSize(32);
        title.setTextColor(Color.WHITE);
        root.addView(title);
        status = new TextView(this);
        status.setTextColor(0xffd0d8e0);
        status.setTextSize(16);
        status.setPadding(0, 24, 0, 24);
        root.addView(status);
        progress = new TextView(this);
        progress.setTextColor(0xff9fc3ff);
        root.addView(progress);
        play = button(root, "Играть", v -> launch());
        importButton = button(root, "Импорт данных игры из папки…", v -> pickFolder());
        button(root, "Технический стенд", v -> startActivity(new Intent(this, MainActivity.class)));
        setContentView(root);
    }

    private Button button(LinearLayout root, String text, android.view.View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setOnClickListener(listener);
        root.addView(button, new LinearLayout.LayoutParams(720, -2));
        return button;
    }

    @Override public void onResume() { super.onResume(); refresh(); }

    private File work() { return getExternalFilesDir(null); }

    /** Missing pieces, empty when the game can start. */
    private List<String> missing() {
        List<String> missing = new ArrayList<>();
        File work = work();
        for (String name : BUNDLE) if (!new File(work, name).isFile()) missing.add(name);
        if (!new File(work, "me_game_readonly/inventory.txt").isFile()) missing.add("me_game_readonly (файлы игры)");
        return missing;
    }

    private void refresh() {
        if (importing) return;
        List<String> missing = missing();
        File saves = new File(work(), "me_saves");
        String savesText = saves.isDirectory() && count(saves) > 0 ? "сохранения Xbox: есть" : "сохранений Xbox нет (игра создаст новые)";
        if (missing.isEmpty()) status.setText("Данные игры на месте · " + savesText + ".\nПеред каждым запуском делается копия сохранений.");
        else status.setText("Не хватает: " + String.join(", ", missing) + ".\nНажми «Импорт данных игры из папки…» и выбери папку с ними.");
        play.setEnabled(missing.isEmpty());
        importButton.setEnabled(true);
    }

    private static int count(File dir) {
        int n = 0;
        File[] list = dir.listFiles();
        if (list != null) for (File f : list) n += f.isDirectory() ? count(f) : 1;
        return n;
    }

    private void launch() {
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
        startActivity(new Intent(this, GameActivity.class));
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

    private void pickFolder() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_FOLDER);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_FOLDER || result != RESULT_OK || data == null || data.getData() == null) return;
        Uri tree = data.getData();
        importing = true;
        play.setEnabled(false);
        importButton.setEnabled(false);
        status.setText("Импорт… Не закрывай приложение.");
        new Thread(() -> runImport(tree), "game-import").start();
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
        runOnUiThread(() -> progress.setText(text));
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
