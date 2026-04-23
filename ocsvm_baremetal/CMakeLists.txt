# CMakeLists.txt — OCSVM anomaly detector for x-heep / ESP-HEEP
#
# USAGE (x-heep CMake build):
#   Place this directory in x-heep/sw/applications/ocsvm_detector/
#   (or in esp-heep/soft/common/apps/baremetal/ocsvm_detector/ for ESP-HEEP)
#
#   Then build:
#     cd <x-heep-root>/build
#     cmake .. -DAPPLICATION=ocsvm_detector [-DTARGET=pynq-z2|sim]
#     make
#
# The x-heep build system sets CMAKE_C_COMPILER to the RV32IMC toolchain,
# provides the linker script, and links startup/runtime code automatically.
# You do NOT need crt0.S when using the CMake flow.

cmake_minimum_required(VERSION 3.13)

# Application name — must match the directory name for x-heep discovery
set(APP_NAME ocsvm_detector)

project(${APP_NAME} C ASM)

# Sources
set(SOURCES
    ocsvm_baremetal.c
)

# Headers are pulled in via #include — no separate compile step needed
set(HEADERS
    ocsvm_model.h
    ocsvm_inference.h
)

add_executable(${APP_NAME} ${SOURCES})

# Include the directory containing the headers
target_include_directories(${APP_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Architecture flags for Ibex (RV32IMC, no FPU)
target_compile_options(${APP_NAME} PRIVATE
    -march=rv32imc
    -mabi=ilp32
    -O2
    -Wall
    -Wextra
    -ffreestanding
    -ffunction-sections
    -fdata-sections
)

# No standard library (bare-metal, no OS)
target_link_options(${APP_NAME} PRIVATE
    -march=rv32imc
    -mabi=ilp32
    -nostdlib
    -nostartfiles
    -Wl,--gc-sections
)

# x-heep provides its own linker script via the parent CMake infrastructure.
# If building standalone (not via x-heep CMake), pass -T link.ld via:
#   cmake .. -DCMAKE_EXE_LINKER_FLAGS="-T ${CMAKE_SOURCE_DIR}/link.ld"
