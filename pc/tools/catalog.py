"""Stage 1 of the PC lifter: function catalog of a PE32 executable.

Usage (from the project root):
  python -I outputs/mirrors-edge-native/pc/tools/catalog.py work/pc/Binaries/MirrorsEdge.exe \
      --llvm work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin --out work/pc-recomp

Writes catalog.json (functions and blocks), summary.json (counts, coverage, problems) and
mnemonics.json (instruction histogram: the lifter's work list). All outputs are derived from
the game binary and stay in work/.
"""
import argparse
import collections
import hashlib
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe32 import PE32  # noqa: E402
import x86cfg as X  # noqa: E402

# RaiseException is not here: a handler may return EXCEPTION_CONTINUE_EXECUTION.
NORETURN_IMPORTS = {
    "ExitProcess", "ExitThread", "FatalAppExitA", "FatalAppExitW", "TerminateProcess",
    "_CxxThrowException", "_exit", "exit", "abort", "_invoke_watson", "_invalid_parameter_noinfo_noreturn",
    "?terminate@@YAXXZ", "_amsg_exit", "longjmp", "_longjmp",
    "?_Xlen@std@@YAXXZ", "?_Xran@std@@YAXXZ", "?_Xmem@tr1@std@@YAXXZ",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--llvm", required=True, help="directory with libLLVM-*.dll")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    t0 = time.time()

    data = open(args.exe, "rb").read()
    pe = PE32(data)
    base = pe.image_base
    tva, code = pe.code_blob()
    tend = tva + len(code)
    in_text = lambda va: tva <= va < tend  # noqa: E731

    imports = pe.imports()
    relocs = pe.relocations()
    reloc_set = set(relocs)
    noreturn_slots = {slot for slot, (_, sym) in imports.items() if sym in NORETURN_IMPORTS}

    dis = X.LLVMDisasm(args.llvm)
    dis.bind(code, tva)

    insn_len = bytearray(len(code))   # length at instruction starts, 0 elsewhere
    owner_mark = bytearray(len(code))  # 1 = covered by decoded code, 2 = jump-table data
    mnem = collections.Counter()
    functions = {}                      # entry -> dict
    origin = {}
    problems = collections.Counter()
    problem_sites = collections.defaultdict(list)
    table_sites = set()

    def note(kind, va):
        problems[kind] += 1
        if len(problem_sites[kind]) < 50:
            problem_sites[kind].append(f"{va:#x}")

    def add_seed(va, why):
        # A strong seed (a call, the entry, an export, a SafeSEH handler) also revives an address
        # that an earlier weak seed (named only by a relocation) failed to decode.
        if in_text(va) and (va not in origin or (why in STRONG and origin[va] not in STRONG and va not in functions)):
            origin[va] = why
            rejected_va.discard(va)
            pending.append(va)

    STRONG = ("call", "entry", "safeseh", "export")
    rejected_va = set()
    reject_log = []
    last_reason = [None, 0]

    def fail(reason, va):
        last_reason[0], last_reason[1] = reason, va
        return None

    STRONG = ("call", "entry", "safeseh", "export")

    table_cache = {}

    def jump_table(table_va):
        # Tables may be shared by several jumps or start inside another table: always re-read.
        if table_va in table_cache:
            return table_cache[table_va]
        targets = []
        p = table_va
        # Leading entries for impossible indices may be left unrelocated ("dd ?" in asm).
        for _ in range(3):
            if p not in reloc_set and p + 4 in reloc_set:
                p += 4
        while p in reloc_set and len(targets) < 4096:
            t = pe.u32(p)
            if not in_text(t):
                break
            table_sites.add(p)
            if in_text(p):
                for i in range(4):
                    owner_mark[p - tva + i] = 2
            targets.append(t)
            p += 4
        # Negative indices (asm memcpy: "sub ecx, 4 / jb" then jmp [ecx*4 + table]).
        p = table_va - 4
        while p in reloc_set and len(targets) < 4096 and table_va - p <= 64:
            t = pe.u32(p)
            if not in_text(t):
                break
            table_sites.add(p)
            targets.append(t)
            p -= 4
        table_cache[table_va] = targets
        return targets

    def descend(entry, weak):
        """Recursive descent from entry. A weak seed (named only by data) is rejected when its
        code does not decode cleanly; nothing it found is kept then."""
        marks = []
        calls = []
        tables = []
        blocks = []
        stack = [entry]
        seen = set()
        fn = {"entry": entry, "blocks": blocks, "tail": [], "flags": set()}
        while stack:
            b = stack.pop()
            if b in seen:
                continue
            if b != entry and b in functions:
                fn["tail"].append(b)
                continue
            seen.add(b)
            blocks.append(b)
            va = b
            while True:
                if not in_text(va):
                    if weak:
                        return fail("runs out of .text", va)
                    fn["flags"].add("runs-out-of-text")
                    note("runs-out-of-text", va)
                    break
                off = va - tva
                if owner_mark[off] == 2 or (weak and va in reloc_set):
                    if weak:
                        return fail("instruction on data or a relocation", va)
                n, txt = dis.decode(va)
                if not n:
                    if weak:
                        return fail("decode error", va)
                    fn["flags"].add("decode-error")
                    note("decode-error", va)
                    break
                if not insn_len[off]:
                    marks.append((off, n, txt))
                kind, tgt = X.branch(code, off, va, n)
                nxt = va + n
                if kind == X.K_CALL:
                    if in_text(tgt):
                        calls.append(tgt)
                    elif weak:
                        return fail("call outside .text", va)
                    else:
                        note("call-outside-text", va)
                elif kind == X.K_CALL_IAT:
                    if tgt in noreturn_slots:
                        break
                elif kind == X.K_JCC:
                    stack.append(tgt)
                    stack.append(nxt)
                    break
                elif kind == X.K_JMP:
                    if in_text(tgt):
                        stack.append(tgt)
                    elif weak:
                        return fail("jmp outside .text", va)
                    else:
                        note("jmp-outside-text", va)
                    break
                elif kind == X.K_JMP_TABLE:
                    ts = jump_table(tgt)
                    tables.append(tgt)
                    if not ts:
                        fn["flags"].add("unresolved-jump-table")
                        note("unresolved-jump-table", va)
                    stack.extend(ts)
                    break
                elif kind == X.K_JMP_IND:
                    fn["flags"].add("indirect-jmp")
                    note("indirect-jmp", va)
                    break
                elif kind == X.K_JMP_IAT:
                    fn["flags"].add("import-thunk" if tgt in imports else "jmp-mem")
                    break
                elif kind in (X.K_RET, X.K_STOP):
                    break
                # Fall through into a known function entry: the block ends here.
                if nxt != entry and nxt in functions:
                    fn["tail"].append(nxt)
                    break
                va = nxt
        blocks.sort()
        for off, n, txt in marks:
            if not insn_len[off]:
                insn_len[off] = n
                for i in range(n):
                    owner_mark[off + i] = 1
                mnem[txt.split(None, 1)[0] if not txt.startswith(("lock", "rep")) else " ".join(txt.split()[:2])] += 1
        for t in calls:
            add_seed(t, "call")
        return fn

    # Strong seeds first: entry point, SafeSEH handlers; calls are added while decoding.
    pending = []
    rejected = collections.Counter()
    add_seed(base + pe.entry_rva, "entry")
    for h in pe.safeseh_handlers():
        add_seed(h, "safeseh")
    for name, va in sorted(pe.exports().items()):
        add_seed(va, "export")

    def run_pending():
        while pending:
            e = pending.pop()
            if e in functions:
                continue
            weak = origin.get(e) not in STRONG
            # A weak seed inside decoded code is dropped only when it points into the middle of an
            # instruction or at data. A relocated pointer to an instruction start is a real
            # function pointer (a vtable entry), even when another function reached that code
            # through a jmp before it was known as a function.
            if weak and owner_mark[e - tva] and not insn_len[e - tva]:
                continue
            fn = descend(e, weak)
            if fn is None:
                rejected[origin[e]] += 1
                rejected_va.add(e)
                if len(reject_log) < 400:
                    reject_log.append(f"{e:#x} {origin[e]}: {last_reason[0]} at {last_reason[1]:#x}")
                continue
            functions[e] = fn

    run_pending()
    strong = len(functions)

    # Weak seeds: addresses in .text named by relocations (vtables, callbacks, SEH scope tables).
    for site in relocs:
        if site in table_sites:
            continue
        v = pe.u32(site)
        if in_text(v) and v not in functions:
            add_seed(v, "reloc-code" if in_text(site) else "reloc-data")
    run_pending()

    # Relocated dwords inside .text that are not instructions are data (tables, literals).
    for site in relocs:
        if in_text(site) and in_text(site + 3) and not any(owner_mark[site - tva + i] for i in range(4)):
            for i in range(4):
                owner_mark[site - tva + i] = 2

    # Everything found so far is reachable: the image has a complete relocation table, so every
    # absolute code address appears as a relocation, and relative branches were followed.
    reachable_mark = bytes(owner_mark)
    reachable_fns = set(functions)
    reachable_insns = sum(1 for x in insn_len if x)
    mnem_reachable = collections.Counter(mnem)

    # Gap scan, for accounting only: code after int3/nop padding (16-byte aligned starts) that no
    # relocation and no reachable branch names. It cannot run, so it is dead code the linker kept.
    gap_rounds = 0
    while True:
        gap_rounds += 1
        before = len(functions)
        off = 0
        L = len(code)
        while off < L:
            if owner_mark[off]:
                off += 1
                continue
            pad_start = off
            while off < L and not owner_mark[off] and code[off] in (0xCC, 0x90):
                off += 1
            if off < L and not owner_mark[off] and off > pad_start and off % 16 == (16 - tva % 16) % 16:
                add_seed(tva + off, "gap")
            while off < L and not owner_mark[off]:
                off += 1
        run_pending()
        if len(functions) == before or gap_rounds >= 20:
            break

    # Coverage: bytes of .text that are neither decoded code, jump tables nor int3/nop padding.
    gaps = []
    off = 0
    L = len(code)
    covered = sum(1 for b in owner_mark if b == 1)
    dead = sum(1 for a, b in zip(reachable_mark, owner_mark) if b == 1 and a != 1)
    tables = sum(1 for b in owner_mark if b == 2)
    padding = 0
    while off < L:
        if owner_mark[off]:
            off += 1
            continue
        start = off
        while off < L and not owner_mark[off]:
            off += 1
        chunk = code[start:off]
        pad = chunk.count(0xCC) + chunk.count(0x90) + chunk.count(0x00)
        padding += pad
        if len(chunk) - pad > 0:
            gaps.append((len(chunk) - pad, tva + start, off - start))
    gaps.sort(reverse=True)
    unexplained = sum(g[0] for g in gaps)

    entries = sorted(functions)
    kinds = collections.Counter()
    for e in entries:
        f = functions[e]
        if e not in reachable_fns:
            kinds["dead"] += 1
        elif origin[e] == "safeseh":
            kinds["seh-handler-stub"] += 1
        elif len(f["blocks"]) == 1 and "import-thunk" in f["flags"] and code[e - tva] == 0xFF:
            kinds["import-thunk"] += 1
        else:
            kinds["code"] += 1
    by_origin = collections.Counter(origin[e] for e in entries)
    for e in entries:
        f = functions[e]
        f["flags"] = sorted(f["flags"])
    flag_counts = collections.Counter(fl for f in functions.values() for fl in f["flags"])

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "catalog.json"), "w") as fp:
        json.dump({"exe_sha256": hashlib.sha256(data).hexdigest(),
                   "functions": [{"entry": f"{e:#x}", "origin": origin[e],
                                  "blocks": [f"{b:#x}" for b in functions[e]["blocks"]],
                                  "tail": [f"{t:#x}" for t in functions[e]["tail"]],
                                  "flags": functions[e]["flags"],
                                  "reachable": e in reachable_fns} for e in entries]}, fp)
    summary = {
        "exe_sha256": hashlib.sha256(data).hexdigest(),
        "text": {"va": f"{tva:#x}", "size": len(code)},
        "functions": len(functions), "functions_from_strong_seeds": strong,
        "by_origin": dict(by_origin), "kinds": dict(kinds), "rejected_weak_seeds": dict(rejected),
        "gap_rounds": gap_rounds, "flags": dict(flag_counts),
        "functions_reachable": len(reachable_fns),
        "instructions": int(sum(1 for x in insn_len if x)), "instructions_reachable": reachable_insns,
        "bytes": {"code": covered, "code_dead": dead, "jump_tables": tables, "padding": padding,
                  "unexplained": unexplained},
        "coverage_of_non_padding": round(100.0 * (covered + tables) / max(1, covered + tables + unexplained), 3),
        "relocations": len(relocs), "jump_table_entries": len(table_sites),
        "imports": len(imports),
        "problems": dict(problems), "problem_sites": {k: v for k, v in problem_sites.items()},
        "rejected_seeds": reject_log,
        "largest_gaps": [{"va": f"{va:#x}", "bytes": n, "non_padding": u} for u, va, n in gaps[:40]],
        "seconds": round(time.time() - t0, 1),
    }
    with open(os.path.join(args.out, "summary.json"), "w") as fp:
        json.dump(summary, fp, indent=1)
    with open(os.path.join(args.out, "mnemonics.json"), "w") as fp:
        json.dump(dict(mnem_reachable.most_common()), fp, indent=0)
    print(json.dumps({k: summary[k] for k in ("functions", "functions_reachable", "functions_from_strong_seeds", "by_origin", "kinds",
                                              "rejected_weak_seeds", "gap_rounds", "flags",
                                              "instructions", "instructions_reachable", "bytes", "coverage_of_non_padding", "problems",
                                              "jump_table_entries", "seconds")}, indent=1))


if __name__ == "__main__":
    main()
