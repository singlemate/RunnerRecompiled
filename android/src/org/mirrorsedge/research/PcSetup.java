package org.mirrorsedge.research;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.graphics.Color;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.view.View;
import android.view.WindowManager;
import android.widget.*;
import static org.mirrorsedge.research.LauncherUi.*;
import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.*;

/**
 * First-run setup of the PC version: language, the user's own GOG copy, frame limit and resolution.
 * The game copy comes from one ZIP of the installed game or from its folder (the GOG installer only
 * runs on Windows); nothing of it is in the APK. Files already there with the right size are
 * skipped, so an interrupted install resumes. Private builds carry
 * the four runtime DLLs the game copy does not have (assets/pcdeps); the game's own DLLs are
 * rebased here, each result checked against the build the code was lifted from.
 */
final class PcSetup {
    static final int PICK_GAME = 30, PICK_ZIP = 31;
    private static final int WELCOME = 0, DATA = 1, OPTIONS = 2, STEPS = 3;
    /** MirrorsEdge.exe of the supported copy (GOG); the recompiled code matches only it. */
    private static final String EXE_SHA256 = "58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd";
    /** Name, image base of the lifted copy, SHA-256 of the rebased file. */
    private static final String[][] REBASE = {
        {"PhysXLoader.dll", "20000000", "f2ae5f04911a781c4e5b4fd800a0819d9d3c34690ff1be3536a809039fbfbc49"},
        {"NxCooking.dll", "23000000", "ca57712c0234e1d51acd81aea05cf7ef4ca333e76c7b191319e0fe3c8b881603"},
        {"PhysXExtensions.dll", "24000000", "01942e506440658b628bdbf2e85bfbde47fcd2ac4725323543e8234d6ed82909"},
        {"NxCharacter.dll", "25000000", "c5f7f6ceccae3d050452ab6d5a87059bcf81e4bd584486723ea1ae7b97a904f1"},
        {"ogg.dll", "26000000", "fcc4536f8ae0894eef07eb68f2c22225bd4cf5926f4698f25b0f3ec2deee155f"},
        {"vorbis.dll", "27000000", "6736a4b8f79c135b1bc01b0e9ca5da09d853e2ab3e667cf1f9d305b6e8e5d1a9"},
        {"vorbisfile.dll", "28000000", "34aab92c2c9c8c152b68a631ae06c345e7963778710ff687ec4a7cf05608eb98"},
        {"vorbisenc.dll", "29000000", "e7d1a36e7289c3b26cd1d05c8be780f5832356bcfa0c2878fa8232c367a7f353"},
        {"libresample.dll", "2A000000", "f352bab07ccacafbdebc27ef8f64057081f13c826a8acb3d44ec3b6be02d5a2e"},
        {"binkw32.dll", "2B000000", "8a7683e0d9675b347cd60a075c9092fdd6ae8701235e1ea7efa537256e53c3a8"},
    };
    /** assets/pcdeps name, place under files/pc. */
    private static final String[][] BUNDLED = {
        {"msvcr80.dll", "deps/msvcr80.dll"}, {"msvcp80.dll", "deps/msvcp80.dll"},
        {"PhysXCore.dll", "deps/rebased/PhysXCore.dll"}, {"PhysXCooking.dll", "deps/rebased/PhysXCooking.dll"},
    };
    private static final String[] GAME_DIRS = {"Binaries", "Engine", "TdGame"};

    /** True when everything the runtime opens at the start is in place. */
    static boolean filesReady(File pc) {
        if (!new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe").isFile() || !new File(pc, "MirrorsEdge/Engine").isDirectory()
            || !new File(pc, "MirrorsEdge/TdGame/CookedPC").isDirectory()) return false;
        for (String[] b : BUNDLED) if (!new File(pc, b[1]).isFile()) return false;
        for (String[] r : REBASE) if (!new File(pc, "deps/rebased/" + r[0]).isFile()) return false;
        return true;
    }

