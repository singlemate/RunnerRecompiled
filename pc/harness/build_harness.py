"""Builds and runs the differential harness for a sample of lifted functions.

From the project root:
  python -I outputs/mirrors-edge-native/pc/harness/build_harness.py work/pc/Binaries/MirrorsEdge.exe \
      --toolchain work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64 \
      --catalog work/pc-recomp/catalog.json --out work/pc-recomp/harness --count 3000 --cases 24

Selected: functions the lifter handles completely that call nothing (no direct, indirect or
import calls, no tail jumps to other functions) and do not touch fs:. The generated C and the
harness binary are derived from the game and stay in work/.
"""
import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import lift  # noqa: E402

LEAF_BAD = re.compile(r"x86_dispatch|x86_import|x86_trap|fs_base|\bf_[0-9A-F]{8}\(ctx\)")
FP_USE = re.compile(r"ST\(|x87_|ctx->xmm|ctx->mm|ctx->fcw|ctx->mxcsr")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--toolchain", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=3000)
    ap.add_argument("--cases", type=int, default=24)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", help="comma-separated entry addresses (hex) instead of a sample")
    ap.add_argument("--opt", default="-O2")
    ap.add_argument("--fp", action="store_true", help="only functions that use x87/SSE/MMX")
    args = ap.parse_args()

    L = lift.Lifter(args.exe, os.path.join(args.toolchain, "bin"), args.catalog)
    if args.only:
        entries = [int(x, 16) for x in args.only.split(",")]
    else:
        entries = sorted(L.entries)
        random.Random(args.seed).shuffle(entries)
    chosen = []
    for e in entries:
        if len(chosen) >= args.count:
            break
        try:
            src, fl = L.lift(e)
        except lift.Unsupported:
            continue
        if fl.unsupported:
            continue
        body = src.split("\n", 1)[1]
        if not args.only and LEAF_BAD.search(body):
            continue
        if args.fp and not FP_USE.search(body):
            continue
        chosen.append((e, src))
    chosen.sort()
    os.makedirs(args.out, exist_ok=True)
    c_path = os.path.join(args.out, "lifted_tests.c")
    with open(c_path, "w") as fp:
        fp.write('#include "x86rt.h"\n\n')
        for e, src in chosen:
            fp.write(src + "\n")
        fp.write("typedef struct { uint32_t entry; X86Fn fn; uint32_t flags; } TestFn;\n")
        fp.write("const TestFn lifted_tests[] = {\n")
        for e, src in chosen:
            flags = (1 if "x87_status" in src else 0) | (2 if "1.0f / " in src else 0) | (
                4 if re.search(r"x87_trig|__builtin_(exp2|log2|atan2|fmod|remainder)", src) else 0) | (
                8 if L.functions[e]["origin"] not in ("call", "entry", "safeseh") else 0) | (
                16 if "ctx->mm[" in src else 0) | (32 if "x87_round" in src else 0)
            fp.write(f"    {{0x{e:X}u, f_{e:08X}, {flags}u}},\n")
        fp.write("};\n")
        fp.write(f"const int lifted_tests_count = {len(chosen)};\n")
    print(f"{len(chosen)} functions -> {c_path}")

    cc = os.path.join(args.toolchain, "bin", "i686-w64-mingw32-clang.exe")
    exe = os.path.join(args.out, "harness.exe")
    rt = os.path.join(HERE, "..", "runtime")
    # -mllvm -fast-isel: clang 23.1.3's i686 SelectionDAG drops the `and` in
    # (x & 0xFFFFFF00) | zext(load i8 [x + k]) and emits a bare movzx (the IR is correct; x86-64
    # and AArch64 are not affected). FastISel keeps the IR-level -O2 optimizations.
    cmd = [cc, args.opt, "-mllvm", "-fast-isel", "-g0", "-DX86_MEM_BASE=0", "-I", rt, "-Wno-unused-label", "-Wno-unused-variable",
           "-Wno-unused-but-set-variable", "-fwrapv", "-msse2", "-mfpmath=sse", "-ffp-contract=off",
           os.path.join(HERE, "harness.c"), os.path.join(HERE, "trampoline.S"), c_path,
           "-o", exe, "-Wl,--image-base,0x10000000", "-Wl,--disable-dynamicbase"]
    subprocess.run(cmd, check=True)
    report = os.path.join(args.out, "harness-report.txt")
    r = subprocess.run([exe, args.exe, str(args.cases), report])
    print(f"exit {r.returncode}; report {report}")
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
