"""Takes single DLLs out of the official Visual C++ and PhysX installers without running them.

Nothing is installed: an installer is a cabinet inside an .exe, an .msi or both, or a 7-Zip
archive behind a small program. This file reads those formats itself (cabinets with LZX, 7-Zip
with LZMA, LZMA2 and BCJ2) and picks the wanted files by SHA-256, so it needs no system tools and
behaves the same on Windows, Wine and other systems.
"""
import hashlib
import lzma
import os
import re
import struct
import zlib

CFB_MAGIC = bytes.fromhex("D0CF11E0A1B11AE1")


class Found(Exception):
    """Raised to stop a walk once every wanted file is out."""


# ---- OLE compound files (.msi) ----
def cfb_streams(data):
    """The large streams of an OLE compound file (.msi), each rebuilt from its sector chain."""
    sector = 1 << struct.unpack_from("<H", data, 30)[0]
    at = lambda n: (n + 1) * sector  # noqa: E731
    fat_count, directory_start = struct.unpack_from("<I", data, 44)[0], struct.unpack_from("<I", data, 48)[0]
    difat = list(struct.unpack_from("<109I", data, 76))
    next_difat, extra = struct.unpack_from("<II", data, 68)
    for _ in range(extra):
        chunk = struct.unpack_from(f"<{sector // 4}I", data, at(next_difat))
        difat += chunk[:-1]
        next_difat = chunk[-1]
    fat = []
    for n in difat[:fat_count]:
        fat += struct.unpack_from(f"<{sector // 4}I", data, at(n))

    def chain(first):
        parts, n = [], first
        while n < 0xFFFFFFFA:
            parts.append(data[at(n):at(n) + sector])
            n = fat[n]
        return b"".join(parts)

    cutoff = struct.unpack_from("<I", data, 56)[0]
    directory = chain(directory_start)
    for off in range(0, len(directory) - 127, 128):
        kind = directory[off + 66]
        first, size = struct.unpack_from("<II", directory, off + 116)
        if kind == 2 and size >= cutoff:
            yield chain(first)[:size]


def cabinets(data):
    """Cabinets glued into an executable or stored in the streams of an .msi."""
    if data[:8] == CFB_MAGIC:
        return [s for s in cfb_streams(data) if s[:4] == b"MSCF"]
    found, pos = [], 0
    while True:
        pos = data.find(b"MSCF\0\0\0\0", pos)
        if pos < 0:
            return found
        size = struct.unpack_from("<I", data, pos + 8)[0]
        if data[pos + 25] == 1 and 0 < size <= len(data) - pos:
            found.append(data[pos:pos + size])
            pos += size
        else:
            pos += 4


# ---- LZX, as cabinets use it ----
_EXTRA = []
_j = 0
for _i in range(0, 52, 2):
    _EXTRA += [_j, _j]
    if _i != 0 and _j < 17:
        _j += 1
_BASE, _j = [], 0
for _i in range(51):
    _BASE.append(_j)
    _j += 1 << _EXTRA[_i]
_SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}


class _Bits:
    """LZX bit order: 16-bit little-endian words, most significant bit first."""

    def __init__(self, data):
        self.d, self.p, self.buf, self.n = data, 0, 0, 0

    def need(self, count):
        while self.n < count:
            p = self.p
            word = self.d[p] | (self.d[p + 1] << 8) if p + 1 < len(self.d) else 0
            self.p = p + 2
            self.buf = ((self.buf & ((1 << self.n) - 1)) << 16) | word
            self.n += 16

    def read(self, count):
        if count == 0:
            return 0
        self.need(count)
        self.n -= count
        return (self.buf >> self.n) & ((1 << count) - 1)

    def align(self):
        if self.n > 0:
            self.need(16)
        self.n -= self.n & 15


def _tree(lengths):
    """Canonical Huffman code as ({(1 << length) | code: symbol}, lengths in use)."""
    table, code = {}, 0
    used = sorted({n for n in lengths if n})
    last = 0
    for n in used:
        code <<= n - last
        last = n
        for sym, length in enumerate(lengths):
            if length == n:
                table[(1 << n) | code] = sym
                code += 1
    return table, used


