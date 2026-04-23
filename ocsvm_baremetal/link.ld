/*
 * link.ld
 *
 * Minimal linker script for bare-metal OCSVM on x-heep / Ibex.
 *
 * Memory layout — ADJUST for your specific FPGA/SoC configuration:
 *   - x-heep typically maps SRAM starting at 0x00000000 or 0x80000000
 *   - Check x-heep/hw/ip/sram/data/ for the exact memory map
 *   - The sizes below are conservative defaults
 *
 * If using x-heep's CMake build system, this file is NOT needed —
 * x-heep provides its own linker script.
 */

OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY
{
    /* x-heep esp_heep config: 2x64KB, code section 0x0000–0xE7FF */
    IRAM (rx)  : ORIGIN = 0x00000000, LENGTH = 0xE800

    /* data section 0xE800–0x1FFFF (remainder of 128KB) */
    DRAM (rwx) : ORIGIN = 0x0000E800, LENGTH = 0x11800
}

SECTIONS
{
    /* ---- Code ---- */
    .text : ALIGN(4)
    {
        *(.text.init)       /* Reset vector / _start */
        *(.text .text.*)
    } > IRAM

    /* ---- Read-only data (model weights live here) ---- */
    .rodata : ALIGN(4)
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
    } > DRAM

    /* ---- Initialized data ---- */
    .data : ALIGN(4)
    {
        _data_start = .;
        *(.data .data.*)
        *(.sdata .sdata.*)
        _data_end = .;
    } > DRAM

    /* ---- BSS (zero-initialized) ---- */
    .bss (NOLOAD) : ALIGN(4)
    {
        _bss_start = .;
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
        _bss_end = .;
    } > DRAM

    /* ---- Stack (grows downward from end of DRAM) ---- */
    _stack_top = ORIGIN(DRAM) + LENGTH(DRAM);

    /* Discard unwanted sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note.*)
        *(.eh_frame*)
    }
}

/*
 * Minimal startup code reference (put in a crt0.S if needed):
 *
 *   .section .text.init
 *   .global _start
 *   _start:
 *       la sp, _stack_top
 *       # Zero BSS
 *       la t0, _bss_start
 *       la t1, _bss_end
 *   1:  bge t0, t1, 2f
 *       sw zero, 0(t0)
 *       addi t0, t0, 4
 *       j 1b
 *   2:
 *       call main
 *       # Halt
 *   3:  wfi
 *       j 3b
 */
