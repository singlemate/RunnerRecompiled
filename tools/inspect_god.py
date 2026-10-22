"""Read-only SVOD inventory and selective default.xex extraction.

Layout reference: RexGlue stfs_container_device.cpp / stfs_xbox.h.
Does not validate the SVOD hash tree or package signature.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('container', type=Path)
    ap.add_argument('--private', type=Path, required=True)
    ap.add_argument('--report', type=Path, required=True)
    args = ap.parse_args()
    header = args.container.read_bytes()
    assert len(header) >= 0x3AD and header[:4] in (b'LIVE', b'PIRS', b'CON ')
    be32 = lambda offset: struct.unpack_from('>I', header, offset)[0]
    assert be32(0x3A9) == 1, 'Only SVOD supported'
    count = be32(0x39D)
    start = int.from_bytes(header[0x395:0x398], 'little')
    enhanced = bool(header[0x391] & 0x40)
    folder = Path(str(args.container) + '.data')
    parts = sorted(folder.glob('Data[0-9][0-9][0-9][0-9]'))
    assert 0 < count <= 10000 and len(parts) == count
    assert [p.name for p in parts] == [f'Data{i:04}' for i in range(count)]
    handles = [p.open('rb') for p in parts]
    sizes = [p.stat().st_size for p in parts]

    def raw(part, offset, size):
        assert 0 <= part < count and 0 <= offset <= sizes[part] - size
        handles[part].seek(offset)
        data = handles[part].read(size)
        assert len(data) == size
        return data

    magic_offset = 0x2000 if enhanced else 0x12000
    assert raw(0, magic_offset, 20) == b'MICROSOFT*XBOX*MEDIA'

    def sector(block):
        n = block - start * 2 + (2 if enhanced else 0)
        assert n >= 0
        part, b = divmod(n, 0x14388)
        l0 = b // 0x198 + 1
        off = b * 0x800 + l0 * 0x1000 + (l0 // 0xA1C4 + 1) * 0x1000
        if off >= 0xA290000:
            part += 1
            off = off % 0xA290000 + 0x2000
        return raw(part, off, 0x800)

    def read(block, offset, length):
        result = bytearray()
        while length:
            delta, within = divmod(offset, 0x800)
            take = min(length, 0x800 - within)
            result.extend(sector(block + delta)[within:within + take])
            offset += take
            length -= take
        return bytes(result)

    root, root_size = struct.unpack('<II', raw(0, magic_offset + 20, 8))
    entries, visited = [], set()

    def directory(block, length, parent, depth=0):
        assert depth < 64 and length <= 64 * 1024 * 1024
        pending = [0]
        while pending:
            ordinal = pending.pop()
            ident = (block, ordinal)
            assert ident not in visited, 'Cyclic directory'
            visited.add(ident)
            assert len(visited) <= 500000
            offset = ordinal * 4
            assert offset + 14 <= length
            left, right, data_block, size, attrs, namelen = struct.unpack('<HHIIBB', read(block, offset, 14))
            assert namelen and offset + 14 + namelen <= length
            name = read(block, offset + 14, namelen).decode('ascii')
            assert name not in ('.', '..') and not any(c in name for c in '/\\:\x00')
            path = f'{parent}/{name}' if parent else name
            entries.append(dict(path=path, block=data_block, size=size, directory=bool(attrs & 16)))
            for child in (left, right):
                if child:
                    assert child != 0xFFFF
                    pending.append(child)
            if attrs & 16 and size:
                directory(data_block, size, path, depth + 1)

    directory(root, root_size, '')
    matches = [e for e in entries if e['path'].lower() == 'default.xex' and not e['directory']]
    assert len(matches) == 1
    exe = matches[0]
    assert 24 <= exe['size'] <= 128 * 1024 * 1024
    data = read(exe['block'], 0, exe['size'])
    assert data[:4] == b'XEX2'
    args.private.mkdir(parents=True, exist_ok=True)
    dest = args.private / 'default.xex'
    if dest.exists():
        assert dest.read_bytes() == data, 'Existing output differs; refusing overwrite'
    else:
        with dest.open('xb') as out:
            out.write(data)
    nopts = struct.unpack_from('>I', data, 20)[0]
    assert 24 + nopts * 8 <= len(data)
    opts = dict(struct.unpack_from('>II', data, 24 + i * 8) for i in range(nopts))
    detail = {'magic': 'XEX2', 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
              'optional_headers': {f'{k:08X}': f'{v:08X}' for k, v in opts.items()}}
    if 0x3FF in opts:
        off = opts[0x3FF]
        length, enc, compression = struct.unpack_from('>IHH', data, off)
        assert 8 <= length <= len(data) - off
        detail.update(encryption_type=enc, compression_type=compression)
    if 0x40006 in opts:
        off = opts[0x40006]
        media, version, base, title = struct.unpack_from('>IIII', data, off)
        detail.update(media_id=f'{media:08X}', version_raw=f'{version:08X}',
                      base_version_raw=f'{base:08X}', title_id=f'{title:08X}')
    if 0x103FF in opts:
        off = opts[0x103FF]
        total, strings_size, strings_count = struct.unpack_from('>III', data, off)
        assert 12 + strings_size <= total <= len(data) - off
        strings, cursor = [], 0
        table = data[off + 12:off + 12 + strings_size]
        for _ in range(strings_count):
            end = table.index(b'\0', cursor)
            strings.append(table[cursor:end].decode('ascii'))
            cursor = (end + 4) & ~3
        cursor, libraries = 12 + strings_size, []
        while cursor < total:
            assert cursor + 40 <= total
            length = struct.unpack_from('>I', data, off + cursor)[0]
            name_index, records = struct.unpack_from('>HH', data, off + cursor + 36)
            assert 40 + records * 4 <= length <= total - cursor
            libraries.append(dict(name=strings[name_index & 255], import_records=records))
            cursor += length
        detail['import_libraries'] = libraries
    report = dict(container=str(args.container), container_magic=header[:4].decode(),
                  container_sha256=hashlib.sha256(header).hexdigest(),
                  title_id=f'{be32(0x360):08X}', parts=count, source_bytes=sum(sizes),
                  enhanced_gdf=enhanced, start_data_block=start,
                  hash_tree_verified=False, extracted_xex=detail, entries=entries)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps({k: v for k, v in report.items() if k != 'entries'}, indent=2))
    print(f'Inventory: {len(entries)} entries')
    for h in handles:
        h.close()


if __name__ == '__main__':
    main()
