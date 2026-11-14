#!/usr/bin/env python3
"""Verify (and optionally repair) the PONGFAT1 CRC32 in a Pong boot image.

PongBootMemoryDxe.Publish() computes a zlib CRC32 over the *whole* FAT region
and compares it with the little-endian u32 at PONGFAT1+24. On mismatch it just
does `continue`: the payload is silently discarded, the device hangs with no
USB enumeration, and there is no diagnostic on any console. That makes a
broken CRC look exactly like a broken kernel.

PONGFAT1 header layout (little endian), from PongInitrd.h / PongBootMemoryDxe.c:

    +0      8 bytes   magic "PONGFAT1"
    +16     u64       FatBytes, size of the region covered by the CRC
    +24     u32       stored CRC32
    +4096             start of the FAT (first byte covered by the CRC)

Everything inside the FAT -- kernel.macho, boot args, ADT, initrd -- is
covered. Any byte patched there invalidates the CRC. Run this as a preflight
on every image before `fastboot boot`.

Usage:
    python check-pongfat-crc.py IMAGE [IMAGE ...]     # verify; exit 1 on mismatch
    python check-pongfat-crc.py --fix IMAGE          # recompute and write back
    python check-pongfat-crc.py --json IMAGE         # machine-readable result
"""
import argparse
import hashlib
import json
import struct
import sys
import zlib
from pathlib import Path

MAGIC = b"PONGFAT1"
HEADER_BYTES = 4096


def inspect(path: Path) -> dict:
    data = path.read_bytes()
    head = data.find(MAGIC)
    if head < 0:
        raise ValueError(f"{path.name}: PONGFAT1 magic not found")
    if data.find(MAGIC, head + 1) >= 0:
        raise ValueError(f"{path.name}: more than one PONGFAT1 header")
    fat_bytes = struct.unpack_from("<Q", data, head + 16)[0]
    fat_start = head + HEADER_BYTES
    if fat_bytes == 0 or fat_start + fat_bytes > len(data):
        raise ValueError(
            f"{path.name}: implausible FatBytes={fat_bytes} "
            f"(image {len(data)} bytes, FAT starts at {fat_start})"
        )
    stored = struct.unpack_from("<I", data, head + 24)[0]
    calc = zlib.crc32(memoryview(data)[fat_start:fat_start + fat_bytes]) & 0xFFFFFFFF
    return {
        "path": str(path),
        "name": path.name,
        "image_bytes": len(data),
        "header_offset": head,
        "fat_bytes": fat_bytes,
        "stored_crc": f"{stored:08x}",
        "calc_crc": f"{calc:08x}",
        "ok": stored == calc,
        "sha256": hashlib.sha256(data).hexdigest(),
        "_data": data,
        "_crc_offset": head + 24,
        "_calc": calc,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("images", nargs="+", type=Path)
    ap.add_argument("--fix", action="store_true", help="rewrite the stored CRC in place")
    ap.add_argument("--json", action="store_true", help="emit one JSON object per image")
    ap.add_argument("--quiet", action="store_true", help="only report failures")
    args = ap.parse_args()

    bad = 0
    for path in args.images:
        try:
            r = inspect(path)
        except (OSError, ValueError) as exc:
            print(f"ERROR {exc}", file=sys.stderr)
            bad += 1
            continue
        if args.fix and not r["ok"]:
            data = bytearray(r["_data"])
            struct.pack_into("<I", data, r["_crc_offset"], r["_calc"])
            path.write_bytes(bytes(data))
            r["stored_crc"] = r["calc_crc"]
            r["ok"] = True
            r["sha256"] = hashlib.sha256(bytes(data)).hexdigest()
            r["fixed"] = True
        if args.json:
            print(json.dumps({k: v for k, v in r.items() if not k.startswith("_")}))
        elif not args.quiet or not r["ok"]:
            state = "OK" if r["ok"] else "CRC BROKEN"
            print(
                f"{r['name']:<40} {state:<11} stored={r['stored_crc']} "
                f"calc={r['calc_crc']} fat={r['fat_bytes']} "
                f"sha256={r['sha256'][:16]}"
            )
        if not r["ok"]:
            bad += 1
    if bad and not args.fix:
        print(
            f"{bad} image(s) would be silently dropped by PongBootMemoryDxe. "
            "Rebuild with patch-boot-image.py or re-run with --fix.",
            file=sys.stderr,
        )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
