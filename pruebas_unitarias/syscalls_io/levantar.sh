#!/bin/bash
# Prueba unitaria: Atencion de Syscalls -- IO (SLEEP, STDIN y STDOUT).
#
# PID 1 reserva 32 bytes, pide 10 caracteres por STDIN, los imprime por STDOUT y
# duerme 2s. Lo que escribas tiene que salir por STDOUT: eso prueba el viaje
# completo teclado -> IO -> KS -> KM -> memoria -> KM -> KS -> IO -> pantalla.
#
# PID 2 es CPU-bound y sirve de testigo: mientras PID 1 esta bloqueado en IO, el
# Scheduler tiene que "enviar un nuevo Proceso a ejecutar si lo hubiera".
#
# INTERACTIVA: hay que escribir en la ventana del IO STDIN.
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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/syscalls_io/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/syscalls_io/ks_sys.cfg test_padre"
sleep 2
term "5 IO SLEEP"       "$BASE/io"  "./bin/io config/io.cfg SLEEP"
sleep 1
term "5 IO STDIN"       "$BASE/io"  "./bin/io config/io.cfg STDIN"
sleep 1
term "5 IO STDOUT"       "$BASE/io"  "./bin/io config/io.cfg STDOUT"
sleep 1
term "6 CPU 1"            "$BASE/cpu" "./bin/cpu config/cpu.cfg 1"
sleep 1
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> ES INTERACTIVA. En la ventana '5 IO STDIN' te va a pedir 10 caracteres."
echo ">>> Escribi 10 letras y dale Enter. Van a aparecer en la ventana '5 IO STDOUT'."
echo ">>> Mientras PID 1 espera tu texto, PID 2 usa la CPU (eso tambien es parte de la prueba)."
