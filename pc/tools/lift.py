"""Stage 2 of the PC lifter: x86-32 functions -> C (contract: pc/runtime/x86rt.h).

Each guest function becomes `void f_XXXXXXXX(X86Ctx *ctx)`. Registers live in C locals, spilled
to ctx around calls; flags are lazy (X86Flags local). Instruction length, mnemonic and operands
come from LLVM MC (Intel syntax); branch targets come from the raw bytes (x86cfg.branch).

This module is imported by the harness generator and can also report coverage:
  python -I outputs/mirrors-edge-native/pc/tools/lift.py work/pc/Binaries/MirrorsEdge.exe \
      --llvm work/toolchains/llvm-mingw/llvm-mingw-20261006-ucrt-x86_64/bin \
      --catalog work/pc-recomp/catalog.json --report work/pc-recomp/lift-report.json
"""
import argparse
import collections
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe32 import PE32  # noqa: E402
import x86cfg as X  # noqa: E402

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
REG16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp", "si": "esi", "di": "edi"}
REG8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
REG8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
PTR_BITS = {"byte": 8, "word": 16, "dword": 32}
CC = {"o": "CC_O", "no": "CC_NO", "b": "CC_B", "c": "CC_B", "nae": "CC_B", "ae": "CC_AE", "nb": "CC_AE",
      "nc": "CC_AE", "e": "CC_E", "z": "CC_E", "ne": "CC_NE", "nz": "CC_NE", "be": "CC_BE", "na": "CC_BE",
      "a": "CC_A", "nbe": "CC_A", "s": "CC_S", "ns": "CC_NS", "p": "CC_P", "pe": "CC_P", "np": "CC_NP",
      "po": "CC_NP", "l": "CC_L", "nge": "CC_L", "ge": "CC_GE", "nl": "CC_GE", "le": "CC_LE", "ng": "CC_LE",
      "g": "CC_G", "nle": "CC_G"}
UTYPE = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t"}
STYPE = {8: "int8_t", 16: "int16_t", 32: "int32_t"}
MASK = {8: "0xFFu", 16: "0xFFFFu", 32: "0xFFFFFFFFu"}

MEM_RE = re.compile(r"^(?:(byte|word|dword|qword|xmmword|tbyte|fword|ptr)\s+ptr\s+)?(?:(\w\w):)?\[(.*)\]$")
NUM_RE = re.compile(r"^-?(0x[0-9a-f]+|\d+)$")


class Unsupported(Exception):
    pass


def num(s):
    s = s.strip()
    neg = s.startswith("-")
    v = int(s.lstrip("-"), 0)
    return -v if neg else v


class Op:
    """Parsed operand: kind 'reg' | 'mem' | 'imm'."""

    def __init__(self, text):
        t = text.strip()
        self.text = t
        m = MEM_RE.match(t)
        if m:
            self.kind = "mem"
            self.bits = PTR_BITS.get(m.group(1)) if m.group(1) else None
            if m.group(1) and m.group(1) not in PTR_BITS:
                raise Unsupported(f"memory size {m.group(1)}")
            self.seg = m.group(2)
            self.addr = self._addr(m.group(3))
        elif t in REG32:
            self.kind, self.bits, self.reg = "reg", 32, t
        elif t in REG16:
            self.kind, self.bits, self.reg = "reg", 16, t
        elif t in REG8L or t in REG8H:
            self.kind, self.bits, self.reg = "reg", 8, t
        elif NUM_RE.match(t):
            self.kind, self.bits, self.imm = "imm", None, num(t)
        else:
            raise Unsupported(f"operand {t}")

    @staticmethod
    def _addr(expr):
        parts = []
        for sign, term in re.findall(r"([+-]?)\s*([^+-]+)", expr.replace(" ", "")):
            term = term.strip()
            if not term:
                continue
            if "*" in term:
                a, b = term.split("*")
                reg, scale = (b, a) if a.isdigit() else (a, b)
                if reg not in REG32:
                    raise Unsupported(f"index {term}")
                c = f"{reg}*{int(scale)}u"
            elif term in REG32:
                c = term
            else:
                c = f"0x{num(term) & 0xFFFFFFFF:X}u"
            parts.append(("-" if sign == "-" else "+", c))
        if not parts:
            return "0u"
        out = "" if parts[0][0] == "+" else "0u-"
        out += parts[0][1]
        for s, c in parts[1:]:
            out += f" {s} {c}"
        return f"(uint32_t)({out})"


