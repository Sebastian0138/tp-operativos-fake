#!/bin/bash
# Prueba unitaria: Atencion de Syscalls -- Mutex, HERENCIA DE PRIORIDADES.
#
# Inversion de prioridades: PID 1 (prioridad 1, baja) toma el mutex m1 y lo retiene.
# PID 2 (prioridad 0, alta) intenta tomarlo y queda bloqueado esperando a un proceso
# menos prioritario. Ante ese evento, PID 1 hereda temporalmente la prioridad 0, y
# la devuelve al liberar el mutex.
#
# PID 1 cede la CPU con SLEEPs cortos en vez de hacer trabajo puro: si no, con
# QUEUE_PREEMPTION=FALSE, PID 2 nunca conseguiria CPU para pedir el mutex.
#
# Duracion: ~12 segundos. No hay que tocar nada.
set -u
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAM_STICK=${1:-256}

pkill -f "[b]in/cpu "             2>/dev/null
pkill -f "[b]in/io "              2>/dev/null
pkill -f "[b]in/kernel_scheduler" 2>/dev/null
pkill -f "[b]in/swap"             2>/dev/null
pkill -f "[b]in/memory_stick"     2>/dev/null
pkill -f "[b]in/kernel_memory"    2>/dev/null
sleep 1

rm -f "$BASE"/kernel_memory/logs/*.log "$BASE"/kernel_scheduler/logs/*.log \
      "$BASE"/cpu/logs/*.log "$BASE"/io/logs/*.log \
      "$BASE"/memory_stick/logs/*.log "$BASE"/swap/logs/*.log

term() {
    local titulo="$1"; local dir="$2"; shift 2
    gnome-terminal --title="$titulo" -- bash -c \
        "cd '$dir' && $*; echo; echo '--- $titulo TERMINADO ---'; exec bash"
}

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/syscalls_mutex_herencia/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/syscalls_mutex_herencia/ks_sys.cfg test_padre"
sleep 2
term "5 IO SLEEP 1"       "$BASE/io"  "./bin/io config/io.cfg SLEEP"
sleep 1
term "5 IO SLEEP 2"       "$BASE/io"  "./bin/io config/io.cfg SLEEP"
sleep 1
term "6 CPU 1"            "$BASE/cpu" "./bin/cpu config/cpu.cfg 1"
sleep 1
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> PID 1 (prioridad 1, BAJA) toma m1. PID 2 (prioridad 0, ALTA) lo pide y se bloquea."
echo ">>> Ahi PID 1 tiene que HEREDAR la prioridad 0:  ## 1 Cambio de prioridad: 1 0"
echo ">>> Y al liberar m1, volver a la suya:            ## 1 Cambio de prioridad: 0 1"
