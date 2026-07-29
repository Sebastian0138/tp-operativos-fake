#!/bin/bash
# Prueba unitaria: des-suspension por liberacion de memoria (SUSP. READY -> READY),
# por orden de prioridad y, a igual prioridad, por antiguedad de suspension.
#
# Uso:
#   ./pruebas_unitarias/dessuspension_compactacion/levantar.sh          # desde la raiz del repo
#   ./pruebas_unitarias/dessuspension_compactacion/levantar.sh 256      # stick de 256 bytes (default: 128)
#
# El padre (PID 0) crea 3 procesos:
#   PID 1 = test_compact_busy : loop infinito de instrucciones PURAS (sin syscalls).
#                               Se queda EN RAFAGA para siempre ocupando la CPU 2.
#   PID 2 = test_compact_hijo prioridad 1  } allocan 30 bytes, se suspenden
#   PID 3 = test_compact_hijo prioridad 0  } y quedan varados en SUSP. READY
#
# Despues el padre llena los 200 bytes con 10 segmentos de 20 y libera 5 alternados:
# quedan 100 bytes libres pero en 5 huecos de 20. Los hijos necesitan 30 contiguos,
# asi que no entran aunque sobre memoria total.
#
# Finalmente el padre pide un segmento de 30: no entra en ningun hueco pero si en el
# total, asi que Kernel Memory pide COMPACTAR. Ahi el KS tiene que DESALOJAR la CPU 2,
# que esta en rafaga ejecutando el proceso busy. Al terminar, los 2 hijos se
# des-suspenden en orden PID 3 -> PID 2 (por prioridad).
#
# Duracion del ciclo completo: ~45 segundos. No hay que tocar nada.
#
# DOS CPUs: con una sola, el unico proceso en ejecucion es el que disparo la
# compactacion, que esta "estacionado" esperando la respuesta de su MEM_ALLOC y por lo
# tanto NO esta en rafaga. El bucle interrumpir_cpus_para_compactar() sale en la primera
# vuelta sin interrumpir a nadie, y toda la rama de desalojo del KS queda sin ejercitar.

set -u

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

TAM_STICK=${1:-200}

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
term "1 KERNEL MEMORY"    "$BASE/kernel_memory"    "./bin/kernel_memory ../pruebas_unitarias/dessuspension_compactacion/km_dessusp.cfg"
sleep 2
term "2 MEMORY STICK"     "$BASE/memory_stick"     "./bin/memory_stick config/memory_stick.cfg $TAM_STICK"
sleep 2
term "3 SWAP"             "$BASE/swap"             "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/dessuspension_compactacion/ks_dessusp.cfg test_compact_padre"
sleep 2

# Cuatro IO SLEEP: una por proceso concurrente (padre + 3 hijos). Ver nota de arriba.
for i in 1 2 3 4; do
    term "5.$i IO SLEEP" "$BASE/io" "./bin/io config/io.cfg SLEEP"
    sleep 1
done

# Esta prueba no usa STDIN ni STDOUT, asi que no se levantan.

term "6 CPU 1"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 1"
sleep 1
term "7 CPU 2"            "$BASE/cpu"              "./bin/cpu config/cpu.cfg 2"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> No hay que tocar nada: mira la ventana '4 KERNEL SCHEDULER'."
echo ">>> ~t+7s   PID 1 (busy) queda en rafaga permanente en una CPU"
echo ">>> ~t+13s  los 2 hijos ya estan suspendidos (segmentos en SWAP)"
echo ">>> ~t+20s  el padre fragmenta: 10 segmentos de 20, libera 5 alternados"
echo ">>> ~t+27s  los hijos despiertan y quedan VARADOS (100 libres, en huecos de 20)"
echo ">>> ~t+33s  MEM_ALLOC 10 30 -> COMPACTACION:"
echo ">>>           - KS desaloja la CPU del proceso busy (INT_COMPACTACION)"
echo ">>>           - PID 1 vuelve AL FRENTE de READY"
echo ">>>           - se des-suspenden PID 3 y PID 2"
