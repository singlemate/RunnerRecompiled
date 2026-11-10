"""Builds your own PC-version APK of RunnerRecompiled from your own GOG copy of Mirror's Edge.

Windows only. See pc/SELFBUILD.md for what to install first.

  python -I pc/tools/selfbuild.py --game "C:/GOG Games/Mirror's Edge" --dxvk libdxvk_d3d9.so \
      --toolchain <llvm-mingw> --ndk <android-ndk> --sdk <android-sdk> --jdk <jdk> --workspace selfbuild

Every input is checked by SHA-256 before anything is built: the lifted code only fits this exact
build of the game and of the runtime libraries. Everything derived from the game goes into the
workspace folder and into the APK, which are yours alone: never publish or share them.
"""
import argparse
import glob
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from rebase_pe import rebase  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
GAME_EXE = "58de3df21f40e9953e00be92f7749097832bad02e5b657d8485cdb61f360a3dd"
# Accepted builds of the CRT, one pair per column: 8.0.50727.9680 (what Windows Update leaves in
# WinSxS) and 8.0.50727.6195 (what Microsoft's current installer contains).
CRT = {
    "msvcr80.dll": ("a74e18c475e7853d6158ae668dcc3f6c30c7b9eb7bb2360a8b58d945c1e81fbb",
                    "c3ac989c8489a23bb96400b1856f5325ffc67e844f04651ea5d61bc20a991c6d"),
    "msvcp80.dll": ("ba5b61eb652f28cda1a80e2ee6839bd0bb043a098d0748f4817fb006b6887949",
                    "372af797353f9335915cd06d4076bab8410775dcaf2dac0593197d7c41bbffb2"),
}
# PhysX System Software 2.8.0: name, SHA-256 of the file, new base, SHA-256 after rebasing.
PHYSX = [
    ("PhysXCore.dll", "ed8a3f0cf46045699f05437348a1bd589a4e023ade5697c13dbc6c56befa545d", 0x21000000,
     "7811b7010a272e308b678655897bd177255a0df7cc63ae181ed1b06ac6bce5d2"),
    ("PhysXCooking.dll", "1371a94489d6b1ed2fd72138f25d15e4b234ea14028363eef2602d9d8cce9094", 0x22000000,
     "43ce2d8c15a8aa7ae7db4979702ade1959558c3a0c5b2944c6b20ed4ca679d5d"),
]
# DLLs of the game's Binaries folder: name, new base, SHA-256 after rebasing.
GAME_DLLS = [
    ("PhysXLoader.dll", 0x20000000, "f2ae5f04911a781c4e5b4fd800a0819d9d3c34690ff1be3536a809039fbfbc49"),
    ("NxCooking.dll", 0x23000000, "ca57712c0234e1d51acd81aea05cf7ef4ca333e76c7b191319e0fe3c8b881603"),
    ("PhysXExtensions.dll", 0x24000000, "01942e506440658b628bdbf2e85bfbde47fcd2ac4725323543e8234d6ed82909"),
    ("NxCharacter.dll", 0x25000000, "c5f7f6ceccae3d050452ab6d5a87059bcf81e4bd584486723ea1ae7b97a904f1"),
    ("ogg.dll", 0x26000000, "fcc4536f8ae0894eef07eb68f2c22225bd4cf5926f4698f25b0f3ec2deee155f"),
    ("vorbis.dll", 0x27000000, "6736a4b8f79c135b1bc01b0e9ca5da09d853e2ab3e667cf1f9d305b6e8e5d1a9"),
    ("vorbisfile.dll", 0x28000000, "34aab92c2c9c8c152b68a631ae06c345e7963778710ff687ec4a7cf05608eb98"),
    ("vorbisenc.dll", 0x29000000, "e7d1a36e7289c3b26cd1d05c8be780f5832356bcfa0c2878fa8232c367a7f353"),
    ("libresample.dll", 0x2A000000, "f352bab07ccacafbdebc27ef8f64057081f13c826a8acb3d44ec3b6be02d5a2e"),
    ("binkw32.dll", 0x2B000000, "8a7683e0d9675b347cd60a075c9092fdd6ae8701235e1ea7efa537256e53c3a8"),
]
# tag under work/pc-recomp, binary relative to the workspace
CATALOGS = [("msvcr80", "work/pc-deps/msvcr80.dll"), ("msvcp80", "work/pc-deps/msvcp80.dll")] + [
    (name[:-4].lower(), f"work/pc-deps/rebased/{name}") for name, *_ in PHYSX + GAME_DLLS]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fail(message):
    sys.exit(f"\nSTOP: {message}")


def find_by_hash(name, want, patterns):
    """The first file called `name` with this hash in the given glob patterns."""
    seen = []
    for pattern in patterns:
        for path in glob.glob(os.path.join(pattern, name)):
            seen.append(path)
            if sha256(path) == want:
                return path
    return None if not seen else ""


def locate_libraries(crt_dir=None, physx_dir=None):
    """Finds the Visual C++ 2005 and PhysX 2.8.0 DLLs by hash: ({name: path}, [names not found])."""
    found, missing = {}, []
    windir = os.environ.get("WINDIR", r"C:\Windows")
    sxs = os.path.join(glob.escape(os.path.join(windir, "WinSxS")), "x86_microsoft.vc80.crt_*")
    for column in range(2):  # never mix the two builds
        pair = {name: find_by_hash(name, want[column], [glob.escape(crt_dir)] if crt_dir else [sxs])
                for name, want in CRT.items()}
        if all(pair.values()):
            break
    found.update(pair)
    pf86 = os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")
    engine = os.path.join(glob.escape(os.path.join(pf86, "NVIDIA Corporation", "PhysX", "Engine")), "*")
    for name, want, *_ in PHYSX:
        path = find_by_hash(name, want, [glob.escape(physx_dir)] if physx_dir else [engine])
        found[name] = path
    missing = [name for name, path in found.items() if not path]
    return {name: path for name, path in found.items() if path}, missing


