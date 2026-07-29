#!/bin/bash
# Prueba unitaria: INTEGRIDAD DE LOS DATOS AL COMPACTAR, con DOS Memory Sticks.
#
# Uso:
#   ./pruebas_unitarias/compactacion_integridad/levantar.sh
#
# DOS sticks de tamanio DISTINTO (90 y 110 bytes) para que la frontera entre ellos
# caiga DENTRO de un segmento y se ejercite copiar_segmento_entre_sticks().
#
#   stick 1 -> direcciones globales [0, 90)
#   stick 2 -> direcciones globales [90, 200)
#
# PID 0 llena los 200 bytes con 10 segmentos de 20, marca los pares con una letra
# en el offset 0 y otra en el 19, los lee (CONTROL), libera los impares, dispara la
# compactacion y vuelve a leer. Las dos lecturas tienen que dar lo MISMO:
#
#     A a B b C c D d E e
#
# El segmento 4 vive en [80,100): CRUZA la frontera de los sticks. Y el segmento 8
# aterriza ahi al compactar. O sea que se ejercita la copia a caballo de dos sticks
# en las dos direcciones, lectura y escritura.
#
# Duracion: ~40 segundos. No hay que tocar nada.

set -u
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAM_1=${1:-90}
TAM_2=${2:-110}

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

term "1 KERNEL MEMORY"    "$BASE/kernel_memory"  "./bin/kernel_memory ../pruebas_unitarias/compactacion_integridad/km_sys.cfg"
sleep 2
# El PRIMERO en registrarse se queda con las direcciones bajas [0, TAM_1).
term "2 MEMORY STICK 1"   "$BASE/memory_stick"   "./bin/memory_stick config/memory_stick.cfg $TAM_1"
sleep 2
term "2b MEMORY STICK 2"  "$BASE/memory_stick"   "./bin/memory_stick config/memory_stick_2.cfg $TAM_2"
sleep 2
term "3 SWAP"             "$BASE/swap"           "./bin/swap config/swap.cfg"
sleep 2
term "4 KERNEL SCHEDULER" "$BASE/kernel_scheduler" "./bin/kernel_scheduler ../pruebas_unitarias/compactacion_integridad/ks_sys.cfg test_padre"
sleep 2
term "5 IO STDOUT"        "$BASE/io"             "./bin/io config/io.cfg STDOUT"
sleep 1
term "6 CPU 1"            "$BASE/cpu"            "./bin/cpu config/cpu.cfg 1"
sleep 2

echo "--- procesos vivos ---"
pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" \
    | grep -v "bash -c" | sed 's/^/  /'
echo
echo ">>> Mira la ventana '5 IO STDOUT'. Tiene que imprimir DOS veces lo mismo:"
echo ">>>     A a B b C c D d E e     (control, antes de compactar)"
echo ">>>     A a B b C c D d E e     (despues de compactar)"
echo ">>> Si la segunda tanda difiere, la compactacion esta corrompiendo datos."
