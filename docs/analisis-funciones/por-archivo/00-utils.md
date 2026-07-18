# utils/ — documentación por archivo

Biblioteca estática compartida. No tiene `main`, no abre sockets propios, no corre hilos.
Los 7 ejecutables la enlazan en tiempo de compilación (`libutils.a` o equivalente vía Makefile)
para hablar el mismo protocolo y no repetir código de bajo nivel. Todo lo que sigue fue leído
directo del código actual, con callers verificados por `grep -rn` sobre los 7 módulos, no
asumidos.

## `utils/src/utils/sockets/sockets.c` (76 líneas) + `sockets.h` (7 líneas)

**Propósito:** wrapper mínimo de la API de sockets Berkeley (`getaddrinfo`/`socket`/`bind`/`listen`/`connect`) para que ningún módulo tenga que repetir el boilerplate de `addrinfo`.

### `int crear_servidor(const char* puerto)` — línea 8
- **Qué hace y cómo:** arma `hints` con `AF_INET`+`SOCK_STREAM`+`AI_PASSIVE`, resuelve con `getaddrinfo(NULL, puerto, ...)` (bind a todas las interfaces), crea el socket, setea `SO_REUSEPORT` (comentario en el código: "Evitar 'Address already in use' al reiniciar el servidor"), hace `bind` + `listen(fd, SOMAXCONN)`. Libera `server_info` con `freeaddrinfo` en todos los caminos, incluso los de error. Devuelve el fd de escucha o `-1` ante cualquier fallo intermedio.
- **Dónde se usa:** `memory_stick/src/conexiones/conexiones.c:24`, `kernel_memory/src/conexiones/conexiones.c:30`, `kernel_scheduler/src/conexiones/conexiones.c:25`. Exactamente los 3 módulos que actúan como servidor TCP puro (memory_stick, kernel_memory, kernel_scheduler). CPU, IO y SWAP nunca la llaman porque son puramente clientes.
- **Para qué:** punto único de arranque de un socket de escucha; evita que cada servidor repita `getaddrinfo`/`bind`/`listen` y sus 4 caminos de error distintos.

### `int crear_conexion(const char* ip, const char* puerto)` — línea 48
- **Qué hace y cómo:** mismo patrón que `crear_servidor` pero sin `AI_PASSIVE` y terminando en `connect()` en vez de `bind`+`listen`. Devuelve el fd conectado o `-1`.
- **Dónde se usa (9 call sites, es la función más invocada del repo):**
  - `memory_stick/src/conexiones/conexiones.c:156` (stick → KM, para informar su tamaño)
  - `kernel_scheduler/src/conexiones/conexiones.c:447` y `:496` (KS → KM, dos conexiones: control y notificaciones)
  - `swap/src/conexiones/conexiones.c:11` (swap → KM)
  - `kernel_memory/src/gestor_memoria/gestor_memoria.c:95` (KM → cada Memory Stick, canal de datos)
  - `cpu/src/conexiones/conexiones.c:13` y `:31` (CPU → KM, CPU → KS)
  - `cpu/src/conexiones/conexiones.c:220` (CPU → cada Memory Stick descubierto dinámicamente)
  - `io/src/conexiones/conexiones.c:12` (IO → KS)
- **Para qué:** todo módulo que actúa de cliente de otro la usa; es literalmente el borde de cada arista del grafo de comunicación del sistema.

**Dependencias:** `<stdlib.h>`, `<string.h>`, `<unistd.h>`, `<sys/socket.h>`, `<netdb.h>` — solo estándar de C/POSIX, cero acoplamiento con el resto del repo. `sockets.h` no declara structs, solo las dos firmas.

**Hallazgo:** ninguno — ambas funciones se usan, con buen manejo de errores (libera recursos en cada `return -1`).

---

## `utils/src/utils/boot_loader/boot_loader.c` (71 líneas) + `boot_loader.h` (32 líneas)

**Propósito declarado (según el Doxygen del header):** "Inicializa el módulo, el logger y el config" — una rutina de arranque común pensada para que los 6 `main.c` no dupliquen la secuencia boot_logger→config→validación→logger principal.

