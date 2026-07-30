#!/bin/bash
# Prueba Base (Documento de Pruebas Finales, pag. 5)
#
# Fase 1: Kernel Scheduler arranca con PLANI_PRE_0.prc
# Fase 2: Kernel Scheduler arranca con MEMORIA_PRE_0.prc
#
# Uso:
#   ./pruebas_oficiales/01_prueba_base/levantar.sh
#
# Requiere el proyecto compilado (./compilar_todo.sh) y gnome-terminal.
# Configs: kernel_scheduler/config/ks_base.cfg, kernel_memory/config/km_base.cfg
# Memory Stick: 1 de 256 bytes.

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
    local script="$1"
    limpiar
    term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory config/km_base.cfg"
    sleep 2
    term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/stick_p1.cfg 256"
    sleep 2
    term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
    sleep 2
    term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler config/ks_base.cfg $script"
    sleep 2
    term "5 IO SLEEP"         "$BASE/io"               "./bin/io config/io.cfg SLEEP"
    sleep 1
    term "6 IO STDOUT"        "$BASE/io"               "./bin/io config/io.cfg STDOUT"
    sleep 1
    term "7 IO STDIN"         "$BASE/io"               "./bin/io config/io.cfg STDIN"
    sleep 1
    term "8 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
    sleep 2

    echo "--- procesos vivos ---"
    pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
        | grep -v "bash -c" | sed 's/^/  /'
    echo
}

echo ">>> FASE 1: PLANI_PRE_0.prc"
correr_fase PLANI_PRE_0.prc
read -p ">>> Cuando termine la Fase 1, presioná ENTER para bajar todo y arrancar la Fase 2 (MEMORIA_PRE_0.prc)... "

echo ">>> FASE 2: MEMORIA_PRE_0.prc"
correr_fase MEMORIA_PRE_0.prc
echo ">>> Prueba Base completa."
