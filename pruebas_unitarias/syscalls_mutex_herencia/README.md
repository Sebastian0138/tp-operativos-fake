# Prueba unitaria — Mutex: herencia de prioridades

Verifica la resolución de la **inversión de prioridades**: si un Proceso de prioridad baja
retiene un mutex que necesita uno de prioridad alta, el de baja hereda temporalmente la
prioridad del otro, y la devuelve al liberar.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Herencia de prioridades"*:

> "A lo largo de la ejecución puede darse la situación en la que un Proceso de prioridad
> baja tome un Mutex libre, y que luego de esto, un Proceso con mayor prioridad intente
> tomar el mismo mutex. Al estar previamente tomado por el Proceso de menor prioridad, este
> segundo queda bloqueado. Ante este evento llamado Inversión de Prioridades, donde un
> Proceso de una prioridad mayor queda bloqueado a la espera de un Proceso con prioridad
> menor, este último heredará temporalmente la prioridad del Proceso con mayor prioridad
> que se encuentre bloqueado esperando el Mutex. Una vez que el Proceso de baja prioridad
> libere el Mutex, volverá a tener su prioridad original."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.8.

---

## Cómo levantarla

```bash
./pruebas_unitarias/syscalls_mutex_herencia/levantar.sh
```

**No hay que tocar nada.** Dura ~12 segundos.

### Qué se espera ver

Corrida real, filtrada:

```
05:26:20:853  ## (1) - Solicitó syscall: MUTEX_CREATE
05:26:21:108  ## (1) - Solicitó syscall: MUTEX_LOCK
05:26:21:108  ## (1) Toma el Mutex m1              <- PID 1, prioridad 1 (BAJA)

05:26:22:876  ## (2) - Solicitó syscall: MUTEX_LOCK    <- PID 2, prioridad 0 (ALTA)
05:26:22:876  ## 1 Cambio de prioridad: 1 0            <- HERENCIA, mismo milisegundo

05:26:26:388  ## (1) - Solicitó syscall: MUTEX_UNLOCK
05:26:26:388  ## (1) Libera el Mutex m1
05:26:26:388  ## 1 Cambio de prioridad: 0 1            <- RESTAURACIÓN
05:26:26:388  ## (2) Toma el Mutex m1
```

### Lo que hay que mirar

**1. La herencia salta en el mismo milisegundo que el bloqueo.** No hay un hilo que revise
periódicamente: `planificador_lock_mutex()` (`planificador.c:1411-1418`), cuando no puede
otorgar el mutex, busca al dueño y compara en el acto:

```c
if (duenio != NULL && pcb->prioridad_actual < duenio->prioridad_actual) {
    cambiar_prioridad(pl, duenio, pcb->prioridad_actual);
}
```

**2. Es condicional, no automática.** Sólo hereda si el que se bloquea es *estrictamente*
más prioritario. Si PID 2 tuviera prioridad 1 o 2, no habría ningún `Cambio de prioridad`
— y estaría bien. Probalo cambiando el `INIT_PROC test_mtx_alto 0` por `... 2`.

**3. Vuelve a la original, no a "una mejor".** El log de restauración dice `0 1`: PID 1
recupera **su** prioridad, la que tenía al crearse. `planificador_unlock_mutex()`
(`:1435-1437`) compara contra `prioridad_original`, un campo aparte de `prioridad_actual`
(`pcb.h:22-23`). Sin esos dos campos separados no habría a dónde volver.

**4. En CMN, cambiar de prioridad implica cambiar de cola.** `cambiar_prioridad()`
(`planificador.c:1348`) no sólo actualiza el número: si el proceso está en READY, lo
reubica en la cola que le corresponde. Acá PID 1 estaba bloqueado en su `SLEEP`, así que se
ve sólo el log, pero el mecanismo es el mismo.

---

## El log de cambio de prioridad: verificado contra el PDF

