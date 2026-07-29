#!/bin/bash
# Prueba unitaria: MUERTE DEL DUENIO DE UN MUTEX.
#
# Uso:
#   ./pruebas_unitarias/mutex_muerte_del_duenio/levantar.sh
#
# PID 1 crea DOS mutex, toma los dos, y se muere con un EXIT SIN LIBERARLOS.
# PID 2 esta esperando m1 y PID 3 esta esperando m2.
#
# Si finalizar_proceso() no liberara los mutex del que muere, PID 2 y PID 3
# quedarian bloqueados PARA SIEMPRE: el mutex tendria duenio y el duenio no existiria.
#
# Se usan DOS mutex a proposito: finalizar_proceso() recorre lista_mutex_tomados
# en un while, asi que con uno solo no se probaria el recorrido.
#
# Duracion: ~20 segundos. No hay que tocar nada.

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"  "./bin/kernel_memory ../pruebas_unitarias/mutex_muerte_del_duenio/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"   "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"           "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/mutex_muerte_del_duenio/ks_sys.cfg test_padre"
sleep 2
term "5.1 IO SLEEP"       "$BASE/io"             "./bin/io config/io.cfg SLEEP"
sleep 1
term "5.2 IO SLEEP"       "$BASE/io"             "./bin/io config/io.cfg SLEEP"
sleep 1
term "6 CPU 1"            "$BASE/cpu"            "./bin/cpu config/cpu.cfg 1"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '4 KERNEL SCHEDULER'. Lo que tiene que pasar:"
echo ">>>   (1) Toma el Mutex m1  y  (1) Toma el Mutex m2"
echo ">>>   (2) y (3) piden y quedan en BLOCK"
echo ">>>   (1) finalizó su ejecución con motivo de SUCCESS   <- muere SIN liberar"
echo ">>>   (2) Toma el Mutex m1  y  (3) Toma el Mutex m2     <- SIN ningun 'Libera' antes"
