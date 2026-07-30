#!/bin/bash
# Runner de pruebas oficiales.
# Uso: run_prueba.sh <km_cfg> <ks_cfg> <script_inicial> <duracion_s> <sticks: "cfg:size cfg:size ..."> [archivo_stdin]
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

KM_CFG=$1; KS_CFG=$2; SCRIPT=$3; DUR=$4; STICKS=$5; STDIN_FILE=$6

# El archivo de STDIN se abre despues de varios cd, asi que hay que resolverlo
# a path absoluto ahora, mientras sigue siendo valido.
if [ -n "$STDIN_FILE" ]; then
    STDIN_FILE="$(realpath "$STDIN_FILE")" || exit 1
fi

pkill -f "[b]in/kernel_memory" 2>/dev/null
pkill -f "[b]in/memory_stick" 2>/dev/null
pkill -f "[b]in/kernel_scheduler" 2>/dev/null
pkill -f "[b]in/cpu " 2>/dev/null
pkill -f "[b]in/io " 2>/dev/null
pkill -f "[b]in/swap" 2>/dev/null
sleep 1

rm -f "$BASE"/kernel_memory/logs/*.log "$BASE"/kernel_scheduler/logs/*.log \
      "$BASE"/cpu/logs/*.log "$BASE"/io/logs/*.log \
      "$BASE"/memory_stick/logs/*.log "$BASE"/swap/logs/*.log

# lanzar <dir> <comando...>: aborta si el cd falla, para que un path invalido
# no termine en una corrida vacia que parece exitosa.
lanzar() {
    local dir="$1"; shift
    cd "$BASE/$dir" || { echo "ERROR: no se pudo entrar a $BASE/$dir" >&2; exit 1; }
    setsid "$@" > /dev/null 2>&1 &
}

lanzar kernel_memory ./bin/kernel_memory "config/$KM_CFG"
sleep 0.7

for s in $STICKS; do
    cfg="${s%%:*}"; size="${s##*:}"
    lanzar memory_stick ./bin/memory_stick "config/$cfg" "$size"
    sleep 0.4
done

lanzar swap ./bin/swap config/swap.cfg
sleep 0.3

lanzar kernel_scheduler ./bin/kernel_scheduler "config/$KS_CFG" "$SCRIPT"
sleep 0.6

cd "$BASE/io" || exit 1
setsid ./bin/io config/io.cfg SLEEP  > /dev/null 2>&1 &
setsid ./bin/io config/io.cfg STDOUT > /dev/null 2>&1 &
if [ -n "$STDIN_FILE" ]; then
    setsid ./bin/io config/io.cfg STDIN < "$STDIN_FILE" > /dev/null 2>&1 &
else
    setsid ./bin/io config/io.cfg STDIN < /dev/null > /dev/null 2>&1 &
fi
sleep 0.4

lanzar cpu ./bin/cpu config/cpu.cfg 1
sleep 1.5

# Chequeo de arranque: si falta algun modulo la prueba no vale, cortar ya.
vivos=$(pgrep -cf "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)")
esperados=$(( 4 + 3 + $(echo $STICKS | wc -w) ))   # km + swap + ks + cpu + 3 io + sticks
if [ "$vivos" -lt "$esperados" ]; then
    echo "ERROR: solo $vivos de $esperados modulos arrancaron" >&2
    pgrep -af "bin/(kernel_memory|memory_stick|kernel_scheduler|swap|cpu|io)" >&2
    exit 1
fi
echo "ARRANQUE_OK ($vivos modulos)"

# Esperar a que la prueba se aquiete en vez de dormir un tiempo fijo: NINGUN log
# crece durante QUIET_S seguidos => no queda trabajo. Hay que mirar todos los
# modulos y no solo el Kernel Scheduler: un proceso puede pasar minutos haciendo
# accesos a memoria sin generar una sola linea de planificacion.
# 75s por defecto: con SLEEP de 60000 y suspensiones de por medio hay huecos
# largos sin una sola linea de log en los que la prueba sigue viva.
QUIET_S=${QUIET_S:-75}
inicio=$SECONDS
ultimo_cambio=$SECONDS
tam_prev=-1
while [ $((SECONDS - inicio)) -lt "$DUR" ]; do
    tam=$(cat "$BASE"/*/logs/*.log 2>/dev/null | wc -c)
    if [ "$tam" != "$tam_prev" ]; then
        tam_prev=$tam
        ultimo_cambio=$SECONDS
    elif [ $((SECONDS - ultimo_cambio)) -ge "$QUIET_S" ]; then
        echo "FIN_PRUEBA (quiescencia tras $((SECONDS - inicio))s)"
        exit 0
    fi
    sleep 2
done
echo "FIN_PRUEBA (tope de ${DUR}s alcanzado, puede haber quedado trabajo pendiente)"
exit 0
