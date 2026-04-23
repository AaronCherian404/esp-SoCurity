#!/bin/bash
# ============================================================================
# run_normal_baseline.sh
#
# Collect "normal" baseline training data on the ESP FPGA.
# Runs Mini-era at normal intensity while collect_esp_monitors records
# hardware counter CSVs.
#
# Run ON the FPGA target (Linux on Ariane), not on the build host.
#
# Usage:
#   ./run_normal_baseline.sh [duration_seconds]
#       default duration: 60 seconds
# ============================================================================
set -euo pipefail

DURATION="${1:-60}"
INTERVAL_MS=100
SAMPLES=$(( DURATION * 1000 / INTERVAL_MS ))
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="/root/data"
mkdir -p "$OUTDIR"

CSV_OUT="$OUTDIR/normal_${TIMESTAMP}.csv"
MON_OUT="$OUTDIR/monitor_normal_${TIMESTAMP}.log"

echo "============================================"
echo " Normal Baseline Data Collection"
echo "============================================"
echo " Duration     : ${DURATION}s"
echo " Samples      : ${SAMPLES} @ ${INTERVAL_MS}ms"
echo " CSV output   : ${CSV_OUT}"
echo " Monitor log  : ${MON_OUT}"
echo "============================================"

# ---- Step 1: Load OCSVM firmware onto Ibex (if not already running) ----
if [[ -x ./xheep_loader.exe ]]; then
    echo "[1/4] Loading OCSVM firmware onto Ibex..."
    ./xheep_loader.exe ./ocsvm_baremetal.bin || {
        echo "[WARN] xheep_loader failed — Ibex may already be running or not present"
    }
else
    echo "[1/4] xheep_loader.exe not found — skipping Ibex firmware load"
fi

# ---- Step 2: Start background monitor (OCSVM + SoCurity alert polling) ----
echo "[2/4] Starting background_monitor..."
./background_monitor.exe -o "$MON_OUT" &
BGMON_PID=$!
echo "  PID=$BGMON_PID"

# ---- Step 3: Start CSV data collector ----
echo "[3/4] Starting data collector (${SAMPLES} samples)..."
./collect_esp_monitors.exe \
    -o "$CSV_OUT" \
    -n "$SAMPLES" \
    -i "$INTERVAL_MS" \
    -a normal &
COLLECTOR_PID=$!
echo "  PID=$COLLECTOR_PID"

# ---- Step 4: Run Mini-era as the normal workload ----
echo "[4/4] Running Mini-era (normal workload)..."
if [[ -x ./cmain.exe ]]; then
    # Trace-driven normal operation
    ./cmain.exe -t traces/tt02.new -v 0 -n 0 &
    MINIERA_PID=$!
    echo "  Mini-era PID=$MINIERA_PID"
elif [[ -x ./csim_main.exe ]]; then
    # Simulation mode, moderate load
    ./csim_main.exe -s "$SAMPLES" -v 0 -n 0 -p 1 &
    MINIERA_PID=$!
    echo "  Mini-era PID=$MINIERA_PID"
else
    echo "  [WARN] No Mini-era executable found — collecting idle baseline"
    MINIERA_PID=""
fi

# ---- Wait for data collection to complete ----
echo ""
echo "Collecting data for ${DURATION}s..."
wait "$COLLECTOR_PID" 2>/dev/null || true
echo "Data collection complete."

# ---- Cleanup ----
if [[ -n "${MINIERA_PID:-}" ]]; then
    kill "$MINIERA_PID" 2>/dev/null || true
    wait "$MINIERA_PID" 2>/dev/null || true
fi

kill "$BGMON_PID" 2>/dev/null || true
wait "$BGMON_PID" 2>/dev/null || true

echo ""
echo "Results:"
echo "  CSV data    : ${CSV_OUT} ($(wc -l < "$CSV_OUT") lines)"
echo "  Monitor log : ${MON_OUT} ($(wc -c < "$MON_OUT") bytes)"
echo "Done."
