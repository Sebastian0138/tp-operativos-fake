# Prueba unitaria — Des-suspensión por compactación (SUSP. READY → READY)

Verifica que, al terminar una compactación, se recorran los Procesos suspendidos y se
des-suspendan los que ahora entran, por orden de prioridad y antigüedad.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Planificación de Mediano Plazo"*:

> "En caso de que se libere memoria, se agregue un memory stick nuevo, **o se compacte la
> misma**, se recorrerán todos los Procesos suspendidos por orden de prioridad y se irán
> des-suspendiendo si cuentan con espacio disponible para re-crear todos sus segmentos sin
> disparar una compactación. En caso de contar con más de un Proceso suspendido con la
> misma prioridad, se evaluará primero el que lleve mayor tiempo suspendido."

Esta prueba cubre el disparador **"o se compacte la misma"**. Los otros dos tienen su
propia carpeta: [`../dessuspension_memoria/`](../dessuspension_memoria/) y
[`../dessuspension_stick/`](../dessuspension_stick/).

De paso ejercita el flujo completo de compactación (KM pide, KS desaloja las CPUs, KM
compacta): `docs/MAPA_TP.md` §2.7 y §3.6.

---

## Cómo levantarla

```bash
./pruebas_unitarias/dessuspension_compactacion/levantar.sh          # stick de 200 bytes
./pruebas_unitarias/dessuspension_compactacion/levantar.sh 400      # otro tamaño (rompe las cuentas)
```

**No hay que tocar nada.** El ciclo completo dura ~45 segundos.

> El tamaño del stick **no es un parámetro libre**: toda la prueba está calculada sobre
> 200 bytes. Cambiarlo rompe la fragmentación y no se dispara la compactación.

### Qué se espera ver

Salida real de una corrida, filtrada y unificando KS, KM y CPU:

```
04:01:2x:xxx  KS   ## (2) Pasa del estado SUSP. BLOCK al estado SUSP. READY
04:01:2x:xxx  KS   ## (3) Pasa del estado SUSP. BLOCK al estado SUSP. READY
              ... varados: hay 100 bytes libres, pero en huecos de 20 ...

04:01:31:988  KS   Kernel Memory pide compactar: desalojando todas las CPUs...
04:01:32:064  CPU  ## PID: 1 - EXECUTE completado - Resultado: 0    <- ultimo NOOP del busy
04:01:32:064  CPU  ## Interrupción recibida
04:01:32:065  KS   ## (1) Pasa del estado EXEC al estado READY      <- DESALOJO
04:01:32:069  KM   ## Inicio de compactación
              ... 625 ms en los que la CPU no ejecuta NADA ...
04:01:32:694  KM   ## Fin de compactación
04:01:32:694  KM   ## PID: 0 - Segmento Creado 10 - Tamaño: 30

04:01:32:868  KS   ## (3) Pasa del estado SUSP. READY al estado READY
04:01:32:959  KS   ## (2) Pasa del estado SUSP. READY al estado READY
```

### Lo que hay que mirar

**1. Los hijos quedan varados con memoria de sobra.** Hay **100 bytes libres** y cada hijo
necesita **30**. Igual no entran, porque los 100 están partidos en 5 huecos de 20. Eso es
lo que quiere decir el enunciado con *"sin disparar una compactación"*: la des-suspensión
usa `buscar_hueco()` y sólo entra si hay un hueco **contiguo** que alcance.

**2. La compactación es lo único que los libera.** No se liberó ni un byte: el total libre
antes y después es el mismo. Lo único que cambió es que quedó **contiguo**.

**3. El desalojo de CPUs se ejercita de verdad.** PID 1 (`test_compact_busy`) está en
ráfaga permanente, así que `interrumpir_cpus_para_compactar()` tiene a quién interrumpir:

```
KS   desalojando todas las CPUs...   04:01:31:988
CPU  ## Interrupción recibida        04:01:32:064   (+76 ms)
KS   ## (1) EXEC -> READY            04:01:32:065
KM   ## Inicio de compactación       04:01:32:069   (+81 ms del pedido)
```

Con **una sola CPU** esos 81 ms son **1 ms** y no aparece ninguna interrupción: el único
proceso en ejecución sería el que disparó la compactación, que está *estacionado* en su
`MEM_ALLOC` y por lo tanto no está en ráfaga. Ver la sección final.

**4. Nada se ejecuta durante la compactación.** Entre `Inicio` y `Fin` el log de la CPU
está **vacío**. Los segmentos se mueven sin que nadie los lea ni los escriba.

**5. El orden es 3, 2** — por prioridad:

