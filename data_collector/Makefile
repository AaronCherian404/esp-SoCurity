##############################################################################
# data_collector/Makefile
#
# Builds collect_esp_monitors — the ESP hardware-monitor CSV data collector
# for OCSVM training data.
#
# Key variables (override on command line or in environment):
#   CROSS_COMPILE   RISC-V Linux cross-compiler prefix
#   ESP_ROOT        Path to the esp-heep checkout
#   DESIGN_PATH     SoC-specific directory containing socgen/esp/soc_defs.h
#   CSV_NTILES      Number of tile columns in the output CSV  (default 9)
#   CSV_NACC        Number of accelerator columns             (default 6)
##############################################################################

CROSS_COMPILE ?= riscv64-unknown-linux-gnu-

ESP_ROOT    ?= $(realpath $(CURDIR)/../esp-heep)
ESP_DRIVERS ?= $(ESP_ROOT)/soft/common/drivers
ESP_DRV_COMMON ?= $(ESP_DRIVERS)/common
DESIGN_PATH ?= $(ESP_ROOT)/socs/xilinx-vc707-xc7vx485t

CC  = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++

CSV_NTILES ?= 9
CSV_NACC   ?= 6

ESP_BUILD_DRIVERS = esp-build/drivers

INCDIR  =
INCDIR += -I$(ESP_DRV_COMMON)/include
INCDIR += -I$(ESP_DRIVERS)/common/include
INCDIR += -I$(ESP_DRIVERS)/linux/include
INCDIR += -I$(DESIGN_PATH)/socgen/esp

CFLAGS  = -O2 -g -Wall -Wextra
CFLAGS += $(INCDIR)
CFLAGS += -D_POSIX_C_SOURCE=199309L
CFLAGS += -D_XOPEN_SOURCE=500
CFLAGS += -DLINUX
CFLAGS += -DCSV_NTILES=$(CSV_NTILES)
CFLAGS += -DCSV_NACC=$(CSV_NACC)

LDLIBS  =
LDLIBS += -L$(ESP_BUILD_DRIVERS)/monitors
LDLIBS += -lmonitors
LDLIBS += -lm
LDLIBS += -lpthread

TARGET = collect_esp_monitors.exe

.PHONY: all esp-libs clean clobber

all: esp-libs $(TARGET)

# ---- build libmonitors if not already present ---------------------------
esp-libs: $(ESP_BUILD_DRIVERS)/monitors/libmonitors.a

$(ESP_BUILD_DRIVERS)/monitors/libmonitors.a:
	@mkdir -p $(ESP_BUILD_DRIVERS)/monitors
	$(MAKE) -C $(ESP_DRV_COMMON)/monitors \
	    CROSS_COMPILE=$(CROSS_COMPILE) \
	    BUILD_PATH=$(abspath $(ESP_BUILD_DRIVERS)/monitors) \
	    DESIGN_PATH=$(DESIGN_PATH) \
	    MODE=LINUX

# ---- compile and link collect_esp_monitors ------------------------------
collect_esp_monitors.o: collect_esp_monitors.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): collect_esp_monitors.o
	$(CXX) $< -o $@ -L$(ESP_BUILD_DRIVERS)/monitors -lmonitors -lm -lpthread

# ---- housekeeping -------------------------------------------------------
clean:
	$(RM) collect_esp_monitors.o $(TARGET)

clobber: clean
	$(RM) -rf esp-build
