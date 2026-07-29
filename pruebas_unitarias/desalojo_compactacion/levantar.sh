#!/bin/bash
# Prueba unitaria: DESALOJO POR COMPACTACION (lado Kernel Scheduler).
#
# TRES CPUs y CUATRO procesos:
#   PID 0  test_padre       fragmenta la memoria y dispara la compactacion.
#                           Sus MEM_ALLOC/MEM_FREE tienen retorno directo, asi que
#                           NO suelta su CPU. Al pedir la compactacion queda
#                           "estacionado" en la syscall: su CPU NO esta en rafaga.
#   PID 1  test_comp_busy   CPU-bound ~20s -> EN RAFAGA, hay que desalojarlo
#   PID 2  test_comp_busy   CPU-bound ~20s -> EN RAFAGA, hay que desalojarlo
#   PID 3  test_comp_busy   se queda esperando en READY: es el TESTIGO
#
# Lo que se verifica:
#   1. Se interrumpen las DOS CPUs en rafaga (no una).
#   2. Durante la compactacion NO se despacha a nadie: PID 3 no entra a EXEC.
#   3. Al terminar, PID 1 y PID 2 (desalojados, al FRENTE de READY) toman CPU
#      ANTES que PID 3, que estaba esperando desde antes.
#
# Duracion: ~30 segundos. No hay que tocar nada.

set -u
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAM_STICK=${1:-200}

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/desalojo_compactacion/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/desalojo_compactacion/ks_sys.cfg test_padre"
sleep 2
term "5 IO SLEEP"         "$BASE/io"               "./bin/io config/io.cfg SLEEP"
sleep 1
term "6 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 1
term "7 CPU 2"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 2"
sleep 1
term "8 CPU 3"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 3"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> Al disparar la compactacion tienen que aparecer DOS desalojos (PID 1 y PID 2),"
echo ">>> PID 3 NO puede entrar a EXEC mientras dura, y al terminar entran 1 y 2 antes que 3."
