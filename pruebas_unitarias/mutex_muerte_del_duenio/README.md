# Prueba unitaria — Muerte del dueño de un mutex

Verifica que, si un Proceso finaliza **sin liberar** los mutex que tenía tomados, el Kernel
Scheduler los libere por él y despierte a los que estaban esperando.

Sin esto, los procesos bloqueados quedan esperando **para siempre**: el mutex tiene dueño y
el dueño ya no existe.

## Qué parte del enunciado prueba

Ninguna, de forma literal. `Plug & Pray.pdf`, sección *"Mutex"*, dice:

> "solo un Proceso los podrá tener tomado al mismo tiempo y el resto de Procesos deberán
> aguardar a ser atendidos. Al momento de liberar un mutex, en caso de que se cuente con
> procesos esperando por dicho mutex, los mismos se irán liberando en el orden en el que
> solicitaron el mutex."

pero **no dice qué pasa si el dueño se muere sin liberar**. Es una decisión de
implementación, tomada en `finalizar_proceso()` (`conexiones.c:143-150`):

```c
// Un proceso que finaliza no puede dejar mutex tomados para siempre:
// se liberan en orden y se despierta al siguiente de cada cola.
while (list_size(pcb->lista_mutex_tomados) > 0) {
    char* nombre = list_get(pcb->lista_mutex_tomados, 0);
    char* copia = strdup(nombre);
    planificador_unlock_mutex(_planificador, pcb, copia);
    free(copia);
}
```

Esta prueba existe porque **es la pregunta que te hacen en la defensa**: *"¿y si el proceso
se muere con el mutex tomado?"*. Y porque el escenario es realista: un `EXIT` sin `UNLOCK`,
un `SEG_FAULT`, o un BSOD dejan mutex huérfanos.

Las cláusulas que el enunciado **sí** exige están en
[`../syscalls_mutex/`](../syscalls_mutex/) (exclusión mutua y orden FIFO),
[`../syscalls_retorno_cpu/`](../syscalls_retorno_cpu/) (retorno a la misma CPU) y
[`../syscalls_mutex_herencia/`](../syscalls_mutex_herencia/) (herencia de prioridades).

Trazabilidad en `docs/MAPA_TP.md` §2.8.

---

## Cómo levantarla

```bash
./pruebas_unitarias/mutex_muerte_del_duenio/levantar.sh
```

**No hay que tocar nada.** Dura ~20 segundos.

### Qué se espera ver

Corrida real, filtrada:

```
06:27:30:848  ## (1) Toma el Mutex m1
06:27:31:103  ## (1) Toma el Mutex m2

06:27:32:432  ## (2) - Solicitó syscall: MUTEX_LOCK
06:27:32:432  ## (2) Pasa del estado EXEC al estado BLOCK      <- esperando m1
06:27:32:983  ## (3) - Solicitó syscall: MUTEX_LOCK
06:27:32:983  ## (3) Pasa del estado EXEC al estado BLOCK      <- esperando m2

06:27:37:613  ## (1) finalizó su ejecución con motivo de SUCCESS   <- MUERE SIN LIBERAR
06:27:37:613  ## (2) Pasa del estado BLOCK al estado READY
06:27:37:613  ## (2) Toma el Mutex m1                          <- mismo milisegundo
06:27:37:613  ## (3) Pasa del estado BLOCK al estado READY
06:27:37:613  ## (3) Toma el Mutex m2                          <- los DOS

06:27:38:000  ## (2) Libera el Mutex m1                        <- cada uno el suyo
06:27:38:255  ## (2) finalizó su ejecución con motivo de SUCCESS
06:27:38:644  ## (3) Libera el Mutex m2
06:27:38:900  ## (3) finalizó su ejecución con motivo de SUCCESS
```

### Lo que hay que mirar

**1. PID 1 nunca pidió un `MUTEX_UNLOCK`.** Verificable por conteo sobre el log:

```
(1) - Solicitó syscall: MUTEX_UNLOCK   ->  0 veces
(1) Libera el Mutex                    ->  0 veces
```

Su pseudocódigo va del `MUTEX_LOCK m2` directo al `EXIT`. Y aun así los dos mutex
cambiaron de dueño.

**2. Los dos mutex se liberan, no uno.** `finalizar_proceso()` recorre
`lista_mutex_tomados` en un `while`, no libera sólo el primero. Con **un solo** mutex esta
prueba no distinguiría entre "libera todos" y "libera el primero y se olvida del resto":
por eso el dueño toma **dos**.

