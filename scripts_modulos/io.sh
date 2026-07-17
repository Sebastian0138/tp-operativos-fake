#!/bin/bash
# Levanta una o mas interfaces de IO, cada una en su propia ventana de terminal.
# Cada instancia atiende UN tipo de IO: SLEEP, STDOUT o STDIN.
# Por defecto levanta las tres (una de cada tipo), que es lo usual para las pruebas.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="config/io.cfg"

# Los tipos se pueden pasar como argumentos: ./io.sh SLEEP STDOUT
# Si no vienen, se preguntan.
TIPOS="$*"
if [ -z "$TIPOS" ]; then
    echo "Tipos disponibles: SLEEP, STDOUT, STDIN"
    read -p "¿Que IOs queres levantar? (separadas por espacio) [SLEEP STDOUT STDIN]: " TIPOS
    TIPOS="${TIPOS:-SLEEP STDOUT STDIN}"
fi

for tipo in $TIPOS; do
    case "$tipo" in
        SLEEP|STDOUT|STDIN) ;;
        *) echo "ERROR: tipo invalido '$tipo'. Validos: SLEEP, STDOUT, STDIN."; exit 1 ;;
    esac
done

for tipo in $TIPOS; do
    echo ">>> Levantando IO $tipo..."
    gnome-terminal --title="IO $tipo" -- bash -c \
        "cd '$BASE_DIR/io' && ./bin/io $CONFIG $tipo; exec bash"
    sleep 1
done

echo ">>> IOs levantadas: $TIPOS"
