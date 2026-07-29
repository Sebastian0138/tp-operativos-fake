# Prueba unitaria — Des-suspensión por liberación de memoria (SUSP. READY → READY)

Verifica que, al liberarse memoria, los Procesos suspendidos se recorran **por orden de
prioridad** y, a igual prioridad, **por antigüedad de suspensión**, y que sólo entren los
que tengan lugar.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Planificación de Mediano Plazo"*:

> "En caso de que se libere memoria, se agregue un memory stick nuevo, o se compacte la
> misma, se recorrerán todos los Procesos suspendidos por orden de prioridad y se irán
> des-suspendiendo si cuentan con espacio disponible para re-crear todos sus segmentos sin
> disparar una compactación. En caso de contar con más de un Proceso suspendido con la
> misma prioridad, se evaluará primero el que lleve mayor tiempo suspendido."

Esta prueba cubre el disparador **"se libere memoria"**. Los otros dos tienen su propia
carpeta: [`../dessuspension_stick/`](../dessuspension_stick/) y
[`../dessuspension_compactacion/`](../dessuspension_compactacion/).

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.4.

---

## Cómo levantarla

```bash
./pruebas_unitarias/dessuspension_memoria/levantar.sh          # desde la raíz del repo
./pruebas_unitarias/dessuspension_memoria/levantar.sh 256      # stick de 256 bytes (default: 128)
```

**No hay que tocar nada.** El ciclo completo dura ~35 segundos. Mirá la ventana
**4 KERNEL SCHEDULER**.

### Qué se espera ver

Salida real de una corrida, filtrada:

```
03:24:41:399  KM   ## PID: 1 - Segmento Creado 0 - Tamaño: 20
03:24:42:530  KM   ## PID: 2 - Segmento Creado 0 - Tamaño: 20
03:24:43:620  KM   ## PID: 3 - Segmento Creado 0 - Tamaño: 20

03:24:46:937  KS   ## (1) Pasa del estado BLOCK al estado SUSP. BLOCK
03:24:48:069  KS   ## (2) Pasa del estado BLOCK al estado SUSP. BLOCK
03:24:49:158  KS   ## (3) Pasa del estado BLOCK al estado SUSP. BLOCK
              KM   PID: N - Proceso suspendido: 1 segmentos (20 bytes) movidos a SWAP

03:24:51:551  KM   ## PID: 0 - Segmento Creado 0 - Tamaño: 120     <- el padre acapara

03:25:01:937  KS   ## (1) Pasa del estado SUSP. BLOCK al estado SUSP. READY
03:25:03:069  KS   ## (2) Pasa del estado SUSP. BLOCK al estado SUSP. READY
03:25:04:158  KS   ## (3) Pasa del estado SUSP. BLOCK al estado SUSP. READY
              ~9 segundos VARADOS: despertaron, pero no hay lugar

03:25:13:852  KS   ## (0) - Solicitó syscall: MEM_FREE             <- EL EVENTO
03:25:14:067  KS   ## (2) Pasa del estado SUSP. READY al estado READY
03:25:14:158  KS   ## (3) Pasa del estado SUSP. READY al estado READY
03:25:14:249  KS   ## (1) Pasa del estado SUSP. READY al estado READY
```

### Lo que hay que mirar: el orden es 2, 3, 1

| PID | Prioridad | Se suspendió | Se des-suspendió |
|---|---|---|---|
| 1 | **2** | 03:24:46:937 (primero) | **último** |
| 2 | **1** | 03:24:48:069 | **primero** |
| 3 | **1** | 03:24:49:158 | **segundo** |

Las dos reglas del enunciado quedan probadas a la vez:

1. **La prioridad manda.** PID 1 fue el primero en suspenderse y salió último, porque su
   prioridad (2) es peor que la de los otros dos (1).
2. **La antigüedad desempata.** PID 2 y PID 3 tienen la misma prioridad, y salió primero
   PID 2, que llevaba 1.1 s más suspendido.

Por eso las prioridades son `2, 1, 1` y no `0, 1, 2`: con prioridades crecientes el orden
por prioridad y el orden por antigüedad coinciden, y la prueba no distingue si estás
ordenando bien o si simplemente estás respetando el orden de llegada.

Vale mirar también la ventana del **SWAP**: 3 escrituras de bloque al suspenderse y 3
lecturas al volver. Si no están, los estados cambiaron pero la memoria no se movió.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (9 ventanas: 4 IO SLEEP, ver abajo) |
| `test_dessusp_padre` | Pseudocódigo del PID 0 |
| `test_dessusp_hijo` | Pseudocódigo de los 3 hijos |
| `km_dessusp.cfg` | Config del Kernel Memory |
| `ks_dessusp.cfg` | Config del Kernel Scheduler |

---

## Los pseudocódigos

### `test_dessusp_padre` (PID 0) — el que acapara