### `bool validate_config_rules(t_config* config, char** required_keys, int num_keys, t_log* boot_logger)` — línea 6
- **Qué hace y cómo:** recorre `required_keys[0..num_keys)` y por cada una llama `config_has_property`; si falta alguna, loggea error con `log_error` y marca `all_valid = false`, pero sigue recorriendo el resto (no corta en el primer faltante) para reportar todas las claves ausentes de una sola pasada.
- **Dónde se usa:** solo internamente en `boot_loader.c:38`, dentro de `system_boot`.
- **Para qué:** dar feedback completo (no "una por una") si falta configuración obligatoria.

### `bool system_boot(const char* module_name, const char* config_path, char** required_keys, int num_keys, t_config** out_config, t_log** out_logger)` — línea 17
- **Qué hace y cómo:** secuencia de 5 pasos documentada en comentarios propios: (1) crea un `boot_logger` transitorio (`boot.log`, nivel INFO); (2) `config_create(config_path)`; (3) si `num_keys > 0`, valida con `validate_config_rules`, aborta si falla; (4) crea el logger principal del módulo con `log_create("<module_name>.log", ..., LOG_LEVEL_DEBUG)`; (5) destruye el `boot_logger` transitorio y devuelve `config`/`logger` por los punteros de salida `out_config`/`out_logger`. Libera recursos correctamente en cada camino de error (config, boot_logger).
- **Dónde se usa:** **en ningún lado.** Verificado con `grep -rn "system_boot(" --include="*.c" .` sobre los 7 módulos: el único match es la propia definición en `boot_loader.c:17`. Ningún `main.c` la invoca.
- **Para qué (en teoría):** ser el punto único de arranque compartido por los 6 ejecutables.

**Hallazgo — corrige la documentación previa del equipo:** `docs/analisis-funciones/00-utils.md` (la versión por tema, escrita antes que este archivo) afirma que `system_boot` es la "rutina de arranque común a los 6 `main.c`". **Eso es falso.** Se comprobó con `grep -rln "boot_loader.h"` que **ningún archivo del repo aparte de `boot_loader.c` incluye `boot_loader.h`** — ni un solo `main.c` lo usa. En su lugar, cada módulo reimplementa su propia secuencia de arranque a mano dentro de `main.c`, llamando a su propio `<modulo>_config_create(argv[1], boot_logger)` (por ejemplo `cpu_config_create` en `cpu/src/main.c:30`, `kernel_scheduler_config_create` en `kernel_scheduler/src/main.c:31`, y así con los 6). El patrón conceptual (boot_logger transitorio → config → logger principal → destruir boot_logger) sí se repite en los 6 `main.c`, pero está **duplicado a mano en cada uno**, no factorizado en `system_boot`. `boot_loader.c` completo (71 líneas, dos funciones) es código muerto — escrito, nunca integrado.

**Dependencias:** `<commons/log.h>`, `<commons/config.h>` (Commons de la cátedra), `<stdbool.h>`.

---

## `utils/src/utils/protocolo.c` (49 líneas) + `protocolo.h` (81 líneas)

**Propósito:** el contrato de protocolo en sí — códigos de handshake, códigos de mensaje entre módulos, códigos de syscall, y sus conversiones a string para logging.

### Constantes de handshake (`protocolo.h:8-14`)
Primer `int32` que cualquier cliente manda al conectarse a un servidor, para que el servidor sepa qué tipo de par tiene enfrente y a qué handler saltar:
- `HANDSHAKE_CPU` = 1
- `HANDSHAKE_IO` = 2
- `HANDSHAKE_SCHEDULER` = 3
- `HANDSHAKE_MEMORY_STICK` = 4
- `HANDSHAKE_SWAP` = 5
- `HANDSHAKE_SCHEDULER_NOTIF` = 6 — comentario propio: "Segunda conexión KS→KM, canal exclusivo de notificaciones KM→KS"
- `HANDSHAKE_KERNEL_MEMORY` = 7 — comentario propio: "KM como cliente de un Memory Stick (canal de datos)"

Notar que hay **7 handshakes para 6 tipos de proceso**: KS abre dos conexiones distintas hacia KM (una de control con `HANDSHAKE_SCHEDULER`, otra de solo-notificaciones con `HANDSHAKE_SCHEDULER_NOTIF`), y KM se identifica como `HANDSHAKE_KERNEL_MEMORY` cuando es *cliente* de un Memory Stick (rol invertido respecto de cuando KM es servidor para CPU/KS).

