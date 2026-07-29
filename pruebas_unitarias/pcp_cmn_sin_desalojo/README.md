# Prueba unitaria — Colas Multinivel SIN desalojo entre colas

Verifica que con `QUEUE_PREEMPTION=FALSE` un Proceso que llega a una cola más prioritaria
**no** desaloje al que está ejecutando: tiene que esperar su turno.

Y de paso comprueba que cada cola use el algoritmo que le corresponde.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Planificación de Corto Plazo"*:

> "Se encargará de gestionar las transiciones entre los estados READY y EXEC. Vamos a
> estar utilizando uno de los tres posibles algoritmos de planificación definidos, los
> cuales van a ser FIFO, RR y Colas Multinivel (no retroalimentadas). El algoritmo a
> utilizar será elegible por archivo de configuración y no cambiará a lo largo de una
> prueba. (...) En los algoritmos de FIFO y RR la prioridad no tendrá injerencia alguna,
> mientras que en el algoritmo de Colas Multinivel, al momento de llegar un Proceso, y si
> el desalojo entre colas está habilitado, se deberá validar si se encuentra ejecutando un
> Proceso con menor prioridad. En caso de que así sea, se deberá desalojar al Proceso menos
> prioritario para darle lugar al más prioritario que acaba de llegar."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.3.

Las cuatro pruebas `pcp_*` cubren la sección entre todas:

| Prueba | `PLANIFICATION_ALGORITHM` | `QUEUES_ALGORITHMS` | `QUEUE_PREEMPTION` |
|---|---|---|---|
| [`pcp_fifo/`](../pcp_fifo/) | FIFO | — | — |
| [`pcp_rr/`](../pcp_rr/) | RR | — | — |
| [`pcp_cmn_sin_desalojo/`](../pcp_cmn_sin_desalojo/) | CMN | `[FIFO,RR]` | FALSE |
| [`pcp_cmn_con_desalojo/`](../pcp_cmn_con_desalojo/) | CMN | `[RR,FIFO]` | TRUE |

Las dos de CMN usan el orden de algoritmos **invertido** a propósito: entre ambas queda
ejercitado FIFO y RR en la cola 0 **y** en la cola 1.

---

## Cómo levantarla

```bash
./pruebas_unitarias/pcp_cmn_sin_desalojo/levantar.sh
```

**No hay que tocar nada.** Dura ~15 segundos.

### Qué se espera ver

Corrida real, filtrada:

```
05:07:41:060  ## (2) Pasa del estado EXEC al estado BLOCK     <- PID 2 arranca su SLEEP 2000
05:07:41:060  ## (1) Pasa del estado READY al estado EXEC     <- PID 1 toma la CPU
05:07:41:406  ## (1) - Desalojado por fin de quantum          }
05:07:41:793  ## (1) - Desalojado por fin de quantum          } PID 1 cicla en RR
05:07:42:140  ## (1) - Desalojado por fin de quantum          }
...
05:07:43:060  ## (2) Pasa del estado BLOCK al estado READY    <- LLEGA EL MAS PRIORITARIO
05:07:43:262  ## (1) - Desalojado por fin de quantum          <- 202 ms despues, por QUANTUM
05:07:43:262  ## (2) Pasa del estado READY al estado EXEC     <- recien ahora entra
05:07:52:813  ## (2) Pasa del estado EXEC al estado EXIT      <- 9.5 SEGUNDOS DE CORRIDO
05:07:52:854  ## (1) Pasa del estado READY al estado EXEC
```

### Lo que hay que mirar

**1. Cero desalojos por prioridad.**

```
desalojos por cola más prioritaria: 0
```

PID 2 llegó a las `43:060` y no pasó nada. La CPU la consiguió recién a las `43:262`, **202
ms después**, y no porque lo hayan desalojado sino porque a PID 1 se le venció el quantum.
Ésa es la diferencia con [`../pcp_cmn_con_desalojo/`](../pcp_cmn_con_desalojo/), donde el
desalojo ocurre en el **mismo milisegundo** que la llegada.

**2. Cada cola usa SU algoritmo — y se ve en la misma corrida.**

Con `QUEUES_ALGORITHMS=[FIFO,RR]`:

| Proceso | Prioridad | Cola | Algoritmo | Qué le pasa |
|---|---|---|---|---|
| PID 1 | 1 | 1 | **RR** | desalojado por quantum ~13 veces |
| PID 2 | 0 | 0 | **FIFO** | corre **9.5 s de corrido**, cero desalojos |

Dos procesos, una sola corrida, dos algoritmos distintos según la cola en la que caen.
Eso es exactamente *"El algoritmo (FIFO o RR) a utilizar dentro de cada cola estará
especificado dentro del archivo de configuración"*.

**3. La prioridad decide el orden, aunque no desaloje.** Apenas se libera la CPU, el que
entra es PID 2 (cola 0) y no PID 1 (cola 1), aunque PID 1 ya estaba en READY. Sin desalojo
la prioridad sigue mandando: lo que no puede hacer es interrumpir a alguien.

