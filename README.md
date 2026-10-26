# Full Meshtastic on a Waveshare USB-TO-LoRa stick (meshtasticd)

The [Waveshare USB-TO-LoRa-HF](https://www.waveshare.com/wiki/USB-TO-LoRa-xF) is a cheap SX1262 USB
stick, but its stock firmware is a transparent serial modem that only talks to other Waveshare units.
This repo turns it into a radio for the **official Meshtastic Linux daemon, `meshtasticd`**. You get the
real thing: encrypted DMs, relaying, the phone/desktop apps, admin, telemetry. It isn't a
Meshtastic look-alike.

```
Meshtastic app ──TCP :4403──▶ meshtasticd ──libpinedio-usb API──▶ libpinedio_wsbridge.c ──USB serial 2 Mbaud──▶
  CH343 ──▶ GD32F103 running bridge/fw (behind Archie3d's bootloader) ──SPI──▶ SX1262 ──▶ antenna
```

The stick's microcontroller (GD32F103, 128 KB flash) is far too small for Meshtastic firmware, so it
runs a ~3 KB **SPI/GPIO bridge** instead. `meshtasticd` already drives the
[MeshStick](https://meshtastic.org/docs/meshtasticd/hardware/radios/usb/meshstick/) through a CH341
USB-to-SPI chip, via the `libpinedio-usb` API. `bridge/host/libpinedio_wsbridge.c` implements that same
API over the stick's serial port, the same way upstream swaps it out for its Windows and browser
builds. meshtasticd's radio code (`Ch341Hal`, RadioLib) is unmodified.

**Status:** used on one USB-TO-LoRa-HF (TCXO variant) on Fedora 44, meshtasticd 2.8.2 (`c39728e`), US
LongFast. Verified: SX1262 init, receiving the local mesh including 3-4 hop nodes and packets at
−16 dB SNR, transmitting its node info and telemetry (sent per meshtasticd's log; reception by a
second node not independently confirmed), control from the Meshtastic CLI and Desktop app,
unplug/replug. **The flashing step permanently erases Waveshare's firmware.** It's read-protected
and can't be backed up.

## Credits and upstream projects

| Project | Used for |
|---|---|
| [Archie3d/waveshare-usb-lora](https://github.com/Archie3d/waveshare-usb-lora) | The reverse-engineering this all rests on: MCU, pinout, SWD pads, TCXO/RF switch details |
| [Archie3d/waveshare-usb-lora-bootloader](https://github.com/Archie3d/waveshare-usb-lora-bootloader) (`b4b8ee3`) | USB bootloader, flashed once over SWD |
| [Archie3d/waveshare-usb-lora-programmer](https://github.com/Archie3d/waveshare-usb-lora-programmer) (`142ad73`) | `wsprog`, for later bridge updates over USB |
| [Archie3d/waveshare-usb-lora-firmware](https://github.com/Archie3d/waveshare-usb-lora-firmware) | Reference for clocks, pins and SX1262 setup (his own LoRa firmware + Go Meshtastic node) |
| [meshtastic/firmware](https://github.com/meshtastic/firmware) (`c39728e`) | meshtasticd, patched by `meshtastic/wsbridge.patch` |
| [libopencm3](https://github.com/libopencm3/libopencm3) (submodule, `201f5bc`) | Bridge firmware HAL |
| [raspberrypi/debugprobe](https://github.com/raspberrypi/debugprobe) | Turns a Pico / Pico 2 into the SWD programmer |
| [meshtastic/Meshtastic-Android](https://github.com/meshtastic/Meshtastic-Android) (Desktop app) | GUI, from Flathub `org.meshtastic.MeshtasticDesktop` |
| [BeaconDB](https://beacondb.net) | Optional WiFi geolocation for `wifi-gps/` |

Archie3d's repositories carry no license, so none of their code is copied here. Clone them yourself
(commands below).

## What you need

- Waveshare **USB-TO-LoRa-HF** (850–930 MHz) or **-LF** (410–510 MHz). Plain `HF`/`LF` = TCXO;
  `-B` = crystal (build the bridge with `WITH_TCXO=0`; untested).
- An SWD probe for the one-time unlock + bootloader: a **Raspberry Pi Pico or Pico 2** with
  debugprobe firmware (used here), or an ST-Link V2.
- 3 wires and a soldering iron (or pins held firmly in the pad holes for ~10 s).
- Linux. Tested on Fedora 44. Tools: `arm-none-eabi-gcc` + newlib, `openocd`, Go (for `wsprog`),
  PlatformIO and meshtasticd's build deps (see [Build dependencies](#build-dependencies)), Python 3 +
  `pyserial` for the self-test.

## Step by step

### 1. Clone and build

```bash
git clone --recursive https://github.com/ezeren76neptune/waveshare-usb-lora-meshtasticd
cd waveshare-usb-lora-meshtasticd
make -C bridge/fw                     # -> bridge/fw/wsbridge.bin (add WITH_TCXO=0 for -B sticks)
bridge/host/test/run.sh               # host library tests against a fake stick, no hardware needed

git clone https://github.com/Archie3d/waveshare-usb-lora-bootloader ../ws-bootloader
git -C ../ws-bootloader checkout b4b8ee3 && git -C ../ws-bootloader submodule update --init
make -C ../ws-bootloader               # -> ../ws-bootloader/bootloader.bin

git clone https://github.com/Archie3d/waveshare-usb-lora-programmer ../ws-programmer
(cd ../ws-programmer && git checkout 142ad73 && go build -o . ./...)   # -> wsprog
```

### 2. Make the Pico (2) an SWD probe

Download `debugprobe_on_pico2.uf2` (or `debugprobe_on_pico.uf2`) from the
[debugprobe releases](https://github.com/raspberrypi/debugprobe/releases). Hold BOOTSEL while plugging
the Pico in, then copy the .uf2 onto the drive that appears. It comes back as `2e8a:000c`. Let openocd
use it without root:

```bash
sudo cp udev/60-pico-debugprobe.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
```

### 3. Wire SWD

The four pads are next to the USB plug ([photo with labels](https://github.com/Archie3d/waveshare-usb-lora/blob/main/images/swd-pinout.jpg)):
3.3V (square), GND, SWDIO, SWCLK.

| Pico pin | Stick pad |
|---|---|
| GP2 (pin 4) | SWCLK |
| GP3 (pin 5) | SWDIO |
| GND (pin 3) | GND |

Leave 3.3V unconnected and power the stick from its own USB plug. Plug both into the computer.

### 4. Unlock, then write the bootloader and bridge

```bash
scripts/flash-swd.sh probe          # read-only. Expect IDCODE 0x...0410 and FLASH_OBR bit1 set (read-protected)
scripts/flash-swd.sh unlock         # ERASES Waveshare's firmware (asks you to type ERASE)
# unplug the stick's USB and plug it back in: the unlock only takes effect after a power cycle
scripts/flash-swd.sh probe          # FLASH_OBR bit1 now clear
scripts/flash-swd.sh bootloader ../ws-bootloader/bootloader.bin
scripts/flash-swd.sh bridge         # bridge/fw/wsbridge.bin at 0x08004000
```

Both LEDs blink once when the bridge starts. Check the whole chain (needs `pyserial`):

```bash
scripts/bridge-selftest.py
# hello: WSBR v1 TCXO / sync word 0x0740: 1424 / version 0x0320: SX1261 V2D 2D02 / round trip ~0.15 ms
```

The SWD wires can come off now. Later bridge updates go over USB: `scripts/flash-usb.sh ../ws-programmer/wsprog`
(hold the stick's button while plugging it in, so both LEDs light).

### 5. Build and install meshtasticd

```bash
meshtastic/build.sh                 # clones meshtastic/firmware at c39728e, patches, builds (several minutes)
install -D meshtastic/firmware/.pio/build/native-wsbridge/meshtasticd ~/.local/bin/meshtasticd-wsbridge
install -D -m644 meshtasticd/config.yaml ~/.config/meshtasticd-wsbridge/config.yaml
mkdir -p ~/.local/share/meshtasticd-wsbridge
install -D -m644 systemd/meshtasticd.service ~/.config/systemd/user/meshtasticd.service
```

Give the stick a stable name and start meshtasticd whenever it's plugged in. Edit the serial in the
rule first: `udevadm info -q property -n /dev/ttyACM0 | grep ID_SERIAL_SHORT`.

```bash
sudo cp udev/70-waveshare-lora.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=tty
systemctl --user daemon-reload && systemctl --user enable --now meshtasticd
journalctl --user -u meshtasticd -f      # look for "sx1262 init success" and "API server listen on TCP port 4403"
```

Your user needs to be in the `dialout` group.

### 6. Configure the node

```bash
uvx meshtastic --host localhost --set lora.region US        # your region; the node reboots
uvx meshtastic --host localhost --set-owner "My Node" --set-owner-short "MN"
uvx meshtastic --host localhost --set telemetry.device_update_interval 1800 --set telemetry.device_telemetry_enabled true
```

Apps connect over **TCP to `127.0.0.1:4403`**, not USB. As shipped, the service only accepts
connections from the same computer (`MESHTASTICD_API_BIND`, see below). The stick doesn't speak
the Meshtastic app protocol; meshtasticd does. meshtasticd serves one API client at a time.

### 7. Desktop GUI (optional)

The official [Meshtastic Desktop](https://meshtastic.org/docs/software/android/user/desktop/) app:
`flatpak install flathub org.meshtastic.MeshtasticDesktop`, then Connect → TCP/IP → `127.0.0.1`, port
`4403`. Two problems with the Flathub build as of 2.8.2 on KDE Plasma (Wayland):

- **Messages vanish on restart.** It keeps its database in `~/.meshtastic`, which the sandbox doesn't
  expose, so the database lives in a throwaway folder. Fix:
  `flatpak override --user --filesystem=~/.meshtastic:create org.meshtastic.MeshtasticDesktop`
- **The tray icon multiplies and doesn't work.** Closing the window hides the app in the tray, but its
  X11 (XEmbed) tray icon has a dead right-click menu and can't restore the window, and every menu
  launch starts another copy. `desktop/meshtastic-desktop` is a launcher that starts the app, leaves it
  alone if its window is open, or cleanly restarts it if it's hidden in the tray. Point a copy of
  the flatpak's `.desktop` file in `~/.local/share/applications/` at it.
  Running it as a native Wayland app doesn't help (see gotchas).

### 8. Optional: location from WiFi

The laptop has no GPS. `wifi-gps/mesh_wifi_gps.py` looks up the position from nearby WiFi via BeaconDB
every 5 minutes and serves it as a minimal gpsd (NMEA) on `127.0.0.1:2947`, which meshtasticd reads
through `GPS: GpsdHost`. It refuses fixes worse than 3 km (BeaconDB's cell/IP fallbacks), and
meshtasticd applies the channel's position precision before anything is sent. BeaconDB coverage is
patchy: contribute your area with [NeoStumbler](https://neostumbler.malkki.xyz/) first, or put a fixed
`{"lat":..,"lon":..}` in `fallback.json` next to the script. To turn it on: copy the script to
`~/.local/share/meshtasticd-wsbridge/`, install `systemd/mesh-wifi-gps.service`, uncomment `Wants=` in
the meshtasticd unit and `GPS:` in config.yaml, then
`--set position.gps_mode ENABLED` and `--ch-set module_settings.position_precision 14 --ch-index 0`
(~1.5 km).

## What the Meshtastic patch changes

`meshtastic/wsbridge.patch`, against `c39728e`:

- `[env:native-wsbridge]` in `variants/native/portduino/platformio.ini`. It builds the drop-in from
  `src/platform/portduino/wsbridge/` instead of pine64's libch341 (guarded by `MESHTASTIC_WSBRIDGE`).
- **Host battery** (`LinuxBatteryLevel` in `src/Power.cpp`): reads `/sys/class/power_supply` so a laptop
  reports its own charge and voltage like a handheld does (101 = on charger). Peripheral batteries
  are skipped, and an unreadable voltage counts as no battery, so the low-voltage shutdown can't trip.
- **DMShell compiled out** (`MESHTASTIC_EXCLUDE_DMSHELL`). On Linux, meshtasticd offers a login
  shell to API clients, and on a laptop the API port is reachable from whatever WiFi it's on (Fedora's
  default firewall zone opens 1025–65535).
- **API bind address** (`-Wl,--wrap=bind` + `bridge/host/api_bind.c`): the API on port 4403 gives full
  control of the node, and portduino's `WiFiServer` always binds every interface. With
  `MESHTASTICD_API_BIND=127.0.0.1` (set in `systemd/meshtasticd.service`), TCP listeners bound to
  `INADDR_ANY` are bound to that address instead, so only this computer can connect. Remove the line
  to let a phone on the LAN connect. UDP and explicit binds are untouched. (systemd's
  `IPAddressDeny=` would be neater, but it isn't enforced for user services.)
- `updateBatteryLevel` no-op on portduino (it logged "not implemented" every 20 s).

## Bridge protocol

`bridge/protocol.h`. Frames in both directions are `A5 CMD LEN_LO LEN_HI PAYLOAD`, at 2 Mbaud through
the CH343. Commands: HELLO, XFER, XFER_CS (CS asserted around one SPI transfer, so a RadioLib command
costs one round trip), WRITE/READ/MODE pin, plus an unsolicited `E1` on DIO1's rising edge. Pin
numbers mirror the MeshStick yaml: CS 0, RXen 1, Reset 2, Busy 4, IRQ 6. The host library delivers
interrupts from a separate thread with no lock held (RadioLib's handler talks to the radio itself),
opens the tty exclusively (`TIOCEXCL`) so apps probing serial ports can't inject bytes, and flags
unplugs immediately so meshtasticd's recovery and the udev-bound service take over.

## Gotchas found along the way

- **Stock firmware:** a transparent DTU that only talks to other Waveshare units. `+++` AT mode never
  answered on this unit. A `?>` reply to everything was its update mode, which a replug clears.
  Don't probe it with esptool or the Meshtastic CLI: whatever you send gets transmitted.
- **Chip:** Archie3d's notes say GD32F103C8; a photo in his repo shows a GD32F303CB. This unit is a
  GD32F103, medium density, 128 KB flash. `probe` tells you which you have.
- **Interrupts:** Archie3d's bootloader jumps to the app with interrupts masked (`cpsid i`). His
  firmware's FreeRTOS unmasks them; a bare-metal app must run `cpsie i` itself.
- **SPI mode fault:** on the GD32, `spi_init_master()` sets MSTR before software-NSS, and the SPI
  mode-faults straight back out of master. Write SSM|SSI|MSTR in one go.
- **RF switch:** PB4 high = RX path, low = TX. That's exactly meshtasticd's `RXen` semantics. DIO2 isn't
  used. The SX1262's TCXO is driven from DIO3 at 1.7 V.
- **Meshtastic Desktop on native Wayland:** JetBrains Runtime only honours
  `-Dawt.toolkit.name=WLToolkit` through `JDK_JAVA_OPTIONS`; its launcher overrides it in
  `JAVA_TOOL_OPTIONS`. Even then Skiko dies with "Can't lock DrawingSurface", including with
  `skiko.renderApi=SOFTWARE`. So it stays on X11/XWayland, which is why the launcher workaround exists.

## Build dependencies

On Fedora (from meshtastic's own `meshtasticd.spec.rpkg`, headless subset):

```bash
sudo dnf install arm-none-eabi-gcc-cs arm-none-eabi-newlib openocd golang platformio gcc-c++ python3-devel \
  'python3dist(protobuf)' 'python3dist(grpcio-tools)' 'pkgconfig(yaml-cpp)' 'pkgconfig(jsoncpp)' \
  'pkgconfig(libgpiod)' 'pkgconfig(bluez)' 'pkgconfig(sdbus-c++)' 'pkgconfig(libusb-1.0)' libi2c-devel \
  'pkgconfig(libuv)' 'pkgconfig(sqlite3)' 'pkgconfig(libcurl)' 'pkgconfig(avahi-client)' 'pkgconfig(openssl)'
```

The web UI needs ulfius/orcania/yder, which Fedora 44 doesn't package. It's optional; the apps and CLI
cover everything.

## License

GPL-3.0 (see `LICENSE`). The patch and the libpinedio-usb drop-in derive from meshtastic/firmware, which
is GPL-3.0. libopencm3 is LGPL-3.0. Archie3d's projects are referenced, not included.
