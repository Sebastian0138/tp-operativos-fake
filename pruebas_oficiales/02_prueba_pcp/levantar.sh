#!/bin/bash
# Prueba Planificacion Corto Plazo (Documento de Pruebas Finales, pag. 6)
#
# Kernel Scheduler arranca con PCP.prc: valida desalojo por prioridad
# multinivel y alternancia por quantum en las colas RR.
#
# Uso:
#   ./pruebas_oficiales/02_prueba_pcp/levantar.sh
#
# Requiere el proyecto compilado (./compilar_todo.sh) y gnome-terminal.
# Configs: kernel_scheduler/config/ks_pcp.cfg, kernel_memory/config/km_pcp.cfg
# Memory Stick: 1 de 256 bytes.

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory config/km_pcp.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/stick_p1.cfg 256"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler config/ks_pcp.cfg PCP.prc"
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
echo ">>> Mira la ventana '4 KERNEL SCHEDULER': los procesos deben desalojarse"
echo ">>> segun las prioridades multinivel y los RR deben alternar por quantum."