```
Enunciado:  ## <PID> Cambio de prioridad: <PRIORIDAD_ANTERIOR> <PRIORIDAD_NUEVA>
Código:     ## 1 Cambio de prioridad: 1 0
```

**Coincide.** El enunciado no lleva guion entre las dos prioridades.

> Una versión anterior de `docs/MAPA_TP.md` §9 afirmaba que faltaba un `" - "` ahí. Era
> falso. Contrastado con el texto literal del PDF (`pdftotext "Plug & Pray.pdf"`, sección
> "Logs mínimos y obligatorios" del Kernel Scheduler), el formato del código es el correcto
> y **no hay que tocarlo**.

---

## Archivos y pseudocódigos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (7 ventanas, 2 IO SLEEP, 1 CPU) |
| `test_padre` | PID 0 |
| `test_mtx_bajo` | PID 1, prioridad **1**: toma el mutex y lo retiene |
| `test_mtx_alto` | PID 2, prioridad **0**: lo pide y se bloquea |

### `test_padre` (PID 0)

```
0  INIT_PROC test_mtx_bajo 1     <- PID 1, prioridad 1 -> cola 1
1  INIT_PROC test_mtx_alto 0     <- PID 2, prioridad 0 -> cola 0
2  EXIT
```

### `test_mtx_bajo` (PID 1, prioridad baja)

```
0  MUTEX_CREATE m1
1  MUTEX_LOCK m1
2  SLEEP 1000       }
3  SLEEP 1000       } cede la CPU 4 veces SIN soltar el mutex
4  SLEEP 1000       }
5  SLEEP 1000       }
6  MUTEX_UNLOCK m1
7  EXIT
```

**Por qué cuatro `SLEEP` cortos y no trabajo de CPU:** PID 1 tiene que retener el mutex
durante varios segundos pero **cediendo la CPU**. Con `QUEUE_PREEMPTION=FALSE`, si PID 1
hiciera trabajo puro no soltaría nunca el procesador, PID 2 no podría siquiera ejecutar su
`MUTEX_LOCK`, y no habría inversión de prioridades que resolver. La prueba se colgaría sin
fallar.

Cada `SLEEP` es de 1000 ms contra un `SUSPENSION_TIMEOUT` de 15000: cede pero no se
suspende.

### `test_mtx_alto` (PID 2, prioridad alta)

```
0  SLEEP 2000       <- llega tarde a propósito: PID 1 ya tiene el mutex
1  MUTEX_LOCK m1    <- se bloquea -> dispara la herencia
2  MUTEX_UNLOCK m1
3  EXIT
```

El `SLEEP 2000` fija el orden. Sin él, PID 2 (que está en la cola más prioritaria y por lo
tanto se despacha primero) tomaría el mutex antes que PID 1 y no habría inversión.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia `SCRIPTS_BASEPATH`.
- **`ks_sys.cfg`**:

```ini
PLANIFICATION_ALGORITHM=CMN      # el default es FIFO
QUEUES_ALGORITHMS=[FIFO,FIFO]    # 2 colas; FIFO en las dos para que no haya quantum
QUEUE_PREEMPTION=FALSE           # ya era el default
SUSPENSION_TIMEOUT=15000         # el default es 1000
```

**Hace falta CMN**: con FIFO o RR *"la prioridad no tendrá injerencia alguna"* según el
enunciado, así que no habría prioridades que heredar. La herencia sólo tiene sentido con
Colas Multinivel.

**`[FIFO,FIFO]` a propósito**: con RR en alguna cola, el quantum llenaría el log de
desalojos y taparía los dos `Cambio de prioridad`, que son las dos líneas que importan.

**`QUEUE_PREEMPTION=FALSE`**: para aislar la herencia del desalojo entre colas, que ya
tiene su prueba en [`../pcp_cmn_con_desalojo/`](../pcp_cmn_con_desalojo/). Con `TRUE`, PID 2
además desalojaría a PID 1 al volver de su sleep y habría dos mecanismos mezclados en la
misma traza.
