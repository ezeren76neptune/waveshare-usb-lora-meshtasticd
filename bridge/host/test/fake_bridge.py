#!/usr/bin/env python3
"""Pretends to be the bridge firmware on a pty, for testing libpinedio_wsbridge without hardware.
XFER replies with each byte inverted; writing RXEN low makes DIO1 'rise' (an IRQ event)."""
import os, pty, sys, time
SYNC = 0xA5
master, slave = pty.openpty()
link = sys.argv[1]
if os.path.lexists(link): os.unlink(link)
os.symlink(os.ttyname(slave), link)
pins = {0: 1, 1: 1, 2: 1, 4: 0, 6: 0}
def send(cmd, data=b""):
    os.write(master, bytes([SYNC, cmd, len(data) & 0xFF, len(data) >> 8]) + data)
def read(n):
    b = b""
    while len(b) < n: b += os.read(master, n - len(b))
    return b
while True:
    if read(1)[0] != SYNC: continue
    cmd, lo, hi = read(3); ln = lo | hi << 8; p = read(ln) if ln else b""
    if cmd == 0x01: send(0x81, b"WSBR\x01\x01")
    elif cmd in (0x02, 0x03): send(cmd | 0x80, bytes(~x & 0xFF for x in p))
    elif cmd == 0x04:
        pins[p[0]] = p[1]
        if p[0] == 1 and p[1] == 0: time.sleep(0.01); pins[6] = 1; send(0xE1)
    elif cmd == 0x05: send(0x85, bytes([pins.get(p[0], 0)]))
