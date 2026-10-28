"""Create a private Xbox Coalesced.ini copy with a lower render screen percentage.

Diagnostic A/B only. This changes the [SystemSettings] ScreenPercentage value,
leaving all shadow settings and the user's original game file untouched.
"""

import argparse
import hashlib
from pathlib import Path

from coalesced_shadow_probe import pack, unpack


def resize(data: bytes, percent: int) -> bytes:
    if percent not in (50, 75):
        raise ValueError("only the 50% and 75% diagnostic variants are supported")
    count, records = unpack(data)
    output = []
    changed = 0
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
            old = b"\r\nScreenPercentage=100.000000\r\n"
            if section.count(old) != 1:
                raise ValueError("expected exactly one 100% ScreenPercentage key")
            new = f"\r\nScreenPercentage={percent}.000000\r\n".encode("ascii")
            body = body[:start] + section.replace(old, new) + body[end:]
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
    parser.add_argument("--percent", type=int, choices=(50, 75), default=75)
    args = parser.parse_args()
    if args.source.resolve() == args.destination.resolve():
        parser.error("destination must differ from source")
    original = args.source.read_bytes()
    if pack(*unpack(original)) != original:
        raise ValueError("source round-trip failed")
    changed = resize(original, args.percent)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(changed)
    print(f"source_sha256={hashlib.sha256(original).hexdigest()}")
    print(f"probe_sha256={hashlib.sha256(changed).hexdigest()}")
    print(f"size={len(original)}->{len(changed)} screen_percent={args.percent}")
    print("NOTE: game must be checked to confirm it honors this setting")


if __name__ == "__main__":
    main()
