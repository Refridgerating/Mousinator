/*
 * The factory BTT SD-card bootloader occupies 0x08000000..0x08006fff.
 * STM32F103RC provides 256 KiB flash and 48 KiB SRAM.
 */
MEMORY
{
	rom (rx)  : ORIGIN = 0x08007000, LENGTH = 0x39000
	ram (rwx) : ORIGIN = 0x20000000, LENGTH = 48K
}

INCLUDE cortex-m-generic.ld

ASSERT(ADDR(.text) == ORIGIN(rom), "vector table is not at application origin")
ASSERT((_etext + SIZEOF(.data)) <= (ORIGIN(rom) + LENGTH(rom)),
	"application exceeds flash region")
ASSERT(end <= (ORIGIN(ram) + LENGTH(ram)), "application exceeds SRAM")

