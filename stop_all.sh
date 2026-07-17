#!/bin/bash

# Detiene todos los procesos de HolyOS lanzados por start_all.sh

PID_DIR="/tmp/holy_os_pids"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

log_ok()    { echo -e "${GREEN}[OK]${NC}    $1"; }
log_warn()  { echo -e "${RED}[STOP]${NC}  $1"; }

echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}   STOP - HolyOS Modules                ${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

if [ ! -d "$PID_DIR" ] || [ -z "$(ls -A "$PID_DIR" 2>/dev/null)" ]; then
    echo "No hay procesos registrados en $PID_DIR"
    exit 0
fi

for pid_file in "$PID_DIR"/*.pid; do
    if [ -f "$pid_file" ]; then
        pid=$(cat "$pid_file")
        name=$(basename "$pid_file" .pid)
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            log_warn "$name detenido (PID: $pid)"
        else
            log_ok "$name ya no estaba corriendo (PID: $pid)"
        fi
        rm -f "$pid_file"
    fi
done

echo ""
log_ok "Todos los modulos detenidos."
echo ""
