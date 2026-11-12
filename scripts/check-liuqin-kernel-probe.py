#!/usr/bin/env python3
"""Check that this kernel is one scripts/patch-xnu-entry-probe.py can patch.

The payload step runs `patch-xnu-entry-probe.py --panic-screen` when board.toml
sets [payload] panic_screen. That script rebases its sites from the Mach-O entry
point, resolves _start_first_cpu, _panic and _vinfo from the symbol table, and
then requires three original instructions at fixed offsets from the entry. It
also requires the cave it links into to be zero fill: run 43184148347 failed
after the kernel had linked with "Padding at 0xfffffff007fe2518 is not empty",
because this kernel's cave is NOP fill (0xd5031f20 words), which an earlier
version of this check wrongly accepted.

With --board the check is skipped when [payload] panic_screen is not true, since
the payload step then never calls the probe script.

Usage:
    python3 scripts/check-liuqin-kernel-probe.py <kernel.macho> [--board board.toml]
"""
import struct
import sys
import tomllib
from pathlib import Path

ENTRY_VA = 0xFFFFFFF00B9D0488      # the reference LC_UNIXTHREAD pc of the probe script
CAVE_FIRST = 0xFFFFFFF00B9D0500    # its stage-1 cave; CAVE_SIZE below covers one stage
CAVE_SIZE = 0x60
NOP = 0xD503201F


def load(path):
    data = open(path, 'rb').read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('%s is not a 64-bit little-endian Mach-O' % path)
    at = 32
    segments = []
    entry = None
    symtab = None
    for _ in range(ncmds):
        command, size = struct.unpack_from('<2I', data, at)
        if command == 0x19:  # LC_SEGMENT_64
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<4Q', data, at + 24)
            segments.append((vmaddr, vmsize, fileoff, filesize))
        elif command == 0x05:  # LC_UNIXTHREAD
            entry = struct.unpack_from('<Q', data, at + 16 + 32 * 8)[0]
        elif command == 0x02:  # LC_SYMTAB
            symtab = struct.unpack_from('<4I', data, at + 8)
        at += size
    return data, segments, entry, symtab


def panic_screen_requested(board):
    if board is None:
        return True
    return bool(tomllib.loads(Path(board).read_text()).get('payload', {}).get('panic_screen', False))


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    kernel = argv[1]
    board = None
    if '--board' in argv:
        board = argv[argv.index('--board') + 1]
    if not panic_screen_requested(board):
        print('%s: [payload] panic_screen is not true, the payload step will not call '
              'patch-xnu-entry-probe.py; nothing to check' % (board or 'board.toml'))
        return 0
    data, segments, entry, symtab = load(kernel)

    def offset(va):
        for vmaddr, vmsize, fileoff, filesize in segments:
            if vmaddr <= va < vmaddr + min(vmsize, filesize):
                return fileoff + (va - vmaddr)
        raise SystemExit('%#x is not in a file-backed segment' % va)

    def word(va):
        return struct.unpack_from('<I', data, offset(va))[0]

    symbols = {}
    if symtab:
        symoff, nsyms, stroff, _ = symtab
        for index in range(nsyms):
            strx, ntype, _, _, value = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
            if (ntype & 0x0E) not in (0x0E, 0x0C):  # N_SECT, N_ABS
                continue
            end = data.index(b'\0', stroff + strx)
            symbols[data[stroff + strx:end].decode(errors='replace')] = value
    problems = []
    for name in ('_start_first_cpu', '_panic', '_vinfo'):
        if name not in symbols:
            problems.append('%s is not in the symbol table; the probe script needs it to rebase' % name)
    print('entry %#x, %d symbols, delta from the reference entry %+#x'
          % (entry, len(symbols), entry - ENTRY_VA))
    if problems:
        for problem in problems:
            print('  ' + problem)
        return 1

    first = symbols['_start_first_cpu']
    target = (first - entry) // 4
    want = 0x14000000 | (target & 0x3FFFFFF)
    if word(entry) != want:
        problems.append('the entry at %#x is %#010x, not a branch to _start_first_cpu (%#010x)'
                        % (entry, word(entry), want))
    else:
        print('  entry branches to _start_first_cpu at %#x' % first)

    sequence = None
    for va in range(first, first + 0x1000, 4):
        if word(va) == 0xD5181000 and word(va + 4) == 0xD5033FDF \
                and word(va + 0x20) == 0xD2A00600 and word(va + 0x24) == 0xD5181040:
            sequence = va
            break
    if sequence is None:
        problems.append('no SCTLR_EL1/ISB/CPACR_EL1 sequence within %#x..%#x of _start_first_cpu'
                        % (first, first + 0x1000))
    else:
        print('  SCTLR/ISB/CPACR sequence at %#x' % sequence)

    # --panic-screen links its code into the cave after the entry, which the
    # probe script requires to be zero fill; a NOP sled is not accepted.
    cave = CAVE_FIRST + (entry - ENTRY_VA)
    body = data[offset(cave):offset(cave) + CAVE_SIZE]
    words = struct.unpack('<%dI' % (CAVE_SIZE // 4), body)
    if any(words):
        filler = 'NOP fill' if all(w in (0, NOP) for w in words) else 'kernel code'
        problems.append('the %d-byte cave at %#x is %s, so patch-xnu-entry-probe.py stops with '
                        '"Padding at %#x is not empty"' % (CAVE_SIZE, cave, filler, cave))
    else:
        print('  cave at %#x is zero fill' % cave)

    for problem in problems:
        print('  ' + problem)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
