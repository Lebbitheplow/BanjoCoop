#!/usr/bin/env python3
"""Verify a Banjo-Kazooie ROM is the revision BanjoRecomp needs, and normalise byte order.

BanjoRecomp targets NTSC-U rev0 only (it ships banjo.us.rev0.toml and bk.us.rev0.*syms.toml
and nothing else). Rev A / v1.1 will not work.

Usage:
    scripts/check_rom.py <rom>              # inspect only
    scripts/check_rom.py <rom> -o out.z64   # inspect and write a big-endian .z64
"""

import argparse
import hashlib
import sys

# sha1 of the raw rev0 cartridge dump — this is the ROM BanjoRecomp's BUILDING.md tells you
# to feed into the decompressor, not the decompressed result. 16 MiB, standard cart size.
RAW_REV0_SHA1 = "1fe1632098865f639e22c11b9a81ee8f29c75d7a"

Z64, V64, N64 = b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80"


def to_z64(data: bytes) -> tuple[bytes, str]:
    """Return (big-endian data, detected original format)."""
    magic = data[:4]
    if magic == Z64:
        return data, "z64 (big-endian)"
    if magic == V64:
        out = bytearray(len(data))
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out), "v64 (byteswapped)"
    if magic == N64:
        out = bytearray(len(data))
        for i in range(0, len(data), 4):
            out[i : i + 4] = data[i : i + 4][::-1]
        return bytes(out), "n64 (little-endian)"
    raise SystemExit(f"Not an N64 ROM: unrecognised magic {magic.hex()}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("-o", "--output", help="write normalised big-endian .z64 here")
    args = ap.parse_args()

    with open(args.rom, "rb") as f:
        raw = f.read()

    data, fmt = to_z64(raw)

    name = data[0x20:0x34].decode("ascii", errors="replace").strip()
    cart = data[0x3B:0x3F].decode("ascii", errors="replace")
    version = data[0x3F]
    sha1 = hashlib.sha1(data).hexdigest()

    print(f"file          : {args.rom}")
    print(f"size          : {len(raw):,} bytes")
    print(f"format        : {fmt}")
    print(f"internal name : {name}")
    print(f"cartridge ID  : {cart}")
    print(f"version byte  : 0x{version:02X}")
    print(f"sha1 (as z64) : {sha1}")
    print()

    ok = True
    if cart != "NBKE":
        print(f"FAIL  cartridge ID is {cart!r}, expected 'NBKE' (Banjo-Kazooie NTSC-U)")
        ok = False
    if version != 0x00:
        rev = "Rev A / v1.1" if version == 0x01 else f"revision 0x{version:02X}"
        print(f"FAIL  this is {rev}. BanjoRecomp needs rev0 (version byte 0x00).")
        print("      Look for the No-Intro dump named exactly 'Banjo-Kazooie (USA)' —")
        print("      with no '(Rev A)' suffix.")
        ok = False
    if sha1 == RAW_REV0_SHA1:
        print("PASS  exact match for the raw rev0 dump BanjoRecomp expects.")
        print("      Next: decompress it with bk_rom_compressor and place the result in")
        print("      the BanjoRecomp repo root as banjo.us.v10.decompressed.z64")
    elif ok:
        print("PASS  correct revision, but sha1 does not match the known-good rev0 dump.")
        print(f"      expected {RAW_REV0_SHA1}")
        print("      Likely an overdumped/trimmed/patched copy — proceed with caution.")
        print("      Next: decompress with bk_rom_compressor ->")
        print("      banjo.us.v10.decompressed.z64")

    if args.output:
        with open(args.output, "wb") as f:
            f.write(data)
        print(f"\nwrote big-endian copy: {args.output}")

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