def require_dir(path, what):
    if not path or not os.path.isdir(path):
        fail(f"{what} not found: {path}")


def main():
    ap = argparse.ArgumentParser(description="Build a personal PC-version APK from your own GOG copy.")
    ap.add_argument("--game", required=True, help="GOG install folder (holds Binaries, Engine, TdGame)")
    ap.add_argument("--shell", required=True, help="RunnerRecompiled-shell.apk: the published app without game code")
    ap.add_argument("--toolchain", required=True, help="llvm-mingw folder (ucrt, x86_64)")
    ap.add_argument("--ndk", required=True, help="Android NDK folder (clang, lld, sysroot)")
    ap.add_argument("--workspace", default="selfbuild", help="output folder (about 2 GB)")
    ap.add_argument("--physx", help="folder with PhysXCore.dll and PhysXCooking.dll 2.8.0, if not installed")
    ap.add_argument("--crt", help="folder with msvcr80.dll and msvcp80.dll, if not in WinSxS")
    ap.add_argument("--keystore", help="signing key; created if missing (default: in the workspace). "
                    "Keep the same one, or a new APK will not install over the old one.")
    args = ap.parse_args()

    if os.name != "nt":
        fail("the build scripts run on Windows only.")
    for path, what in ((args.toolchain, "llvm-mingw"), (args.ndk, "Android NDK")):
        require_dir(path, what)
    if not os.path.isfile(args.shell):
        fail(f"the app without game code not found: {args.shell}")
    args.toolchain, args.ndk, args.shell = (os.path.abspath(p) for p in (args.toolchain, args.ndk, args.shell))

    binaries = os.path.join(args.game, "Binaries")
    exe = os.path.join(binaries, "MirrorsEdge.exe")
    print("[1/6] Checking your files")
    if not os.path.isfile(exe):
        fail(f"no Binaries/MirrorsEdge.exe in {args.game}: point --game at the installed GOG game.")
    if sha256(exe) != GAME_EXE:
        fail("MirrorsEdge.exe is not the GOG build this project supports (Steam/EA/retail copies are protected or differ).")

    sources, missing = locate_libraries(args.crt, args.physx)
    for name in missing:
        if name in CRT:
            fail(f"{name} of Visual C++ 2005 (x86) not found. Install the current "
                 "Microsoft Visual C++ 2005 Redistributable (x86) or pass its folder with --crt.")
        fail(f"{name} 2.8.0 not found. Install NVIDIA PhysX System Software 9.23.1019 (it has Engine\\v2.8.0) "
             "or pass the folder with --physx.")

    work = os.path.abspath(args.workspace)
    pc_bin = os.path.join(work, "work", "pc", "Binaries")
    deps = os.path.join(work, "work", "pc-deps")
    rebased = os.path.join(deps, "rebased")
    for folder in (pc_bin, rebased, os.path.join(work, "work", "pc-recomp")):
        os.makedirs(folder, exist_ok=True)

    print("[2/6] Preparing the libraries")
    shutil.copyfile(exe, os.path.join(pc_bin, "MirrorsEdge.exe"))
    for name in CRT:
        shutil.copyfile(sources[name], os.path.join(deps, name))
    def place(src, name, base, want):
        out = os.path.join(rebased, name)
        rebase(src, out, base)
        if sha256(out) != want:
            fail(f"{name} does not match the supported build after rebasing.")
    for name, _, base, want in PHYSX:
        place(sources[name], name, base, want)
    for name, base, want in GAME_DLLS:
        src = os.path.join(binaries, name)
        if not os.path.isfile(src):
            fail(f"{name} is missing from {binaries}.")
        place(src, name, base, want)

    bindir = os.path.join(args.toolchain, "bin")
    print("[3/6] Finding the functions of the game (about a minute)")
    def catalog(binary, out):
        if not os.path.isfile(os.path.join(work, out, "catalog.json")):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "catalog.py"), binary, "--llvm", bindir, "--out", out],
                           cwd=work, check=True, stdout=subprocess.DEVNULL)
    recomp = os.path.join("work", "pc-recomp")
    catalog("work/pc/Binaries/MirrorsEdge.exe", recomp)
    for tag, binary in CATALOGS:
        catalog(binary, os.path.join(recomp, tag))

    print("[4/6] Translating and compiling (5 to 30 minutes, depending on the CPU)")
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "build_pc_runtime.py"), "--toolchain", args.toolchain,
                    "--work", recomp, "--android", args.ndk], cwd=work, check=True)
    lib = os.path.join(work, recomp, "bin-android", "libmepc.so")

    print("[5/6] Packing the APK")
    apk = os.path.join(work, "RunnerRecompiled-pc-personal.apk")
    keystore = os.path.abspath(args.keystore) if args.keystore else os.path.join(work, "personal-key.json")
    import apk_pack
    apk_pack.pack(args.shell, lib, deps, keystore, apk)

    print(f"\n[6/6] Done: {apk}")
    print("Install it, then choose a ZIP of your game folder (or the unpacked folder) in the app.")
    print("This APK contains code derived from your copy of the game: keep it to yourself, do not publish or share it.")


if __name__ == "__main__":
    main()