### `typedef enum t_codigo_mensaje` (`protocolo.h:20-48`) — 24 opcodes
Los primeros 14 (`MENSAJE_HANDSHAKE` … `MENSAJE_ACTUALIZAR_CONTEXTO`) son el protocolo base. Los siguientes 10 tienen comentarios inline que documentan dirección y payload — literalmente la especificación del wire protocol del checkpoint 3 y la entrega final, escrita en el propio header:
- `MENSAJE_CREAR_SEGMENTO` — KS→KM `{pid, id_segmento, tamanio}` → OK\|ERROR\|COMPACTACION
- `MENSAJE_ELIMINAR_SEGMENTO` — KS→KM `{pid, id_segmento}` → OK\|ERROR
- `MENSAJE_FINALIZAR_PROCESO` — KS→KM `{pid}` → OK
- `MENSAJE_COMPACTACION` — KM→KS (respuesta a CREAR_SEGMENTO: hace falta compactar)
- `MENSAJE_DESALOJO_OK` — KS→KM (todas las CPUs desalojadas, puede compactar)
- `MENSAJE_INFORMAR_TAMANIO` — Stick→KM `{tamanio, ip, puerto}`
- `MENSAJE_TOPOLOGIA_STICKS` — CPU→KM (pedido) / KM→CPU `{cant, [id, tamanio, base, ip, puerto]...}`
- `MENSAJE_MEMORIA_CORRUPTA` — KM→KS, canal de notificaciones: stick desconectado → BSOD
- `MENSAJE_MAS_MEMORIA` — KM→KS, canal de notificaciones: `{nuevo_total}`, se amplió la memoria
- `MENSAJE_SUSPENDER_PROCESO` / `MENSAJE_DESSUSPENDER_PROCESO` — KS→KM `{pid}`, mover a/desde SWAP

Comentarios propios del archivo marcan explícitamente los cortes por entrega: `// --- CP3: opcodes nuevos (agregados al final, sin reordenar) ---` y `// --- Entrega final: mediano plazo + SWAP ---` — evidencia de que el enum se fue extendiendo por checkpoint sin renumerar (buena práctica: mantiene compatibilidad binaria de una entrega a otra si algo quedó serializado).

### `typedef enum t_codigo_syscall` (`protocolo.h:54-67`) — 11 valores
`SYSCALL_SLEEP, STDIN, STDOUT, MUTEX_CREATE, MUTEX_LOCK, MUTEX_UNLOCK, INIT_PROC, EXIT, MEM_ALLOC, MEM_FREE` (10 de la base) + `SYSCALL_SEG_FAULT` agregada en CP3 con comentario "La MMU detectó un acceso inválido: el proceso debe finalizar con motivo SEG_FAULT". Coincide 1:1 con las 10 syscalls + seg fault que pide el enunciado oficial del TP.

### `const char* codigo_mensaje_to_string(t_codigo_mensaje codigo)` — `protocolo.c:3`
- **Qué hace:** `switch` exhaustivo de los 24 opcodes a su nombre string, `default: "DESCONOCIDO"`.
- **Dónde se usa:** **en ningún lado.** `grep -rn "codigo_mensaje_to_string(" --include="*.c" --include="*.h" .` solo devuelve la declaración (`protocolo.h:73`) y la definición (`protocolo.c:3`). Cero callers reales en los 6 módulos.
- **Para qué (en teoría):** loguear de forma legible qué opcode se mandó/recibió — pero nadie lo invoca; los módulos loguean directamente con sus propios strings ad-hoc en cada `log_info`.

### `const char* codigo_syscall_to_string(t_codigo_syscall codigo)` — `protocolo.c:34`
- **Qué hace:** mismo patrón, switch de 11 casos.
- **Dónde se usa:** `kernel_scheduler/src/conexiones/conexiones.c:203` y `cpu/src/conexiones/conexiones.c:61` — ambos en la misma línea de log: `"## (%u) - Solicitó syscall: %s"`. Esta sí se usa, y en el punto exacto donde la cátedra pide loguear la syscall solicitada.
- **Para qué:** nombre legible de la syscall en el log obligatorio "Solicitó syscall".

