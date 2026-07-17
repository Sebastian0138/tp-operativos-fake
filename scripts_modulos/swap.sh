#!/bin/bash
# Levanta el modulo Swap (cliente de Kernel Memory).
# Uso: ./swap.sh [archivo_config]
#   - archivo_config es opcional; por defecto usa config/swap.cfg

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${1:-config/swap.cfg}"

cd "$BASE_DIR/swap"
echo ">>> Swap con config: $CONFIG"
./bin/swap "$CONFIG"
