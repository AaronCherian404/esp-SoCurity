CROSS_COMPILE ?= riscv64-unknown-linux-gnu-

ESP_ROOT ?= $(realpath $(CURDIR)/../esp-heep)
ESP_DRIVERS ?= $(ESP_ROOT)/soft/common/drivers
ESP_DRV_COMMON  ?= $(ESP_DRIVERS)/common
DESIGN_PATH ?= $(ESP_ROOT)/socs/xilinx-vc707-xc7vx485t

CC = gcc

ESP_BUILD_DRIVERS = esp-build/drivers

INCDIR ?=
INCDIR += -I./include
INCDIR += -I$(ESP_DRIVERS)/common/include
INCDIR += -I$(ESP_DRIVERS)/linux/include
INCDIR += -I$(DESIGN_PATH)/socgen/esp

CFLAGS ?= -O2 -g
CFLAGS += $(INCDIR)
CFLAGS += -D_POSIX_C_SOURCE=199309L
CFLAGS += -D_XOPEN_SOURCE=500
CFLAGS += -DLINUX

LDLIBS ?=
LDLIBS += -L$(ESP_BUILD_DRIVERS)/monitors
LDLIBS += -L./

LDFLAGS ?=
LDFLAGS += -lmonitors
LDFLAGS += -lm
LDFLAGS += -lpthread

TARGET=background_monitor.exe

.PHONY: all esp-libs clean clobber

all: esp-libs $(TARGET)

esp-libs: $(ESP_BUILD_DRIVERS)/monitors/libmonitors.a

$(ESP_BUILD_DRIVERS)/monitors/libmonitors.a:
	@mkdir -p $(ESP_BUILD_DRIVERS)/monitors
	$(MAKE) -C $(ESP_DRV_COMMON)/monitors \
	    CROSS_COMPILE=$(CROSS_COMPILE) \
	    BUILD_PATH=$(abspath $(ESP_BUILD_DRIVERS)/monitors) \
	    DESIGN_PATH=$(DESIGN_PATH) \
	    MODE=LINUX

background_monitor.o: background_monitor.c
	$(CROSS_COMPILE)$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): background_monitor.o
	$(CROSS_COMPILE)$(CC) $< -o $@ $(LDLIBS) $(LDFLAGS)

clean:
	$(RM) background_monitor.o

clobber: clean
	$(RM) -rf esp-build
	$(RM) $(TARGET)