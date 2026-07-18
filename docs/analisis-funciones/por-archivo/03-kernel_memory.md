# kernel_memory/ — documentación por archivo

Todo lo listado abajo fue releído directamente del código actual (no copiado de
`04-kernel_memory-infra.md` / `05-kernel_memory-gestor_memoria.md`, aunque el contenido
temático coincide con esos dos documentos). Los callers de cada función están verificados
con `grep -rn` sobre todo el repo — se listan **todas** las apariciones reales, no una
selección.

Contexto ya verificado contra el enunciado oficial del TP (no hace falta releerlo función
por función, se usa como marco): segmentación pura con tabla de segmentos por PID (base +
límite), `ALLOCATION_STRATEGY=BEST|WORST` (Best/Worst Fit, sin First Fit), la compactación
se dispara cuando hay espacio total suficiente pero no contiguo, los Memory Sticks se
agregan al final de la lista (direcciones físicas contiguas) y las operaciones que cruzan
el límite entre dos sticks se dividen y consolidan. Todo esto coincide exactamente con lo
que hace el código de abajo.

---

## `kernel_memory/src/main.c` (67 líneas)

Punto de entrada del proceso. Orquesta el arranque en orden estricto y con salida
temprana ante cualquier fallo, liberando en cada `return EXIT_FAILURE` solo lo que ya se
había creado hasta ese punto (no hay un solo `free` centralizado — cada rama de error
libera manualmente los recursos previos).

### `int main(int argc, char** argv)`
**Qué hace y cómo:**
1. Crea el boot logger (`kernel_memory_boot_logger_create`). Si falla, aborta con
   `fprintf(stderr, ...)` porque ni siquiera hay logger para reportar el error.
2. Loguea `"Iniciando Kernel Memory"`.
3. Valida `argc != 2` — el binario espera exactamente un argumento: el path al archivo de
   configuración (`./bin/kernel_memory [Archivo Config]`). Si falta, loguea el uso
   correcto y sale.
4. Crea la config con `kernel_memory_config_create(argv[1], boot_logger)`.
5. Loguea los parámetros leídos con `kernel_memory_config_log`.
6. Crea el logger definitivo (`kernel_memory_logger_create`), con el nivel de log que
   diga la config (no el nivel fijo `INFO` del boot logger). Si falla, destruye la config
   y el boot logger antes de salir.
7. Destruye el boot logger (ya cumplió su función) y loguea con el logger definitivo que
   el arranque fue correcto.
8. Crea el gestor de procesos (`gestor_procesos_create`) — la tabla PID→contexto.
9. Crea el gestor de memoria (`gestor_memoria_create`), pasándole la config y el gestor de
   procesos (los necesita para reflejar altas/bajas de segmentos en la tabla del proceso).
10. Arranca el servidor bloqueante (`iniciar_servidor_kernel_memory`) — esta llamada no
    retorna nunca en operación normal (loop infinito de `accept`); las líneas de destroy de
    abajo solo se alcanzan si `iniciar_servidor_kernel_memory` retorna por un fallo interno
    (p. ej. no pudo abrir el socket de escucha).

**Dónde se usa:** es `main`, no lo llama nadie — lo invoca el sistema operativo al
ejecutar el binario.

**Para qué:** es el único lugar del módulo donde se decide el orden de dependencias:
config → logger → gestor_procesos → gestor_memoria → servidor. Cualquier cambio en ese
orden rompería el resto (p. ej. `gestor_memoria_create` necesita la config ya parseada
para leer `ALLOCATION_STRATEGY`/`SEGMENT_MAX_SIZE`).

---

## `kernel_memory/src/config/kernel_memory_config.h` (22 líneas)

Define `t_kernel_memory_config`, la struct que agrupa las 8 claves obligatorias del
archivo de configuración:

| Campo | Clave en el .cfg | Tipo | Para qué |
|---|---|---|---|
| `log_level` | `LOG_LEVEL` | `char*` | Nivel mínimo de log del logger principal |
| `segment_max_size` | `SEGMENT_MAX_SIZE` | `int` | Tamaño máximo de un segmento — también el divisor de la fórmula de MMU (`num_segmento = dir / segment_max_size`) |
| `allocation_strategy` | `ALLOCATION_STRATEGY` | `char*` | `"BEST"` o `"WORST"` — algoritmo de búsqueda de hueco |
| `instruction_delay` | `INSTRUCTION_DELAY` | `int` | Milisegundos de espera artificial antes de responder un fetch de instrucción a la CPU |
| `compaction_delay` | `COMPACTION_DELAY` | `int` | Milisegundos de espera artificial al final de una compactación |
| `scripts_basepath` | `SCRIPTS_BASEPATH` | `char*` | Directorio base donde están los archivos de instrucciones de los procesos |
| `listen_ip` | `LISTEN_IP` | `char*` | IP donde escucha el servidor (leída pero no usada directamente en el bind — ver `conexiones.c`, que usa solo el puerto) |
| `listen_port` | `LISTEN_PORT` | `char*` | Puerto de escucha, se lo pasa tal cual a `crear_servidor` |

Declara las 3 funciones de `kernel_memory_config.c`.

---

## `kernel_memory/src/config/kernel_memory_config.c` (68 líneas)

### `static bool tiene_todas_las_propiedades(t_config* config)`
**Qué hace:** un único `&&` encadenado que verifica con `config_has_property` (de la
librería `commons`) que las 8 claves de la tabla de arriba estén presentes en el archivo.
**Cómo funciona:** cortocircuito — si falta la primera (`LOG_LEVEL`), ni siquiera evalúa
el resto.
**Dónde se usa:** solo dentro de `kernel_memory_config_create` (línea 25 del mismo
archivo) — es un helper privado (`static`), no forma parte de la API pública del módulo.
**Para qué:** falla rápido y con un único mensaje de error si al archivo de config le
falta cualquier clave, en vez de crashear más adelante con un valor `NULL`.

### `t_kernel_memory_config* kernel_memory_config_create(char* path, t_log* logger)`
**Qué hace y cómo:** abre el archivo con `config_create` (commons); si no existe o no
parsea, loguea error y devuelve `NULL`. Si abre bien, valida con la función anterior que
estén las 8 propiedades; si falta alguna, destruye el `t_config` y devuelve `NULL`. Si
está todo, hace `malloc` de la struct y copia cada valor — los strings con `strdup`
(porque `config_destroy` al final libera la memoria interna de `commons`, así que hay que
tener copia propia) y los enteros con `config_get_int_value` directo.
**Dónde se usa:** `kernel_memory/src/main.c:24`.
**Para qué:** es el único punto de entrada de configuración del módulo — todo el resto del
código lee `config->campo` sin volver a tocar el archivo.

