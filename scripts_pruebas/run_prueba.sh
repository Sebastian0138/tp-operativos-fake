#!/bin/bash
# Runner de pruebas oficiales.
# Uso: run_prueba.sh <km_cfg> <ks_cfg> <script_inicial> <duracion_s> <sticks: "cfg:size cfg:size ..."> [archivo_stdin]
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export LIBRARY_PATH=/tmp/rl

KM_CFG=$1; KS_CFG=$2; SCRIPT=$3; DUR=$4; STICKS=$5; STDIN_FILE=$6

pkill -f "[b]in/kernel_memory" 2>/dev/null
pkill -f "[b]in/memory_stick" 2>/dev/null
pkill -f "[b]in/kernel_scheduler" 2>/dev/null
pkill -f "[b]in/cpu " 2>/dev/null
pkill -f "[b]in/io " 2>/dev/null
pkill -f "[b]in/swap" 2>/dev/null
sleep 1

rm -f $BASE/kernel_memory/logs/*.log $BASE/kernel_scheduler/logs/*.log \
      $BASE/cpu/logs/*.log $BASE/io/logs/*.log $BASE/memory_stick/logs/*.log $BASE/swap/logs/*.log

cd $BASE/kernel_memory && setsid ./bin/kernel_memory config/$KM_CFG > /dev/null 2>&1 &
sleep 0.7

for s in $STICKS; do
    cfg="${s%%:*}"; size="${s##*:}"
    cd $BASE/memory_stick && setsid ./bin/memory_stick config/$cfg "$size" > /dev/null 2>&1 &
    sleep 0.4
done

cd $BASE/swap && setsid ./bin/swap config/swap.cfg > /dev/null 2>&1 &
sleep 0.3

cd $BASE/kernel_scheduler && setsid ./bin/kernel_scheduler config/$KS_CFG "$SCRIPT" > /dev/null 2>&1 &
sleep 0.6

cd $BASE/io
setsid ./bin/io config/io.cfg SLEEP  > /dev/null 2>&1 &
setsid ./bin/io config/io.cfg STDOUT > /dev/null 2>&1 &
if [ -n "$STDIN_FILE" ]; then
    setsid ./bin/io config/io.cfg STDIN < "$STDIN_FILE" > /dev/null 2>&1 &
else
    setsid ./bin/io config/io.cfg STDIN < /dev/null > /dev/null 2>&1 &
fi
sleep 0.4

cd $BASE/cpu && setsid ./bin/cpu config/cpu.cfg 1 > /dev/null 2>&1 &

sleep "$DUR"
echo "FIN_PRUEBA (${DUR}s)"
exit 0
