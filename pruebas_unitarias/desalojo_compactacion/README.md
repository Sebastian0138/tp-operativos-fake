# Prueba unitaria — Desalojo por compactación (lado Kernel Scheduler)

Verifica las cuatro cosas que el enunciado le exige al Scheduler cuando el Kernel Memory
pide compactar: interrumpir **todas** las CPUs, **no despachar a nadie** mientras dura,
reencolar a los desalojados **al principio** de READY, y replanificar al terminar.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Desalojo por compactación"*:

> "Dentro de la ejecución normal de los diferentes Procesos puede existir la situación en la
> cual un Proceso, al momento de intentar crear un segmento, dispare la compactación de la
> memoria. Cuando esto suceda, se recibirá el pedido desde el módulo Kernel Memory para que
> se desalojen todos los Procesos antes de iniciar la compactación.
>
> Al recibir este pedido, el Kernel Scheduler enviará la solicitud de desalojo a todas las
> CPU y no enviará ningún otro Proceso a ejecutar hasta que reciba la confirmación por parte
> del Kernel Memory que finalizó la compactación. Los Procesos desalojados,
> excepcionalmente, serán colocados al principio de la cola de READY.
>
> Una vez finalizada la misma se procederá a replanificar todos los Procesos de acuerdo al
> algoritmo definido."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.7.

> La compactación **en sí** (el lado del Kernel Memory: mover segmentos, consolidar huecos)
> y su efecto sobre los suspendidos están en
> [`../dessuspension_compactacion/`](../dessuspension_compactacion/). Ésta se ocupa
> exclusivamente de lo que hace el **Scheduler** mientras tanto.

---

## Cómo levantarla

```bash
./pruebas_unitarias/desalojo_compactacion/levantar.sh
```

**No hay que tocar nada.** Dura ~30 segundos.

### Qué se espera ver

Corrida real, uniendo los tres logs:

```
05:40:35:629  KS   ## (3) Pasa del estado NEW al estado READY      <- el testigo, a esperar

05:40:41:180  KM   PID: 0 - Hay 100 bytes libres pero no contiguos para 30:
                   se necesita compactar
05:40:41:181  KS   Kernel Memory pide compactar: desalojando todas las CPUs...

05:40:41:258  CPU  [cpu-proc 27661] ## Interrupción recibida       <- CPU A
05:40:41:258  KS   ## (2) Pasa del estado EXEC al estado READY
05:40:41:272  CPU  [cpu-proc 27645] ## Interrupción recibida       <- CPU B
05:40:41:272  KS   ## (1) Pasa del estado EXEC al estado READY

05:40:41:272  KM   ## Inicio de compactación
              ... 623 ms: CERO lineas en el log de las CPUs ...
05:40:41:895  KM   ## Fin de compactación
05:40:41:895  KM   ## PID: 0 - Segmento Creado 10 - Tamaño: 30

05:40:41:936  KS   ## (1) Pasa del estado READY al estado EXEC     <- desalojado, vuelve
05:40:42:200  KS   ## (3) Pasa del estado READY al estado EXEC     <- el testigo, ULTIMO
```

### Lo que hay que mirar

**1. Se interrumpen TODAS las CPUs en ráfaga, no una.**

```
05:40:41:258  [cpu-proc 27661]  ## Interrupción recibida
05:40:41:272  [cpu-proc 27645]  ## Interrupción recibida
```

Dos procesos-CPU distintos, con 14 ms de diferencia. Y los dos desalojos correspondientes
en el KS. Con una sola CPU en ráfaga esto no se distingue de "interrumpí a la única que
había".

**2. La barrera aguanta: durante la compactación no se ejecuta NADA.**

Entre `Inicio de compactación` (`41:272`) y `Fin de compactación` (`41:895`), el log de las
tres CPUs tiene **cero** líneas. La última actividad es PID 1 terminando el `SUB` que ya
tenía en vuelo, e inmediatamente su interrupción.

**3. Los desalojados van al FRENTE — y esto es lo más fuerte de la prueba.**

| Proceso | En READY desde | Tomó CPU |
|---|---|---|
| PID 3 (testigo) | **05:40:35:629** | `05:40:42:200` — **último** |
| PID 2 (desalojado) | `05:40:41:258` | `05:40:41:258` |
| PID 1 (desalojado) | `05:40:41:272` | `05:40:41:936` |

PID 3 llevaba **6.5 segundos** esperando en READY, muchísimo más que los dos desalojados, y
aun así entró último. Con una cola FIFO común habría entrado primero. Es la prueba directa
del *"excepcionalmente, serán colocados al principio de la cola de READY"*
(`planificador_transicionar_exec_a_ready_frente()`, `planificador.c:499`, vía
`list_add_in_index(..., 0, pcb)` en `encolar_en_ready()`).

**4. Se replanifica normalmente al terminar.** PID 1 y PID 2 no vuelven a la CPU que tenían
por decreto: pasan por READY y los despacha el planificador de corto plazo como a cualquier
otro. Sólo que desde el frente de la cola.

> **Ojo con el log de EXEC.** Vas a ver `## (2) Pasa del estado READY al estado EXEC` en el
> mismo milisegundo que su desalojo, **antes** de `Inicio de compactación`. No ejecutó: el
> log se emite en `planificador.c:423` y la barrera está adentro de `despachar()`
> (`:341-344`), que se llama dos líneas después en `:425`. El PCB queda marcado EXEC y el
> hilo se duerme en el `pthread_cond_wait` hasta que la compactación termina. El log de las
> CPUs lo confirma: cero instrucciones en esa ventana.

