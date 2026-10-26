#!/usr/bin/env python3
"""WiFi-located virtual GPS for meshtasticd.

Looks up the laptop's position from nearby WiFi access points (BeaconDB, MLS-compatible API) and
serves it as a minimal gpsd on 127.0.0.1:2947 streaming NMEA, which meshtasticd's GPS:GpsdHost
reads like a real receiver. meshtasticd then applies the channel's position precision itself.

Fixes worse than MAX_ACCURACY_M are refused (BeaconDB falls back to cell/IP guesses kilometres
off); with no good fix the stream reports no fix, so nothing is broadcast. Optional fallback
position: fallback.json next to this file, {"lat": .., "lon": .., "accuracy": ..}.
"""
import json
import os
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone

LISTEN = ("127.0.0.1", 2947)
LOOKUP_URL = os.environ.get("WIFI_GPS_URL", "https://api.beacondb.net/v1/geolocate")
LOOKUP_INTERVAL_S = int(os.environ.get("WIFI_GPS_INTERVAL", "300"))
MAX_ACCURACY_M = float(os.environ.get("WIFI_GPS_MAX_ACCURACY", "3000"))
FIX_MAX_AGE_S = 3 * LOOKUP_INTERVAL_S
USER_AGENT = "mesh-wifi-gps/0.1 (personal Meshtastic node)"
FALLBACK_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fallback.json")

fix = None  # (lat, lon, accuracy_m, monotonic_time, source)
fix_lock = threading.Lock()


def log(msg):
    print(msg, flush=True)


def scan_wifi():
    out = subprocess.run(
        ["nmcli", "-t", "-e", "yes", "-f", "BSSID,SIGNAL", "dev", "wifi", "list", "--rescan", "auto"],
        capture_output=True, text=True, timeout=30,
    ).stdout
    aps = []
    for line in out.splitlines():
        if not line:
            continue
        bssid, sig = line.rsplit(":", 1)
        # nmcli SIGNAL is 0-100 %; map onto roughly -100..-50 dBm
        aps.append({"macAddress": bssid.replace("\\:", ":").lower(), "signalStrength": int(int(sig) / 2 - 100)})
    return aps


def lookup(aps):
    body = json.dumps({"considerIp": False, "wifiAccessPoints": aps}).encode()
    req = urllib.request.Request(LOOKUP_URL, data=body,
                                 headers={"Content-Type": "application/json", "User-Agent": USER_AGENT})
    try:
        r = json.load(urllib.request.urlopen(req, timeout=20))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None  # no coverage for these APs
        raise
    loc = r.get("location") or {}
    if "lat" not in loc:
        return None
    return loc["lat"], loc["lng"], float(r.get("accuracy", 1e9)), r.get("fallback")


def load_fallback():
    try:
        with open(FALLBACK_FILE) as f:
            fb = json.load(f)
        return float(fb["lat"]), float(fb["lon"]), float(fb.get("accuracy", 100))
    except (OSError, KeyError, ValueError):
        return None


def lookup_loop():
    global fix
    while True:
        new = None
        try:
            aps = scan_wifi()
            res = lookup(aps) if len(aps) >= 2 else None
            if res and res[3] is None and res[2] <= MAX_ACCURACY_M:
                new = (res[0], res[1], res[2], "wifi")
                log(f"wifi fix from {len(aps)} APs, accuracy {res[2]:.0f} m")
            elif res:
                log(f"refused fix: accuracy {res[2]:.0f} m, fallback={res[3]}")
            else:
                log(f"no wifi coverage for {len(aps)} APs")
        except Exception as e:  # network down, nmcli missing, ...
            log(f"lookup failed: {e}")
        if new is None:
            fb = load_fallback()
            if fb:
                new = (*fb, "fallback")
        if new:
            with fix_lock:
                fix = (new[0], new[1], new[2], time.monotonic(), new[3])
        time.sleep(LOOKUP_INTERVAL_S)


def nmea(body):
    cs = 0
    for c in body.encode():
        cs ^= c
    return f"${body}*{cs:02X}\r\n"


def deg(value, is_lat):
    hemi = ("N" if value >= 0 else "S") if is_lat else ("E" if value >= 0 else "W")
    value = abs(value)
    d = int(value)
    m = (value - d) * 60
    return (f"{d:02d}{m:07.4f}" if is_lat else f"{d:03d}{m:07.4f}"), hemi


def sentences():
    now = datetime.now(timezone.utc)
    hms = now.strftime("%H%M%S.00")
    dmy = now.strftime("%d%m%y")
    with fix_lock:
        f = fix
    if f is None or time.monotonic() - f[3] > FIX_MAX_AGE_S:
        return nmea(f"GPGGA,{hms},,,,,0,00,99.9,,M,,M,,") + nmea(f"GPRMC,{hms},V,,,,,,,{dmy},,,N")
    lat, latH = deg(f[0], True)
    lon, lonH = deg(f[1], False)
    hdop = max(1.0, min(f[2] / 10.0, 99.0))  # rough: ~10 m per HDOP unit
    return (nmea(f"GPGGA,{hms},{lat},{latH},{lon},{lonH},1,04,{hdop:.1f},0.0,M,0.0,M,,")
            + nmea(f"GPRMC,{hms},A,{lat},{latH},{lon},{lonH},0.0,0.0,{dmy},,,A"))


def serve_client(conn):
    conn.settimeout(0.0)
    try:
        conn.sendall(b'{"class":"VERSION","release":"mesh-wifi-gps","proto_major":3,"proto_minor":14}\n')
        while True:
            try:
                conn.recv(4096)  # ?WATCH etc.: always stream NMEA regardless
            except BlockingIOError:
                pass
            conn.sendall(sentences().encode())
            time.sleep(1)
    except OSError:
        pass
    finally:
        conn.close()


def main():
    threading.Thread(target=lookup_loop, daemon=True).start()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(LISTEN)
    srv.listen(4)
    log(f"virtual gpsd on {LISTEN[0]}:{LISTEN[1]}, lookups every {LOOKUP_INTERVAL_S}s, max accuracy {MAX_ACCURACY_M:.0f} m")
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=serve_client, args=(conn,), daemon=True).start()


if __name__ == "__main__":
    main()
