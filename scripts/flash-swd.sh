#!/bin/bash
# SWD steps with openocd. Probe: Pi Pico/Pico 2 running Raspberry Pi debugprobe (CMSIS-DAP):
# GP2 -> SWCLK, GP3 -> SWDIO, GND -> GND; the stick powered from its own USB (leave 3.3V unconnected).
# PROBE=stlink for an ST-Link instead.
#   probe                    read-only: chip ID and read protection
#   unlock                   remove read protection. ERASES THE STOCK FIRMWARE FOR GOOD. Replug after.
#   bootloader <file.bin>    write Archie3d's bootloader at 0x08000000
#   bridge [file.bin]        write the bridge firmware at 0x08004000 (default bridge/fw/wsbridge.bin)
# The MCU is a GD32F103 (STM32F1-compatible); CPUTAPID 0 accepts its non-ST debug ID.
set -euo pipefail
cd "$(dirname "$0")/.."
case "${PROBE:-cmsis-dap}" in
  stlink) IF=interface/stlink.cfg ;;
  *)      IF=interface/cmsis-dap.cfg ;;
esac
OCD=(openocd -f "$IF" -c "transport select swd" -c "adapter speed 1000" -c "set CPUTAPID 0" -f target/stm32f1x.cfg)
case "${1:-}" in
  probe)
    "${OCD[@]}" -c "init; reset halt; echo IDCODE=[format 0x%08x [read_memory 0xE0042000 32 1]]; echo FLASH_OBR=[format 0x%08x [read_memory 0x4002201C 32 1]] (bit1=1: read-protected); echo FLASH_WRPR=[format 0x%08x [read_memory 0x40022020 32 1]]; shutdown" ;;
  unlock)
    read -rp "This permanently erases the Waveshare firmware. Type ERASE to continue: " ok
    [ "$ok" = ERASE ] || exit 1
    "${OCD[@]}" -c "init; reset halt; stm32f1x unlock 0; reset halt; shutdown"
    echo ">> Unlocked and erased. Unplug the stick from USB and plug it back in before writing." ;;
  bootloader)
    "${OCD[@]}" -c "program ${2:?bootloader .bin} 0x08000000 verify reset; shutdown" ;;
  bridge)
    "${OCD[@]}" -c "program ${2:-bridge/fw/wsbridge.bin} 0x08004000 verify reset; shutdown" ;;
  *) sed -n 2,11p "$0"; exit 1 ;;
esac