def _sym(bits, tree):
    table, used = tree
    bits.need(16)
    top = (bits.buf >> (bits.n - 16)) & 0xFFFF
    for n in used:
        sym = table.get((1 << n) | (top >> (16 - n)))
        if sym is not None:
            bits.n -= n
            return sym
    raise ValueError("bad LZX data")


def _read_lengths(bits, lens, first, last):
    pre = _tree([bits.read(4) for _ in range(20)])
    x = first
    while x < last:
        z = _sym(bits, pre)
        if z == 17:
            y = bits.read(4) + 4
            lens[x:x + y] = [0] * y
            x += y
        elif z == 18:
            y = bits.read(5) + 20
            lens[x:x + y] = [0] * y
            x += y
        elif z == 19:
            y = bits.read(1) + 4
            z = (lens[x] - _sym(bits, pre)) % 17
            lens[x:x + y] = [z] * y
            x += y
        else:
            lens[x] = (lens[x] - z) % 17
            x += 1


def lzx_decompress(data, window_bits, out_size):
    """Decompresses a whole LZX stream (the data blocks of one cabinet folder joined together)."""
    wsize = 1 << window_bits
    main_n = 256 + _SLOTS[window_bits] * 8
    window = bytearray(wsize)
    bits = _Bits(data)
    main_len, len_len = [0] * main_n, [0] * 250
    main = length = aligned = None
    r0 = r1 = r2 = 1
    posn = 0
    block_type = block_len = block_left = 0
    intel_size = intel_pos = 0
    intel_on = False
    if bits.read(1):
        intel_size = (bits.read(16) << 16) | bits.read(16)
    out = bytearray()
    while len(out) < out_size:
        frame = min(32768, out_size - len(out))
        todo = frame
        start = posn
        while todo > 0:
            if block_left == 0:
                if block_type == 3 and (block_len & 1):
                    bits.p += 1
                block_type = bits.read(3)
                block_len = block_left = (bits.read(16) << 8) | bits.read(8)
                if block_type in (1, 2):
                    if block_type == 2:
                        aligned = _tree([bits.read(3) for _ in range(8)])
                    _read_lengths(bits, main_len, 0, 256)
                    _read_lengths(bits, main_len, 256, main_n)
                    main = _tree(main_len)
                    if main_len[0xE8]:
                        intel_on = True
                    _read_lengths(bits, len_len, 0, 249)
                    length = _tree(len_len)
                elif block_type == 3:
                    intel_on = True
                    bits.need(16)
                    if bits.n > 16:
                        bits.p -= 2
                    bits.n = bits.buf = 0
                    r0, r1, r2 = struct.unpack_from("<III", bits.d, bits.p)
                    bits.p += 12
                else:
                    raise ValueError("bad LZX block")
            run = min(block_left, todo)
            todo -= run
            block_left -= run
            if block_type == 3:
                window[posn:posn + run] = bits.d[bits.p:bits.p + run]
                bits.p += run
                posn += run
                continue
            while run > 0:
                sym = _sym(bits, main)
                if sym < 256:
                    window[posn] = sym
                    posn += 1
                    run -= 1
                    continue
                sym -= 256
                n = sym & 7
                if n == 7:
                    n += _sym(bits, length)
                n += 2
                slot = sym >> 3
                if slot > 2:
                    if slot == 3:
                        off = 1
                    elif block_type == 1:
                        off = _BASE[slot] - 2 + bits.read(_EXTRA[slot])
                    else:
                        extra = _EXTRA[slot]
                        off = _BASE[slot] - 2
                        if extra > 3:
                            off += bits.read(extra - 3) << 3
                            off += _sym(bits, aligned)
                        elif extra == 3:
                            off += _sym(bits, aligned)
                        elif extra > 0:
                            off += bits.read(extra)
                        else:
                            off = 1
                    r2, r1, r0 = r1, r0, off
                elif slot == 0:
                    off = r0
                elif slot == 1:
                    off = r1
                    r1, r0 = r0, off
                else:
                    off = r2
                    r2, r0 = r0, off
                src = posn - off
                if src >= 0 and off >= n:
                    window[posn:posn + n] = window[src:src + n]
                else:
                    for k in range(n):
                        window[posn + k] = window[(src + k) % wsize]
                posn += n
                run -= n
            if run < 0:
                block_left += run
        chunk = window[start:start + frame]
        if intel_on and intel_size and frame > 10 and intel_pos < 32768 * 32768:
            cur, i, end = intel_pos, 0, frame - 10
            while True:
                i = chunk.find(b"\xe8", i, end)
                if i < 0:
                    break
                at = cur + i
                value = struct.unpack_from("<i", chunk, i + 1)[0]
                if -at <= value < intel_size:
                    struct.pack_into("<i", chunk, i + 1, value - at if value >= 0 else value + intel_size)
                i += 5
        intel_pos += frame
        out += chunk
        if posn == wsize:
            posn = 0
        bits.align()
    return bytes(out)


