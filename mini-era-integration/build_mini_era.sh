#!/bin/bash
# ============================================================================
# build_mini_era.sh
#
# Clones IBM Mini-era, configures it for ESP RISC-V software-only mode,
# and cross-compiles for the VC707 FPGA target.
#
# Usage:
#   ./build_mini_era.sh [--hw]
#       --hw : enable FFT + Viterbi hardware accelerator offload
#              (requires ESP accelerator IPs in bitstream)
#
# Prerequisites:
#   - riscv64-unknown-linux-gnu-gcc in PATH
#   - ESP_ROOT set (or defaults to ../esp-heep)
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MINI_ERA_DIR="$PROJ_DIR/mini-era"

ESP_ROOT="${ESP_ROOT:-$PROJ_DIR/esp-heep}"
export ESP_ROOT

RISCV_BIN_DIR="${RISCV_BIN_DIR:-$(dirname "$(command -v riscv64-unknown-linux-gnu-gcc)")}"
export RISCV_BIN_DIR

USE_HW=0
if [[ "${1:-}" == "--hw" ]]; then
    USE_HW=1
fi

# ---- Clone if not present ----
if [[ ! -d "$MINI_ERA_DIR" ]]; then
    echo "[build] Cloning IBM Mini-era..."
    git clone https://github.com/IBM/mini-era.git "$MINI_ERA_DIR"
else
    echo "[build] Mini-era already present at $MINI_ERA_DIR"
fi

cd "$MINI_ERA_DIR"

# ---- Generate hardware_config symlink ----
# Point to the appropriate build config
if [[ $USE_HW -eq 1 ]]; then
    echo "[build] Using RISC-V Hardware accelerator config"
    ln -sf build_riscvHW_config hardware_config
else
    echo "[build] Using RISC-V Software-only config"
    ln -sf build_riscvSW_config hardware_config
fi

# ---- Patch setup_paths.sh ----
cat > setup_paths.sh <<EOF
#!/bin/bash
export RISCV_BIN_DIR="$RISCV_BIN_DIR"
export ESP_ROOT="$ESP_ROOT"
export PATH=\$PATH:\$RISCV_BIN_DIR
EOF
echo "[build] Patched setup_paths.sh with:"
echo "  RISCV_BIN_DIR=$RISCV_BIN_DIR"
echo "  ESP_ROOT=$ESP_ROOT"

# shellcheck source=/dev/null
source setup_paths.sh

# ---- Verify toolchain ----
if ! command -v riscv64-unknown-linux-gnu-gcc &>/dev/null; then
    echo "[ERROR] riscv64-unknown-linux-gnu-gcc not found in PATH"
    exit 1
fi
echo "[build] Toolchain: $(riscv64-unknown-linux-gnu-gcc --version | head -1)"

# ---- Build C-only version (no Keras/Python dependency) ----
echo "[build] Building Mini-era (C-only, no CNN)..."
make allclean 2>/dev/null || true
make cver

echo ""
echo "[build] Build complete. Available executables:"
ls -la *.exe 2>/dev/null || echo "  (check for build errors above)"
echo ""
echo "[build] To run on FPGA (after scp to SoC):"
echo "  # Normal operation:"
echo "  ./cmain.exe -t traces/tt02.new -v 0 -n 0"
echo ""
echo "  # DoS-like stress (heavy load):"
echo "  ./csim_main.exe -s 5000 -A -p 10 -v 3 -n 2 -f 0"
