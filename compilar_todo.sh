#!/bin/bash
# Compila todos los modulos del TP, en orden.
# "utils" va SIEMPRE primero: es la libreria compartida que los demas linkean.
# Si un modulo falla, el script se detiene ahi y muestra el error.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODULOS="utils kernel_memory memory_stick kernel_scheduler cpu io swap"

for modulo in $MODULOS; do
    echo ""
    echo ">>> Compilando $modulo..."
    if make -C "$BASE_DIR/$modulo"; then
        echo ">>> $modulo: OK"
    else
        echo ">>> ERROR compilando $modulo. Se detiene la compilacion."
        exit 1
    fi
done

echo ""
echo ">>> Todos los modulos compilados correctamente."