def cab_files(cab):
    """Yields the content of every file of a cabinet, folder by folder."""
    folders_n, files_n, flags = struct.unpack_from("<HHH", cab, 26)
    files_at = struct.unpack_from("<I", cab, 16)[0]
    p, res_folder, res_data = 36, 0, 0
    if flags & 4:
        res_header, res_folder, res_data = struct.unpack_from("<HBB", cab, 36)
        p = 40 + res_header
    for bit in (1, 2):
        if flags & bit:
            for _ in range(2):
                p = cab.index(b"\0", p) + 1
    folders = []
    for _ in range(folders_n):
        folders.append(struct.unpack_from("<IHH", cab, p))
        p += 8 + res_folder
    entries, p = [], files_at
    for _ in range(files_n):
        size, offset, folder = struct.unpack_from("<IIH", cab, p)
        p = cab.index(b"\0", p + 16) + 1
        entries.append((folder, offset, size))
    for index, (start, blocks, kind) in enumerate(folders):
        mine = [e for e in entries if e[0] == index]
        if not mine:
            continue
        need = max(offset + size for _, offset, size in mine)
        parts, total, p = [], 0, start
        for _ in range(blocks):
            packed, plain = struct.unpack_from("<HH", cab, p + 4)
            parts.append(cab[p + 8 + res_data:p + 8 + res_data + packed])
            total += plain
            p += 8 + res_data + packed
        method = kind & 15
        if method == 0:
            data = b"".join(parts)
        elif method == 3:
            data = lzx_decompress(b"".join(parts), (kind >> 8) & 31, min(total, need))
        elif method == 1:
            out = bytearray()
            for part in parts:
                z = zlib.decompressobj(-15, zdict=bytes(out[-32768:])) if out else zlib.decompressobj(-15)
                out += z.decompress(part[2:])
            data = bytes(out)
        else:
            raise ValueError(f"cabinet compression {method} is not supported")
        for _, offset, size in mine:
            yield data[offset:offset + size]


def walk(data, on_file, depth=0):
    """Unpacks every cabinet in `data`, and whatever those hold, calling on_file for each file."""
    for cab in cabinets(data):
        for content in cab_files(cab):
            on_file(content)
            if depth < 3 and (content[:2] == b"MZ" or content[:8] == CFB_MAGIC or content[:4] == b"MSCF"):
                walk(content, on_file, depth + 1)


# ---- 7-Zip ----
def _number(b, p):
    first, mask, value = b[p], 0x80, 0
    for k in range(8):
        if not first & mask:
            return value | ((first & (mask - 1)) << (8 * k)), p + 1 + k
        value |= b[p + 1 + k] << (8 * k)
        mask >>= 1
    return value, p + 9