---

## Por qué TRES CPUs y CUATRO procesos

Es el mínimo para que las tres afirmaciones sean distinguibles. Cada pieza tiene una razón:

| Proceso | Rol | Por qué hace falta |
|---|---|---|
| **PID 0** `test_padre` | Fragmenta y dispara la compactación | Sus `MEM_ALLOC`/`MEM_FREE` tienen retorno directo, así que **no suelta su CPU**. Al pedir la compactación queda *estacionado* en la syscall: esa CPU **no** está en ráfaga y no se la interrumpe |
| **PID 1** `test_comp_busy` | CPU-bound, en ráfaga | Hay que tener a quién desalojar |
| **PID 2** `test_comp_busy` | CPU-bound, en ráfaga | El **segundo**, para que "todas las CPUs" signifique más de una |
| **PID 3** `test_comp_busy` | Espera en READY | Testigo: prueba la barrera (no entra durante) **y** el frente de cola (entra último) |

Sin PID 3 no se puede probar ni que la planificación se pausó ni que los desalojados van al
frente: sin nadie más en la cola, cualquier orden da el mismo resultado.

Sin PID 2, "se interrumpen todas las CPUs" es indistinguible de "se interrumpe la única que
había" — que es exactamente la limitación que tenía
[`../dessuspension_compactacion/`](../dessuspension_compactacion/), con 2 CPUs.

---

## Archivos y pseudocódigos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (8 ventanas: **3 CPUs**) |
| `test_padre` | PID 0: crea los tres, fragmenta y dispara |
| `test_comp_busy` | PIDs 1, 2 y 3: CPU-bound, sin syscalls |
| `km_sys.cfg` / `ks_sys.cfg` | Configs |

### `test_padre` (PID 0)

```
 0  INIT_PROC test_comp_busy 0     <- PID 1
 1  INIT_PROC test_comp_busy 0     <- PID 2
 2  INIT_PROC test_comp_busy 0     <- PID 3
 3  MEM_ALLOC 0 20                 }
 ..  ...                           } llena los 200 bytes. Retorno directo:
12  MEM_ALLOC 9 20                 } NO suelta la CPU en ninguna
13  MEM_FREE 1                     }
 ..  ...                           } fragmenta: 5 huecos de 20
17  MEM_FREE 9                     }
18  MEM_ALLOC 10 30                <- DISPARA LA COMPACTACIÓN
19  SET CX 1
20  SLEEP 2500
21  JNZ CX 20                      <- queda vivo para poder seguir observando
```

No hay ningún `SLEEP` antes de la línea 18, y es a propósito: gracias al retorno directo de
las syscalls de memoria, PID 0 conserva su CPU durante las 15 syscalls. Si soltara el
procesador, PID 3 se lo quedaría y se perdería el testigo.

### `test_comp_busy` (PIDs 1, 2 y 3)

```
0  SET AX 100
1  SET BX 1
2  NOOP          }
3  NOOP          } ~20 segundos de CPU pura
4  SUB AX BX     }
5  JNZ AX 2      }
6  EXIT
```

Sin syscalls: sólo así `en_rafaga` se mantiene en `true`
(se pone en `despachar()`, `planificador.c:349`, y sólo vuelve a `false` cuando la CPU le
manda un mensaje al Scheduler, `conexiones.c:181`).
`interrumpir_cpus_para_compactar()` (`:975`) exige `!libre && en_rafaga`.

Los ~20 segundos tienen que cubrir el momento de la compactación, que cae alrededor del
segundo 6: las 15 syscalls de PID 0 a ~300 ms cada una.

---

## La aritmética de la memoria

Idéntica a la de [`../dessuspension_compactacion/`](../dessuspension_compactacion/), pero
acá todos los segmentos son de PID 0 (los hijos no piden memoria):

```
Stick: 200 bytes.  10 segmentos de 20 -> lleno.
Se liberan los impares:

base:  0    20   40   60   80   100  120  140  160  180
seg:  [0 ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ][ 8 ][ 9 ]
free:       ^         ^         ^         ^         ^

libre total: 100     hueco más grande: 20

MEM_ALLOC 10 30  ->  30 > 20  (no entra en ningún hueco)
                     30 <= 100 (sí entra en el total)
                     -> CREAR_SEGMENTO_NECESITA_COMPACTACION
```

Esa doble condición está en `gestor_memoria_crear_segmento()` (`gestor_memoria.c:282-292`).

El tamaño del stick **no es libre**: toda la cuenta está armada sobre 200 bytes.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia
  `SCRIPTS_BASEPATH=../pruebas_unitarias/desalojo_compactacion`. El resto default,
  incluido `COMPACTION_DELAY=100`.
- **`ks_sys.cfg`**: **sin cambios** respecto de
  `kernel_scheduler/config/kernel_scheduler.cfg`. FIFO, sin desalojo entre colas.

Que sea FIFO importa: con RR, el quantum desalojaría a los procesos CPU-bound cada 100 ms y
el log se llenaría de transiciones que se confundirían con las del desalojo por
compactación. Con FIFO, el **único** motivo por el que un proceso puede pasar de EXEC a
READY en toda la corrida es la compactación.
