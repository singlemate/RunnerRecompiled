"""Minimal PE32 (i386) reader: sections, imports, base relocations, SafeSEH table. No dependencies."""
import struct


class Section:
    def __init__(self, name, va, vsize, raw_off, raw_size, chars):
        self.name, self.va, self.vsize, self.raw_off, self.raw_size, self.chars = name, va, vsize, raw_off, raw_size, chars

    @property
    def is_code(self):
        return bool(self.chars & 0x20000000)  # IMAGE_SCN_MEM_EXECUTE


class PE32:
    def __init__(self, data: bytes):
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ file")
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        if data[pe:pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        machine, nsec, _, _, _, opt_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
        if machine != 0x14C:
            raise ValueError(f"machine {machine:#x} is not i386")
        opt = pe + 24
        if struct.unpack_from("<H", data, opt)[0] != 0x10B:
            raise ValueError("not PE32")
        self.entry_rva = struct.unpack_from("<I", data, opt + 16)[0]
        self.image_base = struct.unpack_from("<I", data, opt + 28)[0]
        self.size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
        ndirs = struct.unpack_from("<I", data, opt + 92)[0]
        self.dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i) for i in range(ndirs)]
        self.sections = []
        for i in range(nsec):
            off = opt + opt_size + 40 * i
            name = data[off:off + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, raw_size, raw_off = struct.unpack_from("<IIII", data, off + 8)
            chars = struct.unpack_from("<I", data, off + 36)[0]
            self.sections.append(Section(name, va, vsize, raw_off, raw_size, chars))

    # --- address helpers (all public addresses are VAs) ---
    def code_blob(self):
        """(first VA, bytes) covering every executable section, gaps between them zero-filled.
        Most images have one (.text); binkw32.dll has several (.text, BINK, BINK32, ...)."""
        secs = [s for s in self.sections if s.is_code]
        start = min(s.va for s in secs)
        end = max(s.va + s.vsize for s in secs)
        blob = bytearray(end - start)
        for s in secs:
            raw = self.data[s.raw_off: s.raw_off + min(s.raw_size, s.vsize)]
            blob[s.va - start: s.va - start + len(raw)] = raw
        return self.image_base + start, bytes(blob)

    def section_of(self, va):
        rva = va - self.image_base
        for s in self.sections:
            if s.va <= rva < s.va + max(s.vsize, s.raw_size):
                return s
        return None

    def read(self, va, n):
        s = self.section_of(va)
        if s is None:
            return b""
        rva = va - self.image_base
        off = rva - s.va
        chunk = self.data[s.raw_off + off: s.raw_off + min(off + n, s.raw_size)]
        return chunk + b"\0" * (n - len(chunk)) if off < max(s.vsize, s.raw_size) else b""

    def u32(self, va):
        return struct.unpack("<I", self.read(va, 4))[0]

    def cstr(self, va, limit=512):
        b = self.read(va, limit)
        return b.split(b"\0", 1)[0].decode("latin-1")

    # --- directories ---
    def imports(self):
        """Returns {iat_slot_va: (dll, name_or_ordinal)}."""
        rva, size = self.dirs[1]
        out = {}
        if not rva:
            return out
        d = self.image_base + rva
        while True:
            oft, _, _, name_rva, ft = struct.unpack("<IIIII", self.read(d, 20))
            if not name_rva:
                break
            dll = self.cstr(self.image_base + name_rva)
            lookup = oft or ft
            i = 0
            while True:
                ent = self.u32(self.image_base + lookup + 4 * i)
                if not ent:
                    break
                sym = f"#{ent & 0xFFFF}" if ent & 0x80000000 else self.cstr(self.image_base + ent + 2)
                out[self.image_base + ft + 4 * i] = (dll, sym)
                i += 1
            d += 20
        return out

    def exports(self):
        """Returns {name_or_#ordinal: va} for exports that point into the image (not forwarders)."""
        rva, size = self.dirs[0]
        out = {}
        if not rva:
            return out
        d = self.image_base + rva
        _, _, _, _, _, base, nfuncs, nnames, funcs, names, ords = struct.unpack("<IIHHIIIIIII", self.read(d, 40))
        addr = [self.u32(self.image_base + funcs + 4 * i) for i in range(nfuncs)]
        named = set()
        for i in range(nnames):
            name = self.cstr(self.image_base + self.u32(self.image_base + names + 4 * i))
            idx = struct.unpack("<H", self.read(self.image_base + ords + 2 * i, 2))[0]
            if idx < nfuncs and addr[idx] and not (rva <= addr[idx] < rva + size):
                out[name] = self.image_base + addr[idx]
                named.add(idx)
        for idx, a in enumerate(addr):
            if a and not (rva <= a < rva + size):
                out[f"#{base + idx}"] = self.image_base + a
        return out

    def relocations(self):
        """VAs of 32-bit HIGHLOW relocation sites."""
        rva, size = self.dirs[5]
        sites = []
        p, end = self.image_base + rva, self.image_base + rva + size
        while p < end:
            page, block = struct.unpack("<II", self.read(p, 8))
            if block < 8:
                break
            ents = struct.unpack(f"<{(block - 8) // 2}H", self.read(p + 8, block - 8))
            for e in ents:
                if e >> 12 == 3:
                    sites.append(self.image_base + page + (e & 0xFFF))
                elif e >> 12 not in (0,):
                    raise ValueError(f"unsupported relocation type {e >> 12}")
            p += block
        return sites

    def safeseh_handlers(self):
        rva, size = self.dirs[10] if len(self.dirs) > 10 else (0, 0)
        if not rva:
            return []
        lc = self.image_base + rva
        lc_size = self.u32(lc)
        if lc_size < 0x48:
            return []
        table, count = self.u32(lc + 0x40), self.u32(lc + 0x44)
        return [self.image_base + self.u32(table + 4 * i) for i in range(count)]