def _lzma_filter(coder_id, props):
    if coder_id == b"\x21":
        bits = props[0]
        return {"id": lzma.FILTER_LZMA2, "dict_size": 0xFFFFFFFF if bits == 40 else (2 | (bits & 1)) << (bits // 2 + 11)}
    if coder_id == b"\x03\x01\x01":
        return {"id": lzma.FILTER_LZMA1, "lc": props[0] % 9, "lp": props[0] // 9 % 5, "pb": props[0] // 45,
                "dict_size": struct.unpack_from("<I", props, 1)[0]}
    raise ValueError(f"7-Zip coder {coder_id.hex()} is not supported")


def _bcj2(main, call, jump, rc, size):
    """x86 BCJ2: relative call and jump targets were moved to streams of their own."""
    out, done = [], 0
    probs = [1024] * 258
    code = int.from_bytes(rc[1:5], "big")
    rng, rp = 0xFFFFFFFF, 5
    cp = jp = 0
    pos, prev, end = 0, 0, len(main)
    find = re.compile(rb"[\xe8\xe9]|\x0f[\x80-\x8f]").search
    while done < size and pos < end:
        b = main[pos]
        if (b & 0xFE) == 0xE8 or (prev == 0x0F and (b & 0xF0) == 0x80):
            at = pos
        else:
            m = find(main, pos)
            if not m:
                out.append(main[pos:end])
                done += end - pos
                break
            at = m.end() - 1
            b = main[at]
            if at > pos:
                prev = main[at - 1]
        take = min(at + 1 - pos, size - done)
        out.append(main[pos:pos + take])
        done += take
        pos += take
        if done >= size:
            break
        index = prev if b == 0xE8 else 256 if b == 0xE9 else 257
        prob = probs[index]
        bound = (rng >> 11) * prob
        if code < bound:
            rng = bound
            probs[index] = prob + ((2048 - prob) >> 5)
            bit = 0
        else:
            rng -= bound
            code -= bound
            probs[index] = prob - (prob >> 5)
            bit = 1
        if rng < 0x1000000:
            rng = (rng << 8) & 0xFFFFFFFF
            code = ((code << 8) | rc[rp]) & 0xFFFFFFFF
            rp += 1
        if bit:
            if b == 0xE8:
                src = int.from_bytes(call[cp:cp + 4], "big")
                cp += 4
            else:
                src = int.from_bytes(jump[jp:jp + 4], "big")
                jp += 4
            dest = (src - (done + 4)) & 0xFFFFFFFF
            out.append(dest.to_bytes(4, "little"))
            done += 4
            prev = dest >> 24
        else:
            prev = b
    return b"".join(out)[:size]


def sevenzip_files(archive):
    """Yields the content of every file of a 7-Zip archive (bytes that start at its signature)."""
    next_at, next_size = struct.unpack_from("<QQ", archive, 12)
    header = archive[32 + next_at:32 + next_at + next_size]

    def streams_info(h, p):
        pack_pos, pack_sizes, folders, unpack, counts, sizes = 0, [], [], [], None, None
        while h[p] != 0:
            kind = h[p]
            p += 1
            if kind == 6:  # pack info
                pack_pos, p = _number(h, p)
                n, p = _number(h, p)
                while h[p] != 0:
                    sub = h[p]
                    p += 1
                    if sub == 9:
                        for _ in range(n):
                            v, p = _number(h, p)
                            pack_sizes.append(v)
                    elif sub == 10:
                        defined = h[p]
                        p += 1
                        assert defined, "7-Zip: partial CRC lists are not supported"
                        p += 4 * n
                p += 1
            elif kind == 7:  # unpack info
                assert h[p] == 11
                n, p = _number(h, p + 1)
                assert h[p] == 0
                p += 1
                for _ in range(n):
                    coders_n, p = _number(h, p)
                    coders, ins, outs = [], 0, 0
                    for _ in range(coders_n):
                        flag = h[p]
                        cid = h[p + 1:p + 1 + (flag & 15)]
                        p += 1 + (flag & 15)
                        n_in = n_out = 1
                        if flag & 0x10:
                            n_in, p = _number(h, p)
                            n_out, p = _number(h, p)
                        props = b""
                        if flag & 0x20:
                            length, p = _number(h, p)
                            props = h[p:p + length]
                            p += length
                        coders.append((cid, n_in, n_out, props, ins, outs))
                        ins += n_in
                        outs += n_out
                    binds = {}
                    for _ in range(outs - 1):
                        i, p = _number(h, p)
                        o, p = _number(h, p)
                        binds[i] = o
                    packed = [i for i in range(ins) if i not in binds]
                    if len(packed) > 1:
                        packed = []
                        for _ in range(ins - (outs - 1)):
                            i, p = _number(h, p)
                            packed.append(i)
                    folders.append((coders, binds, packed, outs))
                assert h[p] == 12
                p += 1
                for _, _, _, outs in folders:
                    row = []
                    for _ in range(outs):
                        v, p = _number(h, p)
                        row.append(v)
                    unpack.append(row)
                if h[p] == 10:
                    defined = h[p + 1]
                    assert defined
                    p += 2 + 4 * len(folders)
                assert h[p] == 0
                p += 1
            elif kind == 8:  # substreams info
                counts = [1] * len(folders)
                sizes = []
                while h[p] != 0:
                    sub = h[p]
                    p += 1
                    if sub == 13:
                        for k in range(len(folders)):
                            counts[k], p = _number(h, p)
                    elif sub == 9:
                        for k in range(len(folders)):
                            row = []
                            for _ in range(counts[k] - 1):
                                v, p = _number(h, p)
                                row.append(v)
                            sizes.append(row)
                    elif sub == 10:
                        defined = h[p]
                        p += 1
                        assert defined
                        p += 4 * sum(c for c, (_, _, _, outs) in zip(counts, folders) if True)
                p += 1
            else:
                raise ValueError(f"7-Zip: unexpected header part {kind}")
        return pack_pos, pack_sizes, folders, unpack, counts, sizes, p + 1

    def decode(folder, sizes, packs):
        coders, binds, packed, _ = folder
        main_out = next(o for o in range(len(sizes)) if o not in binds.values())
        memo = {}

        def out_stream(o):
            if o in memo:
                return memo[o]
            cid, n_in, _, props, first_in, first_out = next(c for c in coders if c[5] <= o < c[5] + c[2])
            inputs = [in_stream(first_in + k) for k in range(n_in)]
            if cid == b"\x03\x03\x01\x1b":
                data = _bcj2(*inputs, sizes[o])
            else:
                data = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[_lzma_filter(cid, props)]).decompress(inputs[0], sizes[o])
            memo[o] = data
            return data

        def in_stream(i):
            return out_stream(binds[i]) if i in binds else packs[packed.index(i)]

        return out_stream(main_out), sizes[main_out]

    def run(h, p):
        pack_pos, pack_sizes, folders, unpack, counts, sizes, p = streams_info(h, p)
        at, k = 32 + pack_pos, 0
        for index, folder in enumerate(folders):
            packs = []
            for _ in folder[2]:
                packs.append(archive[at:at + pack_sizes[k]])
                at += pack_sizes[k]
                k += 1
            data, total = decode(folder, unpack[index], packs)
            row = list(sizes[index]) if sizes else []
            if counts is None or counts[index] == 1:
                row = []
            row.append(total - sum(row))
            offset = 0
            for size in row:
                yield data[offset:offset + size]
                offset += size

    if header[0] == 0x17:  # the header itself is packed
        header = next(run(header, 1))
    assert header[0] == 1
    p = 1
    while header[p] != 4:
        if header[p] == 2:  # archive properties
            p += 1
            while header[p] != 0:
                size, p = _number(header, p + 1)
                p += size
            p += 1
        else:
            raise ValueError("7-Zip: unexpected header")
    yield from run(header, p + 1)


# ---- picking files by hash ----
def _collect(wanted, target, source):
    left = dict(wanted)

    def on_file(content):
        digest = hashlib.sha256(content).hexdigest()
        if digest in left:
            os.makedirs(target, exist_ok=True)
            with open(os.path.join(target, left.pop(digest)), "wb") as out:
                out.write(content)
            if not left:
                raise Found

    try:
        source(on_file)
    except Found:
        pass
    return sorted(left.values())


def extract(installer, wanted, target):
    """Writes the files of `wanted` ({sha256: name}) to `target`; returns the names not found."""
    with open(installer, "rb") as f:
        data = f.read()
    return _collect(wanted, target, lambda on_file: walk(data, on_file))


def extract_7z(installer, wanted, target):
    """The same as extract() for a 7-Zip self-extracting installer."""
    with open(installer, "rb") as f:
        data = f.read()
    start = data.find(b"7z\xbc\xaf\x27\x1c")
    if start < 0:
        return sorted(wanted.values())
    archive = data[start:]

    def source(on_file):
        for content in sevenzip_files(archive):
            on_file(content)

    return _collect(wanted, target, source)
