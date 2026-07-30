#!/bin/bash
# Prueba Memoria (Documento de Pruebas Finales, pag. 7)
#
# Fase 1: Kernel Memory con ALLOCATION_STRATEGY=BEST, script PLANI_MEM.prc
# Fase 2: Kernel Memory con ALLOCATION_STRATEGY=WORST, mismo script
#
# Uso:
#   ./pruebas_oficiales/03_prueba_memoria/levantar.sh
#
# Requiere el proyecto compilado (./compilar_todo.sh) y gnome-terminal.
# Configs: kernel_scheduler/config/ks_mem.cfg,
#          kernel_memory/config/km_mem_best.cfg y km_mem_worst.cfg
# Memory Sticks: 16, 32, 64 y 128 bytes.

set -u
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

limpiar() {
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
}

term() { # term <titulo> <dir> <comando...>
    local titulo="$1"; local dir="$2"; shift 2
    gnome-terminal --title="$titulo" -- bash -c \
        "cd '$dir' && $*; echo; echo '--- $titulo TERMINADO ---'; exec bash"
}

correr_fase() {
    local km_cfg="$1"
    limpiar
    term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory config/$km_cfg"
    sleep 2
    term "2 MEMORY STICK 16"  "$BASE/memory_stick"     "./bin/memory_stick config/stick_p1.cfg 16"
    sleep 1
    term "3 MEMORY STICK 32"  "$BASE/memory_stick"     "./bin/memory_stick config/stick_p2.cfg 32"
    sleep 1
    term "4 MEMORY STICK 64"  "$BASE/memory_stick"     "./bin/memory_stick config/stick_p3.cfg 64"
    sleep 1
    term "5 MEMORY STICK 128" "$BASE/memory_stick"     "./bin/memory_stick config/stick_p4.cfg 128"
    sleep 2
    term "6 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
    sleep 2
    term "7 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler config/ks_mem.cfg PLANI_MEM.prc"
    sleep 2
    term "8 IO SLEEP"         "$BASE/io"               "./bin/io config/io.cfg SLEEP"
    sleep 1
    term "9 IO STDOUT"        "$BASE/io"               "./bin/io config/io.cfg STDOUT"
    sleep 1
    term "10 IO STDIN"        "$BASE/io"               "./bin/io config/io.cfg STDIN"
    sleep 1
    term "11 CPU 1"           "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
    sleep 2

    echo "--- procesos vivos ---"
    pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
        | grep -v "bash -c" | sed 's/^/  /'
    echo
}

echo ">>> FASE 1: ALLOCATION_STRATEGY=BEST"
correr_fase km_mem_best.cfg
echo ">>> Mira que en alguna de las 2 estrategias se compacte memoria, y que los"
echo ">>> valores leidos tras compactar coincidan con los escritos."
read -p ">>> Cuando termine la Fase 1, presioná ENTER para bajar todo y arrancar la Fase 2 (WORST)... "

echo ">>> FASE 2: ALLOCATION_STRATEGY=WORST"
correr_fase km_mem_worst.cfg
echo ">>> Prueba Memoria completa."
