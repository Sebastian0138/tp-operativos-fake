# Prueba unitaria — BLOCK → SUSP. BLOCK (planificación de mediano plazo)

Verifica que un Proceso que entra a BLOCK y sigue ahí al vencer el timeout configurado
sea movido a SUSP. BLOCK, y que su memoria efectivamente se transfiera a SWAP.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Planificación de Mediano Plazo"*:

> "Para realizar la transición del estado BLOCK a SUSP. BLOCK, por cada Proceso que entre
> al estado BLOCK, se deberá esperar un tiempo determinado por archivo de configuración.
> Si al transcurrir dicho tiempo el Proceso aún continúa en estado BLOCK, se lo deberá
> mover al estado SUSP. BLOCK."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.4.

De yapa, la prueba muestra el **camino de vuelta** completo: SUSP. BLOCK → SUSP. READY →
READY, que es la otra mitad del mediano plazo.

---

## Cómo levantarla

```bash
./pruebas_unitarias/suspension/levantar.sh          # desde la raíz del repo
./pruebas_unitarias/suspension/levantar.sh 512      # sticks de 512 bytes (default: 256)
```

Requiere el proyecto compilado (`./compilar_todo.sh`) y `gnome-terminal`.

Abre las mismas 8 ventanas que la prueba de BSOD, en orden de dependencias.

**No hay que tocar nada.** El ciclo se repite solo, indefinidamente. Mirá la ventana
**4 KERNEL SCHEDULER**.

### Qué se espera ver — un ciclo completo dura ~30s

```
t+0s    ## (0) - Solicitó syscall: SLEEP
t+0s    ## (0) Pasa del estado EXEC al estado BLOCK
t+10s   ## (0) Pasa del estado BLOCK al estado SUSP. BLOCK      <- LA TRANSICIÓN QUE SE PRUEBA
t+10s   (KM swapea el segmento; en la ventana del SWAP: ## Escritura del bloque: 0)
t+30s   ## (0) finalizó IO y pasa a SUSP. READY
t+30s   ## (0) Pasa del estado SUSP. READY al estado READY      <- des-suspensión
t+30s   ## (0) Pasa del estado READY al estado EXEC
        ... y vuelve a empezar
```

Los `10s` salen de `SUSPENSION_TIMEOUT`; los `30s`, del `SLEEP 30000` del pseudocódigo.

Vale mirar también la ventana del **SWAP**: cada suspensión escribe un bloque y cada
des-suspensión lo lee. Si no ves esos logs, el proceso cambió de estado pero su memoria
no se movió, y la prueba está fallando aunque el log del KS diga lo correcto.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador de la prueba (8 ventanas) |
| `test_suspension_loop` | Pseudocódigo del proceso inicial (PID 0) |
| `km_susp.cfg` | Config del Kernel Memory para esta prueba |
| `ks_susp.cfg` | Config del Kernel Scheduler para esta prueba |

Los demás módulos (stick, swap, cpu, io) usan sus configs por defecto, sin cambios.

---

## El pseudocódigo — `test_suspension_loop`

```
 0  MEM_ALLOC 0 32     <- reserva 32 bytes: sin memoria asignada no hay nada que swapear
 1  SET AX 67
 2  SET DI 0
 3  MOV_OUT AX         <- escribe 'C' en la dirección lógica 0
 4  SET CX 1
 5  SLEEP 30000        <- syscall: EXEC -> BLOCK, y se queda 30s
 6  JNZ CX 5           <- CX vale 1 y nunca cambia: salta siempre a la 5
 7  MEM_FREE 0         <- inalcanzable
 8  EXIT               <- inalcanzable
```

El `MEM_ALLOC` no es decorativo: un proceso sin segmentos se suspende igual, pero no hay
transferencia a SWAP y no se ve la mitad interesante del mecanismo.

El `SLEEP 30000` tiene que ser **mayor** que `SUSPENSION_TIMEOUT`, si no el proceso vuelve
de la IO antes de que venza el timer y nunca se suspende. Con 30000 contra 10000 hay
margen de sobra.

El `JNZ CX 5` hace que el ciclo se repita para siempre, así se puede observar con calma en
vez de tener una sola oportunidad.

---

## Las configs — qué cambia y por qué

### `km_susp.cfg` (Kernel Memory)

Idéntico a `kernel_memory/config/kernel_memory.cfg` salvo por:

```ini
SCRIPTS_BASEPATH=../pruebas_unitarias/suspension
```

El KM arma el path como `SCRIPTS_BASEPATH + "/" + nombre`
(`kernel_memory_atencion_scheduler.c:35-37`) y corre parado en `kernel_memory/`, así que
la ruta es relativa a esa carpeta. De ahí el `../`.

### `ks_susp.cfg` (Kernel Scheduler)

Idéntico a `kernel_scheduler/config/kernel_scheduler.cfg` salvo por:

```ini
SUSPENSION_TIMEOUT=10000    # el default es 1000
```

Es el parámetro que la prueba ejerce. El default de 1000 ms funciona igual, pero suspende
tan rápido que en la ventana no se llega a distinguir el BLOCK del SUSP. BLOCK: aparecen
casi pegados. Con 10 segundos hay una pausa clara y se ve que el proceso *estuvo* en BLOCK
antes de suspenderse — que es justamente la condición que pide el enunciado ("si al
transcurrir dicho tiempo el Proceso **aún continúa** en estado BLOCK").

El resto queda como el default: `FIFO`, `QUEUE_PREEMPTION=FALSE`, escucha en `8000`,
memoria en `127.0.0.1:8002`.

---

## Nota — el motivo de bloqueo importa

No todo BLOCK se suspende. En `planificador_transicionar_exec_a_block()`
(`planificador.c:596`) el timer de suspensión **no se arma** si el motivo es `"MEMORIA"`:

```c
if (strcmp(motivo_bloqueo, "MEMORIA") != 0) { ...arma el timer... }
```

Un proceso bloqueado esperando memoria (un `MEM_ALLOC` que no entró) tiene un pedido
pendiente en el gestor de memoria; swapearlo dejaría un estado inconsistente. Por eso esta
prueba usa `SLEEP`, que bloquea con motivo `"SLEEP"` (`conexiones.c:226`).

> Variante: para que el proceso se quede en SUSP. BLOCK **indefinidamente** en vez de
> ciclar, cambiá el `SLEEP 30000` por un `STDIN` y no tipees nada en la ventana del IO
> STDIN. El proceso se suspende a los 10s y se queda ahí hasta que escribas.
