"""Make a private A/B copy of the Xbox Coalesced.ini with global dynamic shadows off.

This is a diagnostic only. It does not distinguish Faith's shadow from world shadows.
The game also needs the separate Coalesced hash-check patch to load modified data.
"""

import argparse
import hashlib
import struct
from pathlib import Path


def unpack(data: bytes):
    if len(data) < 4:
        raise ValueError("truncated Coalesced header")
    count = struct.unpack_from(">I", data)[0]
    pos = 4
    records = []
    while pos < len(data):
        if pos + 4 > len(data):
            raise ValueError("truncated name length")
        name_len = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if not 1 <= name_len <= 512 or pos + name_len + 4 > len(data):
            raise ValueError("invalid name length")
        name = data[pos : pos + name_len]
        pos += name_len
        body_len = struct.unpack_from(">I", data, pos)[0]
        pos += 4
        if body_len > len(data) - pos:
            raise ValueError("invalid body length")
        body = data[pos : pos + body_len]
        pos += body_len
        records.append((name, body))
    if count != len(records) * 2:
        raise ValueError(f"unexpected header count {count} for {len(records)} records")
    return count, records


def pack(count, records):
    result = bytearray(struct.pack(">I", count))
    for name, body in records:
        result += struct.pack(">I", len(name)) + name
        result += struct.pack(">I", len(body)) + body
    return bytes(result)


def shadow_off(data: bytes):
    count, records = unpack(data)
    changed = 0
    output = []
    for name, body in records:
        if name.rstrip(b"\0").lower().endswith(b"xe-tdengine.ini"):
            marker = b"[SystemSettings]\r\n"
            start = body.find(marker)
            if start < 0 or body.find(marker, start + 1) >= 0:
                raise ValueError("expected exactly one [SystemSettings] section")
            end = body.find(b"\r\n[", start + len(marker))
            if end < 0:
                end = len(body)
            section = body[start:end]
            old = b"\r\nDynamicShadows=True\r\n"
            if section.count(old) != 1:
                raise ValueError("expected exactly one enabled DynamicShadows key")
            section = section.replace(old, b"\r\nDynamicShadows=False\r\n")
            body = body[:start] + section + body[end:]
            changed += 1
        output.append((name, body))
    if changed != 1:
        raise ValueError(f"expected one engine record, found {changed}")
    result = pack(count, output)
    if unpack(result)[1] != output:
        raise ValueError("round-trip failed")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        parser.error("destination must differ from source")
    original = args.source.read_bytes()
    if pack(*unpack(original)) != original:
        raise ValueError("source round-trip failed")
    changed = shadow_off(original)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(changed)
    print(f"source_sha256={hashlib.sha256(original).hexdigest()}")
    print(f"probe_sha256={hashlib.sha256(changed).hexdigest()}")
    print(f"size={len(original)}->{len(changed)}")
    print("NOTE: diagnostic global switch; Faith's shadow may disappear too")


if __name__ == "__main__":
    main()