**Hallazgo:** `codigo_mensaje_to_string` es código muerto — la única de las dos funciones "to_string" de este archivo que no se usa. `codigo_syscall_to_string` sí se usa y en el lugar correcto.

**Dependencias:** `protocolo.c` solo incluye su propio header (`<utils/protocolo.h>`); `protocolo.h` no depende de nada del repo, es autocontenido (no incluye `<stdbool.h>` ni structs de otros archivos).

---

## `utils/src/utils/instrucciones.c` (39 líneas) + `instrucciones.h` (54 líneas)

**Propósito:** el ISA (set de instrucciones) que ejecuta un proceso de usuario — el enum de opcodes, la struct de instrucción parseada, y sus conversiones a string.

### `typedef enum t_codigo_instruccion` (`instrucciones.h:11-32`) — 20 valores
`INST_NOOP, SET, MOV_IN, MOV_OUT, SUM, SUB, JNZ, COPY_MEM, SLEEP, WAIT, SIGNAL, MUTEX_CREATE, MUTEX_LOCK, MUTEX_UNLOCK, MEM_ALLOC, MEM_FREE, IO_STDIN, IO_STDOUT, INIT_PROC, EXIT`.

El enunciado oficial del TP (verificado contra el Google Doc real de la cátedra) pide exactamente 18 instrucciones y **no incluye `WAIT`/`SIGNAL` en ningún lado** (este cuatrimestre no tiene semáforos, solo mutex). `INST_WAIT` e `INST_SIGNAL` están declaradas acá pero, como se detalla más abajo, sin `case` de ejecución en `cpu_execute` (`cpu/src/cpu/cpu.c`) ni en `cod_instruccion_to_string`. Es decir: **20 valores en el enum, pero solo 18 tienen camino de ejecución y de logging** — `WAIT`/`SIGNAL` son remanente, probablemente copiado de un enunciado de otro cuatrimestre que sí pedía semáforos.

### `typedef struct t_instruccion` (`instrucciones.h:38-42`)
- `t_codigo_instruccion codigo` — qué instrucción es.
- `char parametros[INSTRUCCION_MAX_PARAMS][INSTRUCCION_PARAM_LEN]` — matriz de hasta 3 parámetros de hasta 64 caracteres cada uno (constantes `INSTRUCCION_MAX_PARAMS=3`, `INSTRUCCION_PARAM_LEN=64` definidas en `instrucciones.h:4-5`), todos representados como texto (sin parsear a entero acá — el parseo numérico lo hace quien consume la instrucción, típicamente `cpu.c`/`instrucciones.c` de CPU).
- `int cantidad_parametros` — cuántos de los 3 slots están realmente ocupados.

Esta es la instrucción ya decodificada, tal como la arma la etapa *Decode* del ciclo de instrucción de la CPU antes de pasarla a *Execute*.

### `const char* cod_instruccion_to_string(t_codigo_instruccion codigo)` — `instrucciones.c:6`
- **Qué hace:** switch de 18 casos (nota: cubre las 18 del enunciado, **no** incluye `INST_WAIT` ni `INST_SIGNAL` — coherente con que esas dos no tienen soporte real), `default: "UNKNOWN"`.
- **Dónde se usa:** `cpu/src/instrucciones/instrucciones.c:141` y `cpu/src/cpu/cpu.c:99`, ambos en logs obligatorios de ejecución/decode de instrucción (formato `"## PID: %u - Ejecutando: %s - %s"` / `"## PID: %u - DECODE completado - Instrucción: %s - Parámetros: %s"`).
- **Para qué:** nombre legible de la instrucción para los logs mínimos que pide la cátedra.

