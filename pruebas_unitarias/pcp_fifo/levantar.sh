#!/bin/bash
# Prueba unitaria: planificacion de CORTO PLAZO -- FIFO
#
# Uso:
#   ./pruebas_unitarias/pcp_fifo/levantar.sh
#
# El padre (PID 0) crea 3 procesos CPU-bound de ~5 segundos cada uno y termina.
# Con FIFO no hay desalojo de ningun tipo: cada proceso, una vez en EXEC, se queda
# hasta que termina. Los tres corren uno detras de otro, en orden de llegada.
#
# Duracion: ~20 segundos. No hay que tocar nada.
#
# UNA sola CPU: es parte de la prueba. Con dos, los procesos no compiten por el
# procesador y no hay ni quantum ni desalojo que observar.
# Los procesos hijos no piden memoria, asi que el tamanio del stick es indistinto.

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/pcp_fifo/km_pcp.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/pcp_fifo/ks_pcp.cfg test_pcp_padre"
sleep 2
term "5.1 IO SLEEP"       "$BASE/io"               "./bin/io config/io.cfg SLEEP"
sleep 1
term "5.2 IO SLEEP"       "$BASE/io"               "./bin/io config/io.cfg SLEEP"
sleep 1
term "6 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> Cada hijo entra a EXEC y NO vuelve a READY hasta terminar."
echo ">>> NO tiene que aparecer ni un solo 'Desalojado por fin de quantum'."
