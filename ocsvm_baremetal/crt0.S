/* crt0.S — Minimal RISC-V RV32IMC startup for x-heep / Ibex bare-metal
 *
 * Task:
 *   1. Set stack pointer to top of DRAM (from linker script symbol _stack_top)
 *   2. Zero the BSS section
 * Place this in the same directory as ocsvm_baremetal.c.
 * The Makefile adds it before ocsvm_baremetal.c on the compiler command line.
 */

    .section .text.init, "ax", @progbits
    .global  _start
    .type    _start, @function

_start:
    /* ---- Set stack pointer ---- */
    la   sp, _stack_top          /* defined in link.ld */

    /* ---- Zero BSS ---- */
    la   t0, _bss_start
    la   t1, _bss_end
.bss_zero_loop:
    bge  t0, t1, .bss_done
    sw   zero, 0(t0)
    addi t0, t0, 4
    j    .bss_zero_loop
.bss_done:

    /* ---- Call main ---- */
    call main

    /* ---- Halt: main() should never return ---- */
.halt:
    wfi
    j    .halt

    .size _start, . - _start
