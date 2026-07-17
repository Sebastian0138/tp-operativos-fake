#!/bin/bash
# Levanta una o mas CPUs, cada una en su propia ventana de terminal.
# Pregunta cuantas queres. Todas usan el mismo config (la CPU es cliente,
# no escucha en ningun puerto), pero cada una recibe un ID distinto (1, 2, 3...).

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="config/cpu.cfg"

# La cantidad se puede pasar como argumento: ./cpu.sh 2
# Si no viene, se pregunta.
CANTIDAD="$1"
if [ -z "$CANTIDAD" ]; then
    read -p "¿Cuantas CPUs queres levantar? [1]: " CANTIDAD
    CANTIDAD="${CANTIDAD:-1}"
fi

if [ "$CANTIDAD" -lt 1 ]; then
    echo "ERROR: la cantidad debe ser 1 o mas."
    exit 1
fi

for i in $(seq 1 "$CANTIDAD"); do
    echo ">>> Levantando CPU $i..."
    gnome-terminal --title="CPU $i" -- bash -c \
        "cd '$BASE_DIR/cpu' && ./bin/cpu $CONFIG $i; exec bash"
    sleep 1
done

echo ">>> $CANTIDAD CPU(s) levantada(s)."