| PID | Prioridad | Se des-suspendió |
|---|---|---|
| 3 | **0** | primero |
| 2 | **1** | segundo |

> **Ojo con el log de EXEC.** Vas a ver `## (1) Pasa del estado READY al estado EXEC` en el
> mismo milisegundo que el desalojo, *antes* de que arranque la compactación. No es que el
> proceso vuelva a ejecutar: el log se emite en `planificador.c:423` y la barrera de
> compactación está adentro de `despachar()`, en `:341-344`, que se llama en `:425`. El PCB
> queda marcado EXEC pero el hilo se duerme 625 ms en el `pthread_cond_wait`. El log del
> CPU lo confirma: cero instrucciones en esa ventana.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (10 ventanas: 4 IO SLEEP + **2 CPUs**) |
| `test_compact_padre` | Pseudocódigo del PID 0 |
| `test_compact_busy` | Pseudocódigo del PID 1: loop de CPU pura, mantiene una CPU en ráfaga |
| `test_compact_hijo` | Pseudocódigo de los hijos (PIDs 2 y 3) |
| `km_dessusp.cfg` | Config del Kernel Memory |
| `ks_dessusp.cfg` | Config del Kernel Scheduler |

---

## La aritmética de la prueba

Todo depende de estos números. Si tocás uno, recalculá el resto.

**Stick: 200 bytes. Hijos: 30 bytes cada uno. Segmentos del padre: 20 bytes.**

### Paso 1 — los hijos allocan y se suspenden

```
2 hijos x 30 = 60 bytes ocupados  ->  se suspenden  ->  60 bytes van a SWAP
memoria principal: 200 libres
```

### Paso 2 — el padre fragmenta

Llena los 200 bytes con 10 segmentos de 20, y libera los impares:

```
base:  0    20   40   60   80   100  120  140  160  180
seg:  [0 ][ 1 ][ 2 ][ 3 ][ 4 ][ 5 ][ 6 ][ 7 ][ 8 ][ 9 ]
free:       ^         ^         ^         ^         ^     <- MEM_FREE 1,3,5,7,9

libre total: 100 bytes     hueco más grande: 20 bytes
```

### Paso 3 — los hijos despiertan y NO entran

```
necesitan 30 contiguos  >  20 del hueco más grande   ->  VARADOS
(aunque el total libre, 100, sea más que suficiente)
```

### Paso 4 — el padre pide 30 y dispara la compactación

```
30 > 20  (no entra en ningún hueco)      ->  no se puede sin compactar
30 <= 100 (sí entra en el total libre)   ->  CREAR_SEGMENTO_NECESITA_COMPACTACION
```

Esa doble condición está en `gestor_memoria_crear_segmento()`
(`gestor_memoria.c:282-292`): si `buscar_hueco()` falla pero `espacio_libre >= tamaño`,
devuelve `NECESITA_COMPACTACION` en vez de `SIN_ESPACIO`.

### Paso 5 — después de compactar

```
5 segmentos x 20 = 100 bytes amontonados en [0, 100)
libre: 100 bytes CONTIGUOS en [100, 200)
   -30 del nuevo segmento 10
   = 70 libres  >=  60 (los 2 hijos x 30)   ->  entran los DOS
```

> Si querés ver entrar **sólo uno** y comprobar la selección por prioridad, subí el pedido
> del padre: cambiá `MEM_ALLOC 10 30` por `MEM_ALLOC 10 60`. Quedan 40 libres, entra un
> solo hijo, y tiene que ser **PID 3** (prioridad 0).

---

## Los pseudocódigos

### `test_compact_padre` (PID 0)

```
 0  INIT_PROC test_compact_busy 1     <- PID 1: ocupa una CPU en rafaga permanente
 1  INIT_PROC test_compact_hijo 1     <- PID 2, prioridad 1
 2  INIT_PROC test_compact_hijo 0     <- PID 3, prioridad 0
 3  SLEEP 2500                        }
 4  SLEEP 2500                        } cede CPU ~10s: los hijos allocan
 5  SLEEP 2500                        } y se suspenden
 6  SLEEP 2500                        }
 7  MEM_ALLOC 0 20                    }
 ..  ...                              } llena los 200 bytes
16  MEM_ALLOC 9 20                    }
17  MEM_FREE 1                        }
18  MEM_FREE 3                        }
19  MEM_FREE 5                        } fragmenta: 5 huecos de 20
20  MEM_FREE 7                        }
21  MEM_FREE 9                        }
22  SLEEP 2500                        }
 ..  ...                              } espera a que los hijos despierten y queden varados
26  SLEEP 2500                        }
27  MEM_ALLOC 10 30                   <- DISPARA LA COMPACTACIÓN
28  SET CX 1
29  SLEEP 2500
30  JNZ CX 29                         <- queda vivo para poder seguir observando
```

