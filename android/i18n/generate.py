"""Writes the translation tables from app.py and builder_strings.py. Run after changing them:

  python -I outputs/mirrors-edge-native/android/i18n/generate.py
"""
import importlib.util
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def table(rows):
    return "".join(f"{k}\t{v}\n" for k, v in rows)


def esc(v):
    return v.replace("\n", "\\n") if "\n" in v else v


app, builder = load("app"), load("builder_strings")
raw = os.path.join(ROOT, "android", "res", "raw")
tools = os.path.join(ROOT, "pc", "tools", "builder-gui", "i18n")
os.makedirs(raw, exist_ok=True)
os.makedirs(tools, exist_ok=True)
for i, lang in enumerate(app.LANGS):
    rows = [(r[0], r[i + 1]) for r in app.ROWS]
    for r in app.NUMBERED:
        rows += [(r[0].replace("{n}", str(n)), r[i + 2].replace("{n}", str(n))) for n in r[1]]
    with open(os.path.join(raw, f"i18n_{lang}.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(table(rows))
    with open(os.path.join(tools, f"{lang}.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write(table((r[0], esc(r[1][i] if isinstance(r[1], tuple) else r[i + 1])) for r in builder.ROWS))
