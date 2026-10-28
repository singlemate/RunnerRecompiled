package org.mirrorsedge.research;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.*;
import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Installs the game from the user's own disc image (ISO) on the device. Plain Java, no
 * Android types, so the same code is checked on the PC against the PC tooling output.
 *
 * 1. XDVDFS: magic "MICROSOFT*XBOX*MEDIA" at 0x10000 from the game partition start
 *    (plain XISO, or a full XGD1/XGD2/XGD3 disc image).
 * 2. default.xex must be the exact build the native code was recompiled from (SHA-256).
 * 3. The XEX image is decrypted (AES-128-CBC, retail key, as XenonUtils xex.cpp), basic
 *    decompression applied, import thunks patched like Xex2LoadImage, then the startup
 *    bundle is built exactly like tools/prepare_xbox_entry_local.py + prepare_xex_resources.py:
 *      me_native_entry_sections_resources.bin (MEBOOT01) and me_native_xex_headers.bin.
 * 4. Game files go to me_game_readonly/ (lower-case paths) with inventory.txt.
 * Files already present with the right size are skipped, so an interrupted install resumes.
 */
final class IsoInstall {
    /** default.xex of the supported copy (title 45410850); the recompiled code matches only it. */
    static final String XEX_SHA256 = "3ed882fd1900ff2bbccf1de745fb7e92c7d2168bb06069cbab311ac8eff28e1b";
    static final int TITLE_ID = 0x45410850;
    static final String BUNDLE = "me_native_entry_sections_resources.bin", HEADERS = "me_native_xex_headers.bin";

    interface Progress { void update(String stage, long done, long total); }

    static final class Failure extends IOException { Failure(String m) { super(m); } }

