"""Downloads chosen files out of a large ZIP on a web server, without downloading the whole ZIP.

The Android NDK is 730 MB and llvm-mingw 190 MB, and the build uses a small part of each. A ZIP keeps
its list of files at the end, so three kinds of requests are enough: the end of the file, the list,
and the byte ranges that hold the wanted files. The list is checked by SHA-256 (it carries the CRC-32
of every file), and every unpacked file by that CRC-32.
"""
import hashlib
import os
import struct
import urllib.request
import zlib

GAP = 512 * 1024  # ranges closer than this are fetched as one


def _get(url, first, last, progress=None):
    request = urllib.request.Request(url, headers={"User-Agent": "RunnerBuilder", "Range": f"bytes={first}-{last}"})
    parts, done = [], 0
    with urllib.request.urlopen(request, timeout=60) as r:
        if r.status != 206:
            raise OSError(f"the server ignored a range request ({r.status})")
        for chunk in iter(lambda: r.read(1 << 20), b""):
            parts.append(chunk)
            done += len(chunk)
            if progress:
                progress(len(chunk))
    data = b"".join(parts)
    if len(data) != last - first + 1:
        raise OSError("the download ended early")
    return data


def directory(url):
    """(entries, raw directory bytes, size of the file); an entry is (name, method, crc, packed, size, offset)."""
    request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "RunnerBuilder"})
    with urllib.request.urlopen(request, timeout=60) as r:
        total = int(r.headers["Content-Length"])
    tail_at = max(0, total - 70000)
    tail = _get(url, tail_at, total - 1)
    end = tail.rfind(b"PK\x05\x06")
    if end < 0:
        raise OSError("not a ZIP file")
    count, size, offset = struct.unpack_from("<HII", tail, end + 10)
    locator = tail.rfind(b"PK\x06\x07", 0, end)
    if locator >= 0 and (count == 0xFFFF or offset == 0xFFFFFFFF):
        at = struct.unpack_from("<Q", tail, locator + 8)[0] - tail_at
        count, size, offset = struct.unpack_from("<QQQ", tail, at + 32)
    raw = tail[offset - tail_at:offset - tail_at + size] if offset >= tail_at else _get(url, offset, offset + size - 1)
    entries, p = [], 0
    for _ in range(count):
        method, _, _, crc, packed, plain, n, m, k = struct.unpack_from("<HHHIIIHHH", raw, p + 10)
        local = struct.unpack_from("<I", raw, p + 42)[0]
        name = raw[p + 46:p + 46 + n].decode("utf-8")
        extra = raw[p + 46 + n:p + 46 + n + m]
        q = 0
        while q + 4 <= len(extra):  # ZIP64 sizes
            tag, length = struct.unpack_from("<HH", extra, q)
            if tag == 1:
                fields = list(struct.unpack_from(f"<{length // 8}Q", extra, q + 4))
                if plain == 0xFFFFFFFF:
                    plain = fields.pop(0)
                if packed == 0xFFFFFFFF:
                    packed = fields.pop(0)
                if local == 0xFFFFFFFF:
                    local = fields.pop(0)
            q += 4 + length
        entries.append((name, method, crc, packed, plain, local))
        p += 46 + n + m + k
    return entries, raw, offset


def fetch(url, choose, target, directory_sha256=None, progress=None):
    """Unpacks every entry for which choose(name) gives a relative path into `target`.

    progress(done bytes, total bytes) is called while downloading. Returns the number of files."""
    entries, raw, directory_at = directory(url)
    if directory_sha256 and hashlib.sha256(raw).hexdigest() != directory_sha256:
        raise OSError("the archive on the server is not the expected one")
    order = sorted(entries, key=lambda e: e[5])
    ends = {e[5]: (order[i + 1][5] if i + 1 < len(order) else directory_at) for i, e in enumerate(order)}
    wanted = [(e, choose(e[0])) for e in order if not e[0].endswith("/")]
    wanted = [(e, path) for e, path in wanted if path]
    ranges = []
    for e, _ in wanted:
        first, last = e[5], ends[e[5]]
        if ranges and first - ranges[-1][1] <= GAP:
            ranges[-1][1] = last
        else:
            ranges.append([first, last])
    total = sum(last - first for first, last in ranges)
    done = [0]

    def step(n):
        done[0] += n
        if progress:
            progress(done[0], total)

    k = 0
    for first, last in ranges:
        data = _get(url, first, last - 1, step)
        while k < len(wanted) and wanted[k][0][5] < last:
            (name, method, crc, packed, plain, local), path = wanted[k]
            p = local - first
            if data[p:p + 4] != b"PK\x03\x04":
                raise OSError(f"bad ZIP entry: {name}")
            n, m = struct.unpack_from("<HH", data, p + 26)
            body = data[p + 30 + n + m:p + 30 + n + m + packed]
            content = body if method == 0 else zlib.decompress(body, -15)
            if len(content) != plain or zlib.crc32(content) != crc:
                raise OSError(f"damaged file in the download: {name}")
            out = os.path.join(target, *path.split("/"))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(out, "wb") as f:
                f.write(content)
            k += 1
    return len(wanted)
