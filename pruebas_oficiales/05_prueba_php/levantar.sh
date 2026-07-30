#!/bin/bash
# Prueba Herencia de Prioridades (Documento de Pruebas Finales, pag. 9)
#
# Kernel Scheduler arranca con PHP.prc. Hay que esperar a que solo queden
# vivos los procesos creados a partir de PHP_3.prc y observar los cambios
# de prioridad temporal por herencia de mutex.
#
# Uso:
#   ./pruebas_oficiales/05_prueba_php/levantar.sh
#
# Requiere el proyecto compilado (./compilar_todo.sh) y gnome-terminal.
# Configs: kernel_scheduler/config/ks_php.cfg, kernel_memory/config/km_php.cfg
# Memory Sticks: 16 y 16 bytes.

set -u
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

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

term() { # term <titulo> <dir> <comando...>
    local titulo="$1"; local dir="$2"; shift 2
    gnome-terminal --title="$titulo" -- bash -c \
        "cd '$dir' && $*; echo; echo '--- $titulo TERMINADO ---'; exec bash"
}

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory config/km_php.cfg"
sleep 2
term "2 MEMORY STICK 16a" "$BASE/memory_stick"     "./bin/memory_stick config/stick_p1.cfg 16"
sleep 1
term "3 MEMORY STICK 16b" "$BASE/memory_stick"     "./bin/memory_stick config/stick_p2.cfg 16"
sleep 2
term "4 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "5 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler config/ks_php.cfg PHP.prc"
sleep 2
term "6 IO SLEEP"         "$BASE/io"               "./bin/io config/io.cfg SLEEP"
sleep 1
term "7 IO STDOUT"        "$BASE/io"               "./bin/io config/io.cfg STDOUT"
sleep 1
term "8 IO STDIN"         "$BASE/io"               "./bin/io config/io.cfg STDIN"
sleep 1
term "9 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Espera a que solo queden vivos los procesos de PHP_3.prc y mira la ventana"
echo ">>> '5 KERNEL SCHEDULER' para los cambios de prioridad temporal por herencia."
