#!/bin/bash
# ============================================================================
# run_dos_attack.sh
#
# Simulate a DoS attack scenario on the ESP FPGA using Mini-era.
# Runs Mini-era in high-stress mode while all monitors record data,
# so you can observe how the OCSVM anomaly detector reacts.
#
# The "attack" works by overloading the SoC with legitimate-but-extreme
# compute work: max-length Viterbi messages, 16k FFT, 10x plan-and-control
# repeat, all lanes with obstacles. This creates NoC congestion, memory
# pressure, and accelerator contention that differs from normal patterns.
#
# Run ON the FPGA target (Linux on Ariane), not on the build host.
#
# Usage:
#   ./run_dos_attack.sh [duration_seconds]
#       default duration: 60 seconds
# ============================================================================
set -euo pipefail

DURATION="${1:-60}"
INTERVAL_MS=100
SAMPLES=$(( DURATION * 1000 / INTERVAL_MS ))
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
OUTDIR="/root/data"
mkdir -p "$OUTDIR"

CSV_OUT="$OUTDIR/dos_${TIMESTAMP}.csv"
MON_OUT="$OUTDIR/monitor_dos_${TIMESTAMP}.log"

echo "============================================"
echo " DoS Attack Simulation"
echo "============================================"
echo " Duration     : ${DURATION}s"
echo " Samples      : ${SAMPLES} @ ${INTERVAL_MS}ms"
echo " CSV output   : ${CSV_OUT}"
echo " Monitor log  : ${MON_OUT}"
echo " Attack mode  : Mini-era max stress"
echo "   -A         : obstacles in ALL 5 lanes"
echo "   -p 10      : 10x plan-and-control repeat"
echo "   -v 3       : max-length Viterbi messages"
echo "   -n 2       : msg per obstacle + global"
echo "   -W         : high-density obstacle world"
echo "============================================"

# ---- Step 1: Load OCSVM firmware onto Ibex (if not already running) ----
if [[ -x ./xheep_loader.exe ]]; then
    echo "[1/4] Loading OCSVM firmware onto Ibex..."
    ./xheep_loader.exe ./ocsvm_baremetal.bin || {
        echo "[WARN] xheep_loader failed — Ibex may already be running"
    }
else
    echo "[1/4] xheep_loader.exe not found — skipping Ibex firmware load"
fi

# ---- Step 2: Start background monitor ----
echo "[2/4] Starting background_monitor (OCSVM + SoCurity alerts)..."
./background_monitor.exe -o "$MON_OUT" &
BGMON_PID=$!
echo "  PID=$BGMON_PID"

# ---- Step 3: Start CSV data collector ----
echo "[3/4] Starting data collector (${SAMPLES} samples, label=dos)..."
./collect_esp_monitors.exe \
    -o "$CSV_OUT" \
    -n "$SAMPLES" \
    -i "$INTERVAL_MS" \
    -a dos \
    -f &
COLLECTOR_PID=$!
echo "  PID=$COLLECTOR_PID"

# ---- Brief baseline period (5 seconds of normal before attack) ----
echo ""
echo "  Collecting 5s baseline before attack..."
sleep 5

# ---- Step 4: Launch DoS attack (Mini-era at max stress) ----
echo "[4/4] Launching DoS attack (Mini-era max stress)..."
ATTACK_STEPS=$(( SAMPLES * 2 ))  # more steps than needed; will be killed

if [[ -x ./csim_main.exe ]]; then
    # Simulation mode with DoS parameters
    ./csim_main.exe \
        -s "$ATTACK_STEPS" \
        -A \
        -p 10 \
        -v 3 \
        -n 2 \
        -W dos_world.desc \
        -f 0 &
    ATTACK_PID=$!
    echo "  Attack PID=$ATTACK_PID"
elif [[ -x ./cmain.exe ]]; then
    # Trace mode fallback — use long trace with max Viterbi load
    ./cmain.exe -t traces/tt02.new -v 3 -n 2 -p 10 &
    ATTACK_PID=$!
    echo "  Attack PID=$ATTACK_PID (trace mode fallback)"
else
    echo "  [ERROR] No Mini-era executable found — cannot simulate attack"
    kill "$COLLECTOR_PID" "$BGMON_PID" 2>/dev/null
    exit 1
fi

echo ""
echo "Attack running. Monitoring for anomalies..."
echo "Watch for [!] ANOMALY lines from background_monitor:"
echo ""

# ---- Monitor for anomalies in real time ----
# Tail the background monitor output to see detections live
REMAINING=$(( DURATION - 5 ))
timeout "$REMAINING" tail -f "$MON_OUT" 2>/dev/null | grep --line-buffered -E "ANOMALY|SOCURITY_IRQ|STATS" &
TAIL_PID=$!

# Wait for data collection to complete
wait "$COLLECTOR_PID" 2>/dev/null || true

# ---- Cleanup ----
kill "$TAIL_PID" 2>/dev/null || true
kill "$ATTACK_PID" 2>/dev/null || true
wait "$ATTACK_PID" 2>/dev/null || true
kill "$BGMON_PID" 2>/dev/null || true
wait "$BGMON_PID" 2>/dev/null || true

echo ""
echo "============================================"
echo " Results"
echo "============================================"

# ---- Report anomaly statistics ----
CSV_LINES="$(wc -l < "$CSV_OUT")"
ANOMALY_COUNT="$(grep -c "ANOMALY" "$MON_OUT" 2>/dev/null || echo 0)"
TOTAL_SAMPLES="$(grep -c "^--- Sample" "$MON_OUT" 2>/dev/null || echo 0)"

echo " CSV data       : ${CSV_OUT} (${CSV_LINES} lines)"
echo " Monitor log    : ${MON_OUT}"
echo " Total samples  : ${TOTAL_SAMPLES}"
echo " Anomalies      : ${ANOMALY_COUNT}"
if [[ "$TOTAL_SAMPLES" -gt 0 ]]; then
    RATE=$(awk "BEGIN { printf \"%.1f\", 100.0 * $ANOMALY_COUNT / $TOTAL_SAMPLES }")
    echo " Detection rate : ${RATE}%"
fi

echo ""
echo " To analyze the results:"
echo "   # Compare normal vs DoS CSV data:"
echo "   python3 -c \"import pandas as pd; n=pd.read_csv('normal_*.csv'); d=pd.read_csv('dos_*.csv'); print(d.describe() - n.describe())\""
echo ""
echo " To view anomaly timeline:"
echo "   grep 'OCSVM' ${MON_OUT} | head -50"
echo ""
echo "Done."
