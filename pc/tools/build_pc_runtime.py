"""Builds the lifted PC game for a Windows x64 host (bring-up): generated shards of the game and
of the CRT DLLs, the runtime, one executable.

From the project root:
  python -I outputs/mirrors-edge-native/pc/tools/build_pc_runtime.py \
      --toolchain work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64 --work work/pc-recomp [--regen]

Expects catalogs in work/pc-recomp (game: catalog.json; msvcr80/, msvcp80/), the game in
work/pc/Binaries and the CRT DLLs in work/pc-deps. Output: work/pc-recomp/bin/me_pc.exe.
"""
import argparse
import concurrent.futures as cf
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.normpath(os.path.join(HERE, "..", "runtime"))
CFLAGS = ["-O2", "-g0", "-fwrapv", "-ffp-contract=off", "-fno-strict-aliasing", "-Wno-unused-label",
          "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function"]

# tag, binary to lift, catalog, guest module name, folder at run time (game | deps | rebased),
# DllMain at start-up (1) or on the first LoadLibrary (0). The PhysX DLLs all ask for 0x10000000:
# they are lifted from copies rebased with rebase_pe.py (0x20000000 + n * 0x01000000; the
# audio DLLs from 0x26000000).
MODULES = [
    ("msvcr80", "work/pc-deps/msvcr80.dll", "msvcr80/catalog.json", "msvcr80.dll", "deps", 1),
    ("msvcp80", "work/pc-deps/msvcp80.dll", "msvcp80/catalog.json", "msvcp80.dll", "deps", 1),
    ("physxloader", "work/pc-deps/rebased/PhysXLoader.dll", "physxloader/catalog.json", "physxloader.dll", "rebased", 0),
    ("physxcore", "work/pc-deps/rebased/PhysXCore.dll", "physxcore/catalog.json", "physxcore.dll", "rebased", 0),
    ("physxcooking", "work/pc-deps/rebased/PhysXCooking.dll", "physxcooking/catalog.json", "physxcooking.dll", "rebased", 0),
    ("nxcooking", "work/pc-deps/rebased/NxCooking.dll", "nxcooking/catalog.json", "nxcooking.dll", "rebased", 0),
    ("physxextensions", "work/pc-deps/rebased/PhysXExtensions.dll", "physxextensions/catalog.json", "physxextensions.dll", "rebased", 0),
    ("nxcharacter", "work/pc-deps/rebased/NxCharacter.dll", "nxcharacter/catalog.json", "nxcharacter.dll", "rebased", 0),
    # Ogg Vorbis decoding and resampling of the game's audio (static imports of the game).
    ("ogg", "work/pc-deps/rebased/ogg.dll", "ogg/catalog.json", "ogg.dll", "rebased", 0),
    ("vorbis", "work/pc-deps/rebased/vorbis.dll", "vorbis/catalog.json", "vorbis.dll", "rebased", 0),
    ("vorbisfile", "work/pc-deps/rebased/vorbisfile.dll", "vorbisfile/catalog.json", "vorbisfile.dll", "rebased", 0),
    ("vorbisenc", "work/pc-deps/rebased/vorbisenc.dll", "vorbisenc/catalog.json", "vorbisenc.dll", "rebased", 0),
    ("libresample", "work/pc-deps/rebased/libresample.dll", "libresample/catalog.json", "libresample.dll", "rebased", 0),
    # Bink video (static import of the game; several code sections, see pe32.code_blob).
    ("binkw32", "work/pc-deps/rebased/binkw32.dll", "binkw32/catalog.json", "binkw32.dll", "rebased", 0),
    ("game", "work/pc/Binaries/MirrorsEdge.exe", "catalog.json", "mirrorsedge.exe", "game", 0),
]


def write_module_table(path):
    """rt_modules_gen.c: the modules rt_main loads, in load order (the game last)."""
    lines = ['#include "rt.h"', ""]
    for tag, *_ in MODULES:
        lines.append(f"extern const X86FnEntry x86_fns_{tag}[];")
        lines.append(f"extern const uint32_t x86_fns_{tag}_count;")
    lines.append("")
    lines.append("const RtModuleSpec rt_module_specs[] = {")
    for tag, binary, _, name, kind, init in MODULES:
        lines.append(f'    {{"{name}", "{kind}", "{os.path.basename(binary)}", x86_fns_{tag}, &x86_fns_{tag}_count, {init}}},')
    lines.append("};")
    lines.append(f"const int rt_module_spec_count = {len(MODULES)};")
    new = "\n".join(lines) + "\n"
    if not os.path.exists(path) or open(path).read() != new:
        open(path, "w").write(new)


