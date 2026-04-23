#!/bin/bash
# ============================================================================
# deploy_to_fpga.sh
#
# Packages all built binaries and scripts into a tarball ready to scp
# to the FPGA running Linux.
#
# Usage:
#   ./deploy_to_fpga.sh
#   scp anomaly_detection_pkg.tar.gz root@<fpga-ip>:/root/
#   ssh root@<fpga-ip> 'cd /root && tar xzf anomaly_detection_pkg.tar.gz'
# ============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJ_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_DIR="$PROJ_DIR/deploy_pkg"
PKG_NAME="anomaly_detection_pkg"

echo "Packaging binaries for FPGA deployment..."

rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/$PKG_NAME/traces"

# ---- Binaries ----
BINS=(
    "background_monitor/background_monitor.exe"
    "data_collector/collect_esp_monitors.exe"
    "xheep_loader/xheep_loader.exe"
    "ocsvm_baremetal/ocsvm_baremetal.bin"
)

for b in "${BINS[@]}"; do
    src="$PROJ_DIR/$b"
    if [[ -f "$src" ]]; then
        cp "$src" "$PKG_DIR/$PKG_NAME/"
        echo "  + $(basename "$src")"
    else
        echo "  [WARN] $b not found — build it first"
    fi
done

# ---- Mini-era binaries (if built) ----
MINI_ERA_DIR="$PROJ_DIR/mini-era"
if [[ -d "$MINI_ERA_DIR" ]]; then
    for exe in "$MINI_ERA_DIR"/*.exe; do
        [[ -f "$exe" ]] && cp "$exe" "$PKG_DIR/$PKG_NAME/" && echo "  + $(basename "$exe") (mini-era)"
    done
    # Copy traces
    if [[ -d "$MINI_ERA_DIR/traces" ]]; then
        cp "$MINI_ERA_DIR/traces"/*.new "$PKG_DIR/$PKG_NAME/traces/" 2>/dev/null || true
        cp "$MINI_ERA_DIR/traces"/*.dfn "$PKG_DIR/$PKG_NAME/traces/" 2>/dev/null || true
        echo "  + traces/"
    fi
fi

# ---- Scripts ----
cp "$SCRIPT_DIR/run_normal_baseline.sh" "$PKG_DIR/$PKG_NAME/"
cp "$SCRIPT_DIR/run_dos_attack.sh" "$PKG_DIR/$PKG_NAME/"
cp "$SCRIPT_DIR/dos_world.desc" "$PKG_DIR/$PKG_NAME/"

# ---- Package ----
cd "$PKG_DIR"
tar czf "$PROJ_DIR/${PKG_NAME}.tar.gz" "$PKG_NAME/"
SIZE=$(du -h "$PROJ_DIR/${PKG_NAME}.tar.gz" | cut -f1)

echo ""
echo "Package ready: ${PKG_NAME}.tar.gz ($SIZE)"
echo ""
echo "Deploy to FPGA:"
echo "  scp ${PKG_NAME}.tar.gz root@<fpga-ip>:/root/"
echo "  ssh root@<fpga-ip>"
echo "  cd /root && tar xzf ${PKG_NAME}.tar.gz && cd ${PKG_NAME}"
echo ""
echo "Then run:"
echo "  # 1. Load OCSVM onto Ibex:"
echo "  ./xheep_loader.exe ./ocsvm_baremetal.bin"
echo ""
echo "  # 2. Collect normal baseline (60s):"
echo "  ./run_normal_baseline.sh 60"
echo ""
echo "  # 3. Run DoS attack test (60s):"
echo "  ./run_dos_attack.sh 60"

rm -rf "$PKG_DIR"
