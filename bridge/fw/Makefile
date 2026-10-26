# Bridge firmware for the Waveshare USB-TO-LoRa (GD32F103 + SX1262).
# Linked at 0x08004000 for Archie3d's USB bootloader. WITH_TCXO=0 for the -B (crystal) variant.
PREFIX  ?= arm-none-eabi
CC      := $(PREFIX)-gcc
OBJCOPY := $(PREFIX)-objcopy
WITH_TCXO ?= 1
OPENCM3 := libopencm3/lib/libopencm3_stm32f1.a

CFLAGS  = -Os -g -Wall -Wextra -std=gnu11 -mthumb -mcpu=cortex-m3 -msoft-float -mfix-cortex-m3-ldrd \
          -fno-common -ffunction-sections -fdata-sections -DSTM32F1 -DWITH_TCXO=$(WITH_TCXO) -Ilibopencm3/include
LDFLAGS = -T wsbridge.ld -Llibopencm3/lib --specs=nosys.specs -nostartfiles -Wl,--gc-sections \
          -Wl,--print-memory-usage -mthumb -mcpu=cortex-m3 -msoft-float

all: wsbridge.bin

$(OPENCM3):
	$(MAKE) -C libopencm3 TARGETS=stm32/f1

wsbridge.elf: main.c ../protocol.h wsbridge.ld Makefile $(OPENCM3)
	$(CC) $(CFLAGS) main.c -o $@ $(LDFLAGS) -lopencm3_stm32f1

wsbridge.bin: wsbridge.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f wsbridge.elf wsbridge.bin
