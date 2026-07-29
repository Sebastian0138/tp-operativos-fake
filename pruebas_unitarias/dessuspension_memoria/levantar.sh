#!/bin/bash
# Prueba unitaria: des-suspension por liberacion de memoria (SUSP. READY -> READY),
# por orden de prioridad y, a igual prioridad, por antiguedad de suspension.
#
# Uso:
#   ./pruebas_unitarias/dessuspension_memoria/levantar.sh          # desde la raiz del repo
#   ./pruebas_unitarias/dessuspension_memoria/levantar.sh 256      # stick de 256 bytes (default: 128)
#
# El proceso padre (PID 0) crea 3 hijos con prioridades 2, 1 y 1. Los hijos allocan
# 20 bytes cada uno y se suspenden. Cuando ya estan los 3 en SWAP, el padre acapara
# 120 de los 128 bytes, asi que al despertar los hijos quedan VARADOS en SUSP. READY.
# Recien cuando el padre hace MEM_FREE se des-suspenden, en orden PID 2 -> 3 -> 1.
#
# Duracion del ciclo completo: ~35 segundos. No hay que tocar nada.
#
# IMPORTANTE: se levantan CUATRO IO SLEEP, una por proceso concurrente.
# planificador_tomar_io_libre() hace sem_wait sobre un semaforo por TIPO de IO, y cada
# dispositivo conectado hace un sem_post. Con una sola IO SLEEP los 4 procesos se
# serializan sobre ella, y el que espera el semaforo lo hace ESTANDO EN BLOCK: el timer
# de suspension le corre igual y lo suspende aunque su SLEEP sea corto. Eso rompe la
# prueba, porque el padre se suspende y suelta la memoria que tiene que estar acaparando.

set -u

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

TAM_STICK=${1:-128}

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
term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/dessuspension_memoria/km_dessusp.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/dessuspension_memoria/ks_dessusp.cfg test_dessusp_padre"
sleep 2

# Cuatro IO SLEEP: una por proceso concurrente (padre + 3 hijos). Ver nota de arriba.
for i in 1 2 3 4; do
    term "5.$i IO SLEEP" "$BASE/io" "./bin/io config/io.cfg SLEEP"
    sleep 1
done

# Esta prueba no usa STDIN ni STDOUT, asi que no se levantan.

term "6 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> No hay que tocar nada: mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> ~t+9s   los 3 hijos ya estan suspendidos (segmentos en SWAP)"
echo ">>> ~t+11s  el padre acapara la memoria: Segmento Creado 0 - Tamanio: 120"
echo ">>> ~t+23s  los hijos despiertan y quedan VARADOS en SUSP. READY (no entran)"
echo ">>> ~t+31s  MEM_FREE del padre -> se des-suspenden en orden PID 2, 3, 1"
