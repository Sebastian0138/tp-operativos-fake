#!/bin/bash
# Levanta TODO el sistema, cada modulo en su propia ventana de terminal,
# en el orden que piden las dependencias (servidores primero, clientes despues):
#
#   1. kernel_memory   (servidor central: todos se conectan a el)
#   2. memory_stick    (se registra en kernel_memory)
#   3. kernel_scheduler (cliente de kernel_memory, servidor de cpu/io)
#   4. swap            (cliente de kernel_memory)
#   5. cpu             (cliente de scheduler, memory y sticks)
#   6. io              (cliente de scheduler)
#
# Primero pregunta cuantas instancias queres de los modulos multiples,
# y despues abre todas las ventanas.

BASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPTS="$BASE_DIR/scripts_modulos"

# Segundos de espera entre modulos, para que cada servidor llegue a escuchar
# antes de que arranque su cliente.
DELAY=2

# --- Preguntas (una sola vez, aca) -------------------------------------

read -p "¿Cuantos Memory Sticks? (1-4) [2]: " CANT_STICKS
CANT_STICKS="${CANT_STICKS:-2}"

read -p "Tamanio en bytes de cada stick [256]: " TAM_STICK
TAM_STICK="${TAM_STICK:-256}"

read -p "¿Cuantas CPUs? [1]: " CANT_CPUS
CANT_CPUS="${CANT_CPUS:-1}"

echo "Tipos de IO disponibles: SLEEP, STDOUT, STDIN"
read -p "¿Que IOs levantar? (separadas por espacio) [SLEEP STDOUT STDIN]: " TIPOS_IO
TIPOS_IO="${TIPOS_IO:-SLEEP STDOUT STDIN}"

# --- Arranque ----------------------------------------------------------

echo ""
echo ">>> 1/6 Kernel Memory..."
gnome-terminal --title="Kernel Memory" -- bash -c "'$SCRIPTS/kernel_memory.sh'; exec bash"
sleep "$DELAY"

echo ">>> 2/6 Memory Sticks ($CANT_STICKS de $TAM_STICK bytes)..."
"$SCRIPTS/memory_stick.sh" "$CANT_STICKS" "$TAM_STICK"
sleep "$DELAY"

echo ">>> 3/6 Kernel Scheduler..."
gnome-terminal --title="Kernel Scheduler" -- bash -c "'$SCRIPTS/kernel_scheduler.sh'; exec bash"
sleep "$DELAY"

echo ">>> 4/6 Swap..."
gnome-terminal --title="Swap" -- bash -c "'$SCRIPTS/swap.sh'; exec bash"
sleep "$DELAY"

echo ">>> 5/6 CPUs ($CANT_CPUS)..."
"$SCRIPTS/cpu.sh" "$CANT_CPUS"
sleep "$DELAY"

echo ">>> 6/6 IOs ($TIPOS_IO)..."
"$SCRIPTS/io.sh" $TIPOS_IO

echo ""
echo ">>> Sistema levantado. Cada modulo esta en su propia ventana."
echo ">>> Para bajar todo: cerra las ventanas o usa ./stop_all.sh"
