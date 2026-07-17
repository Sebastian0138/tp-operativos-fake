#!/bin/bash

# Script de build y arranque de todos los modulos del sistema HolyOS
# Orden de arranque segun dependencias del diagrama:
#   1. utils  (libreria compartida)
#   2. kernel_memory  (servidor central, debe estar antes que todos)
#   3. memory_stick   (servidor + cliente de kernel_memory)
#   4. kernel_scheduler (servidor + cliente de kernel_memory)
#   5. cpu            (cliente de los 3 anteriores)
#   6. io             (cliente de kernel_scheduler)
#   7. swap           (cliente de kernel_memory)

set -e  # Abortar si cualquier make falla

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Colores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log_info()    { echo -e "${CYAN}[INFO]${NC}  $1"; }
log_ok()      { echo -e "${GREEN}[OK]${NC}    $1"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $1"; }

# -----------------------------------------------------------------------
# 1. BUILD
# -----------------------------------------------------------------------

build_module() {
    local module="$1"
    local dir="$BASE_DIR/$module"

    if [ ! -d "$dir" ]; then
        log_error "Directorio no encontrado: $dir"
        exit 1
    fi

    log_info "Compilando $module..."
    if make -C "$dir" --silent 2>&1; then
        log_ok "$module compilado correctamente"
    else
        log_error "Fallo la compilacion de $module"
        exit 1
    fi
}

echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}   BUILD - HolyOS Modules               ${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

build_module "utils"
build_module "kernel_memory"
build_module "memory_stick"
build_module "kernel_scheduler"
build_module "cpu"
build_module "io"
build_module "swap"

echo ""
log_ok "Todos los modulos compilados correctamente."
echo ""

# -----------------------------------------------------------------------
# 2. ARRANQUE
# -----------------------------------------------------------------------

# Directorio donde se guardan los PIDs para poder hacer cleanup
PID_DIR="/tmp/holy_os_pids"
mkdir -p "$PID_DIR"

# Tiempo de espera entre arranques (en segundos)
DELAY=1

launch() {
    local name="$1"
    local module_dir="$2"
    local binary="$3"
    local args="$4"

    log_info "Levantando $name..."

    # Crear carpeta de logs si no existe
    mkdir -p "$module_dir/logs"

    # Ejecutar desde el directorio del modulo para que los paths relativos funcionen
    (cd "$module_dir" && ./$binary $args) &
    local pid=$!
    echo "$pid" > "$PID_DIR/$name.pid"
    log_ok "$name iniciado (PID: $pid)"
    sleep "$DELAY"
}

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}   ARRANQUE - HolyOS Modules            ${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# Orden de arranque (servidores primero, clientes despues)
launch "kernel_memory" \
    "$BASE_DIR/kernel_memory" \
    "bin/kernel_memory" \
    "config/kernel_memory.cfg"

# Memory Sticks: tamano por parametro (consigna: ./bin/memory_stick [Config] [Tamaño])
launch "memory_stick_1" \
    "$BASE_DIR/memory_stick" \
    "bin/memory_stick" \
    "config/memory_stick.cfg 256"

launch "memory_stick_2" \
    "$BASE_DIR/memory_stick" \
    "bin/memory_stick" \
    "config/memory_stick_2.cfg 256"

launch "kernel_scheduler" \
    "$BASE_DIR/kernel_scheduler" \
    "bin/kernel_scheduler" \
    "config/kernel_scheduler.cfg proceso_inicial"

launch "swap" \
    "$BASE_DIR/swap" \
    "bin/swap" \
    "config/swap.cfg"

launch "cpu_1" \
    "$BASE_DIR/cpu" \
    "bin/cpu" \
    "config/cpu.cfg 1"

# IOs: una instancia por tipo (STDIN es interactiva; se lanza igual pero
# sin TTY va a responder con ceros)
launch "io_sleep" \
    "$BASE_DIR/io" \
    "bin/io" \
    "config/io.cfg SLEEP"

launch "io_stdout" \
    "$BASE_DIR/io" \
    "bin/io" \
    "config/io.cfg STDOUT"

launch "io_stdin" \
    "$BASE_DIR/io" \
    "bin/io" \
    "config/io.cfg STDIN"

echo ""
log_ok "Todos los modulos levantados."
echo ""
echo -e "  ${YELLOW}Para detenerlos todos, ejecuta:${NC}"
echo -e "  ${CYAN}$BASE_DIR/stop_all.sh${NC}"
echo ""

# -----------------------------------------------------------------------
# 3. ESPERAR - Manejo de señales para cleanup limpio
# -----------------------------------------------------------------------

cleanup() {
    echo ""
    log_warn "Señal recibida. Deteniendo todos los modulos..."
    for pid_file in "$PID_DIR"/*.pid; do
        if [ -f "$pid_file" ]; then
            local pid
            pid=$(cat "$pid_file")
            local name
            name=$(basename "$pid_file" .pid)
            if kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null
                log_ok "$name detenido (PID: $pid)"
            fi
            rm -f "$pid_file"
        fi
    done
    exit 0
}

trap cleanup SIGINT SIGTERM

log_info "Sistema corriendo. Presiona Ctrl+C para detener todo."
wait
