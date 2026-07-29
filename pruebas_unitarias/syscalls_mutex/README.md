# Prueba unitaria — Mutex: exclusión mutua y orden FIFO de desbloqueo

Verifica que sólo un Proceso pueda tener el mutex tomado, que el resto se bloquee, y que al
liberarse se lo vayan pasando **en el orden en que lo pidieron**.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Mutex"*:

> "Estos semáforos tendrán la particularidad de que, al ser de tipo Mutex, solo un Proceso
> los podrá tener tomado al mismo tiempo y el resto de Procesos deberán aguardar a ser
> atendidos. Al momento de liberar un mutex, en caso de que se cuente con procesos
> esperando por dicho mutex, los mismos se irán liberando en el orden en el que solicitaron
> el mutex. Las operaciones de creación y liberación de mutex, deberán volver a enviar el
> mismo proceso a la CPU que realizó la llamada."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.8.

La última oración —el retorno a la misma CPU— tiene su propia carpeta:
[`../syscalls_retorno_cpu/`](../syscalls_retorno_cpu/). La herencia de prioridades, que es
subsección aparte, también:
[`../syscalls_mutex_herencia/`](../syscalls_mutex_herencia/).

---

## Cómo levantarla

```bash
./pruebas_unitarias/syscalls_mutex/levantar.sh
```

**No hay que tocar nada.** Dura ~15 segundos.

### Qué se espera ver

Corrida real, filtrada:

```
05:25:45:311  ## (1) - Solicitó syscall: MUTEX_LOCK
05:25:45:311  ## (1) Toma el Mutex m1            <- libre: lo toma en el acto
05:25:45:566  ## (1) Pasa del estado EXEC al estado BLOCK    <- SLEEP 6000, sin soltarlo

05:25:46:648  ## (2) - Solicitó syscall: MUTEX_LOCK
05:25:46:648  ## (2) Pasa del estado EXEC al estado BLOCK    <- 1º en la cola
05:25:47:201  ## (3) - Solicitó syscall: MUTEX_LOCK
05:25:47:201  ## (3) Pasa del estado EXEC al estado BLOCK    <- 2º
05:25:47:756  ## (4) - Solicitó syscall: MUTEX_LOCK
05:25:47:756  ## (4) Pasa del estado EXEC al estado BLOCK    <- 3º

05:25:51:821  ## (1) Libera el Mutex m1
05:25:51:821  ## (2) Toma el Mutex m1           <- mismo milisegundo
05:25:52:464  ## (2) Libera el Mutex m1
05:25:52:464  ## (3) Toma el Mutex m1
05:25:53:106  ## (3) Libera el Mutex m1
05:25:53:106  ## (4) Toma el Mutex m1
```

### Lo que hay que mirar

**1. El orden de pedido es el orden de entrega.**

| PID | Lo pidió | Lo obtuvo |
|---|---|---|
| 2 | `46:648` (1º) | `51:821` (1º) |
| 3 | `47:201` (2º) | `52:464` (2º) |
| 4 | `47:756` (3º) | `53:106` (3º) |

Correspondencia exacta. Es una cola FIFO (`queue_push` al bloquearse en
`planificador.c:1404`, `queue_pop` al liberar en `:1447`), no una lista donde el último en
llegar se cuela.

**2. Exclusión mutua real.** Entre `45:311` y `51:821` hay exactamente **un** proceso con
el mutex tomado, y tres esperando en BLOCK. Nunca aparecen dos `Toma el Mutex m1` sin un
`Libera` en el medio.

**3. El traspaso es atómico.** `(1) Libera` y `(2) Toma` caen en el **mismo milisegundo**:
`planificador_unlock_mutex()` hace el `queue_pop` y le asigna el dueño nuevo dentro del
mismo `mutex_lista_mutex`, sin soltar el lock en el medio. No hay ventana en la que el
mutex quede libre y un cuarto proceso pueda robarlo.

**4. El que pide un mutex ocupado SÍ libera la CPU.** Es la excepción a la regla del
retorno directo: `MUTEX_LOCK` devuelve el proceso a la misma CPU **sólo si lo consigue**;
si no, hace `exec → block` + `liberar_cpu` (`conexiones.c:288-291`). Se ve en los tres
`Pasa del estado EXEC al estado BLOCK`.

---

## Archivos y pseudocódigos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (7 ventanas, 2 IO SLEEP, 1 CPU) |
| `test_padre` | PID 0 |
| `test_mtx_duenio` | PID 1: toma el mutex y lo retiene 6 s |
| `test_mtx_espera` | PIDs 2, 3 y 4: lo piden y se bloquean |

### `test_padre` (PID 0)

```
0  INIT_PROC test_mtx_duenio 0
1  SLEEP 1000                    <- deja que PID 1 cree el mutex y lo tome
2  INIT_PROC test_mtx_espera 0   <- PID 2
3  INIT_PROC test_mtx_espera 0   <- PID 3
4  INIT_PROC test_mtx_espera 0   <- PID 4
5  EXIT
```

El `SLEEP 1000` de la línea 1 no es decorativo: **garantiza el orden**. Si los tres
esperadores se crearan antes de que PID 1 tome el mutex, alguno podría agarrarlo primero y
la prueba mediría otra cosa.

### `test_mtx_duenio` (PID 1)

```
0  MUTEX_CREATE m1
1  MUTEX_LOCK m1
2  SLEEP 6000        <- se bloquea SIN soltar el mutex: eso libera la CPU
3  MUTEX_UNLOCK m1
4  EXIT
```

El `SLEEP` es la clave del diseño. PID 1 tiene que **retener el mutex pero soltar la CPU**,
si no los otros tres nunca podrían ejecutar su `MUTEX_LOCK` y no habría cola que observar.

### `test_mtx_espera` (PIDs 2, 3 y 4)

```
0  MUTEX_LOCK m1     <- se bloquea acá
1  NOOP
2  MUTEX_UNLOCK m1   <- y se lo pasa al siguiente
3  EXIT
```

---

## Las configs

- **`km_sys.cfg`**: sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/syscalls_mutex`.
- **`ks_sys.cfg`**: sólo cambia `SUSPENSION_TIMEOUT=15000` (el default es 1000), para que
  el `SLEEP 6000` de PID 1 no lo suspenda. Un PID 1 suspendido seguiría siendo dueño del
  mutex y la prueba funcionaría igual, pero el log se llenaría de transiciones de mediano
  plazo que no tienen nada que ver con lo que se está midiendo.

Algoritmo `FIFO` y 1 CPU: sin desalojos que ensucien la traza.

**Dos IO SLEEP**, aunque en el peor momento duermen dos procesos a la vez (PID 0 y PID 1):
una por durmiente concurrente. Con una sola, el que espera el dispositivo lo espera
**estando en BLOCK** y el timer de suspensión le corre igual. Está explicado en
[`../dessuspension_memoria/README.md`](../dessuspension_memoria/README.md).
