"""Puts the personal parts into the prebuilt app and signs it, with nothing but Python.

  python -I apk_pack.py --shell RunnerRecompiled-shell.apk --lib libmepc.so --deps <folder> --key <file> --out <apk>
  python -I apk_pack.py --make-shell <full apk> --out RunnerRecompiled-shell.apk

The shell is the app as published: no game code, no runtime DLLs. This adds lib/arm64-v8a/libmepc.so
(the user's recompiled game) and assets/pcdeps/*.dll (their Visual C++ and PhysX), aligns stored
entries as zipalign does (16 KB for libraries, 4 bytes for the rest) and signs with APK Signature
Scheme v2 (Android 7 and newer; the app needs 8). The key is the user's own: a PKCS#12 file written
by Java's keytool (password "android", as the earlier builds made it) so that new builds install over
old ones, or a key this file makes on first use.
"""
import argparse
import base64
import hashlib
import json
import os
import secrets
import struct
import sys
import time
import zipfile
import zlib

# ---- DER ----


def der_read(b, p=0):
    """(tag, value start, value end)"""
    tag, length = b[p], b[p + 1]
    p += 2
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(b[p:p + n], "big")
        p += n
    return tag, p, p + length


def der_children(b, start, end):
    out, p = [], start
    while p < end:
        tag, s, e = der_read(b, p)
        out.append((tag, s, e))
        p = e
    return out


def der(tag, content):
    n = len(content)
    if n < 0x80:
        return bytes([tag, n]) + content
    raw = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([tag, 0x80 | len(raw)]) + raw + content


