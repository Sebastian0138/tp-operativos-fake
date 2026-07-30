#!/bin/bash
# Bateria completa de pruebas oficiales. Guarda los logs de cada prueba en
# scratchpad/resultados/<nombre>/ para analisis posterior.
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRATCH="$BASE/scripts_pruebas"
RES=$SCRATCH/resultados
mkdir -p $RES

correr() {
    local nombre=$1; shift
    echo ">>> [$(date +%H:%M:%S)] Prueba: $nombre"
    if ! bash "$SCRATCH/run_prueba.sh" "$@"; then
        echo ">>> [$(date +%H:%M:%S)] $nombre FALLO AL ARRANCAR"
    fi
    mkdir -p $RES/$nombre
    cp $BASE/kernel_scheduler/logs/kernel_scheduler.log $RES/$nombre/ks.log 2>/dev/null
    cp $BASE/kernel_memory/logs/kernel_memory.log $RES/$nombre/km.log 2>/dev/null
    cp $BASE/cpu/logs/cpu.log $RES/$nombre/cpu.log 2>/dev/null
    cp $BASE/io/logs/io.log $RES/$nombre/io.log 2>/dev/null
    cp $BASE/memory_stick/logs/memory_stick.log $RES/$nombre/stick.log 2>/dev/null
    cp $BASE/swap/logs/swap.log $RES/$nombre/swap.log 2>/dev/null
    echo ">>> [$(date +%H:%M:%S)] $nombre guardada"
}

# Los numeros son TOPES, no duraciones: run_prueba.sh corta antes por quiescencia.
# PCP y PHP no terminan nunca (SET PC 0 / JNZ sin decremento), ahi si se usa el tope.
correr pmp        km_pmp.cfg  ks_pmp.cfg  PMP.prc          420 "stick_p1.cfg:16 stick_p2.cfg:16 stick_p3.cfg:32 stick_p4.cfg:64" $SCRATCH/stdin_pmp.txt
correr base1      km_base.cfg ks_base.cfg PLANI_PRE_0.prc  300 "stick_p1.cfg:256"
correr base3      km_base.cfg ks_base.cfg MEMORIA_PRE_0.prc 300 "stick_p1.cfg:256" $SCRATCH/stdin_input.txt
correr pcp        km_pcp.cfg  ks_pcp.cfg  PCP.prc          120 "stick_p1.cfg:256"
correr mem_best   km_mem_best.cfg  ks_mem.cfg PLANI_MEM.prc 300 "stick_p1.cfg:16 stick_p2.cfg:32 stick_p3.cfg:64 stick_p4.cfg:128"
correr mem_worst  km_mem_worst.cfg ks_mem.cfg PLANI_MEM.prc 300 "stick_p1.cfg:16 stick_p2.cfg:32 stick_p3.cfg:64 stick_p4.cfg:128"
correr php        km_php.cfg  ks_php.cfg  PHP.prc          420 "stick_p1.cfg:16 stick_p2.cfg:16"

# Limpieza final
pkill -f "[b]in/kernel_memory"; pkill -f "[b]in/memory_stick"; pkill -f "[b]in/kernel_scheduler"
pkill -f "[b]in/cpu "; pkill -f "[b]in/io "; pkill -f "[b]in/swap"
echo "BATERIA_COMPLETA"
