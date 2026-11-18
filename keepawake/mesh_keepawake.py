#!/usr/bin/env python3
"""Keep the laptop from going to sleep on idle while meshtasticd runs, so the node stays on the
mesh with the screen locked. Holds a desktop power-management inhibition
(org.freedesktop.PowerManagement.Inhibit, which KDE's PowerDevil implements) for as long as this
process lives; the desktop drops it automatically when the process exits. Only *automatic* (idle)
sleep is blocked: closing the lid or choosing Sleep still sleeps."""
import signal
import sys

from gi.repository import Gio, GLib

APP = "meshtasticd"
REASON = "Meshtastic node is running (Waveshare LoRa stick)"

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
cookie = bus.call_sync(
    "org.freedesktop.PowerManagement", "/org/freedesktop/PowerManagement/Inhibit",
    "org.freedesktop.PowerManagement.Inhibit", "Inhibit",
    GLib.Variant("(ss)", (APP, REASON)), GLib.VariantType("(u)"),
    Gio.DBusCallFlags.NONE, -1, None,
).unpack()[0]
print(f"idle sleep inhibited (cookie {cookie})", flush=True)

loop = GLib.MainLoop()
try:
    from gi.repository import GLibUnix
    signal_add = GLibUnix.signal_add
except ImportError:  # older PyGObject
    signal_add = GLib.unix_signal_add
for sig in (signal.SIGTERM, signal.SIGINT):
    signal_add(GLib.PRIORITY_DEFAULT, sig, loop.quit)
loop.run()

try:
    bus.call_sync("org.freedesktop.PowerManagement", "/org/freedesktop/PowerManagement/Inhibit",
                  "org.freedesktop.PowerManagement.Inhibit", "UnInhibit", GLib.Variant("(u)", (cookie,)),
                  None, Gio.DBusCallFlags.NONE, -1, None)
except GLib.Error as e:
    print(f"UnInhibit: {e.message}", file=sys.stderr)
print("idle sleep allowed again", flush=True)
