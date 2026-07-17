# Cómo compilar y levantar el proyecto

Guía de uso de los scripts de arranque. Hay tres formas de levantar el sistema,
de la más automática a la más manual. Las tres hacen lo mismo por debajo:
ejecutar cada binario desde su carpeta, en el orden correcto.

---

## 1. Compilar

```bash
./compilar_todo.sh
```

Compila los 7 módulos en orden. `utils` va primero **siempre** porque es la
librería compartida que los demás linkean. Si un módulo falla, el script se
detiene ahí y muestra el error de compilación.

Compilar un solo módulo a mano:

```bash
make -C kernel_memory      # o cpu, io, swap, etc.
```

---

## 2. Levantar todo de una (forma rápida)

```bash
./iniciar_todo.sh
```

Te hace 4 preguntas (todas con valor por defecto, Enter y listo):

| Pregunta | Default |
|---|---|
| ¿Cuántos Memory Sticks? (1-4) | 2 |
| Tamaño en bytes de cada stick | 256 |
| ¿Cuántas CPUs? | 1 |
| ¿Qué IOs levantar? | SLEEP STDOUT STDIN |

Después abre **una ventana de terminal por cada instancia**, en el orden
correcto de dependencias, esperando 2 segundos entre módulos para que cada
servidor llegue a escuchar antes de que arranque su cliente.

**Orden en que levanta (y por qué):**

```
1. kernel_memory     → servidor central, todos se conectan a él
2. memory_stick(s)   → se registran en kernel_memory
3. kernel_scheduler  → cliente de kernel_memory, servidor de cpu/io
4. swap              → cliente de kernel_memory
5. cpu(s)            → cliente de scheduler, memory y sticks
6. io(s)             → cliente de scheduler
```

La regla es una sola: **nadie puede conectarse a un socket que todavía no
escucha**. Servidores primero, clientes después.

---

## 3. Levantar módulo por módulo (scripts individuales)

En `scripts_modulos/` hay un script por módulo. Sirven para levantar solo una
parte del sistema (por ejemplo, reiniciar una CPU que se cayó sin tocar el resto).

| Script | Qué hace |
|---|---|
| `kernel_memory.sh` | Levanta Kernel Memory en la terminal actual |
| `memory_stick.sh` | Pregunta cuántos sticks (1-4) y el tamaño; abre una ventana por stick |
| `kernel_scheduler.sh` | Levanta el Scheduler en la terminal actual |
| `swap.sh` | Levanta Swap en la terminal actual |
| `cpu.sh` | Pregunta cuántas CPUs; abre una ventana por CPU (IDs 1, 2, 3...) |
| `io.sh` | Pregunta qué tipos (SLEEP/STDOUT/STDIN); abre una ventana por tipo |

Los que preguntan también aceptan la respuesta **por argumento** (no preguntan
si se la pasás):

```bash
./scripts_modulos/memory_stick.sh 2 256        # 2 sticks de 256 bytes
./scripts_modulos/cpu.sh 3                     # 3 CPUs (IDs 1, 2 y 3)
./scripts_modulos/io.sh SLEEP STDOUT           # solo esas dos IOs
```

Los de instancia única aceptan un config alternativo como argumento:

```bash
./scripts_modulos/kernel_memory.sh config/km_mem_best.cfg
./scripts_modulos/kernel_scheduler.sh config/ks_pcp.cfg otro_proceso
```

---

## 4. Levantar 100% a mano (como lo pide la cátedra)

Una terminal por módulo, **parado en la carpeta del módulo** (los paths de
config y logs son relativos). En este orden:

```bash
# Terminal 1 — Kernel Memory (SIEMPRE primero)
cd kernel_memory
./bin/kernel_memory config/kernel_memory.cfg

# Terminal 2 y 3 — Memory Sticks (el 2º parámetro es el tamaño en bytes)
cd memory_stick
./bin/memory_stick config/memory_stick.cfg 256      # terminal 2
./bin/memory_stick config/memory_stick_2.cfg 256    # terminal 3

# Terminal 4 — Kernel Scheduler (el 2º parámetro es el pseudocódigo inicial)
cd kernel_scheduler
./bin/kernel_scheduler config/kernel_scheduler.cfg proceso_inicial

# Terminal 5 — Swap
cd swap
./bin/swap config/swap.cfg

# Terminal 6 — CPU (el 2º parámetro es el ID de la CPU)
cd cpu
./bin/cpu config/cpu.cfg 1

# Terminales 7, 8, 9 — IO (una instancia por tipo)
cd io
./bin/io config/io.cfg SLEEP
./bin/io config/io.cfg STDOUT
./bin/io config/io.cfg STDIN     # esta es interactiva
```

---

## 5. Cómo saber que levantó bien

Cada módulo escribe en `logs/*.log` dentro de su carpeta. Señales de vida:

- **kernel_memory**: sticks registrados y líneas `Obtener instrucción`
- **memory_stick**: `## CPU 1 Conectada`
- **kernel_scheduler**: `## IO SLEEP Conectada` y transiciones `(0) Pasa del estado ...`
- **cpu**: el ciclo `FETCH → DECODE → EXECUTE`
- **swap**: `## Conectado a Kernel Memory`

Si un cliente muere apenas arranca, casi seguro su servidor no estaba levantado
todavía: revisá el orden.

---

## 6. Bajar todo

- `Ctrl+C` en cada ventana (idealmente clientes primero, servidores al final), o
- desde la raíz: `./stop_all.sh`

---

## 7. Puertos del sistema (para no pisarse)

| Puerto | Módulo |
|---|---|
| 8000 | Kernel Scheduler (escucha) |
| 8002 | Kernel Memory (escucha) |
| 8003-8006 | Memory Sticks 1 a 4 (`stick_p1.cfg` ... `stick_p4.cfg`) |

Por esto el límite de 4 sticks: hay 4 configs con puertos distintos. Para un
5º stick, crear `stick_p5.cfg` con `LISTEN_PORT=8007` y subir el límite en
`scripts_modulos/memory_stick.sh`.

---

## 8. Requisitos del entorno

- so-commons-library instalada (la usa todo el proyecto)
- `libreadline-dev` instalado (`sudo apt install libreadline-dev`) — el
  template de la cátedra lo linkea aunque el código no lo use; sin él, el
  linker falla con `no se puede encontrar -lreadline`
- `gnome-terminal` (para los scripts que abren ventanas; viene con Ubuntu)