### `char* parametros_to_string(t_instruccion* inst)` — `instrucciones.c:30`
- **Qué hace y cómo:** si `cantidad_parametros == 0` devuelve `""` directo. Si no, usa un **buffer `static char buffer[256]`** (no hay `malloc`, no hay `free` que hacer, pero tampoco es reentrante ni thread-safe) y concatena los parámetros con `strcat`, separados por un espacio.
- **Dónde se usa:** los mismos dos call sites que `cod_instruccion_to_string` (van siempre juntas, en el mismo log).
- **Para qué:** mostrar los parámetros de la instrucción actual en el log.
- **Nota de diseño (no un bug, pero vale mencionarlo):** al ser `static`, el buffer se comparte entre llamadas — si en algún momento se invocara dos veces antes de imprimir el resultado de la primera (por ejemplo, pasando el resultado como argumento junto con otra llamada a esta misma función en la misma expresión), el segundo `strcat` pisaría el contenido del primero. En el uso real actual esto no pasa porque cada log hace una sola llamada por vez, pero es un patrón frágil si se reutilizara la función en otro contexto. Tampoco es thread-safe: si dos hilos (por ejemplo, dos módulos CPU corriendo en el mismo proceso — aunque en este repo cada CPU es un binario separado, así que no aplica en la práctica) llamaran a la función concurrentemente, se pisarían el buffer.

**Dependencias:** `<string.h>` (`strcat`), `<stdio.h>` (declarado pero no usado en este archivo — posible import de más).

**Hallazgo:** `INST_WAIT`/`INST_SIGNAL` no piden lógica adicional según el enunciado vigente — no es una falta, es un remanente inofensivo (ver arriba).

---

## `utils/src/utils/estados.h` (34 líneas)

**Propósito:** el enum de estados de proceso — la única fuente de verdad de la máquina de estados del sistema — y su conversión a string, **inline en el header** (no hay `estados.c`).

### `typedef enum t_estado_proceso` (líneas 8-16)
`ESTADO_NEW, ESTADO_READY, ESTADO_EXEC, ESTADO_BLOCK, ESTADO_SUSP_BLOCK, ESTADO_SUSP_READY, ESTADO_EXIT` — 7 estados, exactamente el "modelo de 7 estados" que pide el enunciado oficial. El comentario del archivo explica el prefijo `ESTADO_`: "para evitar colisión con macros estándar (EXIT)" — es decir, sin el prefijo `ESTADO_EXIT` colisionaría con la macro `EXIT` de `<stdlib.h>` (o similar) si alguna vez se incluyera junto a este header.

### `static inline const char* estado_to_string(t_estado_proceso estado)` — línea 21
- **Qué hace:** switch de 7 casos a string. Nota los strings con punto: `"SUSP. BLOCK"` y `"SUSP. READY"` (no `"SUSP_BLOCK"`) — formato pedido explícitamente por la cátedra para los logs de cambio de estado.
- **Por qué es `static inline` en el header y no una función normal declarada+definida:** al estar en un `.h` sin `.c` correspondiente, cada archivo que incluye `estados.h` obtiene su propia copia de la función (con enlazado interno gracias a `static`), evitando symbol clashes de múltiples definiciones sin necesitar un `estados.c` separado para una función tan chica. Es una técnica común para headers "solo función utilitaria" en C.
- **Dónde se usa:** **exclusivamente en `kernel_scheduler/src/planificador/planificador.c`**, en 9 líneas distintas (423, 448, 478, 491, 552, 590, 633, 670, 775), todas dentro de los logs obligatorios `"## (%u) Pasa del estado %s al estado %s"` que exige la cátedra. Ningún otro módulo la usa — coherente con que KS es el único dueño de la máquina de estados (CPU y KM solo conocen el PID y su contexto, no gestionan transiciones de estado).
- **Para qué:** cumplir el formato de log obligatorio de cambio de estado.

**Hallazgo:** ninguno — coincide exactamente con el formato de log que pide el enunciado (`"## (<PID>) Pasa del estado <ESTADO_ANTERIOR> al estado <ESTADO_ACTUAL>"`, verificado contra el Google Doc oficial).

---

## `utils/src/utils/registros.h` (27 líneas)

**Propósito:** la struct de registros de CPU, compartida en el sentido literal — es el mismo tipo que usa la CPU para ejecutar y que Kernel Memory usa para persistir el contexto de un proceso entre ráfagas.

