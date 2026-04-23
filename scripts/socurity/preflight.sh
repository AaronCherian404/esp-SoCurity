#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ESP_HEEP_DIR="$ROOT_DIR/esp-heep"
VC707_DIR="$ESP_HEEP_DIR/socs/xilinx-vc707-xc7vx485t"

print_header() {
  echo
  echo "============================================================"
  echo "$1"
  echo "============================================================"
}

require_file() {
  local path="$1"
  if [[ ! -e "$path" ]]; then
    echo "[ERROR] Missing required path: $path"
    exit 1
  fi
}

print_header "SoCurity Preflight"
echo "Date: $(date -Iseconds)"
echo "Repo: $ROOT_DIR"

require_file "$ROOT_DIR/.git"
require_file "$ESP_HEEP_DIR"
require_file "$VC707_DIR"
require_file "$ROOT_DIR/ocsvm_baremetal/ocsvm_baremetal.c"
require_file "$ROOT_DIR/background_monitor/background_monitor.c"
require_file "$ROOT_DIR/SoCurity.pdf"

print_header "Parent Repository Status"
(
  cd "$ROOT_DIR"
  git status --short || true
)

print_header "ESP-HEEP Status"
(
  cd "$ESP_HEEP_DIR"
  git status --short || true
)

print_header "Key File Presence"
for p in \
  "$ESP_HEEP_DIR/accelerators/third-party/xheep/Makefile" \
  "$ESP_HEEP_DIR/accelerators/third-party/xheep/xheep_wrapper.v" \
  "$ESP_HEEP_DIR/accelerators/third-party/xheep/ip/x-heep/configs/esp_heep.hjson" \
  "$ESP_HEEP_DIR/rtl/sockets/proxy/noc2apb.vhd" \
  "$ESP_HEEP_DIR/soft/common/drivers/common/include/monitors.h" \
  "$VC707_DIR/Makefile"; do
  if [[ -e "$p" ]]; then
    echo "[OK] $p"
  else
    echo "[MISSING] $p"
    exit 1
  fi
done

print_header "VC707 Make Help Smoke Check"
(
  cd "$VC707_DIR"
  make help >/tmp/socurity_make_help.txt 2>&1 || true
  if grep -q "::ESP Targets::" /tmp/socurity_make_help.txt; then
    echo "[OK] make help produced ESP targets"
  else
    echo "[WARN] make help did not show expected banner"
  fi
  if grep -q "NV_NVDLA.verilog" /tmp/socurity_make_help.txt; then
    echo "[NOTE] Known warning present: missing NV_NVDLA.verilog"
  fi
)

print_header "Preflight Result"
echo "Preflight completed. Review warnings above before Phase 1 changes."