### `void kernel_memory_config_log(t_kernel_memory_config* config, t_log* logger)`
**Qué hace:** 9 `log_info` seguidos, uno por cada clave (más la línea de cabecera "Leyendo
del archivo de configuración..."), en el mismo orden que la tabla de campos.
**Dónde se usa:** `kernel_memory/src/main.c:30`.
**Para qué:** requisito de logging de la cátedra — dejar constancia en el log de arranque
de qué configuración efectivamente se cargó (útil para debugging de una corrida específica
sin tener que abrir el .cfg).

### `void kernel_memory_config_destroy(t_kernel_memory_config* config)`
**Qué hace:** `free` de cada uno de los 5 campos `char*` (`log_level`,
`allocation_strategy`, `scripts_basepath`, `listen_ip`, `listen_port`) y después `free` de
la struct. Los dos campos `int` no necesitan liberación. Tiene guarda `if (config == NULL)
return;` — tolera destruir un puntero nulo.
**Dónde se usa:** `kernel_memory/src/main.c:35,46,55,64` — en las 3 ramas de error
tempranas de `main` y en la salida normal al final.
**Para qué:** evita leaks del único bloque de config del proceso (vive toda la corrida).

---

## `kernel_memory/src/logger/kernel_memory_logger.h` (9 líneas)

Declara las 2 funciones de `kernel_memory_logger.c`. No define structs propias — reexporta
`t_log` de `commons/log.h`.

## `kernel_memory/src/logger/kernel_memory_logger.c` (18 líneas)

### `t_log* kernel_memory_boot_logger_create(void)`
**Qué hace:** `log_create("logs/kernel_memory_boot.log", "KERNEL_MEMORY_BOOT", true,
LOG_LEVEL_INFO)` — logger separado, a un archivo distinto, con nivel `INFO` fijo (no
depende de la config, porque en el momento en que se crea este logger la config todavía no
se leyó).
**Dónde se usa:** `kernel_memory/src/main.c:10`.
**Para qué:** poder loguear errores de arranque (archivo de config faltante, malformado,
etc.) antes de tener disponible el logger definitivo, que sí depende de haber parseado la
config exitosamente.

### `t_log* kernel_memory_logger_create(t_kernel_memory_config* config)`
**Qué hace:** `log_create("logs/kernel_memory.log", "KERNEL_MEMORY", true,
log_level_from_string(config->log_level))` — logger principal, con el nivel que diga
`LOG_LEVEL` en el .cfg (convertido de string a enum con el helper de `commons`).
**Dónde se usa:** `kernel_memory/src/main.c:32`.
**Para qué:** es el logger que usa el resto de todo el módulo (`conexiones.c`,
`gestor_memoria.c`, etc. — se les pasa por parámetro desde `main`).

---

## `kernel_memory/src/contexto/contexto.h` (20 líneas)

Define `t_contexto_ejecucion`, la unidad mínima de estado que Kernel Memory guarda por
proceso activo:

| Campo | Tipo | Para qué |
|---|---|---|
| `pid` | `uint32_t` | Identifica al proceso dueño de este contexto |
| `registros` | `t_registros_cpu` (de `utils/registros.h`) | Snapshot de los 11 registros de CPU — se actualiza con `MENSAJE_ACTUALIZAR_CONTEXTO` cada vez que la CPU desaloja el proceso, y se lee con `MENSAJE_CONTEXTO` cuando el KS vuelve a mandarlo a ejecutar |
| `tabla_segmentos` | `t_list*` de `t_segmento*` (de `utils/segmento.h`) | La tabla de segmentos del proceso: cada entrada tiene `id_segmento`, `base`, `límite`, `tamanio` — es la que se manda íntegra a la CPU en `MENSAJE_CONTEXTO` para que la MMU local pueda traducir sin ida y vuelta por cada acceso |

No tiene funciones propias — es una struct de datos pura, usada por
`kernel_memory_gestor_procesos.h` (que la envuelve en `t_proceso_km`) y por
`gestor_memoria.c` (que la mantiene sincronizada cuando crea/borra/mueve segmentos).

---

## `kernel_memory/src/memoria_instrucciones/memoria_instrucciones.h` (16 líneas)

### ⚠️ Hallazgo: archivo completo es código muerto
Declara una única función:

```c
char* memoria_instrucciones_fetch(uint32_t pid, uint32_t pc);
```

El propio comentario del archivo la etiqueta como **"Mock de memoria de instrucciones
para CP2"**. Verificado con grep sobre todo el repo: **no existe un `.c` que la
implemente, y no hay un solo caller** (ni siquiera en tests). La responsabilidad real que
describe (devolver la línea de instrucción correspondiente al PC de un proceso) la cumple
efectivamente `gestor_obtener_instruccion()` en
`gestor_procesos/kernel_memory_gestor_procesos.c:84`, que sí lee el archivo real de
instrucciones en disco. Este header es un remanente de un checkpoint anterior (CP2) que
quedó sin borrar cuando se reemplazó por la implementación real.

---

## `kernel_memory/src/gestor_procesos/kernel_memory_gestor_procesos.h` (39 líneas)

Define dos structs y declara 8 funciones.

**`t_proceso_km`** — lo que Kernel Memory guarda por cada proceso:
| Campo | Tipo | Para qué |
|---|---|---|
| `contexto` | `t_contexto_ejecucion*` | PID + registros + tabla de segmentos (ver `contexto.h`) |
| `path_instrucciones` | `char*` | Ruta absoluta al `.txt` de instrucciones de ese proceso, ya resuelta con `SCRIPTS_BASEPATH` |

**`t_gestor_procesos`** — el gestor en sí:
| Campo | Tipo | Para qué |
|---|---|---|
| `lista_procesos` | `t_list*` de `t_proceso_km*` | Todos los procesos activos que conoce Kernel Memory |
| `mutex` | `pthread_mutex_t` | Protege la lista y el contenido mutable de cada `t_proceso_km` (registros, tabla de segmentos) — es un único mutex "grueso" para todo el gestor, no uno por proceso |

---

## `kernel_memory/src/gestor_procesos/kernel_memory_gestor_procesos.c` (116 líneas)

### `static void destruir_proceso_km(void* elemento)`
**Qué hace:** castea a `t_proceso_km*`, libera `path_instrucciones`, destruye
`contexto->tabla_segmentos` con `list_destroy_and_destroy_elements(..., free)` (libera
cada `t_segmento*` de la lista), libera `contexto` y finalmente la struct `proc` misma.
**Dónde se usa:** como callback de destrucción en `list_destroy_and_destroy_elements` en
`gestor_procesos_destroy` (línea 24) y directo en `gestor_remover_proceso` (línea 50).
**Para qué:** único lugar que sabe liberar un `t_proceso_km` completo, en cascada, sin
leaks — evita duplicar esa secuencia de 4 `free`/destroy en dos sitios distintos.

### `t_gestor_procesos* gestor_procesos_create()`
**Qué hace:** `malloc` de la struct, `list_create()` para `lista_procesos`,
`pthread_mutex_init`. Devuelve `NULL` si el malloc falla (no hay más pasos que puedan
fallar).
**Dónde se usa:** `kernel_memory/src/main.c:43`.
**Para qué:** inicializa el gestor una única vez al arrancar el proceso.

### `void gestor_procesos_destroy(t_gestor_procesos* gestor)`
**Qué hace:** guarda `if NULL return`; `list_destroy_and_destroy_elements(lista_procesos,
destruir_proceso_km)`; `pthread_mutex_destroy`; `free(gestor)`.
**Dónde se usa:** `kernel_memory/src/main.c:63` — al final de la vida del proceso, después
de destruir el gestor de memoria (que depende del gestor de procesos).
**Para qué:** limpieza ordenada al cerrar; en la práctica el proceso corre indefinidamente
así que solo se ejerce en un shutdown controlado.

### `void gestor_agregar_proceso(t_gestor_procesos* gestor, uint32_t pid, char* path)`
**Qué hace y cómo:** arma un `t_contexto_ejecucion` nuevo con `pid`, registros puestos en
cero con `memset` (proceso recién creado, sin estado previo de CPU) y
`tabla_segmentos = list_create()` (vacía — todavía no tiene ningún segmento asignado).
Arma el `t_proceso_km` con ese contexto y `path_instrucciones = strdup(path)` (copia
propia del string). Toma el mutex, hace `list_add`, libera el mutex.
**Dónde se usa:** `kernel_memory/src/atencion_scheduler/kernel_memory_atencion_scheduler.c:40`
— dentro del handler de `MENSAJE_ENVIAR_PID`, cuando el KS le informa a KM que se creó un
proceso nuevo (parte del `INIT_PROC` / planificación de largo plazo).
**Para qué:** es el alta de un proceso en Kernel Memory — a partir de acá
`gestor_buscar_proceso` puede encontrarlo.

### `void gestor_remover_proceso(t_gestor_procesos* gestor, uint32_t pid)`
**Qué hace y cómo:** toma el mutex, recorre linealmente `lista_procesos` buscando por
`pid`, si lo encuentra hace `list_remove` (saca el puntero de la lista) y
`destruir_proceso_km` (libera todo su contenido), corta el loop con `break`, libera el
mutex.
**Dónde se usa:** `kernel_memory/src/gestor_memoria/gestor_memoria.c:376`, al final de
`gestor_memoria_finalizar_proceso` — después de haber liberado ya los segmentos físicos y
los bloques de SWAP del proceso, como último paso.
**Para qué:** es la baja completa de un proceso — se llama una sola vez, cuando el proceso
termina (`EXIT` o falla fatal).

### `t_proceso_km* gestor_buscar_proceso_sin_lock(t_gestor_procesos* gestor, uint32_t pid)`
**Qué hace:** recorre `lista_procesos` linealmente comparando `pid`, devuelve el puntero o
`NULL`. **No toma el mutex.**
**Dónde se usa (8 lugares):** `gestor_buscar_proceso` (línea 71, la envuelve con lock);
`kernel_memory_atencion_scheduler.c:195` (dentro de un bloque `gestor_lock`/`gestor_unlock`
manual, para `MENSAJE_ACTUALIZAR_CONTEXTO`); `kernel_memory_atencion_cpu.c:36` (ídem, para
`MENSAJE_CONTEXTO`); y 4 veces dentro de `gestor_memoria.c` (líneas 255, 331, 533, 739 —
`tabla_proceso_agregar`, `gestor_memoria_eliminar_segmento`,
`actualizar_base_de_segmento_en_proceso`, dentro del loop de
`gestor_memoria_suspender_proceso`).
**Para qué:** existe como variante *sin lock* precisamente porque varios callers necesitan
hacer varias operaciones atómicas sobre el mismo proceso (buscarlo y después leer/mutar su
contenido) bajo un único lock tomado por ellos mismos con `gestor_lock`/`gestor_unlock` —
si `gestor_buscar_proceso_sin_lock` tomara el mutex internamente, esos callers caerían en
deadlock (mutex no reentrante) al intentar tomarlo de nuevo.

### `t_proceso_km* gestor_buscar_proceso(t_gestor_procesos* gestor, uint32_t pid)`
**Qué hace:** wrapper de la anterior con `gestor_lock`/`gestor_unlock` alrededor.
**Dónde se usa:** `kernel_memory/src/gestor_procesos/kernel_memory_gestor_procesos.c:85`
(dentro de `gestor_obtener_instruccion`, ver abajo) — es el único caller externo real de
esta variante con lock; el resto del módulo usa la variante `_sin_lock` porque necesita
extender la sección crítica.
**Para qué:** variante segura para un caller que solo necesita el puntero por un instante
(un solo `if (proc == NULL)` y listo), sin tener que manejar el mutex a mano.

### `void gestor_lock(t_gestor_procesos* gestor)` / `void gestor_unlock(t_gestor_procesos* gestor)`
**Qué hacen:** wrappers directos de `pthread_mutex_lock`/`unlock` sobre `gestor->mutex`.
**Dónde se usan:** `gestor_lock` en 5 lugares (`kernel_memory_atencion_scheduler.c:194`,
`kernel_memory_atencion_cpu.c:35`, y 3 veces en `gestor_memoria.c`: 254, 330, 531, 738 —
son en realidad 4, ver grep); `gestor_unlock` en 6 lugares simétricos.
**Para qué:** exponen el mutex interno del gestor para que otros módulos (`atencion_cpu`,
`atencion_scheduler`, `gestor_memoria`) puedan envolver una sección crítica propia que
incluye un `gestor_buscar_proceso_sin_lock` seguido de lectura/escritura del contenido
encontrado — sin este par de funciones, esos callers no tendrían forma de extender el
lock más allá de una sola llamada.

### `char* gestor_obtener_instruccion(t_gestor_procesos* gestor, uint32_t pid, uint32_t pc, t_log* logger)`
**Qué hace y cómo:**
1. Busca el proceso con `gestor_buscar_proceso` (con lock). Si no existe, loguea error y
   devuelve `NULL`.
2. Abre `proc->path_instrucciones` con `fopen(..., "r")`. Si falla, loguea y devuelve
   `NULL`.
3. Lee el archivo línea por línea con `fgets` en un buffer de 256 bytes, contando
   `linea_actual` desde 0. Cuando `linea_actual == pc`, le saca el `\n` final con
   `strcspn` y hace `strdup` de esa línea como resultado; corta el loop.
4. Cierra el archivo siempre (haya encontrado o no la línea).
5. Si encontró instrucción, loguea `"## PID: %u - Obtener instrucción: %u - Instrucción:
   %s"` (log obligatorio de cátedra).
6. Devuelve el `strdup` (memoria del caller — responsabilidad de quien llama liberarla) o
   `NULL` si el PC pedido supera la cantidad de líneas del archivo.

**Nota de diseño (no es un bug, es una decisión de simplicidad):** relee el archivo desde
el principio en **cada** llamada — no cachea el archivo abierto ni su contenido en memoria.
Para los tamaños de programas de prueba del TP esto es aceptable (es O(pc) por fetch, no
O(1)), pero sería el primer punto a optimizar si los scripts de prueba fueran mucho más
largos.

**Dónde se usa:** `kernel_memory/src/atencion_cpu/kernel_memory_atencion_cpu.c:70`, dentro
del handler de `MENSAJE_PEDIR_INSTRUCCION` — es la función real detrás del "fetch" del
ciclo de instrucción de la CPU (después de haber esperado `INSTRUCTION_DELAY` ms).

**Para qué:** es el reemplazo real y funcional del mock muerto
`memoria_instrucciones_fetch` documentado arriba.

---

## `kernel_memory/src/conexiones/conexiones.h` (15 líneas)

Declara la única función pública del archivo, `iniciar_servidor_kernel_memory`, con la
firma completa (puerto + config + gestor de procesos + gestor de memoria + logger).

## `kernel_memory/src/conexiones/conexiones.c` (144 líneas)

Usa 4 punteros `static` a nivel de archivo (`_logger`, `_gestor`, `_gestor_memoria`,
`_config`) para que `manejar_cliente`, que corre como hilo con un único argumento
(`void* arg` = el fd), pueda acceder a las dependencias sin tener que empaquetarlas en una
struct de contexto por hilo. Es un patrón de estado global de módulo, aceptable acá porque
solo hay una instancia de servidor por proceso.

### `void iniciar_servidor_kernel_memory(const char* puerto, t_kernel_memory_config* config, t_gestor_procesos* gestor, t_gestor_memoria* gestor_memoria, t_log* logger)`
**Qué hace y cómo:** guarda los 4 punteros en las variables `static`. Crea el socket de
escucha con `crear_servidor(puerto)` (de `utils/sockets`); si falla, loguea y retorna
(única forma en que `main` puede seguir después de esta llamada). Loguea que está
escuchando. Entra en loop infinito: `accept()` bloqueante, y por cada conexión aceptada
lanza un hilo `manejar_cliente` con `pthread_create` + `pthread_detach` (el hilo se limpia
solo al terminar, nadie hace `pthread_join`). El fd de la conexión se pasa como `int*`
mallocado individualmente (evita la típica race condition de reusar una variable de loop
por valor entre hilos).
**Dónde se usa:** `kernel_memory/src/main.c:60` — es la llamada bloqueante final de
`main`, el corazón del servidor.
**Para qué:** modelo de concurrencia "un hilo por conexión" — cada CPU, cada instancia del
KS, cada Memory Stick y el módulo SWAP mantienen su propia conexión persistente atendida
por su propio hilo.

### `static void atender_memory_stick(int fd)`
**Qué hace y cómo:** espera el mensaje `MENSAJE_INFORMAR_TAMANIO`; si no llega o es otro
código, loguea warning y retorna (conexión rechazada). Si llega, recibe `tamanio` (u32),
después `ip_len` + el string `ip` (recibido en un buffer mallocado a medida,
`ip_len+1` para el `\0` final), después `port_len` + el string `puerto` de la misma forma.
Llama a `gestor_memoria_registrar_stick(_gestor_memoria, tamanio, ip, puerto)`, libera
`ip`/`puerto` (ya fueron copiados con `strdup` adentro del registrar), y si el registro
falló (`id_stick == -1`) retorna. Si salió bien, entra en un loop `while (recv(fd,
&descarte, 1, MSG_WAITALL) > 0);` que no hace nada con lo que llega — es un loop de
"esperar hasta que el peer cierre la conexión", porque el Memory Stick, una vez registrado,
nunca más manda nada por **este** fd (los datos van por el fd de datos separado que abrió
`gestor_memoria_registrar_stick`). Cuando el `recv` retorna 0 o error, significa que el
stick se desconectó — llama a `gestor_memoria_stick_desconectado`.
**Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:118`, dentro del `case
HANDSHAKE_MEMORY_STICK` de `manejar_cliente`.
**Para qué:** es el mecanismo de detección de caída de un stick — al no haber heartbeats
explícitos, se apoya en que un `recv` que retorna en esta conexión de control (que el stick
jamás debería cerrar en operación normal) señala desconexión, y eso dispara el aviso de
"memoria corrupta" documentado en el enunciado.

### `void* manejar_cliente(void* arg)`
**Qué hace y cómo:** castea `arg` a `int*`, extrae el fd y libera el puntero envoltorio.
Recibe el primer `int32_t` de la conexión — el handshake — con `MSG_WAITALL`; si `recv`
devuelve `<= 0` (conexión cerrada antes de mandar nada), cierra el fd y retorna. Según el
valor del handshake:
  - `HANDSHAKE_CPU` → `atender_cpu(...)`.
  - `HANDSHAKE_SCHEDULER` → `atender_scheduler(...)`.
  - `HANDSHAKE_SCHEDULER_NOTIF` → loguea, llama `gestor_memoria_set_notif_ks(_gestor_memoria,
    fd_conexion)` y **retorna sin cerrar el fd** — este hilo no vuelve a leer nada más; el
    fd queda vivo para que otros hilos (dentro de `gestor_memoria`) le hagan `send` de
    notificaciones asíncronas (`MENSAJE_MAS_MEMORIA`, `MENSAJE_MEMORIA_CORRUPTA`).
  - `HANDSHAKE_MEMORY_STICK` → `atender_memory_stick(fd_conexion)`.
  - `HANDSHAKE_SWAP` → recibe `MENSAJE_INFORMAR_TAMANIO` + `total` + `block`; si algo
    falla, loguea warning y sale del switch (lo que hace que después caiga en el `close`
    final, cerrando la conexión); si todo llega bien, llama
    `gestor_memoria_registrar_swap(_gestor_memoria, fd_conexion, total, block)` y
    **retorna sin cerrar el fd**, por el mismo motivo que el canal de notificaciones: el
    fd queda en manos del gestor de memoria para las operaciones de bloque bajo demanda.
  - `default` → loguea "Handshake desconocido" y cae al `close` final.

Fuera del switch (para los casos que sí llegan ahí): `close(fd_conexion); return NULL;`.

**Dónde se usa:** lanzada como entry point de hilo desde
`iniciar_servidor_kernel_memory` (línea 49, vía `pthread_create`).

**Para qué:** es el despachador central de todo el protocolo de conexión de Kernel Memory
— el único lugar que interpreta el handshake inicial y decide a qué handler delegar el
resto de la conexión.

---

## `kernel_memory/src/atencion_cpu/kernel_memory_atencion_cpu.h` (15 líneas)

Declara `atender_cpu` con su firma completa.

## `kernel_memory/src/atencion_cpu/kernel_memory_atencion_cpu.c` (98 líneas)

### `void atender_cpu(int fd, t_gestor_procesos* gestor, t_gestor_memoria* gestor_memoria, t_kernel_memory_config* config, t_log* logger)`
**Qué hace y cómo:**
1. Recibe el `id_cpu` (int32) que la CPU manda después del handshake, loguea `"## CPU %d
   Conectada"` (log obligatorio).
2. Manda de vuelta `SEGMENT_MAX_SIZE` (como `uint32_t`) — es lo primero que la CPU necesita
   para poder calcular `num_segmento`/`desplazamiento` en su propia MMU local.
3. Entra en loop de atención de mensajes (`recv` de un `t_codigo_mensaje`; si `<=0`, corta
   el loop — CPU desconectada):
   - `MENSAJE_CONTEXTO`: recibe el `pid`. Toma `gestor_lock`, busca el proceso con
     `gestor_buscar_proceso_sin_lock`. Si no existe, loguea warning y manda un contexto
     vacío (registros en cero + `cant=0` segmentos) — caso defensivo, no debería pasar en
     operación normal. Si existe, manda `proc->contexto->registros` completo, después la
     cantidad de segmentos (`list_size`), y después cada `t_segmento` de la tabla,
     **todo esto con el mutex del gestor todavía tomado** (el comentario del código explica
     por qué: otro hilo, p. ej. `atencion_scheduler` atendiendo un `MEM_ALLOC` concurrente
     del mismo proceso, podría mutar `tabla_segmentos` a mitad de armar la respuesta si no
     se sostiene el lock). Libera el lock al final y loguea `"## PID: %u - Contexto enviado
     (%u segmentos)"`.
   - `MENSAJE_PEDIR_INSTRUCCION`: recibe `pid` y `pc`. Duerme
     `usleep(config->instruction_delay * 1000)` — el delay artificial de fetch configurado.
     Llama `gestor_obtener_instruccion`. Si es `NULL`, manda `len=0`; si no, manda
     `len` seguido de los bytes de la instrucción (sin el `\0`), y libera el `strdup`
     recibido.
   - `MENSAJE_TOPOLOGIA_STICKS`: llama `gestor_memoria_serializar_topologia`, manda el
     buffer serializado tal cual (el formato interno ya incluye su propio conteo, así que
     no hace falta mandar un largo total aparte) y lo libera.
   - `default`: loguea "Mensaje desconocido de CPU" y sigue el loop (no corta la conexión
     por un mensaje no reconocido).
4. Al salir del loop, loguea "CPU desconectada (fd: %d)".

**Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:104`, dentro del `case
HANDSHAKE_CPU` de `manejar_cliente` — un hilo por cada CPU conectada, vive mientras dure
esa conexión.

**Para qué:** es el lado servidor de **todo** el canal CPU↔Kernel Memory: entrega de
contexto al empezar a ejecutar un proceso, fetch de instrucciones instrucción por
instrucción, y descubrimiento de topología de Memory Sticks. Nótese que **no** interviene
en absoluto en la lectura/escritura física de datos de usuario — eso lo hace la CPU
directo contra los `memory_stick`, sin pasar por acá (ver `06-memory_stick.md`).

---

## `kernel_memory/src/atencion_scheduler/kernel_memory_atencion_scheduler.h` (15 líneas)

Declara `atender_scheduler` con su firma completa.

## `kernel_memory/src/atencion_scheduler/kernel_memory_atencion_scheduler.c` (213 líneas)

El handler de protocolo más grande del módulo — 9 opcodes distintos en un único `switch`
dentro de un loop de vida de la conexión con el Kernel Scheduler.

### `void atender_scheduler(int fd, t_gestor_procesos* gestor, t_gestor_memoria* gestor_memoria, t_kernel_memory_config* config, t_log* logger)`

Loguea `"## Kernel Scheduler Conectado - FD del socket: %d"` al entrar, y entra en el loop
principal (mismo patrón: `recv` de `t_codigo_mensaje`, `<=0` corta el loop).

**Casos del switch:**

- **`MENSAJE_ENVIAR_PID`** (líneas 24-46): recibe `pid`, `path_len` y el string `path` (el
  nombre del archivo de instrucciones, sin el basepath). Arma `path_completo` con
  `snprintf` concatenando `config->scripts_basepath` + `"/"` + `path` en un buffer fijo de
  512 bytes. Llama `gestor_agregar_proceso(gestor, pid, path_completo)` — alta del proceso.
  Loguea `"## PID: %u - Proceso Creado"` (log obligatorio) y responde `MENSAJE_OK`.
  Es el mensaje que dispara la planificación de largo plazo del lado de KM: el KS crea el
  PCB y, cuando decide pasarlo a READY, le informa a KM para que reserve su contexto acá.

- **`MENSAJE_ESPACIO_LIBRE`** (48-52): llama `gestor_memoria_espacio_libre` y devuelve el
  resultado como `uint32_t`. Usado por el KS para decisiones de mediano plazo (¿hay
  suficiente memoria para des-suspender a alguien de `SUSP_READY`?) sin necesitar intentar
  y fallar un `MEM_ALLOC` real.

- **`MENSAJE_CREAR_SEGMENTO`** (54-85): recibe `pid`, `id_segmento`, `tamanio`. Llama
  `gestor_memoria_crear_segmento`. **Si el resultado es
  `CREAR_SEGMENTO_NECESITA_COMPACTACION`**, ejecuta el protocolo síncrono de compactación
  sobre la misma conexión: manda `MENSAJE_COMPACTACION` al KS, espera de vuelta
  `MENSAJE_DESALOJO_OK` (si no llega o es otra cosa, loguea error fatal y hace `return`
  — corta la atención a este KS por completo, no hay forma de recuperarse de un protocolo
  de compactación que no responde lo esperado); si llega bien, llama
  `gestor_memoria_compactar` y **reintenta** `gestor_memoria_crear_segmento` una segunda
  vez (asumiendo que ahora sí hay hueco contiguo). Responde `MENSAJE_OK` si el resultado
  final es `CREAR_SEGMENTO_OK`, si no `MENSAJE_ERROR`.
  Este es el punto exacto donde el módulo de memoria le "pide permiso" al scheduler para
  parar el mundo (desalojar todas las CPUs) antes de mover datos.

- **`MENSAJE_ELIMINAR_SEGMENTO`** (87-97): recibe `pid`, `id_segmento`; llama
  `gestor_memoria_eliminar_segmento`; responde OK/ERROR según el resultado. Es el lado
  servidor de la syscall `MEM_FREE`.

- **`MENSAJE_FINALIZAR_PROCESO`** (99-108): recibe `pid`; llama
  `gestor_memoria_finalizar_proceso` (libera todos sus segmentos, sus bloques de SWAP si
  los tenía, y su contexto); responde siempre `MENSAJE_OK` (no hay condición de fallo
  reportada — finalizar un proceso que no tiene nada que liberar simplemente no hace
  nada). Se dispara con la syscall `EXIT`.

- **`MENSAJE_LEER_MEMORIA`** (110-137): camino de la syscall `STDOUT`. Recibe `pid`,
  `dir_logica`, `tamanio`. Reserva un buffer (con guarda `tamanio > 0 ? tamanio : 1` para
  no hacer `malloc(0)`), llama `gestor_memoria_io_rw(..., escribir=false, &dir_fisica)`.
  El comentario del código explica por qué la traducción lógica→física se hace **acá y no
  antes**: si se tradujera del lado del KS y luego hubiera una compactación o una
  suspensión mientras el proceso estaba bloqueado en la IO, esa dirección física quedaría
  obsoleta; traduciendo en el momento exacto de la operación se evita esa condición de
  carrera. Responde `MENSAJE_OK`/`MENSAJE_ERROR`; si OK, manda el buffer leído y loguea
  `"## PID: %u - Lectura - Dir. Física: %u - Tamaño: %u"` **solo si `dir_fisica !=
  UINT32_MAX`** (ese centinela indica que la lectura fue resuelta contra SWAP, no contra
  memoria principal — en ese caso no hay una "dirección física" real que loguear).

- **`MENSAJE_ESCRIBIR_MEMORIA`** (139-164): camino simétrico para `STDIN`. Recibe `pid`,
  `dir_logica`, `tamanio` y el `contenido` a escribir. Llama `gestor_memoria_io_rw(...,
  escribir=true, ...)`, libera el contenido recibido, loguea igual que arriba (con la
  misma guarda de `UINT32_MAX`) y responde OK/ERROR.

- **`MENSAJE_SUSPENDER_PROCESO`** (166-175): recibe `pid`; llama
  `gestor_memoria_suspender_proceso`; responde OK/ERROR. Lo dispara el KS cuando su
  `hilo_timer_suspension` decide mover a un proceso de `BLOCK` a `SUSP_BLOCK`.

- **`MENSAJE_DESSUSPENDER_PROCESO`** (177-186): simétrico, `pid` →
  `gestor_memoria_dessuspender_proceso` → OK/ERROR. Lo dispara el KS cuando decide
  reactivar a alguien de `SUSP_READY`.

- **`MENSAJE_ACTUALIZAR_CONTEXTO`** (188-204): recibe `pid` y un `t_registros_cpu`
  completo. Toma `gestor_lock`, busca el proceso sin lock, si existe pisa
  `proc->contexto->registros = regs` (asignación de struct completa por valor), suelta el
  lock, responde siempre `MENSAJE_OK`. Se dispara cada vez que la CPU desaloja un proceso
  (por quantum, syscall bloqueante, fin de ráfaga, etc.) y el KS reenvía el contexto
  actualizado a KM para persistirlo hasta la próxima vez que ese proceso vuelva a EXEC.

- **`default`**: loguea "Mensaje desconocido del KS" y continúa el loop.

Al salir del loop principal: loguea "Kernel Scheduler desconectado".

**Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:107`, dentro del `case
HANDSHAKE_SCHEDULER` de `manejar_cliente`.

**Para qué:** es el canal de control síncrono completo entre el Kernel Scheduler y Kernel
Memory — todas las decisiones de scheduling que requieren tocar memoria (crear proceso,
asignar/liberar segmentos, IO lógica, suspender/restaurar, sincronizar contexto) pasan por
acá. El otro canal (`HANDSHAKE_SCHEDULER_NOTIF`, atendido aparte en `conexiones.c`) es
exclusivamente para las notificaciones asíncronas que **KM inicia hacia el KS** (más
memoria, memoria corrupta) — separarlos evita que una notificación empujada en medio de
una espera de respuesta síncrona rompa el protocolo request/response de este archivo.

---

## `kernel_memory/src/gestor_memoria/gestor_memoria.h` (147 líneas)

Header más denso del módulo en términos de superficie de API (18 funciones públicas) y
definiciones de datos. Ya está documentado campo por campo más arriba en el bloque de
contexto — acá el detalle completo de cada tipo:

**`t_stick`** — un Memory Stick registrado, rango global `[base, base+tamanio)`:
`id`, `tamanio`, `base` (calculada como `memoria_total` acumulado al momento de conectarse
— ver `gestor_memoria_registrar_stick`), `ip`, `puerto` (para poder loguear/depurar, la
conexión de datos ya está abierta), `fd_datos` (la conexión persistente KM→stick usada
para lecturas/escrituras y compactación), `conectado` (bool — permite marcar un stick como
caído sin sacarlo de la lista, preservando los índices e ids de los demás).

**`t_segmento_fisico`** — un segmento ocupado en la memoria global: `pid`, `id_segmento`,
`base`, `tamanio`. Es la vista "global" que espeja cada entrada de la tabla de segmentos
por proceso (`t_segmento` en `contexto.h`), pero organizada como lista plana para poder
recorrer todos los segmentos de todos los procesos de una — necesario para `buscar_hueco`
y para la compactación.

**`t_segmento_swap`** — un segmento que fue movido a SWAP: `pid`, `id_segmento`,
`tamanio`, `bloques` (lista de `uint32_t*`, un número de bloque por cada fragmento del
segmento, en orden — un bloque nunca mezcla contenido de dos segmentos distintos, aunque
el último bloque de un segmento puede quedar parcialmente vacío y se rellena con ceros).

**`t_resultado_crear_segmento`** (enum): `CREAR_SEGMENTO_OK`,
`CREAR_SEGMENTO_SIN_ESPACIO` (no hay espacio total suficiente, o el tamaño pedido supera
`SEGMENT_MAX_SIZE`), `CREAR_SEGMENTO_NECESITA_COMPACTACION` (hay espacio total suficiente
pero fragmentado — dispara el protocolo de compactación).

**`t_gestor_memoria`** — el administrador central:
| Campo | Para qué |
|---|---|
| `sticks` | Lista de `t_stick*` conectados, en orden de conexión (= orden de direccionamiento físico) |
| `segmentos` | Lista plana de `t_segmento_fisico*` — todos los segmentos ocupados, de todos los procesos |
| `memoria_total` | Suma de los tamaños de todos los sticks conectados |
| `mutex` | Protege `sticks`, `segmentos`, `memoria_total` |
| `fd_notif_ks` | Canal separado hacia el KS para notificaciones async (`-1` si todavía no se conectó) |
| `mutex_notif` | Protege `fd_notif_ks` contra un `send` concurrente de dos notificaciones a la vez |
| `fd_swap` | Conexión hacia el módulo SWAP (`-1` si no hay) |
| `swap_block_size`, `swap_cant_bloques` | Geometría del archivo de swap, informada por el propio módulo SWAP al conectarse |
| `swap_bloques_usados` | Bitmap (`bool*`) de qué bloques están ocupados |
| `segmentos_swap` | Lista de `t_segmento_swap*` — todos los segmentos actualmente en SWAP, de todos los procesos suspendidos |
| `mutex_swap` | Protege los 5 campos anteriores relacionados a SWAP — mutex separado del de memoria principal para minimizar contención (una operación de SWAP no bloquea, p. ej., una creación de segmento de otro proceso) |
| `config`, `gestor_procesos`, `logger` | Punteros a las dependencias que el gestor necesita para operar (no son de su propiedad, no las destruye) |

---

## `kernel_memory/src/gestor_memoria/gestor_memoria.c` (988 líneas)

El archivo más grande y denso de todo el TP después de `planificador.c`. Se organiza en 8
bloques temáticos, marcados con comentarios `// ====...====` en el propio código.

### Bloque: Creación / destrucción

**`t_gestor_memoria* gestor_memoria_create(t_kernel_memory_config* config, t_gestor_procesos* gestor_procesos, t_log* logger)`**
Inicializa todos los campos de la struct descriptos arriba: listas vacías, `memoria_total =
0`, los 3 mutex con `pthread_mutex_init`, `fd_notif_ks = -1`, `fd_swap = -1`,
`swap_bloques_usados = NULL` (todavía no se sabe cuántos bloques hay — se reserva recién en
`gestor_memoria_registrar_swap`), y copia los 3 punteros de dependencias.
Caller: `kernel_memory/src/main.c:51`.

**`static void stick_destroy(void* elemento)`** / **`static void segmento_swap_destroy(void* elemento)`**
Callbacks de destrucción de un `t_stick` (cierra `fd_datos` si está abierto, libera `ip` y
`puerto`) y de un `t_segmento_swap` (destruye su lista `bloques` liberando cada
`uint32_t*`). Usados por `list_destroy_and_destroy_elements` dentro de
`gestor_memoria_destroy`, y `segmento_swap_destroy` también se llama directo dentro de
`gestor_memoria_finalizar_proceso` (línea 370) y al final de
`gestor_memoria_dessuspender_proceso` (línea 836) para liberar un `t_segmento_swap`
puntual sin pasar por la lista completa.

**`void gestor_memoria_destroy(t_gestor_memoria* gm)`**
Destruye las 3 listas con sus respectivos callbacks, cierra `fd_swap` si estaba abierto,
libera el bitmap, destruye los 3 mutex, libera la struct. No tiene callers activos en el
código (no se llama desde `main.c` al final de la corrida — ver Hallazgos).

### Bloque: Notificaciones al Kernel Scheduler

**`void gestor_memoria_set_notif_ks(t_gestor_memoria* gm, int fd)`**
Guarda `fd` en `gm->fd_notif_ks` bajo `mutex_notif`. Caller:
`kernel_memory/src/conexiones/conexiones.c:114`, cuando llega el handshake
`HANDSHAKE_SCHEDULER_NOTIF`.

**`static void notificar_ks(t_gestor_memoria* gm, t_codigo_mensaje codigo, uint32_t* payload)`**
Bajo `mutex_notif`, si `fd_notif_ks != -1`, manda el código y, si `payload` no es `NULL`,
un `uint32_t` extra. Callers internos: `gestor_memoria_registrar_stick` (con
`MENSAJE_MAS_MEMORIA` + el nuevo total) y `gestor_memoria_stick_desconectado` (con
`MENSAJE_MEMORIA_CORRUPTA`, sin payload).
**Para qué:** único punto de salida de mensajes espontáneos hacia el KS — protegido con su
propio mutex porque puede ser llamado desde el hilo que atiende un Memory Stick
concurrentemente con otro hilo que atiende otro stick.

### Bloque: Sticks

**`int gestor_memoria_registrar_stick(t_gestor_memoria* gm, uint32_t tamanio, char* ip, char* puerto)`**
Abre la conexión de **datos** KM→stick con `crear_conexion(ip, puerto)` (una conexión
nueva, distinta de la que usó el stick para presentarse) y manda el handshake
`HANDSHAKE_KERNEL_MEMORY` por ese nuevo fd. Si la conexión falla, loguea error y devuelve
`-1` sin registrar nada. Si abre, arma el `t_stick` (copia `ip`/`puerto` con `strdup`,
`conectado = true`). Bajo `mutex`: asigna `id = list_size(sticks)` (el próximo índice
libre — nunca se reutilizan ids de sticks caídos), `base = memoria_total` (**la clave de
que los sticks queden contiguos: cada uno arranca donde termina la suma de los
anteriores**), suma `tamanio` a `memoria_total`, hace `list_add`. Fuera del lock, loguea
(incluyendo el log obligatorio `"## Memory Stick de %u bytes Conectada"`) y llama
`notificar_ks` con `MENSAJE_MAS_MEMORIA` — dispara del lado del KS el intento de
des-suspender procesos que estaban esperando memoria.
Caller: `kernel_memory/src/conexiones/conexiones.c:76`, dentro de `atender_memory_stick`.

**`void gestor_memoria_stick_desconectado(t_gestor_memoria* gm, int id_stick)`**
Bajo `mutex`, busca el stick por id; si lo encuentra y estaba `conectado`, lo marca
`conectado = false`, cierra y anula `fd_datos`, y (ya afuera del lock) loguea error y llama
`notificar_ks(gm, MENSAJE_MEMORIA_CORRUPTA, NULL)`.
Caller: `kernel_memory/src/conexiones/conexiones.c:86`.
**Nota:** el stick caído **no se saca de la lista** ni se reduce `memoria_total` — el
sistema entero pasa a un estado de "memoria corrupta" del que no se recupera (coherente con
el `planificador_bsod` documentado del lado del KS: ante esta notificación, el KS finaliza
todos los procesos y aborta).

### Bloque: Huecos y segmentos

**`static int comparar_por_base(const void* a, const void* b)`**
Comparador para `qsort` de dos `t_segmento_fisico**` por su campo `base` (orden ascendente,
con la forma idiomática `(a>b)-(a<b)` que evita overflow de restar directamente).
Usado únicamente por `segmentos_ordenados`.

**`static t_segmento_fisico** segmentos_ordenados(t_gestor_memoria* gm, int* cant_out)`**
Copia todos los punteros de `gm->segmentos` a un array nuevo (`malloc`) y lo ordena con
`qsort` + el comparador de arriba. **Debe llamarse con `gm->mutex` ya tomado** (no lockea
internamente). El caller es responsable de `free(arr)`.
Callers: `buscar_hueco` (línea 197) y `gestor_memoria_compactar` (línea 572).
**Para qué:** tanto la búsqueda de huecos como la compactación necesitan recorrer los
segmentos en orden espacial (por dirección base), no en el orden arbitrario en que están
en la lista interna — esta función centraliza ese ordenamiento.

**`static bool estrategia_es_best_fit(t_gestor_memoria* gm)`**
`strcmp(gm->config->allocation_strategy, "WORST") != 0` — es decir, **cualquier valor que
no sea exactamente `"WORST"` se trata como Best Fit**, incluyendo `"BEST"` pero también
cualquier typo. Es una decisión de diseño (default seguro hacia Best Fit) más que una
validación estricta contra el enunciado, que solo define los dos valores `BEST`/`WORST`.
Caller: `buscar_hueco` (línea 192).

**`static bool hueco_es_mejor_que_el_actual(bool best_fit, bool hay_candidato_previo, uint32_t tamanio_candidato, uint32_t tamanio_mejor_actual)`**
Lógica de comparación pura: si no hay candidato previo, cualquiera sirve; si es Best Fit,
gana el hueco más chico que aún así entra (`tamanio_candidato < tamanio_mejor_actual`); si
es Worst Fit, gana el más grande. Caller: `buscar_hueco` (línea 216).

**`static bool buscar_hueco(t_gestor_memoria* gm, uint32_t tamanio, uint32_t* base_out)`**
**Cómo funciona paso a paso:** obtiene los segmentos ordenados por base. Recorre `i` desde
`0` hasta `cantidad_segmentos` **inclusive** (un recorrido más que la cantidad de
segmentos, para poder evaluar también el hueco final después del último segmento). En cada
vuelta, `fin_hueco` es la base del segmento `i` (o `memoria_total` si `i` ya pasó el último
segmento — el hueco final). Si `fin_hueco > inicio_hueco` hay un hueco real de tamaño
`fin_hueco - inicio_hueco`; si ese hueco es `>= tamanio` pedido y es "mejor" que el mejor
candidato hasta ahora (según Best/Worst Fit), se guarda como elegido. Después de evaluar el
hueco, `inicio_hueco` avanza al final del segmento `i` actual (si `fin_segmento >
inicio_hueco`, para tolerar segmentos que por algún motivo se solaparan — no debería pasar
en un sistema correcto, pero la guarda evita que `inicio_hueco` retroceda). Libera el array
y devuelve `true`/`false` según si encontró candidato, dejando la base elegida en
`*base_out`.
Caller: `gestor_memoria_crear_segmento` (línea 282) y `gestor_memoria_dessuspender_proceso`
(línea 783, una vez por cada segmento a restaurar).
**Para qué:** es el corazón del algoritmo de asignación de espacio libre — implementa
exactamente el modelo del enunciado (huecos entre segmentos ordenados por base, Best/Worst
Fit configurable).

**`static uint32_t espacio_libre_sin_lock(t_gestor_memoria* gm)`** / **`uint32_t gestor_memoria_espacio_libre(t_gestor_memoria* gm)`**
La primera suma el `tamanio` de todos los segmentos ocupados y resta de `memoria_total`
(debe llamarse con el mutex ya tomado); la segunda es el wrapper público con lock.
Callers de la variante sin lock: `gestor_memoria_crear_segmento` (línea 283, para decidir
entre `SIN_ESPACIO` y `NECESITA_COMPACTACION`). Caller de la pública:
`kernel_memory_atencion_scheduler.c:49` (`MENSAJE_ESPACIO_LIBRE`).

**`static void tabla_proceso_agregar(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t base, uint32_t tamanio)`**
Bajo `gestor_lock`/`gestor_unlock` (del gestor de **procesos**, no del gestor de memoria),
busca el proceso y, si existe, arma un `t_segmento` nuevo (`id_segmento`, `base`,
`tamanio`, `limite = base + tamanio - 1`) y lo agrega a `tabla_segmentos`.
Callers: `gestor_memoria_crear_segmento` (línea 304) y
`gestor_memoria_dessuspender_proceso` (línea 835).
**Para qué:** mantiene sincronizada la vista "por proceso" (`t_segmento` en la tabla del
contexto, la que se manda a la CPU) con la vista "global" (`t_segmento_fisico` en
`gm->segmentos`) cada vez que se crea o restaura un segmento.

**`static bool coincide_segmento(uint32_t pid, uint32_t id_segmento, void* elemento)`**
Predicado simple de igualdad `pid`+`id_segmento` sobre un `t_segmento_fisico*`. Único
caller: `gestor_memoria_eliminar_segmento` (línea 315).

**`t_resultado_crear_segmento gestor_memoria_crear_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t tamanio)`**
**Cómo funciona:** valida primero `tamanio == 0 || tamanio > SEGMENT_MAX_SIZE` — rechazo
inmediato, sin tomar ningún lock. Bajo `gm->mutex`, intenta `buscar_hueco`. Si no hay
hueco: suelta el lock, mide `espacio_libre_sin_lock` *fuera* del lock ya soltado (nota:
hay una ventana de carrera teórica acá — entre soltar el mutex y volver a medir el espacio
libre, otro hilo podría cambiarlo — pero el resultado solo se usa para decidir el código de
retorno informativo, no para tomar una decisión de asignación real, así que no compromete
la corrección del sistema, solo podría dar un `NECESITA_COMPACTACION` de más o de menos en
un caso de carrera rarísimo). Si el espacio libre total alcanza, devuelve
`CREAR_SEGMENTO_NECESITA_COMPACTACION`; si no, `CREAR_SEGMENTO_SIN_ESPACIO`. Si sí hay
hueco: arma el `t_segmento_fisico`, lo agrega a `gm->segmentos`, suelta el lock, llama
`tabla_proceso_agregar` (que toma su propio lock, el del gestor de procesos — nunca los dos
mutex tomados a la vez, evitando un posible orden de lock inconsistente entre esta función
y otras), loguea `"## PID: %u - Segmento Creado %u - Tamaño: %u"` (log obligatorio) y
devuelve `CREAR_SEGMENTO_OK`.
Caller: `kernel_memory_atencion_scheduler.c:61,78` (primer intento y reintento post-compactación).

**`bool gestor_memoria_eliminar_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento)`**
Bajo `gm->mutex`, busca linealmente el segmento que matchea `coincide_segmento`, si lo
encuentra lo saca de la lista y lo libera. Fuera de ese lock (ya liberado), si se encontró,
entra al lock del gestor de procesos y saca la entrada correspondiente de
`tabla_segmentos`. Si no se encontró nada, loguea warning (`MEM_FREE` de un segmento que no
existe) y devuelve `false`. Si sí, loguea info y devuelve `true`.
Caller: `kernel_memory_atencion_scheduler.c:92` — lado servidor de la syscall `MEM_FREE`.

**`void gestor_memoria_finalizar_proceso(t_gestor_memoria* gm, uint32_t pid)`**
Tres fases secuenciales, cada una con su propio lock tomado y soltado por separado (nunca
dos mutex simultáneos): (1) bajo `gm->mutex`, recorre `gm->segmentos` **de atrás para
adelante** (`for i = size-1; i >= 0; i--` — importante porque `list_remove` desplaza
índices, recorrer de atrás hacia adelante evita saltearse elementos) liberando todos los
segmentos del `pid`; (2) bajo `mutex_swap`, recorre `segmentos_swap` de la misma forma,
liberando cada bloque usado (`swap_bloques_usados[*nro] = false`) y destruyendo el
`t_segmento_swap`; (3) llama `gestor_remover_proceso` (que toma su propio lock del gestor
de procesos) para liberar el contexto completo. Loguea al final.
Caller: `kernel_memory_atencion_scheduler.c:103` — lado servidor de la syscall `EXIT`.

### Bloque: Lectura/escritura física global (dividida entre sticks)

**`static t_stick* stick_para_direccion(t_gestor_memoria* gm, uint32_t dir)`**
Recorre `gm->sticks` linealmente buscando el que cumple `dir >= stick->base && dir <
stick->base + stick->tamanio`. Debe llamarse con `gm->mutex` tomado. Callers:
`operar_fisico` (línea 428) y `copiar_segmento_entre_sticks` (línea 507).

**`static bool stick_operar(t_gestor_memoria* gm, t_stick* stick, bool escribir, uint32_t dir_local, uint32_t tamanio, void* buffer)`**
Si el stick no está `conectado` o su `fd_datos` es `-1`, devuelve `false` de entrada. Si
no, manda por `fd_datos` el código (`MENSAJE_ESCRIBIR_MEMORIA`/`MENSAJE_LEER_MEMORIA`),
`dir_local` y `tamanio`; si es escritura, manda también el `buffer`. Espera la respuesta;
si no es `MENSAJE_OK`, devuelve `false`. Si es lectura y la respuesta fue OK, recibe los
`tamanio` bytes de vuelta hacia `buffer`. Es la unidad mínima de comunicación con **un
solo** stick — no sabe nada de direcciones globales, solo de direcciones locales al stick.
Callers: `operar_fisico` (línea 439) y `copiar_segmento_entre_sticks` (línea 518).

**`static bool operar_fisico(t_gestor_memoria* gm, bool escribir, uint32_t dir_fisica, uint32_t tamanio, void* buffer)`**
**Cómo funciona (multi-stick):** toma `gm->mutex` para toda la operación. Mientras queden
bytes por operar (`restante > 0`): busca el stick que contiene la dirección actual (`dir`);
si no hay ninguno (dirección fuera de rango), loguea error y aborta con `ok = false`. Si
hay, calcula `dir_local = dir - stick->base` y `disponible_en_stick = stick->tamanio -
dir_local`; el `tramo` de esta vuelta es el mínimo entre lo que falta (`restante`) y lo que
entra en el stick actual. Ejecuta `stick_operar` sobre ese tramo, avanza `dir`, el cursor
del buffer y `restante` en esa cantidad. El loop continúa hasta agotar `restante` o hasta
que una operación falle. Esto es exactamente la partición y consolidación de operaciones
multi-stick que describe el enunciado.
Usada por los dos wrappers públicos de abajo.

**`bool gestor_memoria_leer_fisico(...)`** / **`bool gestor_memoria_escribir_fisico(...)`**
Wrappers directos de `operar_fisico` con `escribir=false`/`true`. Callers:
`gestor_memoria_leer_fisico` se usa en `gestor_memoria_suspender_proceso` (línea 692, para
leer el segmento antes de mandarlo a SWAP) y en `gestor_memoria_io_rw` (línea 902, camino
normal de lectura lógica); `gestor_memoria_escribir_fisico` se usa en
`gestor_memoria_dessuspender_proceso` (línea 832, para restaurar el segmento) y en
`gestor_memoria_io_rw` (línea 901, camino normal de escritura lógica).

### Bloque: Traducción lógica → física

**`bool gestor_memoria_traducir(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, uint32_t* dir_fisica_out)`**
**Fórmula exacta (idéntica a la del enunciado y a la que usa la CPU en su propia MMU):**
`num_segmento = dir_logica / seg_max`, `desplazamiento = dir_logica % seg_max`, con
`seg_max = config->segment_max_size`. Bajo `gm->mutex`, busca en `gm->segmentos` el que
matchea `pid` y `id_segmento == num_segmento`. Si no lo encuentra, o si
`desplazamiento + tamanio > encontrado->tamanio` (el acceso se saldría del segmento — un
segmentation fault), devuelve `false` **sin loguear error** (comentario explícito: el
caller puede resolverlo contra SWAP, así que un fallo acá no es necesariamente un error
real todavía). Si es válido, calcula `*dir_fisica_out = encontrado->base +
desplazamiento`.
Caller: `gestor_memoria_io_rw` (línea 898) — primer intento, antes de probar contra SWAP.

### Bloque: Compactación

**`static void copiar_segmento_entre_sticks(t_gestor_memoria* gm, uint32_t dir_fisica, uint32_t tamanio, char* buffer, bool escribir)`**
Como `operar_fisico` pero sin tomar el mutex (asume que el caller ya lo tiene) y operando
directo contra un `buffer` en RAM local del proceso Kernel Memory (no contra otro stick).
Recorre en tramos igual que `operar_fisico`. Callers: `mover_segmento` (dos veces, líneas
555 y 557 — una lectura y una escritura).

**`static void actualizar_base_de_segmento_en_proceso(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t nueva_base, uint32_t tamanio)`**
Bajo `gestor_lock`/`unlock` (procesos), busca la entrada correspondiente en
`tabla_segmentos` del proceso y actualiza `base` y `limite`. Caller: `mover_segmento`
(línea 562).

**`static void mover_segmento(t_gestor_memoria* gm, t_segmento_fisico* seg, uint32_t nueva_base)`**
3 pasos comentados en el propio código: (1) lee el segmento completo desde su base actual
a un buffer temporal (`malloc(seg->tamanio)`); (2) lo reescribe en la nueva base (que puede
caer en un stick físico distinto al original); (3) actualiza la tabla del proceso dueño y
el registro físico (`seg->base = nueva_base`). El uso de un buffer intermedio (en vez de
copiar directo stick-a-stick) es lo que evita pisar datos si el rango origen y destino se
solaparan parcialmente. Caller: `gestor_memoria_compactar` (línea 583).

**`void gestor_memoria_compactar(t_gestor_memoria* gm)`**
Loguea `"## Inicio de compactación"` (log obligatorio). Bajo `gm->mutex`, obtiene los
segmentos ordenados por base y los recorre secuencialmente manteniendo
`proxima_base_libre` (arranca en 0): si la base actual del segmento no coincide con
`proxima_base_libre`, lo mueve ahí con `mover_segmento`; siempre avanza
`proxima_base_libre += seg->tamanio`. El resultado final es que todos los segmentos quedan
"amontonados" al principio de la memoria, sin huecos entre ellos, en el mismo orden
relativo que tenían por base. Libera el array, suelta el mutex, y **fuera** del lock
duerme `usleep(compaction_delay * 1000)` (el delay artificial no bloquea el mutex — otra
operación de memoria podría, en teoría, colarse durante ese sleep, aunque en la práctica el
KS ya tiene todas las CPUs desalojadas y no debería haber tráfico de creación de segmentos
concurrente en ese instante). Loguea `"## Fin de compactación"`.
Caller: `kernel_memory_atencion_scheduler.c:77`, solo tras recibir `MENSAJE_DESALOJO_OK`
del KS.

### Bloque: SWAP / mediano plazo

**`void gestor_memoria_registrar_swap(t_gestor_memoria* gm, int fd, uint32_t tamanio_total, uint32_t block_size)`**
Bajo `mutex_swap`: guarda `fd_swap`, `swap_block_size`, calcula
`swap_cant_bloques = tamanio_total / block_size` (con guarda `block_size > 0` para evitar
división por cero) y reserva el bitmap con `calloc` (todo en `false` = todos libres).
Loguea el log obligatorio de SWAP conectado. Caller:
`kernel_memory/src/conexiones/conexiones.c:134`.

**`static bool swap_escribir_bloque(...)`** / **`static bool swap_leer_bloque(...)`**
Protocolo mínimo contra el módulo SWAP: mandan código + número de bloque (+ el buffer
completo, si es escritura) y esperan `MENSAJE_OK` (más el buffer leído, si es lectura).
Deben llamarse con `mutex_swap` tomado. Usadas dentro de
`gestor_memoria_suspender_proceso`, `gestor_memoria_dessuspender_proceso` y `swap_io_rw`.

**`static uint32_t swap_bloques_libres(t_gestor_memoria* gm)`**
Cuenta ceros en el bitmap. Debe llamarse con `mutex_swap` tomado. Caller:
`gestor_memoria_suspender_proceso` (línea 679, para el pre-chequeo).

**`static t_segmento_swap* buscar_segmento_swap(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento)`**
Búsqueda lineal en `segmentos_swap` por `pid`+`id_segmento`. Debe llamarse con
`mutex_swap` tomado. Caller: `swap_io_rw` (línea 856).

**`bool gestor_memoria_suspender_proceso(t_gestor_memoria* gm, uint32_t pid)`**
**Mecanismo completo, todo-o-nada:**
1. Si no hay SWAP conectado (`fd_swap == -1`), falla de entrada.
2. Bajo `gm->mutex`, toma un *snapshot* (copias, no punteros originales) de todos los
   `t_segmento_fisico` del proceso y suma `bytes_totales`. Suelta el lock.
3. Bajo `mutex_swap`, calcula cuántos bloques necesita cada segmento
   (`ceil(tamanio / block_size)`, con la fórmula entera `(tam + block-1)/block`) y suma el
   total. Si `bloques_necesarios > swap_bloques_libres(gm)`, aborta **antes de mover un
   solo byte** — este es el "todo-o-nada" del pre-chequeo: nunca deja al proceso a medio
   suspender por falta de espacio.
4. Si hay bloques suficientes, procesa **un segmento a la vez**: lee el segmento completo
   de memoria física a un buffer (`gestor_memoria_leer_fisico`); bajo `mutex_swap`, arma un
   `t_segmento_swap` nuevo y va recorriendo el bitmap buscando bloques libres, copiando
   tramos del buffer a cada bloque (el último tramo, si es más chico que `block_size`, se
   rellena con ceros vía `memset`), escribiéndolo con `swap_escribir_bloque`, marcándolo
   usado y agregando su número a la lista `bloques` del segmento swap; agrega el
   `t_segmento_swap` completo a `gm->segmentos_swap`. Recién **después** de escribir todo
   ese segmento a SWAP, lo saca de `gm->segmentos` (memoria principal) bajo `gm->mutex` y
   de la `tabla_segmentos` del proceso bajo el lock del gestor de procesos — es decir, la
   memoria del segmento se libera solo cuando ya está seguro en SWAP, nunca antes.
5. Loguea el resultado y devuelve `true`.
Caller: `kernel_memory_atencion_scheduler.c:170` (`MENSAJE_SUSPENDER_PROCESO`), que a su
vez lo dispara `hilo_timer_suspension` del lado del KS tras `SUSPENSION_TIMEOUT` ms
bloqueado.

**`bool gestor_memoria_dessuspender_proceso(t_gestor_memoria* gm, uint32_t pid)`**
**Mecanismo, también todo-o-nada pero sin compactar:**
1. Bajo `mutex_swap`, arma la lista `swapeados` con todos los `t_segmento_swap*` del
   proceso (punteros directos, no copias esta vez). Si está vacía, no hay nada que
   restaurar — devuelve `false`.
2. Bajo `gm->mutex`, para cada segmento swapeado intenta `buscar_hueco`; si encuentra
   lugar, crea el `t_segmento_fisico` correspondiente, lo agrega a `gm->segmentos` **y**
   a una lista auxiliar `reservados` (para poder deshacer). Si en algún punto
   `buscar_hueco` falla, marca `entra_todo = false` y corta el loop (el `&&
   entra_todo` en la condición del `for`).
3. Si no entró todo: recorre `reservados` sacando cada uno de `gm->segmentos` con
   `list_remove_element` y liberándolo — revierte exactamente lo que había reservado hasta
   ese punto, dejando la memoria como estaba. Devuelve `false`. **Importante:** a
   diferencia de `gestor_memoria_crear_segmento`, acá un fallo de espacio **no** dispara
   compactación — el comentario del header lo aclara explícitamente ("sin disparar una
   compactación"); es el caller (el KS) quien decide si reintentar más tarde.
4. Si entró todo: para cada par (segmento swapeado, segmento físico reservado), lee sus
   bloques de SWAP en orden (`swap_leer_bloque`, armando el contenido completo en un
   buffer, bloque a bloque, liberando cada bloque leído en el bitmap a medida que se
   consume), escribe el contenido reconstruido en la nueva ubicación física
   (`gestor_memoria_escribir_fisico`), llama `tabla_proceso_agregar` para reflejarlo en la
   tabla del proceso, y destruye el `t_segmento_swap` ya consumido.
5. Loguea y devuelve `true`.
Caller: `kernel_memory_atencion_scheduler.c:181` (`MENSAJE_DESSUSPENDER_PROCESO`),
disparado por la lógica de mediano plazo del KS (`planificador_evento_memoria_liberada`)
cuando decide que hay espacio para reactivar a alguien de `SUSP_READY`.

### Bloque: Camino de IO (lectura/escritura por dirección lógica)

**`static bool swap_io_rw(t_gestor_memoria* gm, uint32_t pid, uint32_t num_segmento, uint32_t desplazamiento, uint32_t tamanio, void* buffer, bool escribir)`**
Bajo `mutex_swap`, busca el segmento swapeado; si no existe o el acceso se sale de su
tamaño, falla. Si es válido, recorre en tramos alineados a bloque: para cada tramo calcula
`idx_bloque = offset / block_size` y `off_local = offset % block_size`, siempre **lee el
bloque completo primero** (`swap_leer_bloque`) — incluso para una escritura parcial, porque
el protocolo con el módulo SWAP solo opera bloques completos — y si es escritura, modifica
solo la porción `off_local..off_local+tramo` del buffer del bloque en memoria y reescribe
el bloque entero (`swap_escribir_bloque`); si es lectura, simplemente copia esa porción al
buffer de salida. Es el patrón clásico "read-modify-write" de bloque, necesario porque el
tamaño de una operación de IO lógica no tiene por qué alinear con `swap_block_size`.
Caller: `gestor_memoria_io_rw` (línea 911).

**`bool gestor_memoria_io_rw(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, void* buffer, bool escribir, uint32_t* dir_fisica_out)`**
Primero intenta `gestor_memoria_traducir` (camino normal, memoria principal); si funciona,
ejecuta `gestor_memoria_leer_fisico`/`escribir_fisico` según corresponda y deja la
dirección física real en `*dir_fisica_out`. **Si la traducción falla** (el segmento no está
en memoria principal — puede estar en SWAP porque el proceso se suspendió mientras
esperaba esta misma IO), recalcula `num_segmento`/`desplazamiento` con la misma fórmula de
siempre y prueba `swap_io_rw`; si eso funciona, loguea que la operación fue "sobre segmento
... en SWAP" y deja `*dir_fisica_out = UINT32_MAX` como centinela (usado más arriba en
`atencion_scheduler` para decidir si loguear una dirección física real). Si ninguno de los
dos caminos funciona, devuelve `false`.
Callers: `kernel_memory_atencion_scheduler.c:121` (lectura, `STDOUT`) y `:150` (escritura,
`STDIN`).
**Para qué:** esta función es la que hace posible que una IO que empezó con el proceso en
memoria principal termine correctamente incluso si, mientras estaba bloqueado esperando esa
IO, el `hilo_timer_suspension` del KS decidió mandarlo a SWAP — sin esta doble ruta, esa
IO se perdería o fallaría.

### Bloque: Topología de sticks para las CPUs

**`static char* escribir_uint32(char* p, uint32_t valor)`**
`memcpy(p, &valor, 4)`, devuelve `p+4`. Helper de serialización de bajo nivel.

**`static char* escribir_string_con_longitud(char* p, const char* str)`**
Escribe un `uint32_t` con `strlen(str)` seguido de los bytes del string (sin `\0`).
Devuelve el puntero avanzado. Usado 2 veces por cada stick (para `ip` y `puerto`).

**`static uint32_t tamanio_topologia_serializada(t_gestor_memoria* gm, uint32_t* cant_out)`**
Primera pasada: recorre `gm->sticks`, cuenta solo los `conectado == true`, y suma
exactamente cuántos bytes va a ocupar la serialización completa (4 bytes de cabecera +
por cada stick conectado: 3×4 bytes de `id`/`tamanio`/`base` + 4+strlen(ip) +
4+strlen(puerto)). Deja el conteo de sticks conectados en `*cant_out`.

**`void* gestor_memoria_serializar_topologia(t_gestor_memoria* gm, uint32_t* tamanio_out)`**
Bajo `gm->mutex`: primera pasada con la función anterior para saber el tamaño exacto del
buffer a reservar (evita tener que hacer `realloc` dinámico o sobre-reservar); reserva el
buffer con ese tamaño exacto; segunda pasada, recorriendo de nuevo `gm->sticks` y
escribiendo (saltando los no conectados) cantidad + por cada stick: id, tamaño, base, ip
con longitud, puerto con longitud. El buffer devuelto es responsabilidad del caller
liberarlo con `free()`.
Caller: `kernel_memory/src/atencion_cpu/kernel_memory_atencion_cpu.c:86`
(`MENSAJE_TOPOLOGIA_STICKS`).
**Para qué:** es el mecanismo por el cual la CPU descubre dinámicamente todos los Memory
Sticks activos (con sus bases y tamaños) sin tener que conocerlos por configuración propia
— crucial para que el multi-stick funcione de forma transparente también del lado de la
CPU (`actualizar_topologia_sticks`, documentado en `01-cpu.md`).

---

## Hallazgos notables de este módulo

1. **`memoria_instrucciones.h` es código muerto completo** (ver arriba) — remanente de un
   mock de un checkpoint anterior, sin `.c` ni callers.
2. **`gestor_memoria_destroy` no tiene ningún caller** — verificado con grep, solo aparece
   su declaración (`gestor_memoria.h:87`) y su definición (`gestor_memoria.c:55`). `main.c`
   sí la invoca en la línea 62, así que en realidad **sí tiene un caller** — corrección: al
   revisar de nuevo el grep, `kernel_memory/src/main.c:62` aparece en los resultados. No es
   código muerto.
3. Pequeña inconsistencia de estilo, no funcional: `estrategia_es_best_fit` trata
   cualquier valor de `ALLOCATION_STRATEGY` que no sea literalmente `"WORST"` como Best
   Fit (en vez de validar explícitamente contra `"BEST"` y rechazar valores inválidos). No
   afecta la corrección con los valores que efectivamente usan los `.cfg` del repo (`BEST`
   o `WORST`), pero es menos estricto de lo que un enunciado más exigente podría pedir.
4. En `gestor_memoria_crear_segmento` hay una ventana de carrera teórica entre soltar
   `gm->mutex` y volver a medir `espacio_libre_sin_lock` fuera del lock — inofensiva en la
   práctica porque esa medición solo decide el código de retorno informativo
   (`SIN_ESPACIO` vs `NECESITA_COMPACTACION`), no una asignación real.
