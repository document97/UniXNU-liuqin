#!/usr/bin/env python3
"""Parse a GPT blob: primary header at LBA 1, header-only extract, or gpt_both0.bin.

Sector size and header offset are discovered by validating the header CRC32 and the
partition-entry-array CRC32, so a 512-byte-sector header, a 4 KiB-sector header and a
bare "header + entries" file all parse without being told which they are.

    parse-gpt.py gpt.bin                      # table + disk extent
    parse-gpt.py gpt.bin --fastboot fastboot-getvar.txt
    parse-gpt.py gpt.bin --json out.json --name userdata
"""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

SIG = b"EFI PART"
LBA_SIZE_HINTS = (512, 4096)


class Header:
    def __init__(self, buf: bytes, off: int, sector: int, size: int):
        self.off = off
        self.sector = sector
        self.size = size
        self.revision, self.header_size = struct.unpack_from("<II", buf, off + 8)
        self.header_crc, = struct.unpack_from("<I", buf, off + 16)
        self.my_lba, self.alt_lba = struct.unpack_from("<QQ", buf, off + 24)
        self.first_usable, self.last_usable = struct.unpack_from("<QQ", buf, off + 40)
        self.disk_guid = buf[off + 56:off + 72]
        self.entries_lba, = struct.unpack_from("<Q", buf, off + 72)
        self.entries_count, self.entry_size, self.entries_crc = struct.unpack_from(
            "<III", buf, off + 80)

    def guid_text(self) -> str:
        b = self.disk_guid
        d1, d2, d3 = struct.unpack_from("<IHH", b, 0)
        return "%08X-%04X-%04X-%s-%s" % (
            d1, d2, d3, b[8:10].hex().upper(), b[10:16].hex().upper())

    def describe(self) -> str:
        return ("header@%d sector=%d my_lba=%d alt_lba=%d entries_lba=%d "
                "count=%d entry_size=%d first_usable=%d last_usable=%d disk_guid=%s"
                % (self.off, self.sector, self.my_lba, self.alt_lba, self.entries_lba,
                   self.entries_count, self.entry_size, self.first_usable,
                   self.last_usable, self.guid_text()))


def validate_header(buf: bytes, off: int, sector: int):
    if off < 0 or off + 92 > len(buf) or buf[off:off + 8] != SIG:
        return None
    h = bytearray(buf[off:off + 512])
    header_size, = struct.unpack_from("<I", h, 12)
    if header_size < 92 or header_size > 512 or off + header_size > len(buf):
        return None
    want, = struct.unpack_from("<I", h, 16)
    probe = bytearray(h[:header_size])
    probe[16:20] = b"\0\0\0\0"
    if zlib.crc32(bytes(probe)) & 0xFFFFFFFF != want:
        return None
    hdr = Header(buf, off, sector, header_size)
    if hdr.entry_size < 128 or hdr.entry_size % 8 or hdr.entries_count > 4096:
        return None
    ent_off = off - (hdr.my_lba - hdr.entries_lba) * sector
    ent_len = hdr.entries_count * hdr.entry_size
    if ent_off < 0 or ent_off + ent_len > len(buf):
        return None
    entries = buf[ent_off:ent_off + ent_len]
    if zlib.crc32(entries) & 0xFFFFFFFF != hdr.entries_crc:
        return None
    return hdr, entries


def find_header(buf: bytes):
    """Every offset where a CRC-valid GPT header sits, with the sector size that fits."""
    found = []
    start = 0
    while True:
        off = buf.find(SIG, start)
        if off < 0:
            break
        start = off + 1
        if off % 8:
            continue
        for sector in LBA_SIZE_HINTS:
            got = validate_header(buf, off, sector)
            if got:
                found.append(got)
                break
    return found


def partitions(buf: bytes, hdr: Header, entries: bytes):
    rows = []
    for index in range(hdr.entries_count):
        base = index * hdr.entry_size
        etype = entries[base:base + 16]
        if etype == b"\0" * 16:
            continue
        first, last, attrs = struct.unpack_from("<QQQ", entries, base + 32)
        raw = entries[base + 56:base + 56 + 72]
        name = raw.decode("utf-16-le", "replace").split("\0")[0]
        rows.append(dict(index=index, name=name, first_lba=first, last_lba=last,
                         attributes=attrs, size_bytes=(last - first + 1) * hdr.sector,
                         type_guid=etype.hex()))
    return rows