class FnLifter:
    def __init__(self, lifter, entry, fn):
        self.L = lifter
        self.entry = entry
        self.fn = fn
        self.lines = []
        self.unsupported = collections.Counter()
        self.calls = set()

    # --- operand access ---
    def ea(self, op):
        a = op.addr
        if op.seg == "fs":
            a = f"(uint32_t)(ctx->fs_base + {a})"
        elif op.seg not in (None, "ds", "es", "ss", "cs"):
            raise Unsupported(f"segment {op.seg}")
        return a

    def rd(self, op, bits):
        if op.kind == "imm":
            return f"(({UTYPE[bits]})0x{op.imm & 0xFFFFFFFF:X}u)"
        if op.kind == "reg":
            r = op.reg
            if r in REG32:
                return r
            if r in REG16:
                return f"((uint16_t){REG16[r]})"
            if r in REG8L:
                return f"((uint8_t){REG8L[r]})"
            return f"((uint8_t)({REG8H[r]} >> 8))"
        return f"ld{bits}({self.ea(op)})"

    def wr(self, op, bits, val):
        if op.kind == "reg":
            r = op.reg
            if r in REG32:
                return f"{r} = (uint32_t)({val});"
            if r in REG16:
                return f"{REG16[r]} = ({REG16[r]} & 0xFFFF0000u) | (uint16_t)({val});"
            if r in REG8L:
                return f"{REG8L[r]} = ({REG8L[r]} & 0xFFFFFF00u) | (uint8_t)({val});"
            return f"{REG8H[r]} = ({REG8H[r]} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({val}) << 8);"
        if op.kind == "mem":
            return f"st{bits}({self.ea(op)}, ({UTYPE[bits]})({val}));"
        raise Unsupported("write to immediate")

    def size(self, *ops):
        for o in ops:
            if o.bits:
                return o.bits
        raise Unsupported("operand size unknown")

    # --- emit helpers ---
    def e(self, s):
        self.lines.append("    " + s)

    def spill(self):
        return " ".join(f"ctx->{r} = {r};" for r in REG32)

    def reload(self):
        return " ".join(f"{r} = ctx->{r};" for r in REG32)

    def flags(self, op, bits, a, b, r, cin=None):
        s = f"F.op = {op}; F.bits = {bits}; F.a = {a}; F.b = {b}; F.r = {r};"
        if cin is not None:
            s += f" F.cin = {cin};"
        return s

    def call_fn(self, target, ret_addr):
        """Direct call of a lifted function: push the return address, run it."""
        self.calls.add(target)
        self.e(f"esp -= 4; st32(esp, 0x{ret_addr:X}u); {self.spill()} f_{target:08X}(ctx); {self.reload()}")

    def goto_or_tail(self, target, cond=None):
        pre = f"if ({cond}) " if cond else ""
        if target in self.blockset:
            self.e(f"{pre}goto L_{target:08X};")
        elif target in self.L.entries:
            self.calls.add(target)
            self.e(f"{pre}{{ {self.spill()} f_{target:08X}(ctx); return; }}")
        else:
            self.e(f"{pre}{{ {self.spill()} x86_dispatch(ctx, 0x{target:X}u); return; }}")

    # --- main ---
    def lift(self):
        L = self.L
        insns = {}
        order = []
        # Own recursive descent from the entry. The catalog's block lists can contain code of a
        # function that was discovered later (a jmp into it was followed before it was known);
        # here every other function's entry is a boundary and a jump to it is a tail call.
        foreign = lambda a: a in L.entries and a != self.entry  # noqa: E731
        in_text = lambda a: L.tva <= a < L.tva + len(L.code)  # noqa: E731
        self.blockset = {self.entry}
        work = [self.entry]

        def add_block(a):
            if in_text(a) and not foreign(a) and a not in self.blockset:
                self.blockset.add(a)
                work.append(a)

        while work:
            va = work.pop()
            while va not in insns:
                n, txt = L.dis.decode(va)
                if not n:
                    raise Unsupported(f"decode {va:#x}")
                kind, tgt = X.branch(L.code, va - L.tva, va, n)
                insns[va] = (n, txt, kind, tgt)
                order.append(va)
                nxt = va + n
                if kind == X.K_JCC:
                    add_block(tgt)
                    add_block(nxt)
                    break
                if kind == X.K_JMP:
                    add_block(tgt)
                    break
                if kind == X.K_JMP_TABLE:
                    for t in L.table(tgt):
                        add_block(t)
                    break
                if kind in (X.K_JMP_IND, X.K_JMP_IAT, X.K_RET, X.K_STOP):
                    break
                if kind == X.K_CALL_IAT and tgt in L.noreturn_slots:
                    break
                if foreign(nxt):
                    break
                va = nxt
        order.sort()
        self.e("uint32_t " + ", ".join(f"{r} = ctx->{r}" for r in REG32) + ";")
        self.e("X86Flags F = {0};")
        self.e("(void)F;")
        if order[0] != self.entry:
            self.e(f"goto L_{self.entry:08X};")  # code below the entry (shared tails) comes first in C
        for i, va in enumerate(order):
            n, txt, kind, tgt = insns[va]
            if va in self.blockset:
                self.lines.append(f"L_{va:08X}:;")
            try:
                self.insn(va, n, txt, kind, tgt)
            except (IndexError, AttributeError, TypeError) as ex:
                self.unsupported[f"internal {type(ex).__name__}: {txt.split()[0]}"] += 1
                self.e(f"x86_trap(ctx, 0x{va:X}u, \"lifter error\");")
            except Unsupported as ex:
                self.unsupported[str(ex)] += 1
                self.e(f"x86_trap(ctx, 0x{va:X}u, \"{txt.replace(chr(9), ' ')}\");")
            # Falling off the end of decoded code: next address is another function or unknown.
            nxt = va + n
            terminal = kind in (X.K_JMP, X.K_JMP_IND, X.K_JMP_TABLE, X.K_JMP_IAT, X.K_RET, X.K_STOP) or (
                kind == X.K_CALL_IAT and tgt in L.noreturn_slots)
            if not terminal and (i + 1 == len(order) or order[i + 1] != nxt):
                self.goto_or_tail(nxt)
        body = "\n".join(self.lines)
        return f"void f_{self.entry:08X}(X86Ctx *ctx)\n{{\n{body}\n}}\n"

    def insn(self, va, n, txt, kind, tgt):
        toks = txt.split(None, 1)
        mn = toks[0]
        rest = toks[1] if len(toks) > 1 else ""
        prefix = None
        if mn in ("rep", "repe", "repne", "repz", "repnz", "lock"):
            prefix = mn
            toks = rest.split(None, 1)
            mn = toks[0]
            rest = toks[1] if len(toks) > 1 else ""
            if prefix == "lock":
                raise Unsupported("lock")
        nxt = va + n
        e = self.e

        # --- control flow (targets from bytes) ---
        if kind == X.K_CALL:
            if tgt in self.L.entries:
                self.call_fn(tgt, nxt)
            else:
                e(f"esp -= 4; st32(esp, 0x{nxt:X}u); {self.spill()} x86_dispatch(ctx, 0x{tgt:X}u); {self.reload()}")
            return
        if kind == X.K_CALL_IAT:
            if tgt in self.L.imports:
                e(f"esp -= 4; st32(esp, 0x{nxt:X}u); {self.spill()} x86_import(ctx, 0x{tgt:X}u); {self.reload()}")
                if tgt in self.L.noreturn_slots:
                    e(f"x86_trap(ctx, 0x{va:X}u, \"noreturn import returned\");")
            else:
                e(f"{{ uint32_t t = ld32(0x{tgt:X}u); esp -= 4; st32(esp, 0x{nxt:X}u); {self.spill()} "
                  f"x86_dispatch(ctx, t); {self.reload()} }}")
            return
        if kind == X.K_CALL_IND:
            op = Op(rest)
            e(f"{{ uint32_t t = {self.rd(op, 32)}; esp -= 4; st32(esp, 0x{nxt:X}u); {self.spill()} "
              f"x86_dispatch(ctx, t); {self.reload()} }}")
            return
        if kind == X.K_JMP:
            self.goto_or_tail(tgt)
            return
        if kind == X.K_JCC:
            cc = mn[1:]
            if mn in ("jecxz", "jcxz"):
                cond = "ecx == 0" if mn == "jecxz" else "(uint16_t)ecx == 0"
            elif mn.startswith("loop"):
                raise Unsupported(f"mnemonic {mn}")
            elif cc in CC:
                cond = f"x86_cc(&F, {CC[cc]})"
            else:
                raise Unsupported(f"mnemonic {mn}")
            self.goto_or_tail(tgt, cond)
            return
        if kind == X.K_JMP_TABLE:
            op = Op(rest)
            targets = self.L.table(tgt)
            cases = " ".join(f"case 0x{t:X}u: goto L_{t:08X};" for t in sorted(set(targets)) if t in self.blockset)
            e(f"switch ({self.rd(op, 32)}) {{ {cases} default: x86_trap(ctx, 0x{va:X}u, \"jump table\"); }}")
            return
        if kind == X.K_JMP_IND:
            op = Op(rest)
            e(f"{{ uint32_t t = {self.rd(op, 32)}; {self.spill()} x86_dispatch(ctx, t); return; }}")
            return
        if kind == X.K_JMP_IAT:
            if tgt in self.L.imports:
                e(f"{self.spill()} x86_import(ctx, 0x{tgt:X}u); return;")
            else:
                e(f"{{ uint32_t t = ld32(0x{tgt:X}u); {self.spill()} x86_dispatch(ctx, t); return; }}")
            return
        if kind == X.K_RET:
            imm = num(rest) if rest else 0
            e(f"esp += {4 + imm}; {self.spill()} return;")
            return
        if kind == X.K_STOP:
            e(f"x86_trap(ctx, 0x{va:X}u, \"{mn}\");")
            return

        ops = [Op(o) for o in split_ops(rest)] if rest else []
        h = getattr(self, "i_" + mn, None)
        if h is None:
            if mn.startswith("set") and mn[3:] in CC:
                e(self.wr(ops[0], 8, f"x86_cc(&F, {CC[mn[3:]]})"))
                return
            if mn.startswith("cmov") and mn[4:] in CC:
                bits = self.size(*ops)
                e(f"if (x86_cc(&F, {CC[mn[4:]]})) {self.wr(ops[0], bits, self.rd(ops[1], bits))}")
                return
            raise Unsupported(f"mnemonic {mn}")
        h(ops, prefix)

    # --- integer instructions ---
    def i_nop(self, ops, p):
        pass

    def i_wait(self, ops, p):
        pass

    def i_mov(self, ops, p):
        bits = self.size(*ops)
        self.e(self.wr(ops[0], bits, self.rd(ops[1], bits)))

    def i_movzx(self, ops, p):
        self.e(self.wr(ops[0], ops[0].bits, f"(uint32_t){self.rd(ops[1], ops[1].bits)}"))

    def i_movsx(self, ops, p):
        s = ops[1].bits
        self.e(self.wr(ops[0], ops[0].bits, f"(uint32_t)(int32_t)({STYPE[s]}){self.rd(ops[1], s)}"))

    def i_lea(self, ops, p):
        if ops[0].bits != 32:
            raise Unsupported("lea16")
        self.e(f"{ops[0].reg} = {ops[1].addr};")

    def i_push(self, ops, p):
        o = ops[0]
        if o.kind != "imm" and o.bits != 32:
            raise Unsupported("push16")
        self.e(f"{{ uint32_t v = {self.rd(o, 32)}; esp -= 4; st32(esp, v); }}")

    def i_pop(self, ops, p):
        o = ops[0]
        if o.bits != 32:
            raise Unsupported("pop16")
        if o.kind == "reg" and o.reg == "esp":
            self.e("esp = ld32(esp);")
            return
        self.e(f"{{ uint32_t v = ld32(esp); esp += 4; {self.wr(o, 32, 'v')} }}")

    def _alu(self, ops, op, fl, store=True, carry=None):
        bits = self.size(*ops)
        a = self.rd(ops[0], bits)
        b = self.rd(ops[1], bits)
        if ops[0].kind == "mem":
            # Compute the address once (read-modify-write).
            self.e("{ uint32_t ea = " + self.ea(ops[0]) + ";")
            a = f"ld{bits}(ea)"
            dst = f"st{bits}(ea, ({UTYPE[bits]})r);"
        else:
            self.e("{")
            dst = self.wr(ops[0], bits, "r")
        self.e(f"  uint32_t a = {a}, b = {b};")
        if carry:
            self.e("  uint32_t c = (uint32_t)x86_cf(&F);")
            expr = op.replace("C", "c")
        else:
            expr = op
        self.e(f"  uint32_t r = ({expr}) & {MASK[bits]};")
        if store:
            self.e("  " + dst)
        self.e("  " + self.flags(fl, bits, "a", "b", "r", "c" if carry else None) + " }")

    def i_add(self, ops, p):
        self._alu(ops, "a + b", "FL_ADD")

    def i_sub(self, ops, p):
        self._alu(ops, "a - b", "FL_SUB")

    def i_cmp(self, ops, p):
        self._alu(ops, "a - b", "FL_SUB", store=False)

    def i_adc(self, ops, p):
        self._alu(ops, "a + b + C", "FL_ADC", carry=True)

    def i_sbb(self, ops, p):
        self._alu(ops, "a - b - C", "FL_SBB", carry=True)

    def i_and(self, ops, p):
        self._alu(ops, "a & b", "FL_LOGIC")

    def i_or(self, ops, p):
        self._alu(ops, "a | b", "FL_LOGIC")

    def i_xor(self, ops, p):
        if ops[0].kind == "reg" and ops[1].kind == "reg" and ops[0].reg == ops[1].reg and ops[0].bits == 32:
            self.e(f"{ops[0].reg} = 0; F.op = FL_LOGIC; F.bits = 32; F.a = F.b = F.r = 0;")
            return
        self._alu(ops, "a ^ b", "FL_LOGIC")

    def i_test(self, ops, p):
        self._alu(ops, "a & b", "FL_LOGIC", store=False)

    def _incdec(self, ops, d, fl):
        bits = self.size(*ops)
        o = ops[0]
        if o.kind == "mem":
            self.e(f"{{ uint32_t ea = {self.ea(o)}; uint32_t a = ld{bits}(ea); int c = x86_cf(&F);")
            self.e(f"  uint32_t r = (a {d} 1) & {MASK[bits]}; st{bits}(ea, ({UTYPE[bits]})r);")
        else:
            self.e(f"{{ uint32_t a = {self.rd(o, bits)}; int c = x86_cf(&F);")
            self.e(f"  uint32_t r = (a {d} 1) & {MASK[bits]}; {self.wr(o, bits, 'r')}")
        self.e(f"  F.op = {fl}; F.bits = {bits}; F.a = a; F.b = 1; F.r = r; F.cf = c; }}")

    def i_inc(self, ops, p):
        self._incdec(ops, "+", "FL_INC")

    def i_dec(self, ops, p):
        self._incdec(ops, "-", "FL_DEC")

    def i_neg(self, ops, p):
        bits = self.size(*ops)
        self._rmw(ops[0], bits, f"(0u - a) & {MASK[bits]}", "F.op = FL_NEG; F.bits = %d; F.a = 0; F.b = a; F.r = r;" % bits)

    def i_not(self, ops, p):
        bits = self.size(*ops)
        self._rmw(ops[0], bits, f"(~a) & {MASK[bits]}", "")

    def _rmw(self, o, bits, expr, tail):
        if o.kind == "mem":
            self.e(f"{{ uint32_t ea = {self.ea(o)}; uint32_t a = ld{bits}(ea); uint32_t r = {expr}; "
                   f"st{bits}(ea, ({UTYPE[bits]})r); {tail} }}")
        else:
            self.e(f"{{ uint32_t a = {self.rd(o, bits)}; uint32_t r = {expr}; {self.wr(o, bits, 'r')} {tail} }}")

    def _shift(self, ops, mn):
        bits = self.size(ops[0])
        if len(ops) == 1:
            cnt = "1u"  # D0/D1 forms: shift by one
        else:
            cnt = "(ecx & 31u)" if ops[1].kind == "reg" else f"{ops[1].imm & 31}u"
        sign = f"0x{1 << (bits - 1):X}u"
        if mn == "shl":
            r = f"(a << n) & {MASK[bits]}"
            cf = f"n <= {bits} ? (int)((a >> ({bits} - n)) & 1u) : 0"
            of = f"((r & {sign}) != 0) ^ cf"
        elif mn == "shr":
            r = f"n < 32 ? (a >> n) : 0"
            cf = "(int)((a >> (n - 1)) & 1u)"
            of = f"(a & {sign}) != 0"
        elif mn == "sar":
            r = f"(uint32_t)((int32_t)({STYPE[bits]})a >> (n < {bits} ? n : {bits} - 1)) & {MASK[bits]}"
            cf = f"(int)(((int32_t)({STYPE[bits]})a >> (n - 1 < {bits} ? n - 1 : {bits} - 1)) & 1)"
            of = "0"
        elif mn in ("rol", "ror"):
            if bits != 32:
                raise Unsupported(f"{mn}{bits}")
            r = "(a << n) | (a >> ((32 - n) & 31))" if mn == "rol" else "(a >> n) | (a << ((32 - n) & 31))"
            cf = "(int)(r & 1u)" if mn == "rol" else "(int)(r >> 31)"
            of = "(int)((r >> 31) ^ (r & 1u))" if mn == "rol" else "(int)(((r >> 31) ^ (r >> 30)) & 1u)"
        else:
            raise Unsupported(f"mnemonic {mn}")
        o = ops[0]
        if o.kind == "mem":
            self.e(f"{{ uint32_t ea = {self.ea(o)}; uint32_t n = {cnt}; if (n) {{ uint32_t a = ld{bits}(ea);")
            store = f"st{bits}(ea, ({UTYPE[bits]})r);"
        else:
            self.e(f"{{ uint32_t n = {cnt}; if (n) {{ uint32_t a = {self.rd(o, bits)};")
            store = self.wr(o, bits, "r")
        self.e(f"  uint32_t r = {r}; int cf = {cf}; int of = {of}; (void)of; {store}")
        if mn in ("rol", "ror"):
            self.e("  int z = x86_zf(&F), s = x86_sf(&F), pp = x86_pf(&F); "
                   "F.op = FL_EXPL; F.zf = z; F.sf = s; F.pf = pp; F.cf = cf; F.of = of; } }")
        else:
            self.e(f"  x86_expl(&F, {bits}, r, cf, of); }} }}")

    def i_shl(self, ops, p):
        self._shift(ops, "shl")

    def i_shr(self, ops, p):
        self._shift(ops, "shr")

    def i_sar(self, ops, p):
        self._shift(ops, "sar")

    def i_rol(self, ops, p):
        self._shift(ops, "rol")

    def i_ror(self, ops, p):
        self._shift(ops, "ror")

    def i_shld(self, ops, p):
        self._shxd(ops, True)

    def i_shrd(self, ops, p):
        self._shxd(ops, False)

    def _shxd(self, ops, left):
        if self.size(ops[0]) != 32:
            raise Unsupported("shxd16")
        cnt = "(ecx & 31u)" if ops[2].kind == "reg" else f"{ops[2].imm & 31}u"
        b = self.rd(ops[1], 32)
        if left:
            r, cf = "(a << n) | (b >> (32 - n))", "(int)((a >> (32 - n)) & 1u)"
        else:
            r, cf = "(a >> n) | (b << (32 - n))", "(int)((a >> (n - 1)) & 1u)"
        o = ops[0]
        self.e(f"{{ uint32_t n = {cnt}; if (n) {{ uint32_t a = {self.rd(o, 32)}, b = {b}; uint32_t r = {r}; "
               f"int cf = {cf}; {self.wr(o, 32, 'r')} x86_expl(&F, 32, r, cf, (int)(((r ^ a) >> 31) & 1u)); }} }}")

    def i_imul(self, ops, p):
        if len(ops) == 1:
            if ops[0].bits != 32:
                raise Unsupported("imul8/16")
            self.e(f"{{ int64_t r = (int64_t)(int32_t)eax * (int32_t){self.rd(ops[0], 32)}; eax = (uint32_t)r; "
                   "edx = (uint32_t)((uint64_t)r >> 32); int ov = r != (int64_t)(int32_t)r; x86_expl(&F, 32, eax, ov, ov); }")
            return
        bits = self.size(ops[0])
        if bits != 32:
            raise Unsupported("imul16")
        a = self.rd(ops[0], 32) if len(ops) == 2 else self.rd(ops[1], 32)
        b = self.rd(ops[1], 32) if len(ops) == 2 else self.rd(ops[2], 32)
        self.e(f"{{ int64_t r = (int64_t)(int32_t){a} * (int32_t){b}; int ov = r != (int64_t)(int32_t)r; "
               f"{self.wr(ops[0], 32, '(uint32_t)r')} x86_expl(&F, 32, (uint32_t)r, ov, ov); }}")

    def i_mul(self, ops, p):
        bits = self.size(ops[0])
        if bits == 32:
            self.e(f"{{ uint64_t r = (uint64_t)eax * {self.rd(ops[0], 32)}; eax = (uint32_t)r; edx = (uint32_t)(r >> 32); "
                   "int ov = edx != 0; x86_expl(&F, 32, eax, ov, ov); }")
        elif bits == 8:
            self.e(f"{{ uint32_t r = (uint32_t)(uint8_t)eax * {self.rd(ops[0], 8)}; eax = (eax & 0xFFFF0000u) | (r & 0xFFFFu); "
                   "int ov = (r >> 8) != 0; x86_expl(&F, 8, r, ov, ov); }")
        else:
            raise Unsupported("mul16")

    def _div(self, ops, signed):
        bits = self.size(ops[0])
        d = self.rd(ops[0], bits)
        if bits == 32:
            if signed:
                self.e(f"{{ int32_t d = (int32_t){d}; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); "
                       f"if (d == 0) x86_trap(ctx, 0u, \"idiv by zero\"); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }}")
            else:
                self.e(f"{{ uint32_t d = {d}; uint64_t n = ((uint64_t)edx << 32) | eax; "
                       f"if (d == 0) x86_trap(ctx, 0u, \"div by zero\"); eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }}")
        elif bits == 8 and not signed:
            self.e(f"{{ uint32_t d = {d}; uint32_t n = (uint16_t)eax; if (d == 0) x86_trap(ctx, 0u, \"div by zero\"); "
                   "eax = (eax & 0xFFFF0000u) | ((n % d) << 8) | (n / d); }")
        else:
            raise Unsupported(f"div{bits}")

    def i_div(self, ops, p):
        self._div(ops, False)

    def i_idiv(self, ops, p):
        self._div(ops, True)

    def i_cdq(self, ops, p):
        self.e("edx = (uint32_t)((int32_t)eax >> 31);")

    def i_cwde(self, ops, p):
        self.e("eax = (uint32_t)(int32_t)(int16_t)eax;")

    def i_cbw(self, ops, p):
        self.e("eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;")

    def i_xchg(self, ops, p):
        bits = self.size(*ops)
        self.e(f"{{ uint32_t t = {self.rd(ops[0], bits)}; {self.wr(ops[0], bits, self.rd(ops[1], bits))} "
               f"{self.wr(ops[1], bits, 't')} }}")

    def i_bswap(self, ops, p):
        self.e(f"{ops[0].reg} = __builtin_bswap32({ops[0].reg});")

    def i_bt(self, ops, p):
        o, b = ops
        if o.kind == "mem" and b.kind == "reg":
            self.e(f"{{ int32_t n = (int32_t){self.rd(b, 32)}; uint32_t v = ld32({self.ea(o)} + (uint32_t)((n >> 5) * 4)); "
                   "int cf = (int)((v >> (n & 31)) & 1u); int z = x86_zf(&F), s = x86_sf(&F), pp = x86_pf(&F), of = x86_of(&F); "
                   "F.op = FL_EXPL; F.cf = cf; F.zf = z; F.sf = s; F.pf = pp; F.of = of; }")
            return
        bits = self.size(o)
        self.e(f"{{ uint32_t v = {self.rd(o, bits)}; int cf = (int)((v >> ({self.rd(b, 32)} & {bits - 1}u)) & 1u); "
               "int z = x86_zf(&F), s = x86_sf(&F), pp = x86_pf(&F), of = x86_of(&F); "
               "F.op = FL_EXPL; F.cf = cf; F.zf = z; F.sf = s; F.pf = pp; F.of = of; }")

    def _bitscan(self, ops, rev):
        if ops[0].bits != 32:
            raise Unsupported("bitscan16")
        f = "31 - __builtin_clz(v)" if rev else "__builtin_ctz(v)"
        self.e(f"{{ uint32_t v = {self.rd(ops[1], 32)}; if (v) {{ {ops[0].reg} = (uint32_t)({f}); }} "
               "F.op = FL_EXPL; F.zf = v == 0; F.cf = 0; F.of = 0; F.sf = 0; F.pf = 0; }")

    def i_bsr(self, ops, p):
        self._bitscan(ops, True)

    def i_bsf(self, ops, p):
        self._bitscan(ops, False)

    def i_leave(self, ops, p):
        self.e("esp = ebp; ebp = ld32(esp); esp += 4;")

    def i_lahf(self, ops, p):
        self.e("eax = (eax & 0xFFFF00FFu) | ((x86_eflags(&F) & 0xFFu) << 8);")

    def i_sahf(self, ops, p):
        self.e("x86_set_eflags(&F, (eax >> 8) & 0xFFu, 0);")

    def i_pushfd(self, ops, p):
        self.e("esp -= 4; st32(esp, x86_eflags(&F));")

    def i_popfd(self, ops, p):
        self.e("x86_set_eflags(&F, ld32(esp), 1); esp += 4;")

    # --- string instructions (DF is never set by this exe: no std/cld, see CHECKLIST) ---
    def _string(self, mn, p):
        bits = {"b": 8, "w": 16, "d": 32}[mn[-1]]
        step = bits // 8
        kind = mn[:-1]
        if kind == "movs":
            body = f"st{bits}(edi, ld{bits}(esi)); esi += {step}; edi += {step};"
        elif kind == "stos":
            body = f"st{bits}(edi, ({UTYPE[bits]})eax); edi += {step};"
        elif kind == "cmps":
            body = (f"{{ uint32_t a = ld{bits}(esi), b = ld{bits}(edi); esi += {step}; edi += {step}; "
                    f"F.op = FL_SUB; F.bits = {bits}; F.a = a; F.b = b; F.r = (a - b) & {MASK[bits]}; }}")
        elif kind == "scas":
            body = (f"{{ uint32_t a = ({UTYPE[bits]})eax, b = ld{bits}(edi); edi += {step}; "
                    f"F.op = FL_SUB; F.bits = {bits}; F.a = a; F.b = b; F.r = (a - b) & {MASK[bits]}; }}")
        else:
            raise Unsupported(f"mnemonic {mn}")
        if p is None:
            self.e(body)
        elif kind in ("cmps", "scas"):
            stop = "!x86_zf(&F)" if p in ("rep", "repe", "repz") else "x86_zf(&F)"
            self.e(f"while (ecx) {{ {body} ecx--; if ({stop}) break; }}")
        else:
            self.e(f"while (ecx) {{ {body} ecx--; }}")

    def __getattr__(self, name):
        if name.startswith("i_") and name[2:-1] in ("movs", "stos", "cmps", "scas") and name[-1] in "bwd":
            return lambda ops, p: self._string(name[2:], p)
        raise AttributeError(name)


