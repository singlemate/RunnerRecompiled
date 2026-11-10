"""x86-32 decoding for control-flow recovery.

Instruction length and text come from LLVM MC (libLLVM C API via ctypes, no downloads:
llvm-mingw in work/toolchains ships libLLVM-*.dll). Branch kinds and targets are decoded
from the raw bytes here, so the analysis does not depend on LLVM's text format.
"""
import ctypes
import glob
import os
import struct

PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}

# Branch kinds
K_NONE, K_CALL, K_CALL_IND, K_CALL_IAT, K_JMP, K_JCC, K_JMP_IND, K_JMP_TABLE, K_JMP_IAT, K_RET, K_STOP = range(11)


class LLVMDisasm:
    def __init__(self, llvm_bin_dir):
        llvm_bin_dir = os.path.abspath(llvm_bin_dir)
        dlls = sorted(glob.glob(os.path.join(llvm_bin_dir, "libLLVM-*.dll")))
        if not dlls:
            raise FileNotFoundError(f"libLLVM-*.dll not found in {llvm_bin_dir}")
        os.add_dll_directory(llvm_bin_dir)
        lib = ctypes.CDLL(dlls[-1])
        for name in ("LLVMInitializeX86TargetInfo", "LLVMInitializeX86TargetMC", "LLVMInitializeX86Disassembler"):
            getattr(lib, name)()
        lib.LLVMCreateDisasm.restype = ctypes.c_void_p
        lib.LLVMCreateDisasm.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p]
        lib.LLVMSetDisasmOptions.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        lib.LLVMDisasmInstruction.restype = ctypes.c_size_t
        lib.LLVMDisasmInstruction.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_uint64,
                                              ctypes.c_char_p, ctypes.c_size_t]
        self.lib = lib
        self.dc = lib.LLVMCreateDisasm(b"i686-pc-windows", None, 0, None, None)
        if not self.dc:
            raise RuntimeError("LLVMCreateDisasm failed")
        lib.LLVMSetDisasmOptions(self.dc, 4)  # LLVMDisassembler_Option_AsmPrinterVariant: Intel syntax
        self.out = ctypes.create_string_buffer(160)

    def bind(self, code: bytes, base_va: int):
        self.code = code
        self.base_va = base_va
        self.buf = ctypes.create_string_buffer(code + b"\0" * 16, len(code) + 16)
        self.addr = ctypes.addressof(self.buf)

    def decode(self, va):
        """Returns (length, text) or (0, None) for invalid bytes."""
        off = va - self.base_va
        n = self.lib.LLVMDisasmInstruction(self.dc, self.addr + off, len(self.code) - off, va, self.out, 160)
        if not n:
            return 0, None
        return n, self.out.value.decode("latin-1").strip()


def branch(code, off, va, length):
    """Classifies the instruction at code[off:off+length]. Returns (kind, target_or_None)."""
    p = off
    end = off + length
    while p < end and code[p] in PREFIXES:
        p += 1
    op = code[p]
    nxt = va + length
    if op == 0xE8:
        return K_CALL, (nxt + struct.unpack_from("<i", code, end - 4)[0]) & 0xFFFFFFFF
    if op == 0xE9:
        return K_JMP, (nxt + struct.unpack_from("<i", code, end - 4)[0]) & 0xFFFFFFFF
    if op == 0xEB:
        return K_JMP, (nxt + struct.unpack_from("<b", code, end - 1)[0]) & 0xFFFFFFFF
    if 0x70 <= op <= 0x7F or 0xE0 <= op <= 0xE3:
        return K_JCC, (nxt + struct.unpack_from("<b", code, end - 1)[0]) & 0xFFFFFFFF
    if op == 0x0F:
        op2 = code[p + 1]
        if 0x80 <= op2 <= 0x8F:
            return K_JCC, (nxt + struct.unpack_from("<i", code, end - 4)[0]) & 0xFFFFFFFF
        if op2 == 0x0B:  # ud2
            return K_STOP, None
        return K_NONE, None
    if op in (0xC3, 0xC2, 0xCB, 0xCA, 0xCF):
        return K_RET, None
    if op in (0xCC, 0xF4):
        return K_STOP, None
    if op == 0xFF:
        modrm = code[p + 1]
        reg = (modrm >> 3) & 7
        if reg == 2 or reg == 4:
            call = reg == 2
            if modrm & 0xC7 == 0x05:  # [disp32]: import slot or global function pointer
                return (K_CALL_IAT if call else K_JMP_IAT), struct.unpack_from("<I", code, p + 2)[0]
            if not call and modrm & 0xC7 == 0x04:  # SIB, mod=00
                sib = code[p + 2]
                if sib & 0x07 == 0x05 and sib >> 6 == 2:  # [index*4 + disp32]
                    return K_JMP_TABLE, struct.unpack_from("<I", code, p + 3)[0]
            return (K_CALL_IND if call else K_JMP_IND), None
    return K_NONE, None
