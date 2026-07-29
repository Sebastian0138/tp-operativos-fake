# Prueba unitaria — BSOD (Blue Screen of Death)

Verifica que, al desconectarse un Memory Stick, el Kernel Memory detecte la memoria
corrupta, se lo notifique al Kernel Scheduler por el canal dedicado, y el Scheduler
finalice **todos** los procesos de **todas** las colas antes de morir.

## Qué parte del enunciado prueba

`Plug & Pray.pdf` — dos secciones, una por cada lado del mecanismo.

**Kernel Memory**, sección *"Desconexión de un Memory Stick"*:

> "Existe la posibilidad de que en medio de una ejecución un Memory Stick se desconecte.
> Dado el caso, se deberá notificar al Kernel Scheduler que la memoria se encuentra
> corrupta."

**Kernel Scheduler**, sección *"Planificación de Largo Plazo"*:

> "En algún momento de la ejecución es posible que se reciba una notificación del Kernel
> Memory de que se detectó corrupción en una parte de la memoria. Ante este evento, se
> deberán finalizar todos los Procesos y finalizar el Kernel Scheduler con motivo de Blue
> Screen of Death (BSOD)."

Notar qué **no** dice: al Kernel Memory no se le pide morir, y al SWAP ni se lo menciona.
Que ambos sigan vivos después del BSOD es correcto, no un bug. Ver la tabla de abajo.

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.10.

---

## Cómo levantarla

```bash
./pruebas_unitarias/bsod/levantar.sh          # desde la raíz del repo
./pruebas_unitarias/bsod/levantar.sh 512      # con sticks de 512 bytes (default: 256)
```

Requiere el proyecto compilado (`./compilar_todo.sh`) y `gnome-terminal`.

El script baja cualquier corrida anterior, borra los logs y abre **8 ventanas**, en
orden de dependencias (servidores primero, clientes después):

```
1 KERNEL MEMORY     4 KERNEL SCHEDULER   7 IO STDIN
2 MEMORY STICK      5 IO SLEEP           8 CPU 1
3 SWAP              6 IO STDOUT
```

Cada ventana sobrevive a la muerte de su módulo (`--- X TERMINADO ---`), así se puede
leer la salida sin que se cierre.

### Disparar el BSOD

Una vez que el PID 0 esté ciclando (lo ves en la ventana del IO SLEEP), matá el stick:

- `Ctrl+C` en la ventana **2 MEMORY STICK**, o
- `kill $(pgrep -f '[b]in/memory_stick' | tail -n1)`

No hay apuro: el proceso inicial cicla indefinidamente (ver abajo).

### Qué se espera ver

Los tres eventos centrales caen en el **mismo milisegundo** — la notificación no es por
polling, viaja por el canal dedicado KM→KS:

```
KS   ## (0) Pasa del estado EXEC al estado BLOCK          <- durmiendo, con datos vivos en el stick
KM   Memory Stick 0 desconectado: memoria corrupta        <- detecta el recv caído
KS   Se detectó memoria corrupta: iniciando Blue Screen of Death (BSOD)
KS   ## (0) finalizó su ejecución con motivo de BSOD      <- motivo BSOD, no SUCCESS
KS   Finalizando el Kernel Scheduler con motivo de BSOD
CPU  Kernel Scheduler desconectado. Cerrando CPU 1.       <- caída en cascada
```

Sobreviven **kernel_memory** y **swap**: el BSOD hace `exit(EXIT_FAILURE)` sólo en el
Kernel Scheduler. La CPU y las IOs se caen solas al perder a su servidor.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador de la prueba (8 ventanas) |
| `test_bsod_loop` | Pseudocódigo del proceso inicial (PID 0) |
| `km_bsod.cfg` | Config del Kernel Memory para esta prueba |
| `ks_bsod.cfg` | Config del Kernel Scheduler para esta prueba |

Los demás módulos (stick, swap, cpu, io) usan sus configs por defecto, sin cambios.

---

## El pseudocódigo — `test_bsod_loop`

```
 0  MEM_ALLOC 0 32     <- reserva un segmento de 32 bytes
 1  SET AX 65
 2  SET DI 0
 3  MOV_OUT AX         <- escribe 'A' en la dirección lógica 0
 4  SET AX 66
 5  SET DI 1
 6  MOV_OUT AX         <- escribe 'B' en la dirección lógica 1
 7  SET CX 1
 8  SLEEP 3000         <- syscall: EXEC -> BLOCK
 9  JNZ CX 8           <- CX vale 1 y nunca cambia: salta siempre a la 8
10  MEM_FREE 0         <- inalcanzable
11  EXIT               <- inalcanzable
```

Las dos escrituras existen para que el stick **tenga datos reales** cuando lo matemos.
Sin eso, el BSOD dispara igual pero se pierde el punto: un proceso muriendo con memoria
viva en el dispositivo que se desconectó.

El `JNZ CX 8` hace que el proceso cicle `BLOCK -> READY -> EXEC -> BLOCK` para siempre.
Es a propósito: con un `SLEEP 30000` seco tenés 30 segundos para reaccionar y matar el
stick, y no alcanzan. Acá la ventana es infinita.

---

## Las configs — qué cambia y por qué

### `km_bsod.cfg` (Kernel Memory)

Idéntico a `kernel_memory/config/kernel_memory.cfg` salvo por:

```ini
SCRIPTS_BASEPATH=../pruebas_unitarias/bsod
```

El Kernel Memory arma el path del pseudocódigo como `SCRIPTS_BASEPATH + "/" + nombre`
(`kernel_memory_atencion_scheduler.c:35-37`) y corre parado en `kernel_memory/`, así que
la ruta es **relativa a esa carpeta**. Por eso el `../`. Sin este cambio, KM buscaría
`test_bsod_loop` en `kernel_memory/scripts/` y no lo encontraría.

El resto queda como el default: `BEST` fit, `SEGMENT_MAX_SIZE=256`,
`INSTRUCTION_DELAY=50`, `COMPACTION_DELAY=100`, escucha en `8002`.

### `ks_bsod.cfg` (Kernel Scheduler)

Idéntico a `kernel_scheduler/config/kernel_scheduler.cfg` salvo por:

```ini
SUSPENSION_TIMEOUT=45000    # el default es 1000
```

**Este es el cambio importante de toda la prueba.** Con el default de 1000 ms, el
planificador de mediano plazo suspende cualquier proceso que lleve más de un segundo en
BLOCK (`planificador_transicionar_exec_a_block()`, `planificador.c:596`). Como el proceso
duerme 3000 ms, se iría a SUSP. BLOCK y KM lo swapearía a `swap.bin`.

Resultado: cuando matás el stick, la memoria del proceso ya **no está ahí**. El BSOD
dispara igual (lo dispara KM al detectar el socket caído, no depende de dónde esté la
memoria), pero se pierde justamente lo que la prueba quiere mostrar.

Con 45000 ms el timer de suspensión nunca vence y el proceso se queda en BLOCK, con su
segmento vivo en el stick.

El resto queda como el default: `FIFO`, `QUEUE_PREEMPTION=FALSE`, `RR_QUANTUM=100`
(irrelevante en FIFO), escucha en `8000`, memoria en `127.0.0.1:8002`.

> Para ver la variante suspendida —proceso en SUSP. BLOCK cuando muere el stick— bajá
> `SUSPENSION_TIMEOUT` a 1000 en `ks_bsod.cfg`. Son dos pruebas distintas y las dos valen.
