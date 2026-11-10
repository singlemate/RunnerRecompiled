"""EFLAGS across calls. A lifted function keeps the flags in a local (cheap, and the compiler drops
most of them), which is right for compiled C++: the ABI does not carry flags over call and ret.
Hand-written assembly does: msvcr80's x87 math calls a helper that classifies the argument and
branches on the ZF the helper returned (`call helper; push edx; fnstcw [esp]; je special`), and one
function can start by reading the flags of the helper its caller ran.

Over the generated C of a whole module this finds, by a linear scan of each function:
  live_in  functions that read the flags before setting them,
  exports  functions whose caller reads the flags right after the call,
and rewrites only those places to go through ctx->flags. Flags read after an indirect call are
only counted (no case is known that needs them). The scan does not follow jumps: a read reached
from a call only by a taken jump is not seen.
"""
import re

TOK = re.compile(
    r"(?P<keep>int c = x86_cf\(&F\)|int cf = x86_cf\(&F\)|int z = x86_zf\(&F\))"
    r"|(?P<set>\bF\.op\s*=|x86_expl\(&F)"
    r"|(?P<read>x86_\w+\(&F)"
    r"|\bf_(?P<call>[0-9A-F]{8})\(ctx\);(?P<tail> return;)?"
    r"|(?P<disp>x86_dispatch\(ctx[^;]*;)(?P<dtail> return;)?"
    r"|(?P<ret>\breturn;)"
    r"|^(?P<goto>\s*goto L_[0-9A-F]{8};)", re.M)


def events(body):
    out, last = [], None
    for m in TOK.finditer(body):
        if m.group("keep") or m.group("set"):
            ev = ("S",)
            if last == "S":
                continue  # runs of writes: one is enough
        elif m.group("read"):
            ev = ("R",)
        elif m.group("call"):
            t = int(m.group("call"), 16)
            ev = ("T" if m.group("tail") else "C", m.start(), m.start("call") + 8 + 6, t)
        elif m.group("disp"):
            ev = ("D",) if not m.group("dtail") else ("DT",)
        elif m.group("ret"):
            ev = ("E", m.start(), m.end())
        else:
            ev = ("G",)
        out.append(ev)
        last = ev[0]
    return out


def walk(f, evs, live_in, exports, edits=None):
    """One pass over a function. Grows live_in/exports; with edits (a dict) collects the rewrites."""
    state, changed, indirect = "E", False, 0
    for ev in evs:
        k = ev[0]
        if k == "S":
            state = None
        elif k in ("R", "C", "T"):
            reads = k == "R" or ev[3] in live_in
            if reads:
                if state == "E":
                    if f not in live_in:
                        live_in.add(f)
                        changed = True
                elif state == "D":
                    indirect += 1
                elif state is not None:
                    if state[3] not in exports:
                        exports.add(state[3])
                        changed = True
                    if edits is not None:
                        edits.setdefault("reload", set()).add(state[2])
                state = None
            if k != "R" and ev[3] in live_in and edits is not None:
                edits.setdefault("store", set()).add(ev[1])
            if k == "C":
                state = ev
            elif k == "T" and f in exports and ev[3] not in exports:
                exports.add(ev[3])
                changed = True
        elif k == "D":
            state = "D"
        elif k == "E":
            if f in exports:
                if state is None:
                    if edits is not None:
                        edits.setdefault("ret", []).append((ev[1], ev[2]))
                elif state not in ("E", "D") and state[3] not in exports:
                    exports.add(state[3])
                    changed = True
            state = None
        elif k == "G":
            state = None
    return changed, indirect


def solve(all_events):
    """all_events: {entry: events}. Returns (live_in, exports)."""
    live_in, exports = set(), set()
    changed = True
    while changed:
        changed = False
        for f, evs in all_events.items():
            changed |= walk(f, evs, live_in, exports)[0]
    return live_in, exports


def apply(f, src, evs, live_in, exports):
    """The function's source with the rewrites; positions are those of the body after line 1."""
    head, body = src.split("\n", 1)
    edits = {}
    walk(f, evs, live_in, exports, edits)
    ins = []
    for pos in edits.get("reload", ()):
        ins.append((pos, pos, " F = ctx->flags;"))
    for pos in edits.get("store", ()):
        ins.append((pos, pos, "ctx->flags = F; "))
    for a, b in edits.get("ret", ()):
        ins.append((a, b, "{ ctx->flags = F; return; }"))
    for a, b, text in sorted(ins, reverse=True):
        body = body[:a] + text + body[b:]
    if f in live_in:
        body = body.replace("X86Flags F = {0};", "X86Flags F = ctx->flags;", 1)
    return head + "\n" + body
