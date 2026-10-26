/* GD32F103 (STM32F103-compatible). The bridge runs behind a USB bootloader that occupies the
   first 16 KB of flash, so the application starts at 0x08004000. 20 KB SRAM. */
MEMORY
{
    rom (rx)  : ORIGIN = 0x08004000, LENGTH = 112K
    ram (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

INCLUDE cortex-m-generic.ld
