#!/bin/bash
# Levanta el Kernel Memory (el servidor central: debe arrancar PRIMERO).
# Uso: ./kernel_memory.sh [archivo_config]
#   - archivo_config es opcional; por defecto usa config/kernel_memory.cfg

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-config/kernel_memory.cfg}"

cd "$BASE_DIR/kernel_memory"
echo ">>> Kernel Memory con config: $CONFIG"
./bin/kernel_memory "$CONFIG"
