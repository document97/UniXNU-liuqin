#!/usr/bin/env python3
"""Write the CI userdata image to the tablet's userdata, directory last.

build-pong-userdata-image.py produces exactly the bytes prepare-ipados18-ufs-layout.py
describes: a 4096-byte SHNGROOT directory at offset 0 and the OS, SystemOS and Data
windows behind it, with the gaps between windows zero-filled. The image is split into
parts because no single upload carries 19 GB, and the parts concatenate back into it.

This streams those parts in order to /dev/block/by-name/userdata with the same device
write the window installer uses: dd with an exact seek, then a receipt the device
prints only after fsync. The directory block goes last, so a run that stops halfway
leaves no directory pointing at windows that are not there yet.

Nothing is hashed and no image bytes are read back, matching the rest of the install
path. The checks here only stop the write from going to the wrong partition or from
running past its end.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import pong_paths

MIB = 1 << 20
SECTOR = 4096
DIRECTORY_BYTES = 4096


class Stream:
    """Sequential read across the parts, as if they were one file."""

    def __init__(self, paths):
        self.handles = [Path(p).open('rb', buffering=0) for p in paths]
        self.index = 0

    def read(self, amount):
        chunks = []
        while amount > 0 and self.index < len(self.handles):
            block = self.handles[self.index].read(amount)
            if not block:
                self.index += 1
                continue
            chunks.append(block)
            amount -= len(block)
        return b''.join(chunks)

    def skip(self, amount):
        while amount > 0:
            block = self.read(min(MIB, amount))
            if not block:
                raise RuntimeError('the parts end before that offset')
            amount -= len(block)

    def close(self):
        for handle in self.handles:
            handle.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--describe', type=Path, required=True,
                   help='userdata-image.json from the build')
    p.add_argument('--parts-dir', type=Path,
                   help='where the extracted parts sit; default is the directory the build recorded')
    p.add_argument('--adb', type=Path, default=pong_paths.adb())
    p.add_argument('--serial', required=True)
    p.add_argument('--port', type=int, default=5038)
    p.add_argument('--output-dir', type=Path, default=ROOT / 'out/ipados18/install')
    p.add_argument('--chunk-mib', type=int, default=512)
    p.add_argument('--resume', action='store_true',
                   help='continue from the state file left by an interrupted run')
    a = p.parse_args()

    if not 16 <= a.chunk_mib <= 2048:
        raise SystemExit('Use 16..2048 MiB chunks')

    image = json.loads(a.describe.read_text())
    image_bytes = image['image_bytes']
    capacity = image['partition_capacity_bytes']
    if image_bytes > capacity:
        raise SystemExit('The image is larger than the partition it targets')
    if image_bytes % SECTOR:
        raise SystemExit('The image is not a whole number of 4 KiB sectors')

    base = a.parts_dir.resolve() if a.parts_dir else Path(image['parts'][0]['path']).parent
    parts = []
    for entry in image['parts']:
        path = base / Path(entry['path']).name
        if not path.is_file():
            raise SystemExit('Missing part: %s' % path)
        size = path.stat().st_size
        if size != entry['bytes']:
            raise SystemExit('%s is %d bytes, the build recorded %d' % (path.name, size, entry['bytes']))
        parts.append(path)
    if sum(entry['bytes'] for entry in image['parts']) != image_bytes:
        raise SystemExit('The parts do not add up to the image size')

    adb = [str(a.adb), '-P', str(a.port), '-s', a.serial]

    def run(args, **kw):
        return subprocess.run(args, check=True, **kw)

    def shell(command):
        return run(adb + ['shell', command], capture_output=True, text=True).stdout.strip()

    if run(adb + ['get-state'], capture_output=True, text=True).stdout.strip() != 'recovery':
        raise SystemExit('Recovery mode is required')

    target = '/dev/block/by-name/userdata'
    real = shell('readlink -f ' + target)
    node, disk = pong_paths.userdata_sysfs()
    size_bytes = int(shell('blockdev --getsize64 ' + target))
    block_bytes = int(shell('cat /sys/class/block/%s/queue/logical_block_size' % disk))
    start_512 = int(shell('cat /sys/class/block/%s/start' % node))
    geometry = dict(device=real, bytes=size_bytes, first_512=start_512, logical_block_bytes=block_bytes)

    if real != pong_paths.userdata_device():
        raise SystemExit('userdata resolves to %s, board.toml says %s' % (real, pong_paths.userdata_device()))
    if size_bytes != capacity:
        raise SystemExit('userdata is %d bytes, the build targeted %d' % (size_bytes, capacity))
    if block_bytes != SECTOR:
        raise SystemExit('userdata does not use 4 KiB logical blocks')
    if shell('ls /sys/class/block/%s/holders' % node):
        raise SystemExit('userdata has active device-mapper holders')
    if any(line.split()[0] in (target, real) for line in shell('cat /proc/mounts').splitlines()):
        raise SystemExit('userdata is mounted; it must stay unmounted')

    a.output_dir.mkdir(parents=True, exist_ok=True)
    record = a.output_dir / 'userdata-image-install.json'
    if a.resume:
        state = json.loads(record.read_text())
        if state['describe'] != str(a.describe.resolve()) or state['serial'] != a.serial \
                or state['geometry'] != geometry or state['image_bytes'] != image_bytes:
            raise SystemExit('The resume state belongs to another image or device')
    else:
        if record.exists():
            raise SystemExit('An install record exists; pass --resume to continue it')
        state = dict(describe=str(a.describe.resolve()), serial=a.serial, geometry=geometry,
                     image_bytes=image_bytes, directory_written=False, completed_bytes=0,
                     image_verification='skipped: windows are copied verbatim',
                     readback_performed=False)

    def save():
        record.write_text(json.dumps(state, indent=2) + '\n')

    def write_range(offset, amount, reader, what):
        """dd one range from reader, and require the device to acknowledge it."""
        if offset % SECTOR or amount % SECTOR:
            raise RuntimeError('a write range must be whole 4 KiB sectors')
        if amount == 0:
            return
        if offset % MIB == 0:
            bs, seek = MIB, offset // MIB
        else:
            bs, seek = SECTOR, offset // SECTOR
        receipt = 'PONG_IMAGE_WRITE_DONE_%d_%d' % (offset, amount)
        command = ('dd of=%s bs=%d seek=%d conv=notrunc,fsync status=none && printf "%%s\\n" %s'
                   % (target, bs, seek, receipt))
        transfer = subprocess.Popen(adb + ['shell', '-T', command],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            left = amount
            while left:
                block = reader.read(min(8 * MIB, left))
                if not block:
                    raise RuntimeError('the parts ended inside ' + what)
                transfer.stdin.write(block)
                left -= len(block)
            transfer.stdin.close()
            transfer.stdin = None
            stdout, stderr = transfer.communicate()
            if transfer.returncode or stdout.strip() != receipt.encode():
                raise RuntimeError('the device did not acknowledge %s: %s %s'
                                   % (what, stdout.decode(errors='replace')[:200],
                                      stderr.decode(errors='replace')[:200]))
        except BaseException:
            if transfer.poll() is None:
                transfer.kill()
            transfer.wait()
            raise

    print('image %d bytes in %d part(s); userdata %d bytes' % (image_bytes, len(parts), capacity), flush=True)
    stream = Stream(parts)
    try:
        directory = stream.read(DIRECTORY_BYTES)
        if len(directory) != DIRECTORY_BYTES:
            raise SystemExit('the parts are shorter than the directory block')
        if directory[:8] != b'SHNGROOT':
            raise SystemExit('the image does not start with a SHNGROOT directory: %r' % directory[:8])

        begin = DIRECTORY_BYTES
        if state['completed_bytes']:
            print('resuming at %d bytes' % state['completed_bytes'], flush=True)

        # Everything up to the first MiB boundary sits behind a 4 KiB offset, so it
        # goes with 4 KiB blocks; past that every write is MiB aligned.
        head_end = min(MIB, image_bytes)
        if state['completed_bytes'] < head_end:
            head = stream.read(head_end - begin)
            write_range(begin, len(head), _Once(head), 'the first block')
            state['completed_bytes'] = head_end
            save()
            print('%d/%d bytes' % (state['completed_bytes'], image_bytes), flush=True)
        elif state['completed_bytes'] > begin:
            # The stream still sits at `begin`; carry it to where the run stopped.
            stream.skip(state['completed_bytes'] - begin)

        body_end = image_bytes - (image_bytes % MIB)
        while state['completed_bytes'] < body_end:
            amount = min(a.chunk_mib * MIB, body_end - state['completed_bytes'])
            write_range(state['completed_bytes'], amount, stream, 'the windows')
            state['completed_bytes'] += amount
            save()
            print('%d/%d bytes' % (state['completed_bytes'], image_bytes), flush=True)

        tail = image_bytes - body_end
        if tail:
            write_range(body_end, tail, stream, 'the last window bytes')
            state['completed_bytes'] = image_bytes
            save()

        # The directory is the commit: until it lands, the windows it describes are
        # unreachable, and a failure above leaves the previous state in place.
        write_range(0, DIRECTORY_BYTES, _Once(directory), 'the SHNGROOT directory')
        shell('sync')
        state['directory_written'] = True
        state['state'] = 'complete'
        save()
    finally:
        stream.close()

    print('userdata image installed; the directory was committed last', flush=True)


class _Once:
    """Hand a fixed byte string to write_range as if it were a reader."""

    def __init__(self, data):
        self.data = data
        self.used = False

    def read(self, amount):
        if self.used:
            return b''
        self.used = True
        return self.data[:amount]


if __name__ == '__main__':
    main()
