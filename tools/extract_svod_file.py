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
    ap.add_argument('--extract-path', required=True)
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
    args.private.mkdir(parents=True,exist_ok=True)
    (args.private / 'inventory.txt').write_text('\n'.join(sorted(e['path'].lower() for e in entries))+'\n',encoding='ascii')
    match = [e for e in entries if e['path'].lower() == args.extract_path.replace('\\','/').lower() and not e['directory']]
    assert len(match) == 1, 'File absent or ambiguous'
    item = match[0]
    dest = args.private / item['path'].lower()
    dest.parent.mkdir(parents=True,exist_ok=True)
    data = read(item['block'],0,item['size'])
    if dest.exists(): assert dest.read_bytes()==data, 'Existing file differs'
    else:
        with dest.open('xb') as output: output.write(data)
    args.report.write_text(json.dumps({'path':item['path'],'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()},indent=2))
    print('extracted',item['path'],len(data),'bytes')
    for handle in handles: handle.close()

if __name__ == '__main__': main()