```
 0  INIT_PROC test_dessusp_hijo 2     <- PID 1, prioridad 2
 1  INIT_PROC test_dessusp_hijo 1     <- PID 2, prioridad 1
 2  INIT_PROC test_dessusp_hijo 1     <- PID 3, prioridad 1
 3  SLEEP 2500                        }
 4  SLEEP 2500                        } cede CPU ~7.5s: los hijos allocan y se suspenden
 5  SLEEP 2500                        }
 6  MEM_ALLOC 0 120                   <- acapara 120 de 128. Quedan 8 libres.
 7  SLEEP 2500                        }
 ..  ...                              } sostiene la memoria ~20s
14  SLEEP 2500                        }
15  MEM_FREE 0                        <- EL EVENTO que dispara las des-suspensiones
16  SET CX 1
17  SLEEP 2500
18  JNZ CX 17                         <- queda vivo para poder seguir observando
```

Los `SLEEP 2500` son **más cortos** que `SUSPENSION_TIMEOUT` (5000): el padre cede la CPU
para que corran los hijos, pero nunca se suspende. Si se suspendiera, soltaría los 120
bytes y los hijos entrarían solos — la prueba se autodestruiría.

El `MEM_ALLOC 0 120` va **después** de los sleeps a propósito: tiene que ejecutarse cuando
los 3 hijos ya están en SWAP, si no no habría lugar para 120 bytes.

### `test_dessusp_hijo` (PIDs 1, 2 y 3)

```
0  MEM_ALLOC 0 20
1  SET AX 88
2  SET DI 0
3  MOV_OUT AX          <- escribe 'X', así hay datos reales que viajen a SWAP
4  SLEEP 20000         <- se suspende a los 5s; despierta a los 20s
5  MEM_FREE 0
6  EXIT
```

`20 bytes` contra los `8` que deja libres el padre: cuando despiertan, ninguno entra.

---

## Las configs

### `km_dessusp.cfg` (Kernel Memory)

Sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/dessuspension_memoria` (relativo a la
carpeta del módulo). El resto es el default: `BEST` fit, `SEGMENT_MAX_SIZE=256`,
`INSTRUCTION_DELAY=50`.

### `ks_dessusp.cfg` (Kernel Scheduler)

Sólo cambia:

```ini
SUSPENSION_TIMEOUT=5000    # el default es 1000
```

Tiene que quedar **entre** el sleep del padre (2500) y el de los hijos (20000): los hijos
se suspenden, el padre no. Con el default de 1000 se suspenden los cuatro y no hay quien
acapare la memoria.

El algoritmo queda en `FIFO`. La prioridad no interviene en la planificación de corto
plazo, pero `comparar_candidatos()` (`planificador.c:709`) ordena por `prioridad_actual`
igual, sea cual sea el algoritmo. Dejarlo en FIFO aísla la variable que se está probando.

### Tamaño del stick: 128 bytes

Pasado como argumento en `levantar.sh`. La cuenta es toda la prueba:

```
128 total
-120 del padre
=  8 libres  <  20 que necesita cada hijo   -> nadie entra
```

---

## Por qué se levantan CUATRO IO SLEEP

Este es el error que hizo fallar la primera versión de la prueba, y vale documentarlo.

`planificador_tomar_io_libre()` (`planificador.c:1175`) hace `sem_wait` sobre un semáforo
**por tipo de IO**, y cada dispositivo conectado hace un `sem_post` al registrarse
(`planificador.c:1149`). Con **una sola** IO SLEEP, los 4 procesos se serializan sobre
ella.

El problema no es la lentitud: es que **el proceso que espera el semáforo lo hace estando
en BLOCK**. El timer de suspensión ya arrancó, y le corre igual. En la corrida fallida, el
padre pidió `SLEEP 2500` con timeout de 3000, pero se quedó esperando el dispositivo detrás
del `SLEEP 20000` de un hijo:

```
03:08:16:734  ## (0) Pasa del estado EXEC al estado BLOCK
03:08:19:734  ## (0) Pasa del estado BLOCK al estado SUSP. BLOCK   <- 3000 ms exactos
```

El padre se suspendió, soltó la memoria, y los hijos entraron sin necesidad de ningún
evento. La prueba pasaba en verde sin probar nada.

Con 4 dispositivos —uno por proceso concurrente— nadie espera el semáforo y los tiempos
son los del pseudocódigo.

> **Vale como observación sobre el TP, no como bug**: es el comportamiento correcto. Un
> proceso esperando un dispositivo ocupado *está* bloqueado, y el enunciado dice que se
> suspenda al vencer el timeout. Pero al armar pruebas hay que tenerlo en cuenta, porque
> el tiempo real en BLOCK es `espera de dispositivo + duración de la IO`, no sólo lo
> segundo.

Esta prueba no usa STDIN ni STDOUT, así que no se levantan.
