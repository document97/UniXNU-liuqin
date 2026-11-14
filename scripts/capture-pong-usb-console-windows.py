#!/usr/bin/env python3
"""Windows counterpart of capture-pong-usb-console.py (no fcntl / termios).

The Linux script uses os.open + termios + fcntl.ioctl(TIOCMBIS) to raise DTR,
which Windows Python does not have. Pong keeps a CDC-ACM console alive after
ExitBootServices; on Windows that gadget shows up as a COM port. This script
is strictly read-only: it never transmits a command or payload, it only raises
DTR, which is the standard CDC host-open handshake the gadget waits for.

Typical use, in a second terminal while run-probe.ps1 boots the tablet:

    python scripts/capture-pong-usb-console-windows.py --list
    python scripts/capture-pong-usb-console-windows.py \
        --wait 90 --seconds 25 --output logs/pong-console-1.bin

If the port never appears, --list-usb dumps every USB PnP device with a
Xiaomi / Qualcomm / Apple VID so you can tell "gadget not started" apart from
"gadget present but missing a CDC driver".

Requires: python -m pip install pyserial
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required: python -m pip install pyserial")

# Pong reuses the Apple recovery VID/PID on some firmware builds; the stock
# Xiaomi / Qualcomm IDs are included so --list is useful for diagnosis too.
INTERESTING_VIDS = {0x05AC, 0x18D1, 0x2717, 0x05C6}
INTERESTING_PIDS = {0x12A8, 0xD00D, 0x4EE0, 0x900E, 0x9008, 0x901D}
CONSOLE_WORDS = ("pong", "cdc", "usbmodem", "serial", "diagnostic")


def candidates():
    """COM ports that look like the Pong CDC gadget, best guess first."""
    scored = []
    for p in list_ports.comports():
        vid, pid = p.vid, p.pid
        text = f"{p.description} {p.manufacturer} {p.product} {p.hwid}".lower()
        score = 0
        if vid in INTERESTING_VIDS:
            score += 2
        if (vid, pid) == (0x05AC, 0x12A8):
            score += 4
        if any(word in text for word in CONSOLE_WORDS):
            score += 1
        if pid in INTERESTING_PIDS:
            score += 1
        scored.append((score, p))
    return [p for score, p in sorted(scored, key=lambda item: -item[0]) if score >= 2]


def describe(p) -> str:
    #虚拟/遗留 COM 口的 vid/pid 可能是 None，格式化前兜底。
    vid = p.vid if p.vid is not None else 0
    pid = p.pid if p.pid is not None else 0
    return (
        f"{p.device:<8} vid={vid:04X} pid={pid:04X} "
        f"{p.description} [{p.hwid}]"
    )


def list_usb() -> None:
    """Dump *currently present* USB PnP devices, even without a COM port."""
    ps = (
        "Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | "
        "Where-Object { $_.InstanceId -match 'VID_05AC|VID_18D1|VID_2717|VID_05C6' } | "
        "Select-Object Status,Class,FriendlyName,InstanceId | ConvertTo-Json -Compress"
    )
    try:
        raw = subprocess.check_output(
            ["powershell.exe", "-NoProfile", "-Command", ps], text=True, timeout=25
        )
        items = json.loads(raw) if raw.strip() else []
        if not isinstance(items, list):
            items = [items]
    except (OSError, subprocess.SubprocessError, ValueError) as exc:
        print(f"could not query PnP: {exc}", file=sys.stderr)
        return
    print(f"{len(items)} matching USB PnP device(s):")
    for item in items:
        print(
            f"  {str(item.get('Status')):<10} {str(item.get('Class')):<22} "
            f"{item.get('FriendlyName')}  {item.get('InstanceId')}"
        )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="explicit COM port, e.g. COM7")
    ap.add_argument("--seconds", type=float, default=15, help="capture window")
    ap.add_argument("--max-bytes", type=int, default=1048576)
    ap.add_argument("--output", type=Path, required=False)
    ap.add_argument("--wait", type=float, default=0,
                    help="poll up to N seconds for the CDC port to appear")
    ap.add_argument("--list", action="store_true", help="list all COM ports and exit")
    ap.add_argument("--list-usb", action="store_true", help="dump USB PnP devices and exit")
    ap.add_argument("--preview", type=int, default=0, help="print first N captured bytes as text")
    a = ap.parse_args()

    if a.list or a.list_usb:
        print("COM ports:")
        ports = list(list_ports.comports())
        marked = {p.device for p in candidates()}
        for p in ports:
            mark = "*" if p.device in marked else " "
            print(f" {mark} {describe(p)}")
        if not ports:
            print("  (none)")
        print("  (* = plausible Pong CDC candidate)")
        if a.list_usb:
            print()
            list_usb()
        return 0

    if not a.output:
        ap.error("--output is required unless --list / --list-usb")
    if not 0 < a.seconds <= 120 or not 1 <= a.max_bytes <= 16 * 1048576:
        ap.error("use 0..120 seconds and 1..16777216 bytes")
    if a.output.exists():
        ap.error("preserving existing output; choose a new path")

    port = a.port
    deadline = time.monotonic() + a.wait
    while not port:
        found = candidates()
        if len(found) == 1:
            port = found[0].device
            print(f"auto-selected {describe(found[0])}", flush=True)
            break
        if len(found) > 1:
            ap.error("several candidate COM ports; pass --port explicitly:\n  "
                     + "\n  ".join(describe(p) for p in found))
        if time.monotonic() >= deadline:
            print("no Pong CDC COM port appeared.", file=sys.stderr)
            print("Hint: run again with --list-usb to see whether the gadget "
                  "enumerated without a CDC driver.", file=sys.stderr)
            return 2
        time.sleep(0.5)

    a.output.parent.mkdir(parents=True, exist_ok=True)
    data = bytearray()
    # 115200 8N1 is what the Linux script configures. Opening the port and
    # toggling DTR false->true is the CDC "host open" signal; no bytes are sent.
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=0,
                       dsrdtr=False, rtscts=False) as ser:
        try:
            ser.dtr = False
            ser.rts = False
            time.sleep(0.05)
            ser.dtr = True
        except Exception as exc:  # some USB-serial drivers reject DTR control
            print(f"warning: DTR handshake failed ({exc}); continuing read-only",
                  file=sys.stderr)
        end = time.monotonic() + a.seconds
        print(f"capturing {port} for {a.seconds:g}s ...", flush=True)
        while time.monotonic() < end and len(data) < a.max_bytes:
            block = ser.read(min(16384, a.max_bytes - len(data)))
            if block:
                data.extend(block)

    with a.output.open("xb") as out:
        out.write(data)
    print(f"{len(data)} live USB console bytes saved to {a.output}")
    if a.preview and data:
        sample = bytes(data[:a.preview])
        print(repr(sample.decode("utf-8", "replace")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
