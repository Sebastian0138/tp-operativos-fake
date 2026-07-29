# Prueba unitaria — Atención de Syscalls: retorno a la MISMA CPU

Verifica que las syscalls de memoria y de mutex devuelvan el Proceso a la CPU que hizo la
llamada, sin pasar por READY y sin que ningún otro Proceso pueda colarse en esa CPU.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"Atención de Syscalls"*:

> "Las Syscalls relacionadas a la memoria tendrán la particularidad de que una vez
> finalizadas deberán volver a enviar el mismo proceso a la CPU que realizó la llamada. Al
> momento de atender la creación de segmentos puede ser que se cuente con el espacio
> suficiente para crear el segmento pero el mismo no se encuentre contiguo, esto deberá
> disparar una Compactación."

Esta prueba cubre la **primera** oración. La segunda —compactación por espacio no
contiguo— tiene su propia carpeta:
[`../dessuspension_compactacion/`](../dessuspension_compactacion/).

Las otras dos remisiones de la sección (*"Las relacionadas a IO se deberán atender de
acuerdo a lo definido en la sección de IO"* y *"Las relacionadas a los Mutex... según lo
definido en la sección de Mutex"*) apuntan a secciones aparte del enunciado.

En el código, el retorno directo es `planificador_despachar_directo()`
(`planificador.c:377`). Trazabilidad en `docs/MAPA_TP.md` §2.6.

---

## Cómo levantarla

```bash
./pruebas_unitarias/syscalls_retorno_cpu/levantar.sh
```

**No hay que tocar nada.** Dura ~25 segundos.

### Qué se espera ver

Lo importante de esta prueba es lo que **no** aparece. Corrida real:

```
05:18:03:270  ## (1) Pasa del estado READY al estado EXEC     <- PID 1 toma una CPU
05:18:04:073  ## (2) Pasa del estado READY al estado EXEC     <- PID 2 toma la otra

              ... 14.5 segundos y 41 syscalls de PID 2, sin UNA sola transición ...

05:18:18:573  ## (2) Pasa del estado EXEC al estado EXIT
05:18:18:911  ## (3) Pasa del estado READY al estado EXEC     <- PID 3 recién ahora
05:18:25:607  ## (1) Pasa del estado EXEC al estado EXIT
```

### Lo que hay que mirar

**1. PID 2 pide 42 syscalls y tiene 3 transiciones de estado. Total.**

```
syscalls de PID 2                    transiciones de PID 2
  10  MEM_ALLOC                        1  NEW  -> READY
  10  MEM_FREE                         1  READY -> EXEC
  10  MUTEX_LOCK                       1  EXEC  -> EXIT
  10  MUTEX_UNLOCK
   1  MUTEX_CREATE                   (cero EXEC -> READY)
   1  EXIT
  ---
  42
```

**Cero `EXEC → READY`.** Ésa es la prueba directa: si el proceso volviera a la cola general
después de cada syscall, habría 40 pares `EXEC → READY` / `READY → EXEC` en el log. No hay
ninguno. El proceso entra a EXEC una vez y sale una vez, para morirse.

**2. Nunca migró de CPU.** Contando los fetch por proceso-CPU en `cpu/logs/cpu.log`:

```
CPU-proc 22668  ->  PID 2   74 fetches      <- todos de la MISMA CPU
CPU-proc 22655  ->  PID 2    0 fetches      <- nunca tocó la otra
```

No alcanza con volver a EXEC: el enunciado pide volver **a la CPU que realizó la llamada**.

**3. PID 3 no pudo colarse.** Entró a EXEC a las `18:911`, **338 ms después** de que PID 2
terminara — no un instante antes. Estuvo ~14 segundos en READY con las dos CPUs ocupadas.

Éste es el control negativo de la prueba, y es la razón de que haya **tres** procesos y no
dos. Con sólo PID 1 y PID 2 el resultado sería el mismo aunque el retorno directo no
existiera: al liberarse la CPU, el único candidato a tomarla sería el propio PID 2. Hace
falta un tercero en la cola para que "soltó la CPU" y "no la soltó" den resultados
distintos.

---

## Por qué funciona: `despachar_directo` no libera la CPU

En `atender_peticiones_cpu()` (`conexiones.c`), las syscalls de retorno directo hacen:

```c
planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);   // MISMA cpu
```

y **no** llaman a `liberar_cpu()`. Ésa es toda la diferencia. `liberar_cpu()`
(`conexiones.c:128`) marca `cpu->libre = true` y hace `sem_post(&sem_cpus_libres)`, que es
lo que despierta al planificador de corto plazo para que despache al siguiente. Sin ese
`sem_post`, PID 3 se queda esperando en el semáforo.

Comparación de los dos caminos, en el mismo `switch`:

| Syscall | Camino | ¿Libera la CPU? |
|---|---|---|
| `MEM_ALLOC` (con espacio) | `despachar_directo` | **no** |
| `MEM_ALLOC` (sin espacio) | `exec → block "MEMORIA"` + `liberar_cpu` | sí |
| `MEM_FREE` | `despachar_directo` | **no** |
| `MUTEX_CREATE` | `despachar_directo` | **no** |
| `MUTEX_LOCK` (lo toma) | `despachar_directo` | **no** |
| `MUTEX_LOCK` (ocupado) | `exec → block "MUTEX"` + `liberar_cpu` | sí |
| `MUTEX_UNLOCK` | `despachar_directo` | **no** |
| `SLEEP` / `STDIN` / `STDOUT` | `exec → block` + `liberar_cpu` | sí |

El pseudocódigo de esta prueba usa a propósito sólo las de la columna "no": el mutex `m1`
lo crea y lo toma el mismo proceso, así que el `MUTEX_LOCK` **siempre** lo consigue y nunca
cae en la rama que bloquea.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (7 ventanas, **2 CPUs**) |
| `test_sys_padre` | Pseudocódigo del PID 0: crea los tres y se va |
| `test_sys_syscalls` | PID 2: el que pide las 40 syscalls de retorno directo |
| `test_sys_cpu` | PIDs 1 y 3: CPU-bound, sin syscalls |
| `km_sys.cfg` | Config del Kernel Memory |
| `ks_sys.cfg` | Config del Kernel Scheduler |

---

## Los pseudocódigos

### `test_sys_padre` (PID 0)

```
0  INIT_PROC test_sys_cpu 0        <- PID 1: ocupa una CPU
1  INIT_PROC test_sys_syscalls 0   <- PID 2: ocupa la otra y pide syscalls
2  INIT_PROC test_sys_cpu 0        <- PID 3: el testigo, se queda en READY
3  EXIT                            <- se va enseguida para no ocupar CPU
```

### `test_sys_syscalls` (PID 2)

```
 0  MUTEX_CREATE m1
 1  SET AX 10
 2  SET BX 1
 3  MEM_ALLOC 0 16      }
 4  MUTEX_LOCK m1       } 4 syscalls de retorno directo por vuelta
 5  NOOP                }
 6  MUTEX_UNLOCK m1     }
 7  MEM_FREE 0          }
 8  SUB AX BX
 9  JNZ AX 3
10  EXIT
```

10 vueltas x 4 syscalls = **40 oportunidades** de soltar la CPU. Que no la suelte ni una
es lo que se está midiendo. Con una sola vuelta el resultado sería igual de correcto pero
mucho menos convincente.

### `test_sys_cpu` (PIDs 1 y 3)

```
0  SET AX 60
1  SET BX 1
2  NOOP          }
3  NOOP          } ~12 segundos de CPU pura
4  SUB AX BX     }
5  JNZ AX 2      }
6  EXIT
```

Sin syscalls, para que no suelten la CPU por su cuenta y el escenario se mantenga estable
mientras PID 2 hace lo suyo. PID 1 dura más que PID 2 a propósito: así el único evento que
puede liberar una CPU durante la prueba es el `EXIT` de PID 2.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia
  `SCRIPTS_BASEPATH=../pruebas_unitarias/syscalls_retorno_cpu`.
- **`ks_sys.cfg`**: **sin cambios** respecto de `kernel_scheduler/config/kernel_scheduler.cfg`.
  FIFO, sin desalojo. Es la única prueba del lote que no necesita tocar ni un parámetro.

Que sea FIFO importa: con RR, el quantum desalojaría a los procesos CPU-bound y el escenario
de "las dos CPUs ocupadas de forma estable" se desarmaría. El retorno directo seguiría
funcionando, pero el log tendría tanto ruido de desalojos que no se distinguiría.

---

## Por qué DOS CPUs

Con una sola CPU la prueba no distingue nada: si PID 2 soltara la CPU en cada syscall, el
planificador se la volvería a dar a algún proceso, y para ver la diferencia habría que
mirar *cuál*. Con dos CPUs y tres procesos, el testigo (PID 3) convierte la pregunta en un
sí/no observable: **si entra a EXEC, el retorno directo está roto.**
