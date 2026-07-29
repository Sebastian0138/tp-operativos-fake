#!/bin/bash
# Conecta un SEGUNDO Memory Stick al sistema ya levantado.
#
# Uso:
#   ./pruebas_unitarias/dessuspension_stick/agregar_stick.sh          # 128 bytes
#   ./pruebas_unitarias/dessuspension_stick/agregar_stick.sh 256      # otro tamanio
#
# Usa memory_stick_2.cfg, que escucha en el puerto 8004 (el primer stick usa el 8003).
#
# Al registrarse, gestor_memoria_registrar_stick() amplia la memoria total y notifica
# MENSAJE_MAS_MEMORIA al Kernel Scheduler por el canal de notificaciones. El Scheduler
# llama a planificador_evento_memoria_liberada() y recorre los suspendidos.

set -u

BASE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TAM=${1:-128}

if ! pgrep -f "[b]in/kernel_memory" > /dev/null; then
    echo "ERROR: Kernel Memory no esta corriendo. Levanta la prueba primero:"
    echo "  ./pruebas_unitarias/dessuspension_stick/levantar.sh"
    exit 1
fi

echo ">>> Conectando Memory Stick 2 ($TAM bytes, puerto 8004)..."
gnome-terminal --title="2b MEMORY STICK 2" -- bash -c \
    "cd '$BASE/memory_stick' && ./bin/memory_stick config/memory_stick_2.cfg $TAM; \
     echo; echo '--- 2b MEMORY STICK 2 TERMINADO ---'; exec bash"

echo ">>> Mira la ventana '4 KERNEL SCHEDULER': deberian des-suspenderse los 3 hijos."
