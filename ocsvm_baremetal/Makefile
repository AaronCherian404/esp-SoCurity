# ============================================================================
# Makefile for bare-metal OCSVM anomaly detector on x-heep / ESP (Ibex RV32IMC)
#
# Usage:
#   make                 — build the ELF
#   make clean           — remove build artifacts
#   make dump            — disassemble for inspection
#
# Before building, set RISCV32 to your RV32 toolchain install prefix, e.g.:
#   export RISCV32=/opt/riscv32
#
# Alternatively, if using x-heep's CMake build system, copy ocsvm_baremetal.c,
# ocsvm_inference.h, and ocsvm_model.h into:
#   x-heep/sw/applications/ocsvm_detector/
# and use x-heep's `cmake --build` instead.
# ============================================================================

# Toolchain
RISCV32     ?= $(RISCV32IMC)
PREFIX      ?= $(RISCV32)/bin/riscv32-unknown-elf-
CC           = $(PREFIX)gcc
OBJDUMP      = $(PREFIX)objdump
OBJCOPY      = $(PREFIX)objcopy
SIZE         = $(PREFIX)size

# Architecture flags for Ibex (RV32IMC, no FPU)
ARCH_FLAGS   = -march=rv32imc -mabi=ilp32

# Compiler flags
CFLAGS       = $(ARCH_FLAGS) -O2 -Wall -Wextra
CFLAGS      += -ffreestanding -nostdlib -nostartfiles
CFLAGS      += -ffunction-sections -fdata-sections
CFLAGS      += -I.

# Optional SoC-generated include path (for soc_defs.h address macros).
SOC_DEFS_DIR ?=
ifneq ($(strip $(SOC_DEFS_DIR)),)
CFLAGS      += -I$(SOC_DEFS_DIR)
endif

# Optional macro overrides, e.g.:
# make SOCURITY_DEFS='-DESP_MON_BASE_ADDR=0x60090000UL -DSOCURITY_ALERT_BASE=0x40010000UL'
SOCURITY_DEFS ?=
ifneq ($(strip $(SOCURITY_DEFS)),)
CFLAGS      += $(SOCURITY_DEFS)
endif

# Linker flags
LDFLAGS      = -T link.ld
LDFLAGS     += -Wl,--gc-sections
LDFLAGS     += -nostdlib -nostartfiles
LDFLAGS     += -lgcc   # soft-float helpers (__mulsf3, __addsf3, etc.)
# Note: We use fast_expf() in ocsvm_inference.h, so -lm is NOT required.
# If you switch to libm's expf(), add: LDFLAGS += -lm

# Source files (crt0.S provides _start, BSS zeroing, stack init)
SRC          = crt0.S ocsvm_baremetal.c
HEADERS      = ocsvm_model.h ocsvm_inference.h ocsvm_platform.h

# Output
TARGET       = ocsvm_baremetal
ELF          = $(TARGET).elf
BIN          = $(TARGET).bin
HEX          = $(TARGET).hex

# ============================================================================
# Build targets
# ============================================================================

.PHONY: all clean dump size

all: $(ELF) $(BIN) size

$(ELF): $(SRC) $(HEADERS) link.ld
	$(CC) $(CFLAGS) $(SRC) -o $@ $(LDFLAGS)

$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

dump: $(ELF)
	$(OBJDUMP) -d -S $< > $(TARGET).dump

size: $(ELF)
	$(SIZE) $<

clean:
	$(RM) $(ELF) $(BIN) $(HEX) $(TARGET).dump

# ============================================================================
# x-heep integration helper
#
# To integrate with x-heep's CMake build system instead of this standalone
# Makefile, create a CMakeLists.txt in x-heep/sw/applications/ocsvm_detector/:
#
#   add_executable(ocsvm_detector ocsvm_baremetal.c)
#   target_include_directories(ocsvm_detector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
#
# Then build with:
#   cd x-heep/build
#   cmake .. -DAPPLICATION=ocsvm_detector
#   make
# ============================================================================
