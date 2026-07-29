#!/bin/bash
# Prueba unitaria: transicion BLOCK -> SUSP. BLOCK (planificacion de mediano plazo).
# Levanta una instancia de cada modulo, cada una en su propia ventana de gnome-terminal.
#
# Uso:
#   ./pruebas_unitarias/suspension/levantar.sh          # desde la raiz del repo
#   ./pruebas_unitarias/suspension/levantar.sh 512      # sticks de 512 bytes (default: 256)
#
# El proceso inicial cicla SLEEP 30000 indefinidamente, y el timeout de suspension
# esta en 10000 ms: cada vuelta se lo ve entrar a BLOCK, suspenderse a los 10s,
# volver a SUSP. READY al terminar la IO y des-suspenderse a READY.

set -u

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

TAM_STICK=${1:-256}

# --- bajar lo que haya quedado vivo de una corrida anterior ---
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

# Orden de arranque: servidores primero, clientes despues.
# Los paths de config son relativos al directorio de cada modulo.
term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/suspension/km_susp.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/suspension/ks_susp.cfg test_suspension_loop"
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
echo ">>> No hay que tocar nada: mirá la ventana '4 KERNEL SCHEDULER'."
echo ">>> A los ~10s del BLOCK aparece: (0) Pasa del estado BLOCK al estado SUSP. BLOCK"