**3. Cada waiter recibe el suyo.** PID 2 despierta con `m1` y PID 3 con `m2` — no se
cruzan. Y después cada uno libera el que le corresponde (`(2) Libera m1`, `(3) Libera m2`).

**4. Todo ocurre en el mismo milisegundo que el `EXIT`.** No hay un hilo de limpieza que
pase cada tanto: la liberación es sincrónica, dentro de `finalizar_proceso()`.

**5. Ausencia de log de `Libera` en la liberación forzada.** Es la firma del caso:
`## (<PID>) Libera el Mutex <M>` se emite en el **handler de la syscall**
(`conexiones.c:306`), no dentro de `planificador_unlock_mutex()`. Cuando libera el kernel,
el mutex cambia de dueño sin que nadie loguee un `Libera`. Lo único que se ve es al que
esperaba tomándolo.

> Los conteos finales cierran: **4 `Toma`** (PID 1 dos veces, PID 2 y PID 3 una cada uno)
> y **2 `Libera`** (sólo los voluntarios de PID 2 y PID 3).

---

## Archivos y pseudocódigos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (7 ventanas, 2 IO SLEEP, 1 CPU) |
| `test_padre` | PID 0 |
| `test_mtx_suicida` | PID 1: toma dos mutex y se muere sin liberarlos |
| `test_mtx_espera_1` | PID 2: espera `m1` |
| `test_mtx_espera_2` | PID 3: espera `m2` |

### `test_padre` (PID 0)

```
0  INIT_PROC test_mtx_suicida 0
1  SLEEP 1500                     <- deja que PID 1 cree y tome los dos mutex
2  INIT_PROC test_mtx_espera_1 0
3  INIT_PROC test_mtx_espera_2 0
4  EXIT
```

El `SLEEP 1500` garantiza el orden: si los esperadores arrancaran antes, alguno tomaría un
mutex libre y no habría cola que heredar.

### `test_mtx_suicida` (PID 1)

```
0  MUTEX_CREATE m1
1  MUTEX_CREATE m2
2  MUTEX_LOCK m1
3  MUTEX_LOCK m2
4  SLEEP 6000        <- retiene los dos mientras PID 2 y PID 3 se encolan
5  EXIT              <- SIN NINGUN MUTEX_UNLOCK
```

El `SLEEP` cumple dos funciones: retener los mutex **y** soltar la CPU, para que los otros
dos puedan ejecutar su `MUTEX_LOCK` y encolarse.

### `test_mtx_espera_1` y `test_mtx_espera_2` (PIDs 2 y 3)

```
0  MUTEX_LOCK m1        (o m2)
1  NOOP
2  MUTEX_UNLOCK m1      (o m2)
3  EXIT
```

Son dos archivos distintos, uno por mutex: así se comprueba que cada waiter recibe **el
suyo** y no cualquiera.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/mutex_muerte_del_duenio`.
- **`ks_sys.cfg`**: sólo cambia `SUSPENSION_TIMEOUT=15000` (el default es 1000). PID 1
  duerme 6 s y los waiters pasan ~5 s bloqueados en el mutex; con el timeout bajo se
  suspenderían los tres y el log se llenaría de swap.

  Un waiter suspendido igual funcionaría —`planificador_unlock_mutex()` contempla ese caso
  (`planificador.c:1466`, pasa a SUSP. READY con el mutex ya tomado)— pero es otra prueba.

FIFO y 1 CPU: sin desalojos que ensucien la traza.

---

## Nota de metodología: cuidado con las corridas contaminadas

La primera corrida de esta prueba dio un resultado alarmante: PID 2 tomaba `m1` pero después
**PID 3** aparecía liberándolo, y PID 2 nunca finalizaba. Parecía un bug serio de
atribución de syscalls.

No lo era. El log lo delató:

```
## CPU 1 Conectada      06:25:45:713
## CPU 1 Conectada      06:25:49:905      <- DOS CPUs, las dos con ID 1
```

Un lanzamiento anterior había quedado a medio ejecutar y siguió abriendo ventanas después
del `pkill` del siguiente. Con dos CPUs registradas bajo el mismo ID, el planificador
despachó dos procesos "a la vez" y las syscalls quedaron cruzadas.

**Antes de reportar un bug raro, contá las conexiones:**

```bash
grep -c "CPU .* Conectada" kernel_scheduler/logs/kernel_scheduler.log
pgrep -af "bin/cpu"
```

Si los números no son los que esperabas, la corrida no sirve: bajá todo, esperá, y volvé a
levantar.
