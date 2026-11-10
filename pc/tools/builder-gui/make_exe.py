"""Builds RunnerBuilder.exe: the window (RunnerBuilder.cs) with the project sources embedded.

  python -I outputs/mirrors-edge-native/pc/tools/builder-gui/make_exe.py --dxvk <libdxvk_d3d9.so> \
      --adrenotools <folder> [--turnip <turnip-*.zip>] --out <folder>

The payload has no game code or data: the build scripts, the runtime sources and the published app
without game code (it holds DXVK with the patches in pc/dxvk and adrenotools), so the exe can be shared. Leave out
--turnip for a public build: the Turnip package is someone else's build. --licenses LABEL=FILE ...
puts the licence texts into the exe and into THIRD-PARTY-NOTICES.txt next to it.
"""
import argparse
import os
import subprocess
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
CSC = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Microsoft.NET", "Framework64", "v4.0.30319", "csc.exe")
TREES = ("pc/runtime",)
FILES = ("pc/dxvk/THIRD-PARTY.md", "LICENSE")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", required=True, help="RunnerRecompiled-shell.apk (apk_pack.py --make-shell)")
    ap.add_argument("--turnip")
    ap.add_argument("--licenses", nargs="*", default=[], metavar="LABEL=FILE")
    ap.add_argument("--out", required=True)
    ap.add_argument("--keep-payload", action="store_true", help="leave payload.zip next to the exe (tests)")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    payload = os.path.join(args.out, "payload.zip")
    notices = ""
    for item in args.licenses:
        label, path = item.split("=", 1)
        with open(path, encoding="utf-8") as f:
            notices += f"===== {label} =====\n\n{f.read().strip()}\n\n"
    if notices:
        with open(os.path.join(args.out, "THIRD-PARTY-NOTICES.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write(notices)
    with zipfile.ZipFile(payload, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        tools = os.path.join(REPO, "pc", "tools")
        for name in sorted(os.listdir(tools)):
            if name.endswith(".py"):
                z.write(os.path.join(tools, name), f"pc/tools/{name}")
        for tree in TREES:
            for base, _, names in os.walk(os.path.join(REPO, *tree.split("/"))):
                for name in names:
                    path = os.path.join(base, name)
                    z.write(path, os.path.relpath(path, REPO).replace(os.sep, "/"))
        for name in sorted(os.listdir(os.path.join(HERE, "i18n"))):
            z.write(os.path.join(HERE, "i18n", name), f"pc/tools/builder-gui/i18n/{name}")
        for name in FILES:
            path = os.path.join(REPO, *name.split("/"))
            if os.path.isfile(path):
                z.write(path, name)
        if notices:
            z.writestr("prebuilt/THIRD-PARTY-NOTICES.txt", notices)
        z.write(args.shell, "prebuilt/RunnerRecompiled-shell.apk")
        if args.turnip:
            z.write(args.turnip, f"prebuilt/{os.path.basename(args.turnip)}")
    exe = os.path.join(args.out, "RunnerBuilder.exe")
    subprocess.run([CSC, "/nologo", "/target:winexe", "/optimize+", "/codepage:65001", f"/out:{exe}",
                    f"/win32icon:{os.path.join(HERE, 'runner.ico')}",
                    f"/resource:{payload},payload.zip",
                    *[f"/resource:{os.path.join(HERE, 'i18n', n)},i18n_{n[:-4]}" for n in sorted(os.listdir(os.path.join(HERE, "i18n")))],
                    "/r:System.Windows.Forms.dll", "/r:System.Drawing.dll",
                    "/r:System.IO.Compression.dll", "/r:System.IO.Compression.FileSystem.dll",
                    os.path.join(HERE, "RunnerBuilder.cs")], check=True)
    if not args.keep_payload:
        os.remove(payload)
    print(f"{exe}: {os.path.getsize(exe) / 2 ** 20:.1f} MB")


if __name__ == "__main__":
    main()