Los 5 `MEM_FREE` disparan `planificador_evento_memoria_liberada()` cada uno, pero no pasa
nada: en ese momento los hijos siguen en SUSP. BLOCK (durmiendo), y aunque estuvieran en
SUSP. READY los huecos de 20 no les alcanzan. La prueba es robusta a eso.

### `test_compact_busy` (PID 1) — el que mantiene una CPU en ráfaga

```
0  SET CX 1
1  NOOP
2  NOOP
3  JNZ CX 1        <- CX vale 1 y nunca cambia: loop infinito
```

**Ni una sola syscall.** Es lo único que importa de este proceso, y es lo que lo hace
servir: `en_rafaga` se pone en `true` al despachar (`planificador.c:349`) y sólo vuelve a
`false` cuando la CPU le manda un mensaje al Scheduler (`conexiones.c:181`). Un proceso que
sólo ejecuta instrucciones puras nunca manda nada, así que **se queda en ráfaga para
siempre**, hasta que lo interrumpan.

No pide memoria a propósito: si allocara, alteraría la aritmética de la fragmentación.

### `test_compact_hijo` (PIDs 2 y 3)

```
0  MEM_ALLOC 0 30
1  SET AX 88
2  SET DI 0
3  MOV_OUT AX          <- escribe 'X', así hay datos reales que viajen a SWAP
4  SLEEP 20000         <- se suspende a los 5s; despierta a los 20s
5  MEM_FREE 0
6  EXIT
```

---

## Las configs

- **`km_dessusp.cfg`**: sólo cambia
  `SCRIPTS_BASEPATH=../pruebas_unitarias/dessuspension_compactacion`. El resto default:
  `BEST` fit, `SEGMENT_MAX_SIZE=256`, `COMPACTION_DELAY=100`.
- **`ks_dessusp.cfg`**: sólo cambia `SUSPENSION_TIMEOUT=5000`, que queda entre el sleep del
  padre (2500) y el de los hijos (20000): se suspenden los hijos y el padre no.

**Cuatro IO SLEEP.** Duermen tres procesos (padre + 2 hijos); la cuarta es margen. El
motivo está en [`../dessuspension_memoria/README.md`](../dessuspension_memoria/README.md):
con menos dispositivos que durmientes, los procesos hacen cola sobre el semáforo **estando
en BLOCK**, el timer de suspensión les corre igual, y el padre se suspende soltando la
memoria que tiene que estar acaparando.

---

## Por qué DOS CPUs

Sin esto, la prueba verifica la compactación pero **no** el desalojo, que es la mitad del
flujo que vive en el Kernel Scheduler (`docs/MAPA_TP.md` §2.7, pasos 4 a 7).

`interrumpir_cpus_para_compactar()` (`planificador.c:970`) sólo cuenta CPUs con
`!libre && en_rafaga`. Con **una sola CPU**, el único proceso en ejecución es el que disparó
la compactación con su `MEM_ALLOC`, y ese está *estacionado* esperando la respuesta: la CPU
no está ejecutando instrucciones, `en_rafaga` es `false`, y el bucle

```c
while (interrumpir_cpus_para_compactar(pl) > 0) { usleep(5 * 1000); }
```

sale en la primera vuelta sin interrumpir a nadie. Es el comportamiento **correcto** — no
tiene sentido interrumpir una CPU que no ejecuta nada — pero deja sin ejercitar toda la
rama del desalojo: `MENSAJE_INTERRUPCION`, `cpu_check_interrupt()`, el reencolado al frente
con `planificador_transicionar_exec_a_ready_frente()` y el `MENSAJE_DESALOJO_OK`.

La diferencia es medible en el log:

| | 1 CPU | 2 CPUs |
|---|---|---|
| `desalojando todas las CPUs` → `Inicio de compactación` | **1 ms** | **81 ms** |
| `## Interrupción recibida` en el log de la CPU | no aparece | **sí, +76 ms** |
| `(N) Pasa del estado EXEC al estado READY` por desalojo | no aparece | **sí** |

Ese milisegundo es la firma de que no se interrumpió a nadie. Con 2 CPUs hay un
round-trip real: interrumpir, esperar el contexto, reencolar al frente, y recién ahí
avisarle a KM que puede compactar.

Es estructural, no de timing: **con 1 CPU es imposible** que haya alguien en ráfaga cuando
se dispara una compactación, porque el que la dispara ocupa la única CPU que hay.
