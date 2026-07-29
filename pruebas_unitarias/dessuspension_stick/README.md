# Prueba unitaria — Des-suspensión por Memory Stick nuevo (SUSP. READY → READY)

Verifica que conectar un Memory Stick en caliente dispare el recorrido de los Procesos
suspendidos, y que se des-suspendan por orden de prioridad y antigüedad.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Planificación de Mediano Plazo"*:

> "En caso de que se libere memoria, **se agregue un memory stick nuevo**, o se compacte la
> misma, se recorrerán todos los Procesos suspendidos por orden de prioridad y se irán
> des-suspendiendo si cuentan con espacio disponible para re-crear todos sus segmentos sin
> disparar una compactación. En caso de contar con más de un Proceso suspendido con la
> misma prioridad, se evaluará primero el que lleve mayor tiempo suspendido."

Y del lado de **Kernel Memory**, sección *"Conexión de un Memory Stick"*:

> "se deberá notificar al Kernel Scheduler que se dispone de más memoria."

Esta prueba cubre el disparador **"se agregue un memory stick nuevo"**. Los otros dos
tienen su propia carpeta: [`../dessuspension_memoria/`](../dessuspension_memoria/) y
[`../dessuspension_compactacion/`](../dessuspension_compactacion/).

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.4 y §3.9.

---

## Cómo levantarla

Son **dos pasos**, y el segundo lo disparás vos:

```bash
# 1. Levantar el sistema y esperar ~23 segundos
./pruebas_unitarias/dessuspension_stick/levantar.sh

# 2. Cuando los 3 hijos estén varados en SUSP. READY:
./pruebas_unitarias/dessuspension_stick/agregar_stick.sh
```

**Sin apuro en el paso 2.** A diferencia de la prueba de BSOD, acá el padre nunca suelta
la memoria: cicla en un `SLEEP` corto para siempre. Los hijos se quedan varados
indefinidamente hasta que vos conectes el stick. En la corrida de referencia pasaron 25
segundos entre una cosa y la otra, sin problema.

### Qué se espera ver

Salida real de una corrida, filtrada:

```
03:28:11:025  KM   ## PID: 0 - Segmento Creado 0 - Tamaño: 120    <- el padre acapara

03:28:21:407  KS   ## (1) Pasa del estado SUSP. BLOCK al estado SUSP. READY
03:28:22:543  KS   ## (2) Pasa del estado SUSP. BLOCK al estado SUSP. READY
03:28:23:634  KS   ## (3) Pasa del estado SUSP. BLOCK al estado SUSP. READY
              ... 25 segundos varados. Acá corrés agregar_stick.sh ...

03:28:48:853  KM   ## Memory Stick de 128 bytes Conectada
03:28:48:853  KS   Kernel Memory informa mas memoria disponible: 256 bytes totales
03:28:49:035  KS   ## (2) Pasa del estado SUSP. READY al estado READY
03:28:49:126  KS   ## (3) Pasa del estado SUSP. READY al estado READY
03:28:49:259  KS   ## (1) Pasa del estado SUSP. READY al estado READY
              KM   PID: N - Proceso des-suspendido: 1 segmentos restaurados desde SWAP
```

### Lo que hay que mirar

**El orden es 2, 3, 1** — el mismo que en `dessuspension_memoria`, y por las mismas dos
razones:

| PID | Prioridad | Se suspendió | Se des-suspendió |
|---|---|---|---|
| 1 | **2** | primero | **último** |
| 2 | **1** | segundo | **primero** |
| 3 | **1** | tercero | **segundo** |

La prioridad le gana a la antigüedad (PID 1 sale último aunque se suspendió primero), y la
antigüedad desempata entre iguales (PID 2 antes que PID 3).

**El total de memoria cambia en caliente**: `256 bytes totales`. El stick nuevo no
reemplaza al viejo, lo amplía.

**La notificación llega por el canal dedicado.** El log del KS y el del KM caen en el
mismo milisegundo (`03:28:48:853`), igual que en la prueba de BSOD. Es el mismo canal
`HANDSHAKE_SCHEDULER_NOTIF`: `MENSAJE_MAS_MEMORIA` y `MENSAJE_MEMORIA_CORRUPTA` viajan por
ahí.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (9 ventanas: 4 IO SLEEP) |
| `agregar_stick.sh` | Conecta el 2º stick al sistema ya levantado |
| `test_dessusp_padre` | Pseudocódigo del PID 0 |
| `test_dessusp_hijo` | Pseudocódigo de los 3 hijos |
| `km_dessusp.cfg` | Config del Kernel Memory |
| `ks_dessusp.cfg` | Config del Kernel Scheduler |

---

## Los pseudocódigos

### `test_dessusp_padre` (PID 0)

```
0  INIT_PROC test_dessusp_hijo 2     <- PID 1, prioridad 2
1  INIT_PROC test_dessusp_hijo 1     <- PID 2, prioridad 1
2  INIT_PROC test_dessusp_hijo 1     <- PID 3, prioridad 1
3  SLEEP 2500                        }
4  SLEEP 2500                        } cede CPU ~7.5s: los hijos allocan y se suspenden
5  SLEEP 2500                        }
6  MEM_ALLOC 0 120                   <- acapara 120 de 128. Quedan 8 libres.
7  SET CX 1
8  SLEEP 2500
9  JNZ CX 8                          <- NO suelta la memoria nunca
```

La única diferencia con `dessuspension_memoria` es el final: acá **no hay `MEM_FREE`**. El
padre acapara para siempre, así el evento tiene que venir de afuera. Los `SLEEP 2500` son
más cortos que `SUSPENSION_TIMEOUT` (5000), así que cede la CPU pero nunca se suspende.

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

---

## Las configs

### `km_dessusp.cfg` / `ks_dessusp.cfg`

Idénticas a las de `dessuspension_memoria`: `SCRIPTS_BASEPATH` apuntando a esta carpeta y
`SUSPENSION_TIMEOUT=5000` (entre el sleep del padre y el de los hijos, así se suspenden los
hijos y el padre no).

### El segundo stick: `memory_stick_2.cfg`

Es un config que **ya existía** en el repo, sin tocar. Escucha en el puerto **8004**; el
primer stick usa el **8003**. Por eso se pueden levantar los dos a la vez.

### La cuenta de memoria

```
ANTES:   128 total - 120 del padre =  8 libres  <  20 por hijo  -> nadie entra
DESPUÉS: 256 total - 120 del padre = 136 libres >= 60 (3 x 20)  -> entran los 3
```

Si quisieras que entre **sólo uno** y ver la selección por prioridad en acción, conectá un
stick más chico:

```bash
./pruebas_unitarias/dessuspension_stick/agregar_stick.sh 24
```

Con 24 bytes extra hay lugar para un solo hijo de 20, y tiene que entrar **PID 2** — el de
mejor prioridad entre los suspendidos.

---

## Por qué se levantan CUATRO IO SLEEP

Mismo motivo que en `dessuspension_memoria`: `planificador_tomar_io_libre()` hace
`sem_wait` sobre un semáforo por tipo de IO, y con un solo dispositivo los 4 procesos se
serializan. El que espera el semáforo **está en BLOCK**, así que el timer de suspensión le
corre igual y el padre se suspende soltando la memoria que tiene que estar acaparando.

Está explicado en detalle en
[`../dessuspension_memoria/README.md`](../dessuspension_memoria/README.md).
