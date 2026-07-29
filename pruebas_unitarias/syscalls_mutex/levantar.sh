#!/bin/bash
# Prueba unitaria: Atencion de Syscalls -- Mutex (exclusion mutua y orden FIFO).
#
# PID 1 crea y toma el mutex m1, y se duerme 6 segundos sin soltarlo.
# PIDs 2, 3 y 4 piden m1 en ese orden y se bloquean los tres.
# Cuando PID 1 libera, se lo van pasando en el orden en que lo pidieron.
#
# Duracion: ~15 segundos. No hay que tocar nada.
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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/syscalls_mutex/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/syscalls_mutex/ks_sys.cfg test_padre"
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
echo ">>> PID 1 toma m1 y duerme 6s. PIDs 2, 3 y 4 lo piden y se bloquean EN ESE ORDEN."
echo ">>> Al liberarlo PID 1, lo tienen que tomar en el MISMO orden: 2, 3, 4."