    /** A new APK may be built from other runtime DLLs than the installed ones: the code has to match them. */
    static void refreshBundled(android.content.Context c, File pc) {
        for (String[] b : BUNDLED) {
            File target = new File(pc, b[1]);
            if (!target.isFile()) continue;
            try (InputStream in = c.getAssets().open("pcdeps/" + b[0])) {
                java.io.ByteArrayOutputStream all = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0; ) all.write(buf, 0, n);
                byte[] wanted = all.toByteArray();
                if (!java.util.Arrays.equals(wanted, Files.readAllBytes(target.toPath()))) {
                    Files.write(target.toPath(), wanted);
                    android.util.Log.i("me_pc", "updated " + b[1]);
                }
            } catch (IOException e) {
                android.util.Log.i("me_pc", "bundled " + b[0] + ": " + e);
            }
        }
    }

    /** The result of a section-table walk is the same as pc/tools/rebase_pe.py; REBASE checks it. */
    static byte[] rebase(byte[] file, int newBase) {
        byte[] out = file.clone();
        ByteBuffer b = ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN);
        int pe = b.getInt(0x3C), sections = b.getShort(pe + 6) & 0xFFFF, optSize = b.getShort(pe + 20) & 0xFFFF;
        int opt = pe + 24, table = opt + optSize, delta = newBase - b.getInt(opt + 28);
        int rva = b.getInt(opt + 96 + 5 * 8), size = b.getInt(opt + 96 + 5 * 8 + 4);
        int p = offset(b, table, sections, rva, 8), end = p + size;
        while (p + 8 <= end) {
            int page = b.getInt(p), block = b.getInt(p + 4);
            if (block < 8) break;
            for (int e = p + 8; e < p + block; e += 2) {
                int entry = b.getShort(e) & 0xFFFF;
                if (entry >> 12 != 3) continue;
                int site = offset(b, table, sections, page + (entry & 0xFFF), 4);
                if (site >= 0) b.putInt(site, b.getInt(site) + delta);
            }
            p += block;
        }
        b.putInt(opt + 28, newBase);
        return out;
    }

    /** File offset of an RVA; -1 when those bytes are not stored in the file. */
    private static int offset(ByteBuffer b, int table, int sections, int rva, int bytes) {
        for (int i = 0; i < sections; ++i) {
            int s = table + 40 * i, vsize = b.getInt(s + 8), va = b.getInt(s + 12), raw = b.getInt(s + 16), off = b.getInt(s + 20);
            if (rva >= va && rva < va + Math.max(vsize, raw)) return rva - va + bytes > raw ? -1 : off + rva - va;
        }
        return -1;
    }

    private static String hex(byte[] d) {
        StringBuilder s = new StringBuilder();
        for (byte x : d) s.append(String.format(Locale.ROOT, "%02x", x & 0xFF));
        return s.toString();
    }

    private final PcLauncherActivity a;
    private final FrameLayout overlay;
    private int step;
    private volatile boolean busy;
    private TextView status, progress;
    private LauncherUi.Bar bar;
    private Btn pick, pickFolder, next;
    private String lastStatus = "", lastProgress = "";
    private float lastFraction;
    private long bytesDone, bytesTotal, lastReport;

    PcSetup(PcLauncherActivity a) {
        this.a = a;
        overlay = new FrameLayout(a);
        overlay.setBackgroundColor(BG);
        overlay.setClickable(true);
        overlay.setFitsSystemWindows(true);
        overlay.setVisibility(View.GONE);
    }

    View view() { return overlay; }
    boolean visible() { return overlay.getVisibility() == View.VISIBLE; }
    boolean isBusy() { return busy; }

    void show(int at) {
        step = at;
        overlay.setVisibility(View.VISIBLE);
        render();
    }

    void showData() { show(DATA); }

    /** Back: the previous step; false on the first one or while copying is not going to stop. */
    boolean back() {
        if (busy) return true;
        if (step == WELCOME) return false;
        step--;
        render();
        return true;
    }

    private void go(int to) {
        if (to >= STEPS) {
            a.prefs().edit().putBoolean("setupDone", true).apply();
            overlay.setVisibility(View.GONE);
            a.refresh();
            return;
        }
        step = to;
        render();
    }

    private TextView body(String s) { return text(a, s, 15, MUTED, false); }

    private LinearLayout.LayoutParams gap(int topDp) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(-2, -2);
        p.topMargin = dp(a, topDp);
        return p;
    }

    /** A settings row takes the card's whole width, so its control sits at the right edge away from the label. */
    private LinearLayout.LayoutParams wide(int topDp) {
        LinearLayout.LayoutParams p = gap(topDp);
        p.width = -1;
        return p;
    }

    private void render() {
        overlay.removeAllViews();
        status = progress = null;
        bar = null;
        pick = pickFolder = next = null;
        overlay.addView(new LauncherUi.Backdrop(a, step == WELCOME ? LauncherUi.Backdrop.CITY : LauncherUi.Backdrop.HORIZON, 0), new FrameLayout.LayoutParams(-1, -1));
        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundColor(Color.WHITE);
        card.setPadding(dp(a, 32), dp(a, 28), dp(a, 32), dp(a, 24));
        ScrollView scroll = new ScrollView(a);
        scroll.addView(card);
        int width = a.getResources().getDisplayMetrics().widthPixels;
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(Math.min(dp(a, 620), width - dp(a, 32)), -2);
        lp.leftMargin = dp(a, 16) + Math.max(0, (width - dp(a, 620)) / 8);
        lp.topMargin = dp(a, 24);
        lp.bottomMargin = dp(a, 24);
        overlay.addView(scroll, lp);
        card.addView(wordmark(a, 15));
        switch (step) {
            case WELCOME:
                card.addView(kicker(a, Tr.t("MIRROR'S EDGE · ПК-ВЕРСИЯ")), gap(22));
                card.addView(text(a, Tr.t("Настроим за пару минут"), 26, INK, true), gap(6));
                card.addView(body(Tr.t("Приложение запускает ПК-версию игры на Android. Файлы игры берутся из вашей копии GOG, в приложении их нет.")), gap(8));
                card.addView(row(a, text(a, Tr.t("Язык игры"), 15, INK, false), a.languageBox()), wide(16));
                next = cta(a, Tr.t("Далее"), v -> go(DATA));
                break;
            case DATA:
                card.addView(kicker(a, Tr.t("ФАЙЛЫ ИГРЫ")), gap(22));
                card.addView(text(a, Tr.t("Подключите свою копию"), 26, INK, true), gap(6));
                card.addView(body(Tr.t("Нужен один файл: ZIP-архив папки с вашей игрой из GOG (внутри Binaries, Engine и TdGame). ")
                    + Tr.t("Приложение само его распакует. Нужно около 12 ГБ свободного места.")), gap(8));
                pick = cta(a, Tr.t("Выбрать архив игры (.zip)…"), v -> {
                    Intent open = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                    open.addCategory(Intent.CATEGORY_OPENABLE);
                    open.setType("*/*");
                    a.startActivityForResult(open, PICK_ZIP);
                });
                card.addView(pick, gap(16));
                pickFolder = ghost(a, Tr.t("Или выбрать распакованную папку…"), v -> a.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), PICK_GAME));
                card.addView(pickFolder, gap(10));
                status = text(a, "", 15, INK, false);
                progress = text(a, "", 13, MUTED, false);
                bar = new LauncherUi.Bar(a);
                card.addView(status, gap(14));
                card.addView(bar, new LinearLayout.LayoutParams(-1, -2));
                card.addView(progress, gap(6));
                next = cta(a, Tr.t("Далее"), v -> go(OPTIONS));
                updateData();
                break;
            default:
                card.addView(kicker(a, Tr.t("НАСТРОЙКИ")), gap(22));
                card.addView(text(a, Tr.t("Почти готово"), 26, INK, true), gap(6));
                card.addView(body(Tr.t("Это можно изменить позже в разделе «Настройки».")), gap(8));
                String[] fps = new String[PcLauncherActivity.FPS.length];
                for (int i = 0; i < fps.length; ++i) fps[i] = String.valueOf(PcLauncherActivity.FPS[i]);
                int current = 0;
                for (int i = 0; i < fps.length; ++i) if (PcLauncherActivity.FPS[i] == a.prefs().getInt("fps", 62)) current = i;
                card.addView(row(a, text(a, Tr.t("Лимит кадров"), 15, INK, false),
                    segments(a, fps, current, i -> a.prefs().edit().putInt("fps", PcLauncherActivity.FPS[i]).apply())), wide(12));
                card.addView(row(a, text(a, Tr.t("Начальное разрешение"), 15, INK, false),
                    segments(a, PcLauncherActivity.SCALES, a.prefs().getInt("scale", 0), i -> a.prefs().edit().putInt("scale", i).apply())));
                next = cta(a, Tr.t("Готово"), v -> go(STEPS));
                break;
        }
        card.addView(next, gap(20));
        if (step == DATA) next.setVisibility(PcSetup.filesReady(a.pcDir()) ? View.VISIBLE : View.GONE);
    }

    private void updateData() {
        if (status == null) return;
        boolean ready = filesReady(a.pcDir());
        status.setText(busy ? lastStatus : ready ? Tr.t("Файлы игры на месте.") : lastStatus);
        progress.setText(lastProgress);
        bar.set(lastFraction);
        bar.setVisibility(busy || lastFraction > 0 ? View.VISIBLE : View.GONE);
        pick.setEnabled(!busy);
        pick.setAlpha(busy ? 0.4f : 1f);
        pickFolder.setEnabled(!busy);
        pickFolder.setAlpha(busy ? 0.4f : 1f);
        if (next != null) next.setVisibility(!busy && ready ? View.VISIBLE : View.GONE);
    }

    private void say(String s, String p, float fraction) {
        lastStatus = s;
        lastProgress = p;
        lastFraction = fraction;
        a.runOnUiThread(this::updateData);
    }

    void picked(Uri uri, boolean zip) {
        busy = true;
        a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        say(Tr.t("Проверяю копию игры…"), "", 0);
        new Thread(() -> install(uri, zip), "pc-install").start();
    }

    /** Relative path under the game folder: from the first Binaries/Engine/TdGame component; null if none. */
    private static String gamePath(String entry) {
        String[] parts = entry.replace('\\', '/').split("/");
        for (int i = 0; i < parts.length; ++i)
            for (String d : GAME_DIRS)
                if (parts[i].equalsIgnoreCase(d)) {
                    StringBuilder s = new StringBuilder(d);
                    for (int k = i + 1; k < parts.length; ++k) {
                        if (parts[k].equals("..")) return null;
                        s.append('/').append(parts[k]);
                    }
                    return s.toString();
                }
        return null;
    }

    private static String sha256(File f) throws Exception {
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 20];
            for (int n; (n = in.read(buf)) > 0; ) sha.update(buf, 0, n);
        }
        return hex(sha.digest());
    }

    /** One file of the archive, as its central directory describes it. */
    private static final class ZipItem {
        final int method; final long packed, size, crc, offset; final File target;
        ZipItem(int method, long packed, long size, long crc, long offset, File target) {
            this.method = method; this.packed = packed; this.size = size; this.crc = crc; this.offset = offset; this.target = target;
        }
    }

    private static ByteBuffer readAt(java.nio.channels.FileChannel in, long position, int size) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
        while (b.hasRemaining()) {
            int n = in.read(b, position);
            if (n <= 0) throw new EOFException(Tr.t("архив обрывается"));
            position += n;
        }
        b.flip();
        return b;
    }

    /** The game files of a ZIP, ZIP64 included; null when the directory cannot be used and the archive has to be streamed. */
    private static List<ZipItem> directory(java.nio.channels.FileChannel in, File pc) throws IOException {
        long size = in.size();
        if (size < 22) return null;
        int tailSize = (int) Math.min(size, 65557);
        ByteBuffer tail = readAt(in, size - tailSize, tailSize);
        int end = -1;
        for (int i = tailSize - 22; i >= 0 && end < 0; --i) if (tail.getInt(i) == 0x06054b50) end = i;
        if (end < 0) return null;
        long count = tail.getShort(end + 10) & 0xFFFF, length = tail.getInt(end + 12) & 0xFFFFFFFFL, start = tail.getInt(end + 16) & 0xFFFFFFFFL;
        if (end >= 20 && tail.getInt(end - 20) == 0x07064b50) {
            ByteBuffer z = readAt(in, tail.getLong(end - 20 + 8), 56);
            if (z.getInt(0) != 0x06064b50) return null;
            count = z.getLong(32);
            length = z.getLong(40);
            start = z.getLong(48);
        }
        if (length < 46 || length > 1 << 26 || start + length > size) return null;
        ByteBuffer c = readAt(in, start, (int) length);
        List<ZipItem> items = new ArrayList<>();
        final long wide = 0xFFFFFFFFL;
        for (long i = 0, p = 0; i < count; ++i) {
            int at = (int) p;
            if (at + 46 > length || c.getInt(at) != 0x02014b50) return null;
            int flags = c.getShort(at + 8) & 0xFFFF, method = c.getShort(at + 10) & 0xFFFF;
            int nameLength = c.getShort(at + 28) & 0xFFFF, extraLength = c.getShort(at + 30) & 0xFFFF, commentLength = c.getShort(at + 32) & 0xFFFF;
            long crc = c.getInt(at + 16) & wide, packed = c.getInt(at + 20) & wide, plain = c.getInt(at + 24) & wide, offset = c.getInt(at + 42) & wide;
            for (int q = at + 46 + nameLength, stop = q + extraLength; q + 4 <= stop; ) {
                int id = c.getShort(q) & 0xFFFF, n = c.getShort(q + 2) & 0xFFFF, v = q + 4;
                if (id == 1) {
                    if (plain == wide) { plain = c.getLong(v); v += 8; }
                    if (packed == wide) { packed = c.getLong(v); v += 8; }
                    if (offset == wide) offset = c.getLong(v);
                }
                q += 4 + n;
            }
            byte[] raw = new byte[nameLength];
            for (int k = 0; k < nameLength; ++k) raw[k] = c.get(at + 46 + k);
            String name = new String(raw, java.nio.charset.StandardCharsets.UTF_8);
            p += 46 + nameLength + extraLength + commentLength;
            String rel = name.endsWith("/") ? null : gamePath(name);
            if (rel == null) continue;
            if ((flags & 1) != 0 || (method != 0 && method != 8)) return null;
            items.add(new ZipItem(method, packed, plain, crc, offset, new File(pc, "MirrorsEdge/" + rel)));
        }
        return items;
    }

    /** Inflates one file with its own buffers; called from several threads. A file that fails its CRC is removed. */
    private void extract(java.nio.channels.FileChannel in, ZipItem z) throws IOException, java.util.zip.DataFormatException {
        mkdirs(z.target.getParentFile());
        ByteBuffer head = readAt(in, z.offset, 30);
        if (head.getInt(0) != 0x04034b50) throw new IOException(Tr.t("архив повреждён"));
        long position = z.offset + 30 + (head.getShort(26) & 0xFFFF) + (head.getShort(28) & 0xFFFF), left = z.packed, total = 0;
        byte[] input = new byte[1 << 20], output = new byte[1 << 20];
        java.util.zip.CRC32 crc = new java.util.zip.CRC32();
        java.util.zip.Inflater inflater = z.method == 8 ? new java.util.zip.Inflater(true) : null;
        boolean padded = false;
        try (OutputStream out = new FileOutputStream(z.target)) {
            while (inflater == null ? left > 0 : !inflater.finished()) {
                int ready = 0;
                if (inflater == null || inflater.needsInput()) {
                    if (left > 0) {
                        ByteBuffer chunk = ByteBuffer.wrap(input, 0, (int) Math.min(left, input.length));
                        while (chunk.hasRemaining()) {
                            int n = in.read(chunk, position);
                            if (n <= 0) throw new EOFException(Tr.t("архив обрывается"));
                            position += n;
                        }
                        ready = chunk.limit();
                        left -= ready;
                        advance(ready);
                        if (inflater != null) inflater.setInput(input, 0, ready);
                    } else if (!padded) {  // raw deflate may ask for one byte past the data
                        padded = true;
                        inflater.setInput(new byte[1]);
                    } else throw new IOException(Tr.t("архив повреждён"));
                }
                byte[] data = inflater == null ? input : output;
                int n = inflater == null ? ready : inflater.inflate(output);
                if (n > 0) {
                    out.write(data, 0, n);
                    crc.update(data, 0, n);
                    total += n;
                } else if (inflater != null && inflater.needsDictionary()) throw new IOException(Tr.t("архив повреждён"));
            }
        } finally {
            if (inflater != null) inflater.end();
        }
        if (total != z.size || crc.getValue() != z.crc) {
            z.target.delete();
            throw new IOException(Tr.t("архив повреждён: ") + z.target.getName());
        }
    }

    /** The executable first, so a wrong copy stops at once; then the rest on several cores. */
    private void unzipParallel(java.nio.channels.FileChannel in, List<ZipItem> items, File pc) throws Exception {
        File exe = new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe");
        ZipItem first = null;
        long missing = 0;
        bytesDone = bytesTotal = 0;
        java.util.concurrent.ConcurrentLinkedQueue<ZipItem> queue = new java.util.concurrent.ConcurrentLinkedQueue<>();
        for (ZipItem z : items) {
            bytesTotal += z.packed;
            boolean present = z.target.isFile() && z.target.length() == z.size;
            if (!present) missing += z.size;
            if (z.target.equals(exe)) first = z;
            else if (present) bytesDone += z.packed;
            else queue.add(z);
        }
        if (first == null) throw new IOException(Tr.t("в архиве нет Binaries/MirrorsEdge.exe"));
        if (pc.getUsableSpace() < missing + (1L << 30))
            throw new IOException(String.format(Locale.US, Tr.t("мало места: нужно не меньше %.1f ГБ"), (missing + (1L << 30)) / 1e9));
        if (exe.isFile() && exe.length() == first.size) advance(first.packed);
        else extract(in, first);
        checkExe(exe);
        int threads = Math.max(2, Math.min(6, Runtime.getRuntime().availableProcessors()));
        java.util.concurrent.ExecutorService pool = java.util.concurrent.Executors.newFixedThreadPool(threads);
        List<java.util.concurrent.Future<Void>> runs = new ArrayList<>();
        for (int i = 0; i < threads; ++i) runs.add(pool.submit(() -> {
            try {
                for (ZipItem z; (z = queue.poll()) != null; ) extract(in, z);
            } catch (Exception e) {
                queue.clear();  // the other threads finish their current file and stop
                throw e;
            }
            return null;
        }));
        pool.shutdown();
        for (java.util.concurrent.Future<Void> run : runs) {
            try { run.get(); }
            catch (java.util.concurrent.ExecutionException e) { throw e.getCause() instanceof Exception ? (Exception) e.getCause() : e; }
        }
    }

    private synchronized void advance(long n) {
        bytesDone += n;
        report("zip");
    }

    /** Direct access to the archive when the provider gives a seekable file; streaming otherwise. */
    private void unzip(Uri uri, File pc) throws Exception {
        try (android.os.ParcelFileDescriptor fd = a.getContentResolver().openFileDescriptor(uri, "r")) {
            if (fd != null) try (FileInputStream file = new FileInputStream(fd.getFileDescriptor())) {
                java.nio.channels.FileChannel in = file.getChannel();
                List<ZipItem> items = null;
                try { items = directory(in, pc); }
                catch (IOException e) { android.util.Log.w("PcSetup", "no random access to the archive: " + e); }
                if (items != null) { unzipParallel(in, items, pc); return; }
            }
        }
        unzipStream(uri, pc);
    }

    /** One pass over the archive; the check of the executable stops a wrong copy as soon as it is out. */
    private void unzipStream(Uri uri, File pc) throws Exception {
        long size = -1;
        try (Cursor c = a.getContentResolver().query(uri, new String[] {android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst() && !c.isNull(0)) size = c.getLong(0);
        }
        if (size > 0 && pc.getUsableSpace() < size + (1L << 30))
            throw new IOException(String.format(Locale.US, Tr.t("мало места: нужно не меньше %.1f ГБ"), (size + (1L << 30)) / 1e9));
        bytesDone = 0;
        bytesTotal = Math.max(size, 0);
        File exe = new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe");
        boolean checked = false;
        byte[] buf = new byte[1 << 20];
        InputStream raw = a.getContentResolver().openInputStream(uri);
        if (raw == null) throw new IOException(Tr.t("не удалось открыть архив"));
        try (java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(new BufferedInputStream(new FilterInputStream(raw) {
            @Override public int read(byte[] b, int off, int len) throws IOException {
                int n = super.read(b, off, len);
                if (n > 0) { bytesDone += n; report("zip"); }
                return n;
            }
        }, 1 << 20))) {
            for (java.util.zip.ZipEntry e; (e = zip.getNextEntry()) != null; ) {
                String rel = e.isDirectory() ? null : gamePath(e.getName());
                if (rel == null) continue;
                File target = new File(pc, "MirrorsEdge/" + rel);
                if (e.getSize() >= 0 && target.isFile() && target.length() == e.getSize()) {
                    if (target.equals(exe) && !checked) { checkExe(exe); checked = true; }
                    continue;
                }
                mkdirs(target.getParentFile());
                try (OutputStream out = new FileOutputStream(target)) {
                    for (int n; (n = zip.read(buf)) > 0; ) out.write(buf, 0, n);
                }
                if (target.equals(exe)) { checkExe(exe); checked = true; }
            }
        }
        if (!checked) throw new IOException(Tr.t("в архиве нет Binaries/MirrorsEdge.exe"));
    }

    private static void checkExe(File exe) throws Exception {
        if (!EXE_SHA256.equals(sha256(exe))) {
            exe.delete();
            throw new IOException(Tr.t("эта версия игры не подходит: нужна копия GOG той же сборки"));
        }
    }

    private static final class Entry {
        final String id, name; final boolean dir; final long size;
        Entry(String id, String name, boolean dir, long size) { this.id = id; this.name = name; this.dir = dir; this.size = size; }
    }

    private List<Entry> children(Uri tree, String parent) {
        List<Entry> out = new ArrayList<>();
        Uri uri = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parent);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE, DocumentsContract.Document.COLUMN_SIZE};
        try (Cursor c = a.getContentResolver().query(uri, columns, null, null, null)) {
            while (c != null && c.moveToNext())
                out.add(new Entry(c.getString(0), c.getString(1), DocumentsContract.Document.MIME_TYPE_DIR.equals(c.getString(2)), c.isNull(3) ? -1 : c.getLong(3)));
        }
        return out;
    }

    private void collect(Uri tree, Entry dir, File target, List<Entry> from, List<File> to) {
        for (Entry e : children(tree, dir.id)) {
            if (e.dir) collect(tree, e, new File(target, e.name), from, to);
            else { from.add(e); to.add(new File(target, e.name)); }
        }
    }

    private static Entry named(List<Entry> list, String name) {
        for (Entry e : list) if (e.name.equalsIgnoreCase(name)) return e;
        return null;
    }

    private static void mkdirs(File dir) throws IOException {
        if (dir != null && !dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) throw new IOException(Tr.t("не удалось создать ") + dir);
    }

    private void report(String file) {
        long now = System.currentTimeMillis();
        if (now - lastReport < 300) return;
        lastReport = now;
        say(Tr.t("Копирую файлы игры… Не закрывайте приложение."), String.format(Locale.US, Tr.t("%.2f / %.2f ГБ"), bytesDone / 1e9, bytesTotal / 1e9),
            bytesTotal > 0 ? (float) bytesDone / bytesTotal : 0);
    }

    /** Name of the socket RunnerBuilder reaches through `adb forward tcp:N localabstract:...`. */
    static final String RECEIVE_SOCKET = "org.mirrorsedge.research.receive";
    private static final int RECEIVE_WAIT_MS = 120_000;

    /**
     * Takes the game files from RunnerBuilder over adb (USB or Wi-Fi), so nothing has to be copied
     * to the device by hand. This app writes the files itself: files that adb writes into
     * Android/data belong to the shell user, and FUSE may not let the app read them.
     * Only adbd may connect (shell uid); any other local client is turned away.
     * Protocol (big-endian): server "RRX1"; client "RRX1", then 'T' total, 'F' path size (reply
     * 0 = already there, 1 = send; after the bytes 1 = written), 'E' (reply 1).
     * Any error is 3 + message, then the connection is closed.
     */
    void receive(boolean test) {
        if (busy) return;
        testing = test;
        busy = true;
        a.getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        say(Tr.t("Жду файлы игры с компьютера…"), "", 0);
        new Thread(this::receiveLoop, "pc-receive").start();
    }

    private volatile boolean connected;
    /** Check of the transfer only: files go to pc/transfer-test and are deleted at the end. */
    private boolean testing;

    private void receiveLoop() {
        String error = null;
        android.net.LocalServerSocket server = null;
        connected = false;
        try {
            server = new android.net.LocalServerSocket(RECEIVE_SOCKET);
            final android.net.LocalServerSocket listening = server;
            Thread timer = new Thread(() -> {
                try { Thread.sleep(RECEIVE_WAIT_MS); if (!connected) listening.close(); }
                catch (Exception ignored) { }
            }, "pc-receive-timeout");
            timer.setDaemon(true);
            timer.start();
            while (true) {
                android.net.LocalSocket peer;
                try { peer = server.accept(); }
                catch (IOException e) { throw new IOException(Tr.t("компьютер не подключился"), e); }
                int uid = peer.getPeerCredentials().getUid();
                if (uid != android.os.Process.SHELL_UID && uid != 0) { peer.close(); continue; }
                connected = true;
                try (android.net.LocalSocket s = peer) {
                    if (serve(new DataInputStream(new BufferedInputStream(s.getInputStream(), 1 << 20)),
                              new DataOutputStream(new BufferedOutputStream(s.getOutputStream())))) break;
                }
            }
        } catch (Exception e) {
            error = e.getMessage() == null ? e.toString() : e.getMessage();
        } finally {
            try { if (server != null) server.close(); } catch (IOException ignored) { }
        }
        final String failed = error;
        a.runOnUiThread(() -> {
            busy = false;
            a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lastStatus = failed == null ? Tr.t("Файлы игры на месте.") : Tr.t("Не получилось: ") + failed + ".";
            lastProgress = failed == null ? "" : Tr.t("Запустите установку на компьютере ещё раз, готовые файлы пропускаются.");
            updateData();
            a.refresh();
        });
    }

    /** One connection; true when the files are complete and checked. */
    private boolean serve(DataInputStream in, DataOutputStream out) throws IOException {
        File pc = a.pcDir();
        File test = new File(pc, "transfer-test");
        File root = testing ? new File(test, "MirrorsEdge") : new File(pc, "MirrorsEdge");
        byte[] buf = new byte[1 << 20];
        out.writeBytes("RRX1");
        out.flush();
        byte[] magic = new byte[4];
        in.readFully(magic);
        if (!"RRX1".equals(new String(magic, "US-ASCII"))) return false;
        try {
            mkdirs(pc);
            for (String[] b : BUNDLED) {
                try (InputStream ignored = a.getAssets().open("pcdeps/" + b[0])) { /* present */ }
                catch (IOException e) { throw new IOException(Tr.t("в этой сборке приложения нет библиотек ПК-версии")); }
            }
            while (true) {
                int cmd = in.read();
                if (cmd == 'T') {
                    bytesTotal = in.readLong();
                    bytesDone = 0;
                    long have = size(root);
                    if (pc.getUsableSpace() + have < bytesTotal + (1L << 30))
                        throw new IOException(String.format(Locale.US, Tr.t("мало места: нужно ещё %.1f ГБ"),
                            (bytesTotal + (1L << 30) - have - pc.getUsableSpace()) / 1e9));
                } else if (cmd == 'F') {
                    String path = in.readUTF();
                    long size = in.readLong();
                    File target = gameFile(root, path);
                    if (target.isFile() && target.length() == size) {
                        bytesDone += size;
                        out.write(0);
                        out.flush();
                        report(path);
                        continue;
                    }
                    out.write(1);
                    out.flush();
                    mkdirs(target.getParentFile());
                    File part = new File(target.getPath() + ".part");
                    try (OutputStream f = new FileOutputStream(part)) {
                        for (long left = size; left > 0; ) {
                            int n = in.read(buf, 0, (int) Math.min(buf.length, left));
                            if (n < 0) throw new EOFException(Tr.t("связь с компьютером прервалась"));
                            f.write(buf, 0, n);
                            left -= n;
                            bytesDone += n;
                            report(path);
                        }
                    }
                    if (!part.renameTo(target)) {
                        target.delete();
                        if (!part.renameTo(target)) throw new IOException(Tr.t("не удалось записать ") + path);
                    }
                    out.write(1);
                    out.flush();
                } else if (cmd == 'E' && testing) {
                    long got = size(root);
                    deleteAll(test);
                    if (got != bytesTotal) throw new IOException("transfer test: " + got + " of " + bytesTotal + " bytes");
                    android.util.Log.i("me_pc", "transfer test: " + got + " bytes received and deleted");
                    out.write(1);
                    out.flush();
                    return true;
                } else if (cmd == 'E') {
                    say(Tr.t("Проверяю копию игры…"), "", 1);
                    File exe = new File(root, "Binaries/MirrorsEdge.exe");
                    if (!exe.isFile()) throw new IOException(Tr.t("в Binaries нет MirrorsEdge.exe"));
                    MessageDigest sha = MessageDigest.getInstance("SHA-256");
                    try (InputStream f = new FileInputStream(exe)) {
                        for (int n; (n = f.read(buf)) > 0; ) sha.update(buf, 0, n);
                    }
                    if (!EXE_SHA256.equals(hex(sha.digest()))) throw new IOException(Tr.t("эта версия игры не подходит: нужна копия GOG той же сборки"));
                    finish(pc, buf);
                    out.write(1);
                    out.flush();
                    return true;
                } else {
                    throw new EOFException(Tr.t("связь с компьютером прервалась"));
                }
            }
        } catch (Exception e) {
            String m = e.getMessage() == null ? e.toString() : e.getMessage();
            try { out.write(3); out.writeUTF(m); out.flush(); } catch (IOException ignored) { }
            if (e instanceof IOException) throw (IOException) e;
            throw new IOException(m, e);
        }
    }

    /** A path from the computer, kept inside Binaries, Engine or TdGame of the game folder. */
    private static File gameFile(File root, String path) throws IOException {
        String p = path.replace('\\', '/');
        boolean known = false;
        for (String d : GAME_DIRS) known |= p.startsWith(d + "/");
        if (!known || p.contains("/../") || p.endsWith("/..") || p.contains("/./") || p.contains("//")
            || p.indexOf('\0') >= 0 || p.endsWith(".part"))
            throw new IOException(Tr.t("неверный путь: ") + path);
        return new File(root, p);
    }

    private static void deleteAll(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteAll(c);
        f.delete();
    }

    private static long size(File f) {
        if (f.isFile()) return f.length();
        long n = 0;
        File[] list = f.listFiles();
        if (list != null) for (File c : list) n += size(c);
        return n;
    }

    private void install(Uri tree, boolean zip) {
        String error = null;
        try {
            File pc = a.pcDir();
            mkdirs(pc);
            for (String[] b : BUNDLED) {
                try (InputStream in = a.getAssets().open("pcdeps/" + b[0])) { /* present */ }
                catch (IOException e) { throw new IOException(Tr.t("в этой сборке приложения нет библиотек ПК-версии")); }
            }
            byte[] buf = new byte[1 << 20];
            if (zip) unzip(tree, pc);
            else copyFolder(tree, pc, buf);
            finish(pc, buf);
        } catch (Exception e) {
            error = e.getMessage() == null ? e.toString() : e.getMessage();
        }
        final String failed = error;
        a.runOnUiThread(() -> {
            busy = false;
            a.getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            lastStatus = failed == null ? Tr.t("Файлы игры на месте.") : Tr.t("Не получилось: ") + failed + ".";
            lastProgress = failed == null ? "" : Tr.t("Можно выбрать файл ещё раз, готовые файлы пропускаются.");
            updateData();
            a.refresh();
        });
    }

    private void copyFolder(Uri tree, File pc, byte[] buf) throws Exception {
            String root = DocumentsContract.getTreeDocumentId(tree);
            List<Entry> top = children(tree, root);
            List<Entry> from = new ArrayList<>();
            List<File> to = new ArrayList<>();
            for (String d : GAME_DIRS) {
                Entry e = named(top, d);
                if (e == null || !e.dir) throw new IOException(Tr.t("в выбранной папке нет ") + d + Tr.t(" (нужна папка, где лежат Binaries, Engine и TdGame)"));
                collect(tree, e, new File(pc, "MirrorsEdge/" + d), from, to);
            }
            int exe = -1;
            for (int i = 0; i < from.size(); ++i) if (to.get(i).equals(new File(pc, "MirrorsEdge/Binaries/MirrorsEdge.exe"))) exe = i;
            if (exe < 0) throw new IOException(Tr.t("в Binaries нет MirrorsEdge.exe"));
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            try (InputStream in = open(tree, from.get(exe))) {
                for (int n; (n = in.read(buf)) > 0; ) sha.update(buf, 0, n);
            }
            if (!EXE_SHA256.equals(hex(sha.digest()))) throw new IOException(Tr.t("эта версия игры не подходит: нужна копия GOG той же сборки"));
            long missing = 0;
            bytesDone = bytesTotal = 0;
            for (int i = 0; i < from.size(); ++i) {
                long size = Math.max(0, from.get(i).size);
                bytesTotal += size;
                if (!(to.get(i).isFile() && to.get(i).length() == size)) missing += size;
            }
            if (pc.getUsableSpace() < missing + (1L << 30))
                throw new IOException(String.format(Locale.US, Tr.t("мало места: нужно ещё %.1f ГБ"), (missing + (1L << 30)) / 1e9));
            for (int i = 0; i < from.size(); ++i) {
                Entry e = from.get(i); File target = to.get(i);
                if (target.isFile() && e.size >= 0 && target.length() == e.size) { bytesDone += e.size; continue; }
                mkdirs(target.getParentFile());
                try (InputStream in = open(tree, e); OutputStream out = new FileOutputStream(target)) {
                    for (int n; (n = in.read(buf)) > 0; ) { out.write(buf, 0, n); bytesDone += n; report(e.name); }
                }
            }
    }

    private void finish(File pc, byte[] buf) throws Exception {
            say(Tr.t("Подготавливаю библиотеки…"), "", 1);
            mkdirs(new File(pc, "deps/rebased"));
            mkdirs(new File(pc, "user"));
            for (String[] b : BUNDLED) {
                try (InputStream in = a.getAssets().open("pcdeps/" + b[0]); OutputStream out = new FileOutputStream(new File(pc, b[1]))) {
                    for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                }
            }
            for (String[] r : REBASE) {
                byte[] copy = rebase(Files.readAllBytes(new File(pc, "MirrorsEdge/Binaries/" + r[0]).toPath()), (int) Long.parseLong(r[1], 16));
                if (!r[2].equals(hex(MessageDigest.getInstance("SHA-256").digest(copy)))) throw new IOException(r[0] + Tr.t(" не совпадает с поддерживаемой сборкой"));
                Files.write(new File(pc, "deps/rebased/" + r[0]).toPath(), copy);
            }
    }

    /**
     * The game has no entry for today's GPUs and falls back to its lowest preset. This returns its own
     * bucket 3 from DefaultCompat.ini with full-size textures and this screen's resolution, as Key=Value.
     * The game checks that file's integrity and refuses to start if it differs: only read it, never write.
     */
    static List<String> firstRunPreset(File pc, int resX, int resY) throws IOException {
        File file = new File(pc, "MirrorsEdge/TdGame/Config/DefaultCompat.ini");
        List<String> preset = new ArrayList<>();
        boolean inside = false;
        for (String l : new String(Files.readAllBytes(file.toPath()), java.nio.charset.StandardCharsets.ISO_8859_1).split("\r?\n")) {
            if (l.startsWith("[")) inside = l.equals("[AppCompatBucket3]");
            else if (inside && l.contains("=") && !l.startsWith(";")) {
                if (l.startsWith("ResX=")) preset.add("ResX=" + resX);
                else if (l.startsWith("ResY=")) preset.add("ResY=" + resY);
                else if (l.startsWith("TEXTUREGROUP_") && !l.matches("TEXTUREGROUP_(UI|Cinematic|RenderTarget)=.*"))
                    preset.add(l.substring(0, l.indexOf('=')) + "=(MinLODSize=256,MaxLODSize=4096,LODBias=0)");
                else preset.add(l);
            }
        }
        if (preset.isEmpty()) throw new IOException(Tr.t("в DefaultCompat.ini нет пресетов"));
        return preset;
    }

    private InputStream open(Uri tree, Entry e) throws IOException {
        InputStream in = a.getContentResolver().openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, e.id));
        if (in == null) throw new IOException(Tr.t("не удалось открыть ") + e.name);
        return in;
    }
}
