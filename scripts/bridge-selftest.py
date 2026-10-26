#!/usr/bin/env python3
"""Talk to the bridge firmware directly and read the SX1262 behind it (meshtasticd must be stopped).
   scripts/bridge-selftest.py [port]     default: /dev/waveshare-lora or the first 1a86:55d3 tty
Expected: hello WSBR, sync word 1424 (SX1262 reset default), version string 'SX1261 V2D 2D02'
(every SX1262 reports itself as SX1261 there)."""
import glob, os, sys, time
import serial  # pip install pyserial

port = sys.argv[1] if len(sys.argv) > 1 else next(
    (p for p in ["/dev/waveshare-lora"] + sorted(glob.glob("/dev/serial/by-id/usb-1a86_*")) if os.path.exists(p)), None)
if not port:
    sys.exit("no Waveshare stick found")
s = serial.Serial(os.path.realpath(port), 2000000, timeout=0.5)
time.sleep(0.1); s.reset_input_buffer()

def req(cmd, data=b"", reply=True):
    s.write(bytes([0xA5, cmd, len(data) & 255, len(data) >> 8]) + data)
    if not reply:
        return None
    h = s.read(4)
    if len(h) < 4:
        sys.exit(f"timeout on command {cmd:#x}: is the bridge firmware running, and meshtasticd stopped?")
    return s.read(h[2] | h[3] << 8)

hello = req(0x01)
print("hello:", hello[:4].decode(), "v%d" % hello[4], "TCXO" if hello[5] else "XTAL")
req(0x04, bytes([2, 0]), False); time.sleep(0.01); req(0x04, bytes([2, 1]), False); time.sleep(0.05)  # reset SX1262
print("BUSY:", req(0x05, bytes([4])).hex(), " DIO1:", req(0x05, bytes([6])).hex())
print("GetStatus:", req(0x03, bytes([0xC0, 0])).hex())
print("sync word 0x0740:", req(0x03, bytes([0x1D, 0x07, 0x40, 0, 0, 0]))[4:6].hex(), "(expect 1424)")
print("version 0x0320:", req(0x03, bytes([0x1D, 0x03, 0x20, 0]) + bytes(16))[4:].split(b"\0")[0].decode())
t = time.time()
for _ in range(200):
    req(0x05, bytes([4]))
print("round trip: %.2f ms" % ((time.time() - t) / 200 * 1000))