def compile_one(cc, src, obj, extra=()):
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and \
            os.path.getmtime(obj) >= os.path.getmtime(os.path.join(RUNTIME, "x86rt.h")):
        return obj, None
    flags = list(extra)
    if os.path.basename(src) == "rt_main.c" and "-fPIC" in flags:
        flags.append("-Dmain=me_pc_main")  # Android: a library entry the app calls on its thread
    r = subprocess.run([cc, *CFLAGS, *flags, "-I", RUNTIME, "-c", src, "-o", obj], capture_output=True, text=True)
    return obj, (r.stderr if r.returncode else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--regen", action="store_true", help="regenerate the lifted C even if present")
    ap.add_argument("--regen-only", help="comma-separated module tags to regenerate")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    ap.add_argument("--android", metavar="NDK", help="build libmepc.so for Android ARM64 with this NDK "
                    "(the lifted C is generated with --toolchain's LLVM as usual)")
    args = ap.parse_args()
    bindir = os.path.join(args.toolchain, "bin")
    cc = os.path.join(bindir, "x86_64-w64-mingw32-clang.exe")
    extra = ()
    gen_root = os.path.join(args.work, "gen")
    obj_root = os.path.join(args.work, "obj")
    host_file = "rt_host_win.c"
    if args.android:
        cc = os.path.join(args.android, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin", "clang.exe")
        extra = ("--target=aarch64-linux-android26", "-fPIC")
        obj_root = os.path.join(args.work, "obj-android")
        host_file = "rt_host_android.c"
    t0 = time.time()

    regen = set(args.regen_only.split(",")) if args.regen_only else None
    for name, binary, cat, *_ in MODULES:
        out = os.path.join(gen_root, name)
        if args.regen or (regen and name in regen) or not os.path.exists(os.path.join(out, f"dispatch_{name}.c")):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_module.py"), binary, "--llvm", bindir,
                            "--catalog", os.path.join(args.work, cat), "--name", name, "--out", out], check=True)

    os.makedirs(gen_root, exist_ok=True)
    write_module_table(os.path.join(gen_root, "rt_modules_gen.c"))
    jobs = [(os.path.join(gen_root, "rt_modules_gen.c"), os.path.join(obj_root, "rt_modules_gen.o"))]
    os.makedirs(obj_root, exist_ok=True)
    for name, *_ in MODULES:
        src_dir = os.path.join(gen_root, name)
        od = os.path.join(obj_root, name)
        os.makedirs(od, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".c"):
                jobs.append((os.path.join(src_dir, f), os.path.join(od, f[:-2] + ".o")))
    od = os.path.join(obj_root, "runtime")
    os.makedirs(od, exist_ok=True)
    for f in ("rt_core.c", "rt_kernel32.c", "rt_sync.c", "rt_files.c", "rt_user32.c", "rt_d3d9.c", "rt_d3d9_gen.c", "rt_physx_support.c", "rt_dinput.c", "rt_ue3.c", "rt_openal.c", "rt_stubs.c", host_file, "rt_main.c"):
        jobs.append((os.path.join(RUNTIME, f), os.path.join(od, f[:-2] + ".o")))

    errors = 0
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for i, (obj, err) in enumerate(ex.map(lambda j: compile_one(cc, *j, extra), jobs)):
            if err:
                errors += 1
                print(f"error in {obj}:\n{err[:3000]}")
            if (i + 1) % 50 == 0:
                print(f"compiled {i + 1}/{len(jobs)} ({time.time() - t0:.0f} s)", flush=True)
    if errors:
        sys.exit(f"{errors} files failed to compile")

    if args.android:
        lib = os.path.join(args.work, "bin-android", "libmepc.so")
        os.makedirs(os.path.dirname(lib), exist_ok=True)
        rsp = os.path.join(obj_root, "link.rsp")
        with open(rsp, "w") as fp:
            for _, obj in jobs:
                fp.write(obj.replace("\\", "/") + "\n")
        subprocess.run([cc, "--target=aarch64-linux-android26", "-shared", f"@{rsp}", "-o", lib,
                        "-Wl,-z,max-page-size=16384", "-llog", "-landroid", "-ldl", "-lm"], check=True)
        print(f"built {lib} in {time.time() - t0:.0f} s")
        return
    exe = os.path.join(args.work, "bin", "me_pc.exe")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    rsp = os.path.join(obj_root, "link.rsp")
    with open(rsp, "w") as fp:
        for _, obj in jobs:
            fp.write(obj.replace("\\", "/") + "\n")
    subprocess.run([cc, f"@{rsp}", "-o", exe, "-Wl,--stack,8388608", "-lwinmm"], check=True)
    print(f"built {exe} in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
