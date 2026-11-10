package org.mirrorsedge.research;

import android.content.Context;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;

/** Interface language: the Russian source text is the key into res/raw/i18n_<lang>.txt (android/i18n). */
final class Tr {
    private static HashMap<String, String> map;
    private static boolean loaded;

    private Tr() {}

    private static String unescape(String s) {
        return s.replace("\\\\", "\u0001").replace("\\n", "\n").replace("\u0001", "\\");
    }

    /** The menus follow the chosen game language; before one is chosen, the system's. */
    private static String language(String game) {
        if (game == null) {
            String system = Locale.getDefault().getLanguage();
            return system.equals("uk") || system.equals("be") ? "ru" : system;
        }
        switch (game) {
            case "rus": return "ru";
            case "deu": return "de";
            case "fra": return "fr";
            case "esn": return "es";
            case "por": return "pt";
            case "pol": return "pl";
            case "chs": case "cht": return "zh";
            default: return "en";
        }
    }

    private static synchronized void load() {
        if (loaded) return;
        loaded = true;
        try {
            Context c = (Context) Class.forName("android.app.ActivityThread").getMethod("currentApplication").invoke(null);
            String lang = language(c.getSharedPreferences("pc", Context.MODE_PRIVATE).getString("lang", null));
            if (lang.equals("ru")) return;
            int id = c.getResources().getIdentifier("i18n_" + lang, "raw", c.getPackageName());
            if (id == 0) id = c.getResources().getIdentifier("i18n_en", "raw", c.getPackageName());
            if (id == 0) return;
            HashMap<String, String> m = new HashMap<>();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getResources().openRawResource(id), StandardCharsets.UTF_8))) {
                for (String line; (line = r.readLine()) != null; ) {
                    int tab = line.indexOf('\t');
                    if (tab > 0) m.put(unescape(line.substring(0, tab)), unescape(line.substring(tab + 1)));
                }
            }
            map = m;
        } catch (Exception e) {
            android.util.Log.i("me_pc", "translations: " + e);
        }
    }

    /** The translation of a Russian interface string, or the string itself. */
    static String t(String source) {
        load();
        String v = map == null ? null : map.get(source);
        return v != null ? v : source;
    }
}
