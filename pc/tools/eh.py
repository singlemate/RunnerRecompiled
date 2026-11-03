"""Exception-handling continuations of MSVC x86 code.

When a catch block or an __except block runs, the EH runtime of msvcr80 does not return to the
function that registered the handler: it restores ebp/esp and jumps to an address inside it
(C++: the address the catch funclet returns; SEH4: the __except block itself). In lifted code
such an address is reached by an indirect jump to a non-entry, so the lifter makes each of them
a resume point of its function (see FnLifter and x86_resume_* in x86rt.h).

Found statically from the tables the compiler emits:
- C++ EH: per-function handler stubs `mov eax, FuncInfo / jmp __CxxFrameHandler3`; the function
  pushes the stub in its prologue (`push offset stub`). FuncInfo -> try blocks -> catch handlers
  (funclets); a funclet ends with `mov eax, continuation / ret`.
- SEH4: the prologue pushes the scope table right before the handler
  (`push offset scopetable / push offset __except_handler4`); records with a filter have the
  __except block address as HandlerFunc (a null filter is a __finally, called as a function).
"""
import bisect
import collections
import struct

import x86cfg as X

EH_MAGIC = {0x19930520, 0x19930521, 0x19930522}


def continuations(L):
    """Returns ({parent function entry: sorted targets}, stats) for a lift.Lifter."""
    pe, code, tva = L.pe, L.code, L.tva
    relocs = L._relocs
    stats = collections.Counter()

    def in_text(a):
        return tva <= a < tva + len(code)

    def u32(a):
        b = pe.read(a, 4)
        return struct.unpack("<I", b)[0] if len(b) == 4 else None

    entries = sorted(L.entries)

    def parent_of(site):
        i = bisect.bisect_right(entries, site) - 1
        return entries[i] if i >= 0 else None

    # Handler stubs: mov eax, FuncInfo (relocated) followed by a jmp.
    stubs = {}
    for s in relocs:
        if in_text(s) and in_text(s - 1) and code[s - 1 - tva] == 0xB8 and in_text(s + 4):
            fi = u32(s)
            if fi is not None and u32(fi) in EH_MAGIC and code[s + 4 - tva] in (0xE9, 0xFF, 0xEB):
                stubs[s - 1] = fi
    stats["cxx_stubs"] = len(stubs)
    safeseh = set(pe.safeseh_handlers())
    mov_sites = sorted(stubs)

    def stub_funcinfo(v):
        """FuncInfo of the handler at v: stubs with /GS first check the frame's cookie, so the
        `mov eax, FuncInfo` may come a few instructions after the handler's address."""
        i = bisect.bisect_left(mov_sites, v)
        if i < len(mov_sites) and mov_sites[i] - v < 64:
            return stubs[mov_sites[i]]
        return None

    def noreturn_thunk(t):
        """A `jmp [import slot]` thunk of a noreturn import (e.g. _CxxThrowException)."""
        if not in_text(t):
            return False
        n, _ = L.dis.decode(t)
        if not n:
            return False
        kind, tgt = X.branch(code, t - tva, t, n)
        return kind == X.K_JMP_IAT and tgt in L.noreturn_slots

    def funclet_returns(f):
        """Continuations returned by a catch funclet: `mov eax, imm32` right before a `ret`."""
        out, seen, work = set(), set(), [f]
        budget = 600
        while work and budget > 0:
            va = work.pop()
            prev = None
            while in_text(va) and va not in seen and budget > 0:
                budget -= 1
                seen.add(va)
                n, _ = L.dis.decode(va)
                if not n:
                    break
                kind, tgt = X.branch(code, va - tva, va, n)
                if kind == X.K_RET:
                    if prev is not None and code[prev - tva] == 0xB8:
                        out.add(struct.unpack_from("<I", code, prev - tva + 1)[0])
                    break
                if kind == X.K_JMP:
                    work.append(tgt)
                    break
                if kind == X.K_JCC:
                    work.append(tgt)
                elif kind in (X.K_JMP_IND, X.K_JMP_TABLE, X.K_JMP_IAT, X.K_STOP):
                    break
                elif kind == X.K_CALL_IAT and tgt in L.noreturn_slots:
                    break  # rethrow
                elif kind == X.K_CALL and noreturn_thunk(tgt):
                    break  # rethrow through the module's own _CxxThrowException thunk
                prev = va
                va += n
        return {t for t in out if in_text(t)}

    def pushes_seh4_handler(f):
        """__SEH_prolog4: pushes __except_handler4 within its first instructions."""
        va = f
        for _ in range(8):
            if not in_text(va):
                return False
            n, _ = L.dis.decode(va)
            if not n:
                return False
            if code[va - tva] == 0x68 and (va + 1) in relocs and u32(va + 1) in safeseh \
                    and stub_funcinfo(u32(va + 1)) is None:
                return True
            va += n
        return False

    prolog_cache = {}

    def seh4_frame(s):
        """`push scopetable` at s - 1, then either `push offset __except_handler4` or
        `call __SEH_prolog4` (which pushes the handler; the frame size was pushed before)."""
        if not in_text(s + 10):
            return False
        nxt = s + 4
        if code[nxt - tva] == 0x68 and (nxt + 1) in relocs:
            h = u32(nxt + 1)
            return h in safeseh and stub_funcinfo(h) is None
        if code[nxt - tva] == 0xE8:  # push size / push scopetable / call __SEH_prolog4
            call = nxt
        else:
            return False
        if code[call - tva] != 0xE8:
            return False
        tgt = call + 5 + struct.unpack_from("<i", code, call + 1 - tva)[0]
        if tgt not in prolog_cache:
            prolog_cache[tgt] = pushes_seh4_handler(tgt)
        return prolog_cache[tgt]

    res = collections.defaultdict(set)
    for s in sorted(relocs):
        if not (in_text(s) and in_text(s - 1)) or code[s - 1 - tva] != 0x68:
            continue
        v = u32(s)
        parent = parent_of(s)
        if parent is None:
            continue
        fi = stub_funcinfo(v) if v in safeseh else None
        if fi is not None:
            ntry, ptry = u32(fi + 12) or 0, u32(fi + 16) or 0
            for i in range(min(ntry, 512)):
                tb = ptry + 20 * i
                ncatch, phandlers = u32(tb + 12) or 0, u32(tb + 16) or 0
                for j in range(min(ncatch, 64)):
                    funclet = u32(phandlers + 16 * j + 12)
                    if funclet is None or not in_text(funclet):
                        stats["cxx_bad_funclet"] += 1
                        continue
                    ts = funclet_returns(funclet)
                    if not ts:
                        stats["cxx_funclet_without_continuation"] += 1
                    res[parent] |= ts
                    stats["cxx_catches"] += 1
        elif seh4_frame(s):
            # SEH4 scope table: 16 bytes of cookie offsets, then {EnclosingLevel, Filter, Handler}.
            for i in range(64):
                rec = v + 16 + 12 * i
                if (rec + 8) not in relocs:
                    break
                filt, handler = u32(rec + 4), u32(rec + 8)
                if filt and (rec + 4) in relocs and in_text(handler):
                    res[parent].add(handler)
                    stats["seh_except_blocks"] += 1
    out = {}
    for p, ts in res.items():
        ts = sorted(t for t in ts if t > p)
        if ts:
            out[p] = ts
    stats["functions_with_resume_points"] = len(out)
    stats["resume_points"] = sum(len(t) for t in out.values())
    return out, stats
