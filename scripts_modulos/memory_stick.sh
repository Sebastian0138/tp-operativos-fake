#!/bin/bash
# Levanta uno o mas Memory Sticks, cada uno en su propia ventana de terminal.
# Pregunta cuantos queres (1 a 4) y el tamanio en bytes de cada uno.
#
# Cada instancia necesita un config con un puerto de escucha DISTINTO.
# Por eso usamos los configs ya preparados: stick_p1.cfg (puerto 8003),
# stick_p2.cfg (8004), stick_p3.cfg (8005) y stick_p4.cfg (8006).

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Cantidad y tamanio se pueden pasar como argumentos: ./memory_stick.sh 2 256
# Si no vienen, se preguntan.
CANTIDAD="$1"
TAMANIO="$2"

if [ -z "$CANTIDAD" ]; then
    read -p "¿Cuantos Memory Sticks queres levantar? (1-4) [2]: " CANTIDAD
    CANTIDAD="${CANTIDAD:-2}"
fi

if [ "$CANTIDAD" -lt 1 ] || [ "$CANTIDAD" -gt 4 ]; then
    echo "ERROR: la cantidad debe estar entre 1 y 4 (hay 4 configs con puertos distintos)."
    exit 1
fi

if [ -z "$TAMANIO" ]; then
    read -p "Tamanio en bytes de cada stick [256]: " TAMANIO
    TAMANIO="${TAMANIO:-256}"
fi

for i in $(seq 1 "$CANTIDAD"); do
    CONFIG="config/stick_p$i.cfg"
    echo ">>> Levantando Memory Stick $i (config: $CONFIG, tamanio: $TAMANIO)..."
    gnome-terminal --title="Memory Stick $i" -- bash -c \
        "cd '$BASE_DIR/memory_stick' && ./bin/memory_stick $CONFIG $TAMANIO; exec bash"
    sleep 1
done

echo ">>> $CANTIDAD Memory Stick(s) levantado(s)."
