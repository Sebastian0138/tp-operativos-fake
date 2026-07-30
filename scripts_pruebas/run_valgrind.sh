#!/bin/bash
# Prueba de Estabilidad General: corre la prueba base con todos los modulos bajo
# valgrind y deja un reporte de leaks por modulo en logs_valgrind/.
# Uso: run_valgrind.sh [duracion_s]
BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DUR=${1:-180}
OUT="$BASE/logs_valgrind"

VG=(valgrind --leak-check=full --show-leak-kinds=definite,indirect --track-origins=yes --error-exitcode=0)

pkill -f "[b]in/kernel_memory"    2>/dev/null
pkill -f "[b]in/memory_stick"     2>/dev/null
pkill -f "[b]in/kernel_scheduler" 2>/dev/null
pkill -f "[b]in/cpu "             2>/dev/null
pkill -f "[b]in/io "              2>/dev/null
pkill -f "[b]in/swap"             2>/dev/null
sleep 1

mkdir -p "$OUT"
rm -f "$OUT"/*.log
rm -f "$BASE"/*/logs/*.log

lanzar() { # lanzar <dir> <nombre_reporte> <comando...>
    local dir="$1" nombre="$2"; shift 2
    cd "$BASE/$dir" || exit 1
    setsid "${VG[@]}" --log-file="$OUT/$nombre.log" "$@" > /dev/null 2>&1 &
}

lanzar kernel_memory    kernel_memory    ./bin/kernel_memory config/km_base.cfg
sleep 3
lanzar memory_stick     memory_stick     ./bin/memory_stick config/stick_p1.cfg 256
sleep 3
lanzar swap             swap             ./bin/swap config/swap.cfg
sleep 3
lanzar kernel_scheduler kernel_scheduler ./bin/kernel_scheduler config/ks_base.cfg PLANI_PRE_0.prc
sleep 3
cd "$BASE/io" || exit 1
setsid "${VG[@]}" --log-file="$OUT/io_sleep.log"  ./bin/io config/io.cfg SLEEP  > /dev/null 2>&1 &
setsid "${VG[@]}" --log-file="$OUT/io_stdout.log" ./bin/io config/io.cfg STDOUT > /dev/null 2>&1 &
setsid "${VG[@]}" --log-file="$OUT/io_stdin.log"  ./bin/io config/io.cfg STDIN < /dev/null > /dev/null 2>&1 &
sleep 3
lanzar cpu cpu ./bin/cpu config/cpu.cfg 1

echo ">>> corriendo bajo valgrind ${DUR}s..."
sleep "$DUR"

# SIGTERM (no SIGKILL): valgrind alcanza a emitir el reporte de leaks.
pkill -TERM -f "[b]in/cpu "             2>/dev/null
pkill -TERM -f "[b]in/io "              2>/dev/null
pkill -TERM -f "[b]in/kernel_scheduler" 2>/dev/null
pkill -TERM -f "[b]in/swap"             2>/dev/null
pkill -TERM -f "[b]in/memory_stick"     2>/dev/null
pkill -TERM -f "[b]in/kernel_memory"    2>/dev/null
sleep 8

echo ">>> reportes en $OUT/"
