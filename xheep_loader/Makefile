##############################################################################
# xheep_loader/Makefile
#
# Builds xheep_loader.exe — Linux utility to DMA-load OCSVM firmware onto
# the x-heep (Ibex) accelerator tile via /dev/mem + /dev/contig.
##############################################################################

CROSS_COMPILE ?= riscv64-unknown-linux-gnu-
CC = $(CROSS_COMPILE)gcc

CFLAGS  = -O2 -g -Wall -Wextra
CFLAGS += -D_GNU_SOURCE

TARGET = xheep_loader.exe

.PHONY: all clean

all: $(TARGET)

xheep_loader.o: xheep_loader.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): xheep_loader.o
	$(CC) $< -o $@

clean:
	$(RM) xheep_loader.o $(TARGET)