---

## El escenario, y por qué está armado así

```
0  INIT_PROC test_pcp_cpu 1        <- PID 1, prioridad 1 -> COLA 1, CPU-bound ~5s
1  INIT_PROC test_pcp_espera 0     <- PID 2, prioridad 0 -> COLA 0, duerme 2s y despues CPU-bound
2  EXIT                            <- el padre se va enseguida y no molesta
```

Secuencia:

1. El padre crea los dos hijos y termina.
2. **PID 2 corre primero**, porque `desencolar_ready()` (`planificador.c:97`) recorre las
   colas de menor índice a mayor, y la cola 0 es la más prioritaria.
3. PID 2 ejecuta su `SLEEP 2000` y se bloquea, liberando la CPU.
4. PID 1 toma la CPU y entra en ráfaga.
5. **A los 2 segundos PID 2 vuelve a READY.** Ése es el momento que la prueba mide: llega
   un proceso más prioritario mientras uno menos prioritario está ejecutando.

El "llega un Proceso" del enunciado se materializa acá con un fin de IO, no con un
`INIT_PROC`. Es más robusto: `planificador_transicionar_block_a_ready()` termina llamando
a `encolar_en_ready()` igual que la creación, y así el instante de llegada queda fijado por
un `SLEEP` de duración conocida en vez de depender de la carrera entre el hilo de largo
plazo y el de corto plazo.

`SUSPENSION_TIMEOUT` está en **10000 ms** justamente para que ese `SLEEP 2000` **no**
dispare una suspensión: si PID 2 se suspendiera, volvería por `SUSP. READY -> READY` y
estaríamos midiendo otra cosa.

### `test_pcp_espera`

```
0  SLEEP 2000     <- la unica syscall: fija el instante de "llegada"
1  SET AX 25
2  SET BX 1
3  NOOP           }
4  NOOP           } igual que test_pcp_cpu
5  SUB AX BX      }
6  JNZ AX 3       }
7  EXIT
```

## El pseudocódigo CPU-bound

### `test_pcp_cpu`

```
0  SET AX 25
1  SET BX 1
2  NOOP          }
3  NOOP          } cuerpo del loop: 4 instrucciones
4  SUB AX BX     }
5  JNZ AX 2      }
6  EXIT
```

25 vueltas x 4 instrucciones x 50 ms de `INSTRUCTION_DELAY` = **~5 segundos de CPU pura**.

**Ni una sola syscall**, y es deliberado: un proceso estacionado en una syscall no está
`en_rafaga` y no se lo puede desalojar. `chequear_desalojo_por_prioridad()`
(`planificador.c:57`) e `interrumpir_cpus_para_compactar()` (`:975`) exigen las dos
condiciones `!libre && en_rafaga`. Sin procesos CPU-bound no hay nada que desalojar y la
prueba pasa en verde sin probar nada.

`SUB` opera **registro contra registro** (`instrucciones.c:163`), no acepta un inmediato:
por eso el `SET BX 1` y el `SUB AX BX` en vez de un `SUB AX 1`.

---

## Las configs

- **`km_pcp.cfg`**: sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/pcp_cmn_sin_desalojo`.
- **`ks_pcp.cfg`**:

```ini
PLANIFICATION_ALGORITHM=CMN     # el default es FIFO
QUEUES_ALGORITHMS=[FIFO,RR]     # cola 0 = FIFO, cola 1 = RR
QUEUE_PREEMPTION=FALSE          # ya era el default; acá es EL parámetro que se prueba
RR_QUANTUM=300                  # el default es 100
SUSPENSION_TIMEOUT=10000        # el default es 1000: evita que el SLEEP 2000 suspenda
```

La cantidad de colas sale de `QUEUES_ALGORITHMS`, no de un parámetro aparte:
`planificador_create()` (`planificador.c:118-125`) cuenta los elementos. Dos elementos =
dos colas = prioridades válidas 0 y 1.

> **Prioridad fuera de rango:** con CMN, un `INIT_PROC` con prioridad >= cantidad de colas
> se **rechaza** y el proceso no se crea (`conexiones.c:381-384`). Probalo cambiando un
> `INIT_PROC test_pcp_cpu 1` por `INIT_PROC test_pcp_cpu 5`: aparece
> `INIT_PROC con prioridad 5 fuera de rango [0, 2): no se planifica`.


## Por qué UNA sola CPU

Es parte de la prueba, no una simplificación. Con dos CPUs los procesos no compiten por el
procesador: cada uno agarra la suya y no hay ni quantum que vencer ni cola más prioritaria
que desaloje a nadie. La contención es la condición necesaria para que la planificación de
corto plazo tenga algo que decidir.

(Es exactamente al revés que en `dessuspension_compactacion/`, donde hacen falta **dos**
CPUs para que haya alguien en ráfaga a quien desalojar mientras el otro está estacionado en
su syscall.)

Los hijos no piden memoria, así que el tamaño del stick es indistinto.