    private static final int SECTOR = 0x800;
    private static final long[] PARTITIONS = {0, 0x18300000L, 0xFD90000L, 0x2080000L};  // XISO, XGD1, XGD2, XGD3
    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);
    // Xex2RetailKey (XenonUtils xex.h, MIT).
    private static final byte[] RETAIL_KEY = {0x20, (byte) 0xB1, (byte) 0x85, (byte) 0xA5, (byte) 0x9D, 0x28, (byte) 0xFD, (byte) 0xC3,
                                              0x40, 0x58, 0x3F, (byte) 0xBB, 0x08, (byte) 0x96, (byte) 0xBF, (byte) 0x91};
    // Variable exports (Xenia xboxkrnl_table.inc / xam_table.inc, kVariable): imports of these
    // are bound to guard pages by the runtime; the names go into the bundle.
    private static final Object[][] VARIABLE_EXPORTS = {
        {"xboxkrnl.exe", 0x0C, "ExConsoleGameRegion"}, {"xboxkrnl.exe", 0x0E, "ExEventObjectType"},
        {"xboxkrnl.exe", 0x12, "ExMutantObjectType"}, {"xboxkrnl.exe", 0x17, "ExSemaphoreObjectType"},
        {"xboxkrnl.exe", 0x1B, "ExThreadObjectType"}, {"xboxkrnl.exe", 0x1C, "ExTimerObjectType"},
        {"xboxkrnl.exe", 0x36, "IoCompletionObjectType"}, {"xboxkrnl.exe", 0x3A, "IoDeviceObjectType"},
        {"xboxkrnl.exe", 0x3E, "IoFileObjectType"}, {"xboxkrnl.exe", 0x59, "KeDebugMonitorData"},
        {"xboxkrnl.exe", 0xAD, "KeTimeStampBundle"}, {"xboxkrnl.exe", 0xB5, "KiBugCheckData"},
        {"xboxkrnl.exe", 0xCB, "Nls844UnicodeCaseTable"}, {"xboxkrnl.exe", 0x106, "ObDirectoryObjectType"},
        {"xboxkrnl.exe", 0x112, "ObSymbolicLinkObjectType"}, {"xboxkrnl.exe", 0x156, "XboxHardwareInfo"},
        {"xboxkrnl.exe", 0x157, "XboxKrnlBaseVersion"}, {"xboxkrnl.exe", 0x158, "XboxKrnlVersion"},
        {"xboxkrnl.exe", 0x193, "XexExecutableModuleHandle"}, {"xboxkrnl.exe", 0x1AE, "ExLoadedCommandLine"},
        {"xboxkrnl.exe", 0x1AF, "ExLoadedImageName"}, {"xboxkrnl.exe", 0x1BE, "VdGlobalDevice"},
        {"xboxkrnl.exe", 0x1BF, "VdGlobalXamDevice"}, {"xboxkrnl.exe", 0x1C0, "VdGpuClockInMHz"},
        {"xboxkrnl.exe", 0x1C1, "VdHSIOCalibrationLock"}, {"xboxkrnl.exe", 0x25C, "HalFsbInterruptCount"},
        {"xboxkrnl.exe", 0x266, "KeCertMonitorData"}, {"xboxkrnl.exe", 0x26B, "VdHSIOTrainCount"},
        {"xboxkrnl.exe", 0x26D, "VdHSIOTrainingStatus"}, {"xboxkrnl.exe", 0x26E, "RgcBindInfo"},
        {"xboxkrnl.exe", 0x2AB, "HalFsbResetCount"}, {"xboxkrnl.exe", 0x2DB, "UsbdBootEnumerationDoneEvent"},
        {"xboxkrnl.exe", 0x2DC, "StfsDeviceErrorEvent"}, {"xboxkrnl.exe", 0x2F1, "UsbdDriverLoadRequiredEvent"},
        {"xboxkrnl.exe", 0x345, "XboxKrnlVersion4Digit"},
        {"xam.xex", 0x3DB, "g_XuiAutomation"}, {"xam.xex", 0xA29, "XamSwapDiscPatchAddress"},
    };

    static final class Entry {
        final String path; final long block, size; final boolean dir;
        Entry(String path, long block, long size, boolean dir) { this.path = path; this.block = block; this.size = size; this.dir = dir; }
    }

    private final FileChannel iso;
    private long partition = -1;
    final List<Entry> entries = new ArrayList<>();

    IsoInstall(FileChannel iso) { this.iso = iso; }

    private byte[] read(long offset, int length) throws IOException {
        ByteBuffer b = ByteBuffer.allocate(length);
        while (b.hasRemaining()) {
            int n = iso.read(b, offset + b.position());
            if (n < 0) throw new Failure("образ обрывается (файл неполный?)");
        }
        return b.array();
    }

    /** Finds the game partition and lists every file and directory. */
    void scan() throws IOException {
        for (long p : PARTITIONS) {
            if (p + 0x10000 + 28 > iso.size()) continue;
            if (Arrays.equals(Arrays.copyOf(read(p + 0x10000, 20), 20), MAGIC)) { partition = p; break; }
        }
        if (partition < 0) throw new Failure("это не образ диска Xbox 360 (нет файловой системы XDVDFS)");
        ByteBuffer head = ByteBuffer.wrap(read(partition + 0x10000 + 20, 8)).order(ByteOrder.LITTLE_ENDIAN);
        long root = head.getInt() & 0xFFFFFFFFL, rootSize = head.getInt() & 0xFFFFFFFFL;
        entries.clear();
        directory(root, rootSize, "", 0);
    }

    private void directory(long block, long length, String parent, int depth) throws IOException {
        if (depth > 32 || length > (64 << 20)) throw new Failure("повреждённый каталог в образе");
        byte[] data = read(partition + block * SECTOR, (int) length);
        ByteBuffer b = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
        Deque<Integer> pending = new ArrayDeque<>();
        Set<Integer> seen = new HashSet<>();
        pending.push(0);
        while (!pending.isEmpty()) {
            int ordinal = pending.pop();
            if (!seen.add(ordinal) || seen.size() > 100000) continue;
            int o = ordinal * 4;
            if (o + 14 > data.length) throw new Failure("повреждённый каталог в образе");
            int left = b.getShort(o) & 0xFFFF, right = b.getShort(o + 2) & 0xFFFF;
            if (left == 0xFFFF && right == 0xFFFF) continue;  // sector padding
            long dataBlock = b.getInt(o + 4) & 0xFFFFFFFFL, size = b.getInt(o + 8) & 0xFFFFFFFFL;
            int attrs = data[o + 12] & 0xFF, nameLength = data[o + 13] & 0xFF;
            if (nameLength == 0 || o + 14 + nameLength > data.length) throw new Failure("повреждённый каталог в образе");
            String name = new String(data, o + 14, nameLength, StandardCharsets.US_ASCII);
            if (name.equals(".") || name.equals("..") || name.contains("/") || name.contains("\\") || name.contains(":"))
                throw new Failure("недопустимое имя файла в образе");
            String path = parent.isEmpty() ? name : parent + "/" + name;
            boolean dir = (attrs & 0x10) != 0;
            entries.add(new Entry(path, dataBlock, size, dir));
            if (dir && size > 0) directory(dataBlock, size, path, depth + 1);
            if (left != 0) pending.push(left);
            if (right != 0) pending.push(right);
        }
    }

    private Entry find(String path) {
        for (Entry e : entries) if (!e.dir && e.path.equalsIgnoreCase(path)) return e;
        return null;
    }

    private static int be32(byte[] d, int o) { return ((d[o] & 0xFF) << 24) | ((d[o + 1] & 0xFF) << 16) | ((d[o + 2] & 0xFF) << 8) | (d[o + 3] & 0xFF); }
    private static int le32(byte[] d, int o) { return (d[o] & 0xFF) | ((d[o + 1] & 0xFF) << 8) | ((d[o + 2] & 0xFF) << 16) | ((d[o + 3] & 0xFF) << 24); }
    private static int le16(byte[] d, int o) { return (d[o] & 0xFF) | ((d[o + 1] & 0xFF) << 8); }

    private static Map<Integer, Integer> optionalHeaders(byte[] xex) {
        Map<Integer, Integer> opts = new HashMap<>();
        int n = be32(xex, 20);
        for (int i = 0; i < n; ++i) opts.put(be32(xex, 24 + 8 * i), be32(xex, 28 + 8 * i));
        return opts;
    }

    /** Reads default.xex and checks it is the supported build. */
    byte[] readXex() throws IOException {
        Entry e = find("default.xex");
        if (e == null) throw new Failure("в образе нет default.xex");
        if (e.size > (64 << 20)) throw new Failure("default.xex слишком большой");
        byte[] xex = read(partition + e.block * SECTOR, (int) e.size);
        if (xex.length < 0x200 || xex[0] != 'X' || xex[1] != 'E' || xex[2] != 'X' || xex[3] != '2') throw new Failure("default.xex повреждён");
        Integer exec = optionalHeaders(xex).get(0x40006);
        int title = exec == null ? 0 : be32(xex, exec + 12);
        if (title != TITLE_ID) throw new Failure(String.format(Locale.US, "это другая игра (Title ID %08X)", title));
        if (!XEX_SHA256.equals(sha256(xex))) throw new Failure("эта версия игры не поддерживается: нужна та же сборка, из которой сделан код (Title ID 45410850, другой default.xex)");
        return xex;
    }

    static String sha256(byte[] data) {
        try {
            byte[] h = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder s = new StringBuilder();
            for (byte x : h) s.append(String.format("%02x", x));
            return s.toString();
        } catch (Exception e) { throw new RuntimeException(e); }
    }

    /** Decrypts and decompresses the XEX image and patches the import thunks (XenonUtils Xex2LoadImage). */
    static byte[] loadImage(byte[] xex) throws IOException {
        Map<Integer, Integer> opts = optionalHeaders(xex);
        int headerSize = be32(xex, 8), security = be32(xex, 16);
        Integer format = opts.get(0x3FF);
        if (format == null) throw new Failure("default.xex без описания формата");
        int infoSize = be32(xex, format), encryption = ((xex[format + 4] & 0xFF) << 8) | (xex[format + 5] & 0xFF),
            compression = ((xex[format + 6] & 0xFF) << 8) | (xex[format + 7] & 0xFF);
        byte[] body = Arrays.copyOfRange(xex, headerSize, xex.length);
        if (encryption == 1) {
            try {
                Cipher c = Cipher.getInstance("AES/CBC/NoPadding");
                c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(RETAIL_KEY, "AES"), new IvParameterSpec(new byte[16]));
                byte[] key = c.doFinal(Arrays.copyOfRange(xex, security + 0x150, security + 0x160));
                c.init(Cipher.DECRYPT_MODE, new SecretKeySpec(key, "AES"), new IvParameterSpec(new byte[16]));
                int whole = body.length & ~15;
                byte[] plain = c.doFinal(body, 0, whole);
                System.arraycopy(plain, 0, body, 0, whole);
            } catch (Exception e) { throw new Failure("не удалось расшифровать default.xex: " + e.getMessage()); }
        } else if (encryption != 0) throw new Failure("неизвестное шифрование default.xex");
        if (compression != 1) throw new Failure("неподдерживаемое сжатие default.xex (" + compression + ")");
        int blocks = infoSize / 8 - 1;
        long total = 0;
        for (int i = 0; i < blocks; ++i) total += (be32(xex, format + 8 + 8 * i) & 0xFFFFFFFFL) + (be32(xex, format + 12 + 8 * i) & 0xFFFFFFFFL);
        if (total > (256 << 20)) throw new Failure("default.xex: неверный размер образа");
        byte[] image = new byte[(int) total];
        int src = 0, dst = 0;
        for (int i = 0; i < blocks; ++i) {
            int data = be32(xex, format + 8 + 8 * i), zero = be32(xex, format + 12 + 8 * i);
            System.arraycopy(body, src, image, dst, data);
            src += data;
            dst += data + zero;
        }
        // Import thunks: every thunk word byte-swapped; function thunks (type != 0) become
        // nop, nop, nop, blr (byte layout as XenonUtils writes it).
        Integer imports = opts.get(0x103FF);
        if (imports != null) {
            int base = be32(xex, security + 0x110);
            Integer baseOverride = opts.get(0x10201);
            if (baseOverride != null) base = baseOverride;
            int stringTable = be32(xex, imports + 4), count = be32(xex, imports + 8);
            int library = imports + 12 + stringTable;
            for (int i = 0; i < count; ++i) {
                int records = ((xex[library + 38] & 0xFF) << 8) | (xex[library + 39] & 0xFF);
                for (int r = 0; r < records; ++r) {
                    int at = be32(xex, library + 40 + 4 * r) - base;
                    if (at < 0 || at + 16 > image.length) throw new Failure("default.xex: неверная таблица импорта");
                    int value = be32(image, at);
                    if ((value >>> 24) != 0) {
                        byte[] thunk = {0x60, 0, 0, 0, 0x60, 0, 0, 0, 0x60, 0, 0, 0, 0x4E, (byte) 0x80, 0, 0x20};
                        System.arraycopy(thunk, 0, image, at, 16);
                    } else {
                        image[at] = (byte) value; image[at + 1] = (byte) (value >> 8); image[at + 2] = (byte) (value >> 16); image[at + 3] = (byte) (value >>> 24);
                    }
                }
                library += 40 + records * 4;
            }
        }
        return image;
    }

    /** MEBOOT01 startup bundle: sections, resource ranges, variable-import guards. */
    static byte[] buildBundle(byte[] xex, byte[] image) throws IOException {
        Map<Integer, Integer> opts = optionalHeaders(xex);
        final long lo = 0x82000000L, hi = 0x83170000L;
        long base = be32(xex, be32(xex, 16) + 0x110) & 0xFFFFFFFFL;
        if (opts.get(0x10201) != null) base = opts.get(0x10201) & 0xFFFFFFFFL;
        // PE sections of the decoded image (VirtualAddress, VirtualSize).
        int nt = le32(image, 0x3C);
        int sectionCount = le16(image, nt + 6), optionalSize = le16(image, nt + 20);
        TreeMap<Long, byte[]> sections = new TreeMap<>();
        for (int i = 0; i < sectionCount; ++i) {
            int h = nt + 24 + optionalSize + 40 * i;
            long virtualSize = le32(image, h + 8) & 0xFFFFFFFFL, va = le32(image, h + 12) & 0xFFFFFFFFL,
                 rawSize = le32(image, h + 16) & 0xFFFFFFFFL;
            long addr = base + va;
            // As the PC tooling: a section that does not fit in the decoded image (here
            // .reloc, not loaded at run time) or has no raw bytes is left out.
            long available = rawSize == 0 || va + virtualSize > image.length ? 0 : virtualSize;
            if (available == 0 || addr < lo || addr >= hi || addr + available > hi) continue;
            sections.put(addr, Arrays.copyOfRange(image, (int) va, (int) (va + available)));
        }
        // Guards for imports of variable exports.
        List<byte[]> guards = new ArrayList<>();
        Integer imports = opts.get(0x103FF);
        if (imports == null) throw new Failure("default.xex без таблицы импорта");
        int total = be32(xex, imports), stringSize = be32(xex, imports + 4), count = be32(xex, imports + 8);
        List<String> names = new ArrayList<>();
        int cur = imports + 12;
        for (int i = 0; i < count; ++i) {
            int end = cur;
            while (xex[end] != 0) ++end;
            names.add(new String(xex, cur, end - cur, StandardCharsets.US_ASCII));
            cur = imports + 12 + (((end - (imports + 12)) + 4) & ~3);
        }
        int at = 12 + stringSize;
        while (at < total) {
            int p = imports + at, length = be32(xex, p);
            int index = ((xex[p + 36] & 0xFF) << 8) | (xex[p + 37] & 0xFF), records = ((xex[p + 38] & 0xFF) << 8) | (xex[p + 39] & 0xFF);
            String lib = names.get(index & 255);
            for (int r = 0; r < records; ++r) {
                long iat = be32(xex, p + 40 + 4 * r) & 0xFFFFFFFFL;
                Map.Entry<Long, byte[]> s = sections.floorEntry(iat);
                if (s == null || iat + 4 > s.getKey() + s.getValue().length) throw new Failure("импорт вне секций");
                int value = le32(s.getValue(), (int) (iat - s.getKey()));
                int ordinal = value & 0xFFFF;
                if ((value >>> 24) != 0) continue;
                String name = null;
                for (Object[] v : VARIABLE_EXPORTS) if (v[0].equals(lib) && (Integer) v[1] == ordinal) name = (String) v[2];
                if (name == null) continue;
                ByteBuffer g = ByteBuffer.allocate(108).order(ByteOrder.LITTLE_ENDIAN);
                g.putInt((int) iat).putInt(0x50000000 + guards.size() * 0x1000).putInt(ordinal);
                g.put(name.getBytes(StandardCharsets.US_ASCII));
                guards.add(g.array());
            }
            at += length;
        }
        // Resource ranges (XEX resource info) from the decoded image.
        List<long[]> resources = new ArrayList<>();
        Integer res = opts.get(0x2FF);
        if (res != null) {
            int length = be32(xex, res);
            for (int pos = res + 4; pos < res + length; pos += 16) {
                long address = be32(xex, pos + 8) & 0xFFFFFFFFL, size = be32(xex, pos + 12) & 0xFFFFFFFFL;
                long start = address - 0x82000000L;
                if (size == 0 || start < 0 || start + size > image.length || address + size > hi) throw new Failure("неверный ресурс в default.xex");
                resources.add(new long[] {address, size});
            }
        }
        if (sections.size() + resources.size() > 32) throw new Failure("слишком много секций");
        ByteArrayOutputStream out = new ByteArrayOutputStream(20 << 20);
        DataOutputStream le = new DataOutputStream(out);
        out.write("MEBOOT01".getBytes(StandardCharsets.US_ASCII));
        writeLe(out, sections.size() + resources.size());
        for (Map.Entry<Long, byte[]> s : sections.entrySet()) { writeLe(out, s.getKey().intValue()); writeLe(out, s.getValue().length); out.write(s.getValue()); }
        for (long[] r : resources) {
            writeLe(out, (int) r[0]); writeLe(out, (int) r[1]);
            out.write(image, (int) (r[0] - 0x82000000L), (int) r[1]);
        }
        writeLe(out, guards.size());
        for (byte[] g : guards) out.write(g);
        le.flush();
        return out.toByteArray();
    }

    private static void writeLe(OutputStream out, int v) throws IOException {
        out.write(v); out.write(v >>> 8); out.write(v >>> 16); out.write(v >>> 24);
    }

    /** Full install into dir (the app's files directory). */
    void install(File dir, Progress progress) throws IOException {
        progress.update("Чтение образа", 0, 1);
        scan();
        progress.update("Проверка версии игры", 0, 1);
        byte[] xex = readXex();
        progress.update("Подготовка кода игры", 0, 1);
        byte[] image = loadImage(xex);
        writeAtomically(new File(dir, BUNDLE), buildBundle(xex, image));
        writeAtomically(new File(dir, HEADERS), Arrays.copyOf(xex, be32(xex, 8)));
        // Game files: everything except the dashboard update and the executable.
        File root = new File(dir, "me_game_readonly");
        List<Entry> files = new ArrayList<>();
        long totalBytes = 0;
        for (Entry e : entries) {
            String lower = e.path.toLowerCase(Locale.ROOT);
            if (e.dir || lower.startsWith("$systemupdate") || lower.equals("default.xex")) continue;
            files.add(e);
            totalBytes += e.size;
        }
        long done = 0;
        byte[] buffer = new byte[1 << 20];
        for (Entry e : files) {
            File target = new File(root, e.path.toLowerCase(Locale.ROOT));
            if (target.isFile() && target.length() == e.size) { done += e.size; progress.update("Распаковка файлов игры", done, totalBytes); continue; }
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent);
            File temp = new File(target.getPath() + ".part");
            try (FileOutputStream out = new FileOutputStream(temp)) {
                long offset = partition + e.block * SECTOR, left = e.size;
                while (left > 0) {
                    int n = (int) Math.min(buffer.length, left);
                    ByteBuffer b = ByteBuffer.wrap(buffer, 0, n);
                    while (b.hasRemaining()) if (iso.read(b, offset + b.position()) < 0) throw new Failure("образ обрывается (файл неполный?)");
                    out.write(buffer, 0, n);
                    offset += n; left -= n; done += n;
                    progress.update("Распаковка файлов игры", done, totalBytes);
                }
            }
            if (target.exists() && !target.delete()) throw new IOException("replace " + target);
            if (!temp.renameTo(target)) throw new IOException("rename " + temp);
        }
        // inventory.txt last: its presence marks a finished install.
        List<String> inventory = new ArrayList<>();
        for (Entry e : entries) inventory.add(e.path.toLowerCase(Locale.ROOT));
        Collections.sort(inventory);
        StringBuilder text = new StringBuilder();
        for (String s : inventory) text.append(s).append('\n');
        writeAtomically(new File(root, "inventory.txt"), text.toString().getBytes(StandardCharsets.US_ASCII));
    }

    /** Bytes the install needs (game files + bundle), for the free-space check. */
    long requiredBytes() {
        long n = 40L << 20;
        for (Entry e : entries) if (!e.dir && !e.path.toLowerCase(Locale.ROOT).startsWith("$systemupdate")) n += e.size;
        return n;
    }

    private static void writeAtomically(File target, byte[] data) throws IOException {
        File parent = target.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("mkdir " + parent);
        File temp = new File(target.getPath() + ".part");
        try (FileOutputStream out = new FileOutputStream(temp)) { out.write(data); }
        if (target.exists() && !target.delete()) throw new IOException("replace " + target);
        if (!temp.renameTo(target)) throw new IOException("rename " + temp);
    }
}
