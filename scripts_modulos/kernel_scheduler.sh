#!/bin/bash
# Levanta el Kernel Scheduler.
# Uso: ./kernel_scheduler.sh [archivo_config] [proceso_inicial]
#   - archivo_config: opcional, por defecto config/kernel_scheduler.cfg
#   - proceso_inicial: opcional, por defecto "proceso_inicial"
#     (es el archivo de pseudocodigo del primer proceso que crea el sistema)

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-config/kernel_scheduler.cfg}"
PROCESO_INICIAL="${2:-proceso_inicial}"

cd "$BASE_DIR/kernel_scheduler"
echo ">>> Kernel Scheduler con config: $CONFIG, proceso inicial: $PROCESO_INICIAL"
./bin/kernel_scheduler "$CONFIG" "$PROCESO_INICIAL"
