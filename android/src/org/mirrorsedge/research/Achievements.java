package org.mirrorsedge.research;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/**
 * Achievement list for the launcher. Names, descriptions, gamerscore and icons come from
 * the title XDBF (SPA) resource of the user's own copy, found in the installed startup
 * bundle (me_native_entry_sections_resources.bin, a section starting with "XDBF"); nothing
 * of the game ships in the APK. Unlocks come from the runtime's per-profile store
 * me_saves/<XUID>/45410850/achievements.txt ("<id> <unix seconds>" lines).
 * Layout as xbox/startup/achievements.h (same rules for languages and absent strings).
 */
final class Achievements {
    static final class Item {
        int id, gamerscore, flags;
        long imageId;
        String label = "", description = "", unachieved = "";
        byte[] icon;           // PNG, may be null
        long unlockedAt = -1;  // unix seconds, -1 = locked
        boolean unlocked() { return unlockedAt >= 0; }
        /** Hidden until earned (the "show unachieved" bit 0x8 is clear). */
        boolean secret() { return (flags & 0x8) == 0; }
    }

    private Achievements() {}

    /** Reads the XDBF resource out of the startup bundle; null if absent. */
    static byte[] xdbfFromBundle(File bundle) throws IOException {
        if (!bundle.isFile()) return null;
        try (RandomAccessFile f = new RandomAccessFile(bundle, "r")) {
            byte[] magic = new byte[8];
            f.readFully(magic);
            if (!new String(magic, StandardCharsets.US_ASCII).equals("MEBOOT01")) return null;
            int count = Integer.reverseBytes(f.readInt());
            long at = 12;
            for (int i = 0; i < count && i < 64; ++i) {
                f.seek(at);
                f.readInt();  // address
                int size = Integer.reverseBytes(f.readInt());
                if (size < 0 || at + 8 + size > f.length()) return null;
                if (size >= 24) {
                    byte[] head = new byte[4];
                    f.readFully(head);
                    if (head[0] == 'X' && head[1] == 'D' && head[2] == 'B' && head[3] == 'F' && size < (32 << 20)) {
                        byte[] data = new byte[size];
                        f.seek(at + 8);
                        f.readFully(data);
                        return data;
                    }
                }
                at += 8 + size;
            }
        }
        return null;
    }

    private static int be16(byte[] d, int o) { return ((d[o] & 0xFF) << 8) | (d[o + 1] & 0xFF); }
    private static int be32(byte[] d, int o) { return ((d[o] & 0xFF) << 24) | ((d[o + 1] & 0xFF) << 16) | ((d[o + 2] & 0xFF) << 8) | (d[o + 3] & 0xFF); }

    private static final class Xdbf {
        final byte[] d; final int used, content;
        Xdbf(byte[] d) throws IOException {
            if (d == null || d.length < 24 || d[0] != 'X' || d[1] != 'D' || d[2] != 'B' || d[3] != 'F') throw new IOException("нет XDBF");
            int entries = be32(d, 8), free = be32(d, 16);
            used = be32(d, 12);
            long start = 24L + entries * 18L + free * 8L;
            if (used > entries || start > d.length) throw new IOException("повреждённый XDBF");
            this.d = d;
            content = (int) start;
        }
        /** {offset, length} of the entry, or null. */
        int[] entry(int section, long id) {
            for (int i = 0; i < used; ++i) {
                int e = 24 + i * 18;
                long eid = 0;
                for (int k = 2; k < 10; ++k) eid = (eid << 8) | (d[e + k] & 0xFF);
                if (be16(d, e) != section || eid != id) continue;
                long offset = content + (be32(d, e + 10) & 0xFFFFFFFFL), length = be32(d, e + 14) & 0xFFFFFFFFL;
                if (offset + length > d.length) return null;
                return new int[] {(int) offset, (int) length};
            }
            return null;
        }
        /** String id of a language table; null when absent. */
        String string(int language, int id) {
            int[] t = entry(3, language);
            if (t == null || t[1] < 14 || d[t[0]] != 'X' || d[t[0] + 1] != 'S' || d[t[0] + 2] != 'T' || d[t[0] + 3] != 'R') return null;
            int at = t[0] + 14, end = t[0] + t[1];
            for (int i = 0, n = be16(d, t[0] + 12); i < n && at + 4 <= end; ++i) {
                int sid = be16(d, at), len = be16(d, at + 2);
                at += 4;
                if (at + len > end) return null;
                if (sid == id) return new String(d, at, len, StandardCharsets.UTF_8);
                at += len;
            }
            return null;
        }
        int resolveLanguage(int requested) {
            if (string(requested, 0x8000) != null) return requested;
            int[] t = entry(1, 0x58535443L);  // XSTC: default language
            return t != null && t[1] >= 16 ? be32(d, t[0] + 12) : 1;
        }
    }

    /** Parses the list in the game language (falls back to the title's default language). */
    static List<Item> read(byte[] xdbf, int language) throws IOException {
        Xdbf x = new Xdbf(xdbf);
        int lang = x.resolveLanguage(language);
        int[] t = x.entry(1, 0x58414348L);  // XACH
        if (t == null || t[1] < 14) throw new IOException("в данных игры нет списка достижений");
        byte[] d = x.d;
        int count = be16(d, t[0] + 12);
        if (14 + count * 36 > t[1]) throw new IOException("повреждённый список достижений");
        List<Item> items = new ArrayList<>();
        for (int i = 0; i < count; ++i) {
            int e = t[0] + 14 + i * 36;
            Item a = new Item();
            a.id = be16(d, e);
            a.imageId = be32(d, e + 8) & 0xFFFFFFFFL;
            a.gamerscore = be16(d, e + 12);
            a.flags = be32(d, e + 16);
            a.label = text(x, lang, be16(d, e + 2));
            a.description = text(x, lang, be16(d, e + 4));
            a.unachieved = text(x, lang, be16(d, e + 6));
            int[] img = x.entry(2, a.imageId);
            if (img != null) a.icon = Arrays.copyOfRange(d, img[0], img[0] + img[1]);
            items.add(a);
        }
        return items;
    }

    private static String text(Xdbf x, int lang, int id) {
        if (id == 0xFFFF) return "";
        String s = x.string(lang, id);
        return s == null ? "" : s;
    }

    /** Marks unlocks from every profile's store under me_saves (union, earliest time wins). */
    static void applyUnlocks(List<Item> items, File saves) {
        Map<Integer, Long> unlocked = new HashMap<>();
        File[] profiles = saves.listFiles(File::isDirectory);
        if (profiles != null) for (File profile : profiles) {
            File store = new File(profile, "45410850/achievements.txt");
            if (!store.isFile()) continue;
            try (BufferedReader in = new BufferedReader(new InputStreamReader(new FileInputStream(store), StandardCharsets.US_ASCII))) {
                for (String line; (line = in.readLine()) != null; ) {
                    String[] p = line.trim().split("\\s+");
                    if (p.length < 2) continue;
                    try {
                        int id = Integer.parseInt(p[0]);
                        long when = Long.parseLong(p[1]);
                        Long old = unlocked.get(id);
                        if (old == null || when < old) unlocked.put(id, when);
                    } catch (NumberFormatException ignored) {}
                }
            } catch (IOException ignored) {}
        }
        for (Item a : items) { Long when = unlocked.get(a.id); a.unlockedAt = when == null ? -1 : when; }
    }
}
