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

MODULES = [  # name, input binary, catalog
    ("msvcr80", "work/pc-deps/msvcr80.dll", "msvcr80/catalog.json"),
    ("msvcp80", "work/pc-deps/msvcp80.dll", "msvcp80/catalog.json"),
    ("game", "work/pc/Binaries/MirrorsEdge.exe", "catalog.json"),
]


def compile_one(cc, src, obj):
    if os.path.exists(obj) and os.path.getmtime(obj) >= os.path.getmtime(src) and \
            os.path.getmtime(obj) >= os.path.getmtime(os.path.join(RUNTIME, "x86rt.h")):
        return obj, None
    r = subprocess.run([cc, *CFLAGS, "-I", RUNTIME, "-c", src, "-o", obj], capture_output=True, text=True)
    return obj, (r.stderr if r.returncode else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toolchain", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--regen", action="store_true", help="regenerate the lifted C even if present")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 8)
    args = ap.parse_args()
    bindir = os.path.join(args.toolchain, "bin")
    cc = os.path.join(bindir, "x86_64-w64-mingw32-clang.exe")
    gen_root = os.path.join(args.work, "gen")
    obj_root = os.path.join(args.work, "obj")
    t0 = time.time()

    for name, binary, cat in MODULES:
        out = os.path.join(gen_root, name)
        if args.regen or not os.path.exists(os.path.join(out, f"dispatch_{name}.c")):
            subprocess.run([sys.executable, "-I", os.path.join(HERE, "gen_module.py"), binary, "--llvm", bindir,
                            "--catalog", os.path.join(args.work, cat), "--name", name, "--out", out], check=True)

    jobs = []
    for name, _, _ in MODULES:
        src_dir = os.path.join(gen_root, name)
        od = os.path.join(obj_root, name)
        os.makedirs(od, exist_ok=True)
        for f in sorted(os.listdir(src_dir)):
            if f.endswith(".c"):
                jobs.append((os.path.join(src_dir, f), os.path.join(od, f[:-2] + ".o")))
    od = os.path.join(obj_root, "runtime")
    os.makedirs(od, exist_ok=True)
    for f in ("rt_core.c", "rt_kernel32.c", "rt_sync.c", "rt_files.c", "rt_user32.c", "rt_stubs.c", "rt_host_win.c", "rt_main.c"):
        jobs.append((os.path.join(RUNTIME, f), os.path.join(od, f[:-2] + ".o")))

    errors = 0
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        for i, (obj, err) in enumerate(ex.map(lambda j: compile_one(cc, *j), jobs)):
            if err:
                errors += 1
                print(f"error in {obj}:\n{err[:3000]}")
            if (i + 1) % 50 == 0:
                print(f"compiled {i + 1}/{len(jobs)} ({time.time() - t0:.0f} s)", flush=True)
    if errors:
        sys.exit(f"{errors} files failed to compile")

    exe = os.path.join(args.work, "bin", "me_pc.exe")
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    rsp = os.path.join(obj_root, "link.rsp")
    with open(rsp, "w") as fp:
        for _, obj in jobs:
            fp.write(obj.replace("\\", "/") + "\n")
    subprocess.run([cc, f"@{rsp}", "-o", exe, "-Wl,--stack,8388608"], check=True)
    print(f"built {exe} in {time.time() - t0:.0f} s")


if __name__ == "__main__":
    main()
