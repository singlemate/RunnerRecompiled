"""Generates the complete lifted C of one module (exe or DLL) in shards, plus its dispatch table.

From the project root:
  python -I outputs/mirrors-edge-native/pc/tools/gen_module.py work/pc/Binaries/MirrorsEdge.exe \
      --llvm work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin \
      --catalog work/pc-recomp/catalog.json --name game --out work/pc-recomp/gen/game

Output (derived from the game or the user's own DLLs, stays in work/):
  shard_NNN.c       lifted functions, each shard declares only the functions it calls
  dispatch_<name>.c sorted table {guest address, function} of every lifted entry
  gen-report.json   counts, functions with traps (unsupported instructions)
"""
import argparse
import collections
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift  # noqa: E402
import xflags  # noqa: E402

HEADER = '#include "x86rt.h"\n'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--llvm", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--name", required=True, help="module tag for the dispatch table symbol")
    ap.add_argument("--out", required=True)
    ap.add_argument("--shard-bytes", type=int, default=3_000_000, help="approximate C size of one shard")
    args = ap.parse_args()
    t0 = time.time()

    L = lift.Lifter(args.exe, args.llvm, args.catalog)
    os.makedirs(args.out, exist_ok=True)
    for f in os.listdir(args.out):
        if f.startswith("shard_") and f.endswith(".c"):
            os.remove(os.path.join(args.out, f))

    entries = sorted(L.entries)
    shard, shard_calls, shard_defs, size, n_shard = [], set(), set(), 0, 0
    traps = collections.Counter()
    trap_fns = 0
    failed = []
    lifted = []

    def flush():
        nonlocal shard, shard_calls, shard_defs, size, n_shard
        if not shard:
            return
        path = os.path.join(args.out, f"shard_{n_shard:03d}.c")
        with open(path, "w") as fp:
            fp.write(HEADER)
            for t in sorted(shard_calls - shard_defs):
                fp.write(f"void f_{t:08X}(X86Ctx *ctx);\n")
            for t in sorted(shard_defs):
                fp.write(f"void f_{t:08X}(X86Ctx *ctx);\n")
            fp.write("\n")
            fp.write("".join(shard))
        n_shard += 1
        shard, shard_calls, shard_defs, size = [], set(), set(), 0

    # First all the functions: which of them pass EFLAGS across calls is a whole-module question.
    results = []
    for e in entries:
        try:
            src, fl = L.lift(e)
        except lift.Unsupported as ex:
            # Whole-function failure (undecodable entry): a stub that traps keeps the table complete.
            failed.append((f"{e:#x}", str(ex)))
            src = (f"void f_{e:08X}(X86Ctx *ctx)\n{{\n    x86_trap(ctx, 0x{e:X}u, \"not lifted: "
                   f"{str(ex)[:40]}\");\n}}\n")
            fl = None
        results.append((e, src, fl))
    evs = {e: xflags.events(src.split("\n", 1)[1]) for e, src, fl in results}
    live_in, exports = xflags.solve(evs)
    print(json.dumps({"flags_live_in": len(live_in), "flags_exports": len(exports)}))
    for e, src, fl in results:
        if e in live_in or e in exports or any(ev[0] in "CT" and ev[3] in live_in | exports for ev in evs[e]):
            src = xflags.apply(e, src, evs[e], live_in, exports)
        if fl is not None and fl.unsupported:
            trap_fns += 1
            traps.update(fl.unsupported)
        shard.append(src + "\n")
        shard_defs.add(e)
        if fl is not None:
            shard_calls |= fl.calls
        lifted.append(e)
        size += len(src)
        if size >= args.shard_bytes:
            flush()
    flush()

    with open(os.path.join(args.out, f"dispatch_{args.name}.c"), "w") as fp:
        fp.write(HEADER + '#include "x86rt_module.h"\n\n')
        for e in lifted:
            fp.write(f"void f_{e:08X}(X86Ctx *ctx);\n")
        fp.write(f"\nconst X86FnEntry x86_fns_{args.name}[] = {{\n")
        for e in lifted:
            fp.write(f"    {{0x{e:X}u, f_{e:08X}}},\n")
        fp.write("};\n")
        fp.write(f"const uint32_t x86_fns_{args.name}_count = {len(lifted)};\n")

    rep = {"module": args.exe, "functions": len(lifted), "shards": n_shard, "functions_with_traps": trap_fns,
           "not_lifted": failed[:50], "not_lifted_count": len(failed), "traps": dict(traps.most_common(40)),
           "seconds": round(time.time() - t0, 1)}
    json.dump(rep, open(os.path.join(args.out, "gen-report.json"), "w"), indent=1)
    print(json.dumps({k: rep[k] for k in ("functions", "shards", "functions_with_traps", "not_lifted_count",
                                          "seconds")}))


if __name__ == "__main__":
    main()