def split_ops(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


class Lifter:
    def __init__(self, exe, llvm, catalog_path):
        data = open(exe, "rb").read()
        self.pe = pe = PE32(data)
        text = next(s for s in pe.sections if s.is_code)
        self.tva = pe.image_base + text.va
        self.code = data[text.raw_off: text.raw_off + min(text.raw_size, text.vsize)]
        self.code += b"\0" * (text.vsize - len(self.code))
        self.dis = X.LLVMDisasm(llvm)
        self.dis.bind(self.code, self.tva)
        self.imports = pe.imports()
        from catalog import NORETURN_IMPORTS
        self.noreturn_slots = {s for s, (_, n) in self.imports.items() if n in NORETURN_IMPORTS}
        cat = json.load(open(catalog_path))
        self.functions = {int(f["entry"], 16): f for f in cat["functions"] if f.get("reachable", True)}
        self.entries = set(self.functions)
        # Jump tables: re-read from relocations (same rule as the catalog).
        self._relocs = set(pe.relocations())
        self.tables = {}

    def table(self, va):
        if va not in self.tables:
            ts, p = [], va
            while p in self._relocs and len(ts) < 4096:
                t = self.pe.u32(p)
                if not (self.tva <= t < self.tva + len(self.code)):
                    break
                ts.append(t)
                p += 4
            self.tables[va] = ts
        return self.tables[va]

    def lift(self, entry):
        fl = FnLifter(self, entry, self.functions[entry])
        return fl.lift(), fl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--llvm", required=True)
    ap.add_argument("--catalog", required=True)
    ap.add_argument("--report", required=True)
    args = ap.parse_args()
    L = Lifter(args.exe, args.llvm, args.catalog)
    total = collections.Counter()
    clean = 0
    failed = collections.Counter()
    size = 0
    for i, e in enumerate(sorted(L.entries)):
        try:
            src, fl = L.lift(e)
        except Unsupported as ex:
            failed[str(ex)[:40]] += 1
            continue
        size += len(src)
        if fl.unsupported:
            total.update(fl.unsupported)
        else:
            clean += 1
    rep = {"functions": len(L.entries), "fully_lifted": clean, "failed": dict(failed.most_common(30)),
           "c_bytes": size, "unsupported_instructions": dict(total.most_common(80))}
    json.dump(rep, open(args.report, "w"), indent=1)
    print(json.dumps({k: rep[k] for k in ("functions", "fully_lifted", "c_bytes", "failed")}, indent=1))
    print(list(rep["unsupported_instructions"].items())[:40])


if __name__ == "__main__":
    main()