def read_fastboot(path: Path):
    import re
    text = path.read_text(encoding="utf-8", errors="replace")
    out = {}
    for name, value in re.findall(r"partition-size:([^:\s]+):\s*(0x[0-9A-Fa-f]+)", text):
        out[name] = int(value, 16)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, help="GPT blob, disk-head dump, or gpt_both0.bin")
    ap.add_argument("--fastboot", type=Path,
                    help="fastboot getvar all output to cross-check sizes against")
    ap.add_argument("--json", type=Path, help="write the parsed table as JSON")
    ap.add_argument("--name", help="print only this partition, as key=value lines")
    ap.add_argument("--prefer", type=int, default=None,
                    help="choose the header at this byte offset when several validate")
    args = ap.parse_args()

    buf = args.image.read_bytes()
    print("file: %s (%d B)" % (args.image, len(buf)))
    found = find_header(buf)
    if not found:
        raise SystemExit("no CRC-valid GPT header in this file")

    for candidate in found:
        print("  candidate:", candidate[0].describe())
    if args.prefer is not None:
        chosen = [c for c in found if c[0].off == args.prefer]
        if not chosen:
            raise SystemExit("no header at the requested offset")
    else:
        chosen = found
    hdr, entries = chosen[0]
    rows = partitions(buf, hdr, entries)
    print()
    print("disk: %d sectors x %d B = %d B (%.3f GiB), backup header at LBA %d"
          % (hdr.alt_lba + 1, hdr.sector, (hdr.alt_lba + 1) * hdr.sector,
             (hdr.alt_lba + 1) * hdr.sector / 1024 ** 3, hdr.alt_lba))
    print("usable: LBA %d..%d" % (hdr.first_usable, hdr.last_usable))
    print("partitions: %d" % len(rows))
    print()

    want = read_fastboot(args.fastboot) if args.fastboot else None
    print("%-4s %-22s %14s %14s %14s %14s %s"
          % ("idx", "name", "first_lba", "last_lba", "size_bytes", "size_GiB", "fastboot"))
    mismatch = 0
    for row in rows:
        note = ""
        if want is not None:
            if row["name"] not in want:
                note = "absent"
            elif want[row["name"]] != row["size_bytes"]:
                note = "DIFF (got 0x%X)" % want[row["name"]]
                mismatch += 1
            else:
                note = "ok"
        print("%-4d %-22s %14d %14d %14d %14.3f %s"
              % (row["index"], row["name"], row["first_lba"], row["last_lba"],
                 row["size_bytes"], row["size_bytes"] / 1024 ** 3, note))
    if want is not None:
        missing = sorted(set(want) - {r["name"] for r in rows})
        print()
        print("size mismatches: %d" % mismatch)
        if missing:
            print("in fastboot but not in GPT: %s" % ", ".join(missing))
        if not missing and not mismatch:
            print("every fastboot partition matches this GPT byte for byte")

    if args.name:
        for row in rows:
            if row["name"] == args.name:
                print()
                print("partition=%s" % row["name"])
                print("first_lba=%d" % row["first_lba"])
                print("last_lba=%d" % row["last_lba"])
                print("sectors=%d" % (row["last_lba"] - row["first_lba"] + 1))
                print("first_lba_hex=0x%X" % row["first_lba"])
                print("last_lba_hex=0x%X" % row["last_lba"])
                print("lba_bytes=%d" % hdr.sector)

    if args.json:
        args.json.write_text(json.dumps(dict(
            file=str(args.image), sector=hdr.sector, disk_guid=hdr.guid_text(),
            disk_sectors=hdr.alt_lba + 1, disk_bytes=(hdr.alt_lba + 1) * hdr.sector,
            first_usable=hdr.first_usable, last_usable=hdr.last_usable,
            partitions=rows), indent=2) + "\n")
        print("\nwrote %s" % args.json)


if __name__ == "__main__":
    sys.exit(main())
