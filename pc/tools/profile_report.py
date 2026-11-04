"""Names the samples of a ME_PC_PROFILE run with the symbols of me_pc.exe, per guest thread.

  python -I outputs/mirrors-edge-native/pc/tools/profile_report.py work/pc-recomp/bin/me_pc.exe \
      work/pc-recomp/bin/pc-profile.bin --nm work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin/llvm-nm.exe

pc-profile.bin holds (thread index, offset in me_pc.exe) pairs; pc-profile-threads.txt next to it
names the threads. Lifted functions are f_XXXXXXXX (the guest address) and are also summed by
guest module (game, msvcr80, PhysX, ...).
"""
import argparse
import bisect
import collections
import os
import struct
import subprocess

IMAGE_BASE = 0x140000000

# guest address ranges of the lifted modules (build_pc_runtime.py MODULES)
GUEST_MODULES = [
    (0x00400000, 0x02200000, "game"), (0x20000000, 0x26000000, "physx"), (0x26000000, 0x2B000000, "ogg/vorbis"),
    (0x2B000000, 0x2C000000, "bink"), (0x78100000, 0x78200000, "msvcr80"), (0x7C400000, 0x7C500000, "msvcp80"),
]


def guest_module(name):
    if not name.startswith("f_"):
        return "runtime" if name != "(outside me_pc.exe)" else name
    try:
        a = int(name[2:], 16)
    except ValueError:
        return "runtime"
    for lo, hi, m in GUEST_MODULES:
        if lo <= a < hi:
            return m
    return "lifted (other)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("profile")
    ap.add_argument("--nm", required=True)
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()
    out = subprocess.run([os.path.abspath(args.nm), "--numeric-sort", "--defined-only", args.exe],
                         capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "Tt":
            addrs.append(int(parts[0], 16) - IMAGE_BASE)
            names.append(parts[2])
    threads = {}
    tfile = os.path.join(os.path.dirname(args.profile), "pc-profile-threads.txt")
    if os.path.exists(tfile):
        for line in open(tfile):
            idx, tid, name = (line.split(None, 2) + ["-"])[:3]
            threads[int(idx)] = f"{name.strip()} ({tid})"
    data = open(args.profile, "rb").read()
    pairs = struct.unpack(f"<{len(data) // 4}I", data)
    per_thread = collections.defaultdict(collections.Counter)
    for k in range(0, len(pairs) - 1, 2):
        t, off = pairs[k], pairs[k + 1]
        if off == 0xFFFFFFFF:
            fn = "(outside me_pc.exe)"
        else:
            i = bisect.bisect_right(addrs, off) - 1
            fn = names[i] if i >= 0 else "?"
        per_thread[t][fn] += 1
    for t, counts in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        total = sum(counts.values())
        busy = total - counts["(outside me_pc.exe)"]
        print(f"== thread {threads.get(t, t)}: {total} samples, {100 * busy / total:.1f}% in me_pc.exe")
        by_mod = collections.Counter()
        for fn, c in counts.items():
            by_mod[guest_module(fn)] += c
        print("   " + ", ".join(f"{m} {100 * c / total:.1f}%" for m, c in by_mod.most_common()))
        for fn, c in counts.most_common(args.top):
            if fn != "(outside me_pc.exe)":
                print(f"   {100 * c / total:5.1f}%  {fn}")


if __name__ == "__main__":
    main()
