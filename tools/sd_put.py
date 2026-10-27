#!/usr/bin/env python3
"""Copies files to the SD card in the Tab5 over the USB cable, while the launcher runs (main/l5_link.c).

  python tools/sd_put.py COM3 put LOCAL /path/on/card [LOCAL /path/on/card ...]
  python tools/sd_put.py COM3 rm /path/on/card

Examples:
  python tools/sd_put.py COM3 put photo.jpg /wallpaper.jpg
  python tools/sd_put.py COM3 put icon.png /apps/orbit_drum/icon.png about.txt /apps/orbit_drum/about.txt

The port is opened without a reset (DTR and RTS stay low), so the launcher keeps running. Each file is checked
(FNV-1a) before it replaces the one on the card. Needs pyserial.
"""
import sys
import time
from pathlib import Path

import serial

BLOCK = 8192  # the launcher acknowledges every 8 KB (l5_link.c BLOCK)


def open_port(name):
    deadline = time.monotonic() + 20
    while True:
        try:
            p = serial.Serial(port=None, baudrate=115200, timeout=0.1)
            p.dtr = False
            p.rts = False
            p.port = name
            p.open()
            return p
        except serial.SerialException:
            if time.monotonic() > deadline:
                raise SystemExit('port not available: ' + name)
            time.sleep(0.3)


_pending = bytearray()  # read but not used yet: the line after an ACK often comes in the same read


def reply(port, prefix, seconds):
    """the first line that starts with prefix (the launcher's log lines in between are skipped)"""
    end = time.monotonic() + seconds
    while True:
        while b'\n' in _pending:
            i = _pending.index(b'\n')
            text = _pending[:i].decode('utf-8', 'replace').strip()
            del _pending[:i + 1]
            if text.startswith(prefix):
                return text
        if time.monotonic() > end:
            return None
        _pending.extend(port.read(4096))


def fnv1a(data):
    h = 0x811c9dc5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return h


def send(port, line):
    port.write(line.encode('utf-8') + b'\n')  # "\n" alone: a "\r\n" would leave a byte in front of the data
    port.flush()


def put(port, local, remote):
    data = Path(local).read_bytes()
    t0 = time.monotonic()
    port.reset_input_buffer()
    _pending.clear()
    send(port, 'l5put %s %d %08x' % (remote, len(data), fnv1a(data)))
    r = reply(port, 'L5PUT', 10)
    if r != 'L5PUT READY':
        raise SystemExit('%s refused: %s' % (remote, r or 'no reply (is the launcher running?)'))
    for k in range(0, len(data), BLOCK):
        port.write(data[k:k + BLOCK])
        port.flush()
        r = reply(port, 'L5PUT', 10)
        if not r or not r.startswith('L5PUT ACK'):
            raise SystemExit('%s stopped at %d: %s' % (remote, k, r or 'no reply'))
    r = reply(port, 'L5PUT', 30)  # the file closed and renamed on the card
    if not r or not r.startswith('L5PUT OK'):
        raise SystemExit('%s failed: %s' % (remote, r or 'no reply'))
    dt = time.monotonic() - t0
    print('%s: %d bytes in %.1f s (%.0f KB/s)' % (remote, len(data), dt, len(data) / 1024 / max(dt, 0.001)))


def main():
    if len(sys.argv) < 4 or sys.argv[2] not in ('put', 'rm'):
        raise SystemExit(__doc__)
    port = open_port(sys.argv[1])
    if sys.argv[2] == 'put':
        args = sys.argv[3:]
        if len(args) % 2:
            raise SystemExit('put needs pairs: LOCAL /path/on/card')
        for local, remote in zip(args[::2], args[1::2]):
            put(port, local, remote)
    else:
        for remote in sys.argv[3:]:
            send(port, 'l5rm ' + remote)
            print(reply(port, 'L5RM', 10) or 'no reply')


if __name__ == '__main__':
    main()
