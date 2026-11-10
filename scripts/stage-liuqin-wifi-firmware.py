#!/usr/bin/env python3
"""Stage liuqin's QCA6490 firmware for build-pong-wifi-native.py.

Input: the stock vendor qca6490 directory (fw/wlan/ of the encrypted stock
release, from NON-HLOS.bin of V14.0.9.0.TMYCNXM). Output: OUT/qca6490/ with the
radio images and OUT/local-inputs.json, the provenance record the Wi-Fi build
checks. Nothing here is committed; the build embeds the files in the kernel,
which leaves CI only encrypted.

Board data. Pong's ath11k patch asks for bdwlan[g].e<board_id> from the QMI
target info (the "g" prefix when chip_id & 0x10). liuqin's radio reports
qmi-chip-id 18 and qmi-board-id 255 (verified on hardware by the mainline
liuqin port, xiaomipad-6pro-mainline tools/build-liuqin-wlan.py), so it asks
for bdwlang.elf, which in the stock set is Qualcomm's generic reference
calibration. Xiaomi's own calibration for this board is bd_m81gf.elf (stock
bd_m81.elf is byte-identical). It is staged under both names ath11k can ask
for on this chip, bdwlang.elf and bdwlan.elf, and the generic bdwlan.eXX set is
left out: a radio that reported another board id fails loudly instead of
loading a reference calibration.

usage: stage-liuqin-wifi-firmware.py STOCK_WLAN_DIR OUT
"""
import hashlib
import json
import shutil
import sys
from pathlib import Path

RADIO = ('amss.bin', 'amss20.bin', 'm3.bin', 'regdb.bin')
BOARD = 'bd_m81gf.elf'
# The mainline port's pin for bd_m81gf.elf, unchanged between OS2.0.6.0 and V14.0.9.0.
BOARD_SHA256 = 'aa8ae92d781e09db8cffa960b00c9606e4bb65d9c7a89f9b32df1bd24aee8573'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    stock, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    missing = [n for n in RADIO + (BOARD,) if not (stock / n).is_file()]
    if missing:
        raise SystemExit(f'{stock} lacks {", ".join(missing)}')
    if sha(stock / BOARD) != BOARD_SHA256:
        raise SystemExit(f'{BOARD} is not the calibration the mainline port validated')
    target = out / 'qca6490'
    if target.exists():
        shutil.rmtree(target)
    target.mkdir(parents=True)
    files = {}
    for name in RADIO:
        shutil.copyfile(stock / name, target / name)
        files[name] = dict(source=name, sha256=sha(target / name))
    for name in ('bdwlang.elf', 'bdwlan.elf'):
        shutil.copyfile(stock / BOARD, target / name)
        files[name] = dict(source=BOARD, sha256=BOARD_SHA256)
    (out / 'local-inputs.json').write_text(json.dumps(dict(
        firmware_directory=str(target),
        source=dict(product='liuqin', capture='stock fastboot ROM NON-HLOS.bin',
                    rom='V14.0.9.0.TMYCNXM', radio='QCA6490', qmi_chip_id=18, qmi_board_id=255),
        files=files), indent=2) + '\n')
    print(f'staged {len(files)} files in {target}')


if __name__ == '__main__':
    main()
