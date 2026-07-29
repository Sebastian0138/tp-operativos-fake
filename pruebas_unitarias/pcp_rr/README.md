# Prueba unitaria — Planificación de corto plazo: Round Robin

Verifica que con `PLANIFICATION_ALGORITHM=RR` ningún Proceso ejecute más de un quantum
seguido, y que los procesos se intercalen.

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
./pruebas_unitarias/pcp_rr/levantar.sh
```

**No hay que tocar nada.** Dura ~20 segundos.

Mismos 3 procesos CPU-bound que `pcp_fifo`. Lo único que cambia es el algoritmo.

### Qué se espera ver

Corrida real, filtrada:

```
05:06:13:419  ## (1) Pasa del estado READY al estado EXEC
05:06:13:806  ## (1) - Desalojado por fin de quantum          <- 387 ms
05:06:13:806  ## (0) Pasa del estado READY al estado EXEC
05:06:14:104  ## (2) Pasa del estado READY al estado EXEC
05:06:14:493  ## (2) - Desalojado por fin de quantum          <- 389 ms
05:06:14:493  ## (1) Pasa del estado READY al estado EXEC
...
```

El patrón de ejecución es `1, 2, 1, 3, 2, 1, 3, ...` — round robin.

### Lo que hay que mirar

**1. Muchísimos desalojos.** En la corrida de referencia:

```
desalojos por fin de quantum: 153
```

Contra **0** en `pcp_fifo`, con exactamente el mismo pseudocódigo. La única diferencia es
una línea del config.

**2. Las ráfagas duran ~387 ms con `RR_QUANTUM=300`.** No es un error de 87 ms: la CPU
chequea la interrupción **después** de terminar la instrucción en curso
(`cpu_check_interrupt()` — `instrucciones.c:337`, llamado al final del ciclo en
`cpu.c:80-131`), y cada instrucción cuesta 50 ms de `INSTRUCTION_DELAY` más el fetch.

El quantum es un **piso, no un techo**: garantiza que nadie ejecute *menos* de 300 ms
seguidos, no que nadie ejecute más. Es lo correcto — desalojar en medio de una instrucción
dejaría el contexto inconsistente.

**3. Ningún proceso termina antes que otro empiece.** Los tres avanzan en paralelo, a
diferencia de FIFO donde el primero terminaba antes de que el segundo tocara la CPU.

---

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

### `test_pcp_padre`

```
0  INIT_PROC test_pcp_cpu 0
1  INIT_PROC test_pcp_cpu 0
2  INIT_PROC test_pcp_cpu 0
3  EXIT
```

Prioridades en 0: en RR tampoco tienen injerencia.

---

## Las configs

- **`km_pcp.cfg`**: sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/pcp_rr`.
- **`ks_pcp.cfg`**: dos cambios respecto del default:

```ini
PLANIFICATION_ALGORITHM=RR    # el default es FIFO
RR_QUANTUM=300                # el default es 100
```

Subí el quantum de 100 a 300 ms para que las ráfagas se distingan de la granularidad de
una instrucción (50 ms). Con 100 ms cada ráfaga sería de 1 o 2 instrucciones y el log
quedaría ilegible.


## Por qué UNA sola CPU

Es parte de la prueba, no una simplificación. Con dos CPUs los procesos no compiten por el
procesador: cada uno agarra la suya y no hay ni quantum que vencer ni cola más prioritaria
que desaloje a nadie. La contención es la condición necesaria para que la planificación de
corto plazo tenga algo que decidir.

(Es exactamente al revés que en `dessuspension_compactacion/`, donde hacen falta **dos**
CPUs para que haya alguien en ráfaga a quien desalojar mientras el otro está estacionado en
su syscall.)

Los hijos no piden memoria, así que el tamaño del stick es indistinto.
