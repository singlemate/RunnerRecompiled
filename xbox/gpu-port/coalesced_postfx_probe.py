"""Make a private A/B copy of Xbox Coalesced.ini with DoF and bloom disabled.

The original game file, render resolution and shadow settings are unchanged.
This is a diagnostic, not a default quality preset.
"""

import argparse
import hashlib
from pathlib import Path

from coalesced_shadow_probe import pack, unpack


def postfx_low(data: bytes) -> bytes:
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
            for key in (b"DepthOfField", b"Bloom", b"QualityBloom"):
                old = b"\r\n" + key + b"=True\r\n"
                if section.count(old) != 1:
                    raise ValueError(f"expected one enabled {key.decode()} key")
                section = section.replace(old, b"\r\n" + key + b"=False\r\n")
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
    changed = postfx_low(original)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(changed)
    print(f"source_sha256={hashlib.sha256(original).hexdigest()}")
    print(f"probe_sha256={hashlib.sha256(changed).hexdigest()}")
    print(f"size={len(original)}->{len(changed)}")


if __name__ == "__main__":
    main()
