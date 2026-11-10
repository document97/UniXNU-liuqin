#!/usr/bin/env python3
"""Rebuild a signed Qualcomm PIL image (.mbn) from its split form (.mdt + .bNN).

The stock vendor partition ships liuqin's ADSP as adsp.mdt plus adsp.b00..b25;
drivers/PongPower loads one file, /pong/firmware/adsp.mbn. Each .bNN is program
header NN's file bytes, so the .mbn is every segment written at its p_offset.
Nothing is re-signed or altered: the hash segment is copied as it is, and every
segment's size is checked against its program header. A segment with no file
bytes (p_filesz 0, typically a BSS) has no .bNN and is skipped.

usage: merge-qcom-mdt.py DIR/NAME.mdt OUT.mbn
"""
import struct
import sys
from pathlib import Path


def headers(mdt):
    if mdt[:4] != b'\x7fELF' or mdt[5] != 1:
        raise SystemExit('not a little-endian ELF')
    if mdt[4] == 1:
        phoff, = struct.unpack_from('<I', mdt, 28)
        size, count = struct.unpack_from('<HH', mdt, 42)
        fmt, fields = '<8I', ('type', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'flags', 'align')
    elif mdt[4] == 2:
        phoff, = struct.unpack_from('<Q', mdt, 32)
        size, count = struct.unpack_from('<HH', mdt, 54)
        fmt, fields = '<IIQQQQQQ', ('type', 'flags', 'offset', 'vaddr', 'paddr', 'filesz', 'memsz', 'align')
    else:
        raise SystemExit('unknown ELF class')
    if size != struct.calcsize(fmt) or phoff + size * count > len(mdt):
        raise SystemExit('program headers lie outside the .mdt')
    return [dict(zip(fields, struct.unpack_from(fmt, mdt, phoff + i * size))) for i in range(count)]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    mdt_path, out = Path(sys.argv[1]), Path(sys.argv[2])
    mdt = mdt_path.read_bytes()
    phdrs = headers(mdt)
    end = max(p['offset'] + p['filesz'] for p in phdrs)
    if end > 64 << 20:
        raise SystemExit(f'image would be {end} bytes; the kernel loader takes at most 64 MiB')
    image = bytearray(end)
    for i, p in enumerate(phdrs):
        if not p['filesz']:
            continue
        part = mdt_path.with_suffix('.b%02d' % i)
        if not part.is_file():
            raise SystemExit(f'{part.name} missing for program header {i} ({p["filesz"]} bytes)')
        data = part.read_bytes()
        if len(data) != p['filesz']:
            raise SystemExit(f'{part.name} is {len(data)} bytes, program header {i} says {p["filesz"]}')
        image[p['offset']:p['offset'] + len(data)] = data
    # The .mdt is the ELF headers followed by the hash segment: both must match what was merged.
    if image[:phdrs[0]['filesz']] != mdt[:phdrs[0]['filesz']]:
        raise SystemExit('.b00 does not match the headers in the .mdt')
    hashes = [p for p in phdrs if (p['flags'] >> 24) & 7 == 2]
    if len(hashes) != 1:
        raise SystemExit(f'{len(hashes)} hash segments; expected exactly one')
    h = hashes[0]
    tail = mdt[phdrs[0]['filesz']:]
    if tail and tail != bytes(image[h['offset']:h['offset'] + h['filesz']]):
        raise SystemExit('the hash segment differs from the one in the .mdt')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(image)
    print(f'{out.name}: {len(image)} bytes from {sum(1 for p in phdrs if p["filesz"])} segments')


if __name__ == '__main__':
    main()