def der_int(v):
    raw = v.to_bytes(v.bit_length() // 8 + 1, "big")
    return der(0x02, raw)


def der_oid(text):
    parts = [int(x) for x in text.split(".")]
    out = bytearray([parts[0] * 40 + parts[1]])
    for v in parts[2:]:
        chunk = [v & 0x7F]
        v >>= 7
        while v:
            chunk.append(0x80 | (v & 0x7F))
            v >>= 7
        out += bytes(reversed(chunk))
    return der(0x06, bytes(out))


def seq(*items):
    return der(0x30, b"".join(items))


# ---- AES-256-CBC decryption (for the key file) ----
_SBOX = [0] * 256
_INV = [0] * 256


def _aes_tables():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (((q << 1) | (q >> 7)) & 0xFF) ^ (((q << 2) | (q >> 6)) & 0xFF) ^ (((q << 3) | (q >> 5)) & 0xFF) ^ (((q << 4) | (q >> 4)) & 0xFF)
        _SBOX[p] = x ^ 0x63
        if p == 1:
            break
    _SBOX[0] = 0x63
    for i, v in enumerate(_SBOX):
        _INV[v] = i


_aes_tables()


def _xt(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xt(a)
        b >>= 1
    return r


def _expand(key):
    words = [list(key[i:i + 4]) for i in range(0, len(key), 4)]
    nk = len(key) // 4
    rounds = nk + 6
    rcon = 1
    for i in range(nk, 4 * (rounds + 1)):
        t = list(words[-1])
        if i % nk == 0:
            t = [_SBOX[x] for x in t[1:] + t[:1]]
            t[0] ^= rcon
            rcon = _xt(rcon)
        elif nk > 6 and i % nk == 4:
            t = [_SBOX[x] for x in t]
        words.append([a ^ b for a, b in zip(words[i - nk], t)])
    return [sum(words[r * 4:r * 4 + 4], []) for r in range(rounds + 1)]


def _decrypt_block(keys, block):
    s = [a ^ b for a, b in zip(block, keys[-1])]
    for r in range(len(keys) - 2, -1, -1):
        s = [s[((i // 4 - i % 4) % 4) * 4 + i % 4] for i in range(16)]  # inverse ShiftRows (column-major state)
        s = [_INV[x] for x in s]
        s = [a ^ b for a, b in zip(s, keys[r])]
        if r:
            t = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                t += [_mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                      _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                      _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                      _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14)]
            s = t
    return bytes(s)


def aes_cbc_decrypt(key, iv, data):
    keys = _expand(key)
    out, prev = bytearray(), iv
    for i in range(0, len(data), 16):
        block = data[i:i + 16]
        out += bytes(a ^ b for a, b in zip(_decrypt_block(keys, block), prev))
        prev = block
    return bytes(out[:-out[-1]])


# ---- PKCS#12 (keytool) ----
OID_PBES2, OID_PBKDF2 = "2a864886f70d01050d", "2a864886f70d01050c"
OID_AES256, OID_AES128 = "60864801650304012a", "608648016503040102"
OID_HMAC_SHA256, OID_HMAC_SHA1 = "2a864886f70d0209", "2a864886f70d0207"
OID_DATA, OID_ENCRYPTED = "2a864886f70d010701", "2a864886f70d010706"
OID_SHROUDED_KEY, OID_CERT_BAG = "2a864886f70d010c0a0102", "2a864886f70d010c0a0103"


def _pbes2(b, params, encrypted, password):
    kdf, scheme = der_children(b, params[1], params[2])
    kdf_oid, kdf_params = der_children(b, kdf[1], kdf[2])
    assert b[kdf_oid[1]:kdf_oid[2]].hex() == OID_PBKDF2, "only PBKDF2 key files are supported"
    fields = der_children(b, kdf_params[1], kdf_params[2])
    salt = b[fields[0][1]:fields[0][2]]
    rounds = int.from_bytes(b[fields[1][1]:fields[1][2]], "big")
    prf = "sha1"
    for f in fields[2:]:
        if f[0] == 0x30:
            prf = "sha256" if b[der_children(b, f[1], f[2])[0][1]:der_children(b, f[1], f[2])[0][2]].hex() == OID_HMAC_SHA256 else "sha1"
    s_oid, s_iv = der_children(b, scheme[1], scheme[2])
    cipher = b[s_oid[1]:s_oid[2]].hex()
    size = {OID_AES256: 32, OID_AES128: 16}.get(cipher)
    assert size, "only AES key files are supported"
    key = hashlib.pbkdf2_hmac(prf, password, salt, rounds, size)
    return aes_cbc_decrypt(key, b[s_iv[1]:s_iv[2]], encrypted)


def read_p12(data, password=b"android"):
    """(RSA n, e, d, certificate DER) from a keytool PKCS#12 file."""
    pfx = der_children(data, *der_read(data)[1:])
    auth = der_children(data, pfx[1][1], pfx[1][2])
    content = der_children(data, auth[1][1], auth[1][2])[0]
    safe = der_children(data, *der_read(data, content[1])[1:])
    key = cert = None
    for info in safe:
        oid, body = der_children(data, info[1], info[2])
        kind = data[oid[1]:oid[2]].hex()
        if kind == OID_DATA:
            octets = der_children(data, body[1], body[2])[0]
            bags_raw = data[octets[1]:octets[2]]
        else:
            encrypted_data = der_children(data, body[1], body[2])[0]
            content_info = der_children(data, encrypted_data[1], encrypted_data[2])[1]
            _, algo, payload = der_children(data, content_info[1], content_info[2])
            algo_oid, algo_params = der_children(data, algo[1], algo[2])
            assert data[algo_oid[1]:algo_oid[2]].hex() == OID_PBES2
            bags_raw = _pbes2(data, algo_params, data[payload[1]:payload[2]], password)
        for bag in der_children(bags_raw, *der_read(bags_raw)[1:]):
            bag_oid, bag_value = der_children(bags_raw, bag[1], bag[2])[:2]
            bag_kind = bags_raw[bag_oid[1]:bag_oid[2]].hex()
            inner = der_children(bags_raw, bag_value[1], bag_value[2])[0]
            if bag_kind == OID_SHROUDED_KEY:
                algo, octets = der_children(bags_raw, inner[1], inner[2])
                algo_oid, algo_params = der_children(bags_raw, algo[1], algo[2])
                pkcs8 = _pbes2(bags_raw, algo_params, bags_raw[octets[1]:octets[2]], password)
                fields = der_children(pkcs8, *der_read(pkcs8)[1:])
                rsa = pkcs8[fields[2][1]:fields[2][2]]
                numbers = [int.from_bytes(rsa[s:e], "big") for _, s, e in der_children(rsa, *der_read(rsa)[1:])]
                key = numbers[1], numbers[2], numbers[3]
            elif bag_kind == OID_CERT_BAG:
                cert_id, cert_value = der_children(bags_raw, inner[1], inner[2])
                octets = der_children(bags_raw, cert_value[1], cert_value[2])[0]
                cert = bags_raw[octets[1]:octets[2]]
    assert key and cert, "the key file has no key or no certificate"
    return key[0], key[1], key[2], cert


# ---- a new key ----
def _prime(bits):
    while True:
        n = secrets.randbits(bits) | (3 << (bits - 2)) | 1
        if all(n % p for p in (3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47)):
            d, s = n - 1, 0
            while d % 2 == 0:
                d //= 2
                s += 1
            for _ in range(40):
                x = pow(secrets.randbelow(n - 3) + 2, d, n)
                if x in (1, n - 1):
                    continue
                for _ in range(s - 1):
                    x = pow(x, 2, n)
                    if x == n - 1:
                        break
                else:
                    break
            else:
                return n


SHA256_RSA = seq(der_oid("1.2.840.113549.1.1.11"), b"\x05\x00")


def public_key_info(n, e):
    return seq(seq(der_oid("1.2.840.113549.1.1.1"), b"\x05\x00"), der(0x03, b"\x00" + seq(der_int(n), der_int(e))))


def rsa_sign(n, d, message):
    prefix = bytes.fromhex("3031300d060960864801650304020105000420")
    digest = prefix + hashlib.sha256(message).digest()
    k = (n.bit_length() + 7) // 8
    block = b"\x00\x01" + b"\xff" * (k - len(digest) - 3) + b"\x00" + digest
    return pow(int.from_bytes(block, "big"), d, n).to_bytes(k, "big")


def new_key():
    e = 65537
    while True:
        p, q = _prime(1024), _prime(1024)
        phi = (p - 1) * (q - 1)
        if p != q and phi % e:
            break
    n, d = p * q, pow(e, -1, (p - 1) * (q - 1))
    name = seq(der(0x31, seq(der_oid("2.5.4.3"), der(0x0C, b"RunnerRecompiled personal key"))))
    now = time.gmtime()
    begin = time.strftime("%Y%m%d%H%M%SZ", now).encode()
    end = time.strftime("%Y%m%d%H%M%SZ", time.gmtime(time.time() + 30 * 365 * 86400)).encode()
    tbs = seq(der(0xA0, der_int(2)), der_int(secrets.randbits(63)), SHA256_RSA, name,
              seq(der(0x18, begin), der(0x18, end)), name, public_key_info(n, e))
    cert = seq(tbs, SHA256_RSA, der(0x03, b"\x00" + rsa_sign(n, d, tbs)))
    return n, e, d, cert


def load_key(path):
    """The user's key: a keytool PKCS#12 file, or this file's JSON (made on first use)."""
    if os.path.isfile(path):
        with open(path, "rb") as f:
            data = f.read()
        if data[:1] == b"{":
            k = json.loads(data)
            return int(k["n"], 16), int(k["e"], 16), int(k["d"], 16), base64.b64decode(k["certificate"])
        return read_p12(data)
    n, e, d, cert = new_key()
    with open(path, "w", encoding="ascii") as f:
        json.dump({"n": f"{n:x}", "e": f"{e:x}", "d": f"{d:x}", "certificate": base64.b64encode(cert).decode()}, f)
    return n, e, d, cert


# ---- ZIP and signing ----
def _entries(path):
    """(name, method, crc, compressed bytes, size, date, time) of each entry, in order."""
    out = []
    with open(path, "rb") as f, zipfile.ZipFile(path) as z:
        for info in z.infolist():
            f.seek(info.header_offset)
            local = f.read(30)
            n, m = struct.unpack_from("<HH", local, 26)
            f.seek(info.header_offset + 30 + n + m)
            raw = f.read(info.compress_size)
            out.append([info.filename, info.compress_type, info.CRC, raw, info.file_size, info.date_time])
    return out


def _dos(date_time):
    y, mo, d, h, mi, s = date_time
    return ((y - 1980) << 9) | (mo << 5) | d, (h << 11) | (mi << 5) | (s // 2)


def _write_zip(entries):
    body, central = bytearray(), bytearray()
    for name, method, crc, raw, size, date_time in entries:
        encoded = name.encode("utf-8")
        date, tme = _dos(date_time)
        align = 0
        if method == 0:
            align = 16384 if name.endswith(".so") else 4
        offset = len(body)
        pad = b""
        if align:
            start = offset + 30 + len(encoded)
            pad = b"\0" * ((-start) % align)
        body += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, 0x800, method, tme, date, crc, len(raw), size, len(encoded), len(pad))
        body += encoded + pad + raw
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, 20, 20, 0x800, method, tme, date, crc, len(raw), size,
                               len(encoded), 0, 0, 0, 0, 0, offset) + encoded
    count = len(entries)
    eocd = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, count, count, len(central), len(body), 0)
    return bytes(body), bytes(central), eocd


def _chunked(sections):
    digests = []
    for data in sections:
        for i in range(0, len(data), 1 << 20):
            chunk = data[i:i + (1 << 20)]
            digests.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(digests)) + b"".join(digests)).digest()


def _lp(b):
    return struct.pack("<I", len(b)) + b


def sign(entries, key):
    """The APK bytes with an APK Signature Scheme v2 block (RSA PKCS#1 v1.5, SHA-256)."""
    n, e, d, cert = key
    body, central, eocd = _write_zip(entries)
    digest = _chunked([body, central, eocd])
    algo = 0x0103
    signed = _lp(_lp(struct.pack("<I", algo) + _lp(digest))) + _lp(_lp(cert)) + _lp(b"")
    signature = rsa_sign(n, d, signed)
    signer = _lp(signed) + _lp(_lp(struct.pack("<I", algo) + _lp(signature))) + _lp(public_key_info(n, e))
    value = _lp(_lp(signer))
    pair = struct.pack("<QI", len(value) + 4, 0x7109871A) + value
    size = len(pair) + 8 + 16
    block = struct.pack("<Q", size) + pair + struct.pack("<Q", size) + b"APK Sig Block 42"
    eocd = eocd[:16] + struct.pack("<I", len(body) + len(block)) + eocd[20:]
    return body + block + central + eocd


def make_shell(full_apk, out):
    """The published app: the full APK without the game code and the runtime DLLs, unsigned."""
    entries = [e for e in _entries(full_apk)
               if e[0] != "lib/arm64-v8a/libmepc.so" and not e[0].startswith("assets/pcdeps/") and not e[0].startswith("META-INF/")]
    body, central, eocd = _write_zip(entries)
    with open(out, "wb") as f:
        f.write(body + central + eocd)


def pack(shell, lib, deps, key_path, out):
    entries = [e for e in _entries(shell) if not e[0].startswith("META-INF/")]
    stamp = time.localtime()[:6]

    def stored(name, path):
        with open(path, "rb") as f:
            data = f.read()
        return [name, 0, zlib.crc32(data), data, len(data), stamp]

    entries.append(stored("lib/arm64-v8a/libmepc.so", lib))
    for name in ("msvcr80.dll", "msvcp80.dll", "PhysXCore.dll", "PhysXCooking.dll"):
        path = os.path.join(deps, name)
        if not os.path.isfile(path):
            path = os.path.join(deps, "rebased", name)
        entries.append(stored(f"assets/pcdeps/{name}", path))
    apk = sign(entries, load_key(key_path))
    with open(out, "wb") as f:
        f.write(apk)
    return len(apk)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--make-shell", help="full APK to turn into the published shell")
    ap.add_argument("--shell")
    ap.add_argument("--lib")
    ap.add_argument("--deps")
    ap.add_argument("--key")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    if args.make_shell:
        make_shell(args.make_shell, args.out)
    else:
        print(f"{args.out}: {pack(args.shell, args.lib, args.deps, args.key, args.out)} bytes")


if __name__ == "__main__":
    sys.exit(main())
