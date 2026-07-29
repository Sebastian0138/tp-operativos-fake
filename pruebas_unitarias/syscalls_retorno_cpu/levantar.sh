#!/bin/bash
# Prueba unitaria: Atencion de Syscalls -- retorno del proceso a la MISMA CPU.
#
# Uso:
#   ./pruebas_unitarias/syscalls_retorno_cpu/levantar.sh
#
# DOS CPUs y TRES procesos, para que siempre haya uno esperando:
#   PID 1  test_sys_cpu       CPU-bound ~12s, ocupa una CPU
#   PID 2  test_sys_syscalls  10 vueltas x 4 syscalls de retorno directo, ocupa la otra
#   PID 3  test_sys_cpu       CPU-bound, se queda en READY todo el tiempo
#
# Las 40 syscalls de PID 2 (MEM_ALLOC, MUTEX_LOCK, MUTEX_UNLOCK, MEM_FREE) devuelven
# el proceso a la misma CPU sin pasar por READY. Si alguna liberara la CPU, PID 3
# entraria a ejecutar: que NUNCA entre es la prueba.
#
# Duracion: ~25 segundos. No hay que tocar nada.

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/syscalls_retorno_cpu/km_sys.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/syscalls_retorno_cpu/ks_sys.cfg test_sys_padre"
sleep 2
term "5 IO SLEEP"         "$BASE/io"               "./bin/io config/io.cfg SLEEP"
sleep 1
term "6 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 1
term "7 CPU 2"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 2"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> PID 2 pide 40 syscalls y NO aparece ni un '(2) Pasa del estado EXEC al estado READY'."
echo ">>> PID 3 NO entra a EXEC hasta que PID 1 o PID 2 terminen."
