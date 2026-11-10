"""Rebases a PE32 DLL to a new image base by applying its HIGHLOW relocations to a copy.

The lifter then works on the rebased copy, so every absolute address in the lifted code is
already the final guest address (DLLs of the game and of PhysX all ask for 0x10000000).

  python -I outputs/mirrors-edge-native/pc/tools/rebase_pe.py <in.dll> <out.dll> <new base, hex>
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe32 import PE32  # noqa: E402


def rebase(src, dst, new_base):
    data = bytearray(open(src, "rb").read())
    pe = PE32(bytes(data))
    delta = (new_base - pe.image_base) & 0xFFFFFFFF
    n = 0
    for site in pe.relocations():
        rva = site - pe.image_base
        sec = next((s for s in pe.sections if s.va <= rva < s.va + max(s.vsize, s.raw_size)), None)
        if sec is None or rva - sec.va + 4 > sec.raw_size:
            continue  # site in uninitialized data: nothing stored in the file
        off = sec.raw_off + rva - sec.va
        v = struct.unpack_from("<I", data, off)[0]
        struct.pack_into("<I", data, off, (v + delta) & 0xFFFFFFFF)
        n += 1
    opt = struct.unpack_from("<I", data, 0x3C)[0] + 24
    struct.pack_into("<I", data, opt + 28, new_base)
    open(dst, "wb").write(data)
    return n


if __name__ == "__main__":
    count = rebase(sys.argv[1], sys.argv[2], int(sys.argv[3], 16))
    print(f"{sys.argv[1]} -> {sys.argv[2]} at {sys.argv[3]}: {count} relocations applied")