### `typedef struct t_registros_cpu` (líneas 10-25)
11 campos, exactamente los que pide el enunciado (tabla de registros del TP), verificados campo por campo:
- `uint32_t pc` — Program Counter, 32 bits.
- `uint8_t ax, bx, cx, dx` — 4 registros de propósito general de 8 bits.
- `uint32_t eax, ebx, ecx, edx` — 4 registros de propósito general de 32 bits (versión "extendida" de los anteriores, pero **no** son la misma memoria reinterpretada — son campos separados en la struct, a diferencia de la arquitectura x86 real donde AX es la mitad baja de EAX; acá se tratan como registros completamente independientes).
- `uint32_t si, di` — Source Index / Destination Index, 32 bits, usados típicamente como registros de dirección para `MOV_IN`/`MOV_OUT`/`COPY_MEM`.

**Quién la llena/lee:** `cpu.c` (CPU) los usa directamente durante `cpu_execute` para correr las instrucciones `SET/SUM/SUB/JNZ` sobre los de 8 bits y `MOV_IN/MOV_OUT/COPY_MEM` sobre los de 32 (como offset/tamaño); `kernel_memory` guarda una copia de esta struct por proceso como parte de su contexto (`t_proceso_km` en `gestor_procesos.h`), y se la manda a la CPU cuando el KS despacha un PID (mensaje `MENSAJE_CONTEXTO`) y la CPU se la devuelve actualizada cuando el proceso se bloquea/finaliza/es desalojado.

**Dependencias:** solo `<stdint.h>`.

**Hallazgo:** ninguno — coincide exactamente con la tabla de registros del enunciado oficial (11 registros, mismos nombres, mismos tamaños en bits).

---

## `utils/src/utils/segmento.h` (13 líneas)

**Propósito:** la unidad de segmentación pura — el modelo más pequeño y más reutilizado del repo.

### `typedef struct t_segmento` (líneas 6-11)
- `uint32_t id_segmento` — identificador único del segmento dentro del proceso (no global).
- `uint32_t base` — dirección física de inicio del segmento.
- `uint32_t limite` — tamaño del segmento (a pesar del nombre "límite", en el código se usa consistentemente como el tamaño, no como `base + tamanio`; hay que sumar `base + limite` para obtener el fin del rango válido).
- `uint32_t tamanio` — también el tamaño del segmento. **Nota:** `limite` y `tamanio` son, en la práctica, el mismo valor en la mayoría de los usos del repo (redundancia del modelo, no dos conceptos distintos) — no se verificó un caso donde difieran; probablemente quedó `limite` de una primera versión y `tamanio` se agregó después como nombre más claro, sin eliminar el campo viejo.

**Quién la usa:** es el tipo elemental de la tabla de segmentos por proceso que administra `kernel_memory/src/gestor_memoria/gestor_memoria.c` (crear/eliminar/buscar hueco/compactar), y el que recorre la MMU de la CPU (`cpu/src/instrucciones/instrucciones.c`, función `mmu_traducir`) para traducir direcciones lógicas a físicas. También viaja serializado dentro de `MENSAJE_TOPOLOGIA_STICKS` y en la tabla de segmentos que KM le manda a la CPU.

**Dependencias:** solo `<stdint.h>`.

**Hallazgo:** el campo `limite` es, en la práctica, redundante con `tamanio` — no es un bug funcional (el código es consistente en cuál usa dónde) pero sí un olor de diseño: dos campos para el mismo dato.

---

## Resumen de hallazgos de este archivo (utils/)

1. **`boot_loader.c` es código muerto en su totalidad** (71 líneas, 2 funciones) — ningún `main.c` lo incluye ni lo llama. Esto **corrige** la documentación previa del equipo, que describía `system_boot` como "la rutina de arranque común a los 6 `main.c`". El patrón conceptual sí se repite en los 6 mains, pero copiado a mano, no factorizado acá.
2. `codigo_mensaje_to_string()` (`protocolo.c`) — declarada, nunca llamada (ya reportado antes, reconfirmado archivo por archivo).
3. `INST_WAIT`/`INST_SIGNAL` en `instrucciones.h` — no forman parte del enunciado vigente (que no tiene semáforos), sin `case` en ningún lado del repo; remanente inofensivo.
4. `t_segmento.limite` vs `t_segmento.tamanio` — campos redundantes, no es un bug pero es duplicación de dato.
