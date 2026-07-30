#!/bin/bash
# Prueba Planificacion Mediano Plazo (Documento de Pruebas Finales, pag. 8)
#
# Kernel Scheduler arranca con PMP.prc. Ante cada pedido de STDIN hay que
# tipear un texto en la ventana '10 IO STDIN'.
#
# Uso:
#   ./pruebas_oficiales/04_prueba_pmp/levantar.sh
#
# Requiere el proyecto compilado (./compilar_todo.sh) y gnome-terminal.
# Configs: kernel_scheduler/config/ks_pmp.cfg, kernel_memory/config/km_pmp.cfg
# Memory Sticks: 16, 16, 32 y 64 bytes.

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory config/km_pmp.cfg"
sleep 2
term "2 MEMORY STICK 16a" "$BASE/memory_stick"     "./bin/memory_stick config/stick_p1.cfg 16"
sleep 1
term "3 MEMORY STICK 16b" "$BASE/memory_stick"     "./bin/memory_stick config/stick_p2.cfg 16"
sleep 1
term "4 MEMORY STICK 32"  "$BASE/memory_stick"     "./bin/memory_stick config/stick_p3.cfg 32"
sleep 1
term "5 MEMORY STICK 64"  "$BASE/memory_stick"     "./bin/memory_stick config/stick_p4.cfg 64"
sleep 2
term "6 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "7 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler config/ks_pmp.cfg PMP.prc"
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
echo ">>> Cada vez que un proceso pida STDIN, tipea algo en la ventana '10 IO STDIN'."
echo ">>> Verifica que los procesos se desbloqueen segun lo indicado en el enunciado."
