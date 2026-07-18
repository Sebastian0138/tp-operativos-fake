# Módulo utils — Análisis detallado

`utils/` es una biblioteca estática (`libutils.a`) que compilan y linkean los seis módulos
ejecutables (`cpu`, `io`, `kernel_memory`, `kernel_scheduler`, `memory_stick`, `swap`). No es un
proceso en sí mismo: no tiene `main()`, no abre sockets propios, no tiene hilo de ejecución. Es el
"idioma común" y las herramientas de bajo nivel que todos comparten. Provee tres cosas: (1) helpers
de sockets TCP, (2) una rutina estándar de arranque (config + logger), y (3) el contrato de
protocolo — los enums de mensajes/syscalls/instrucciones y los structs que viajan por los sockets.

---

### `sockets/sockets.c` — helpers de TCP crudo

Envuelve la secuencia estándar de Berkeley sockets (`getaddrinfo`/`socket`/`bind`/`listen` y
`getaddrinfo`/`socket`/`connect`) para que ningún módulo tenga que repetir ese boilerplate.

#### `int crear_servidor(const char* puerto)` — sockets.c:8
- **Qué hace:** crea un socket de escucha TCP en el puerto dado (bind + listen) y devuelve su file
  descriptor, o `-1` si falla cualquier paso.
- **Dónde se usa:** `kernel_scheduler/src/conexiones/conexiones.c:25` (servidor del KS, puerto de
  CPU/IO), `kernel_memory/src/conexiones/conexiones.c:30` (servidor del KM),
  `memory_stick/src/conexiones/conexiones.c:24` (servidor del stick). Es decir, todo módulo que
  actúa como **servidor** en algún canal (KS, KM, memory_stick) lo usa una vez al arrancar.
- **Para qué:** cada módulo servidor necesita el mismo ritual de bind/listen; centralizarlo evita
  duplicar manejo de errores de sockets en 3 módulos distintos.
- **Cómo funciona:** arma un `hints` con `AF_INET`/`SOCK_STREAM`/`AI_PASSIVE`, resuelve con
  `getaddrinfo(NULL, puerto, ...)` (NULL = "cualquier interfaz local"), crea el socket, setea
  `SO_REUSEPORT` (para poder reiniciar el proceso sin esperar el timeout de `TIME_WAIT` del puerto),
  hace `bind` y `listen(fd, SOMAXCONN)` (cola de conexiones pendientes al máximo permitido por el
  SO). Libera `server_info` con `freeaddrinfo` en todos los caminos, incluidos los de error.

#### `int crear_conexion(const char* ip, const char* puerto)` — sockets.c:48
- **Qué hace:** abre una conexión TCP cliente hacia `ip:puerto` y devuelve el fd conectado, o `-1`
  si falla.
- **Dónde se usa:** es la función de sockets más usada del repo — cualquier módulo que actúa como
  **cliente** de otro la llama: `kernel_scheduler/conexiones.c:447,496` (KS→KM, canal de comandos y
  canal de notificaciones), `swap/conexiones.c:11` (swap se conecta a KM... en realidad es KM quien
  se conecta al swap — ver nota abajo), `cpu/conexiones.c:13,31,220` (CPU→KS, CPU→KM, CPU→stick),
  `memory_stick/conexiones.c:156` (stick→KM para informar su tamaño), `io/conexiones.c:12` (IO→KS),
  `kernel_memory/gestor_memoria.c:95` (KM→stick, canal de datos físicos).
- **Para qué:** todo el sistema es un grafo de conexiones TCP cliente-servidor; esta función es el
  único punto donde se abre el lado cliente de cualquiera de esas conexiones.
- **Cómo funciona:** resuelve `ip:puerto` con `getaddrinfo` (sin `AI_PASSIVE`, porque es para
  conectarse a una IP concreta, no para escuchar), crea el socket y hace `connect`. No reintenta:
  si el destino no está levantado todavía, devuelve `-1` y el que llama decide qué hacer (algunos
  módulos reintentan en un loop, otros abortan el arranque).

---

### `boot_loader/boot_loader.c` — arranque estándar, pero código muerto

> **Corrección (verificada con `grep -rln "boot_loader.h"` sobre los 7 módulos):** ningún archivo
> del repo aparte de `boot_loader.c` incluye `boot_loader.h`, y `grep -rn "system_boot("` solo
> encuentra la propia definición. **`boot_loader.c` completo (71 líneas, 2 funciones) es código
> muerto — nunca se integró.** Lo que sigue describe qué hace el archivo, no un flujo real del
> sistema. Cada `main.c` reimplementa a mano su propia secuencia de arranque (boot_logger
> transitorio → `<módulo>_config_create` → logger principal → destruir boot_logger) — el patrón
> conceptual se repite en los 6, pero copiado, no factorizado acá. Ver detalle en
> [por-archivo/00-utils.md](por-archivo/00-utils.md).

#### `bool validate_config_rules(t_config* config, char** required_keys, int num_keys, t_log* boot_logger)` — boot_loader.c:6
- **Qué hace:** recorre `required_keys` y verifica con `config_has_property` (de la librería
  `commons` de la cátedra) que cada clave exista en el `.cfg` cargado. Loguea con `log_error` cada
  clave faltante y devuelve `false` si falta alguna.
- **Dónde se usa:** solo internamente, desde `system_boot` (boot_loader.c:38) — y `system_boot` a
  su vez no la llama nadie. Ambas funciones son código muerto.
- **Para qué (diseño, no en uso):** falla rápido y con mensaje claro si el `.cfg` de un módulo está
  incompleto, en vez de que el proceso reviente más adelante con un `config_get_string_value` que
  devuelve `NULL`.
- **Cómo funciona:** loop simple `for` sobre `required_keys[0..num_keys)`, sin early-return —
  intencionalmente seguimos chequeando todas las claves para reportarlas todas de una, no solo la
  primera que falta.

#### `bool system_boot(const char* module_name, const char* config_path, char** required_keys, int num_keys, t_config** out_config, t_log** out_logger)` — boot_loader.c:17
- **Qué hace:** rutina de arranque completa de un módulo: crea un logger temporal de boot
  (`boot.log`), carga el `.cfg` del `config_path`, valida las claves requeridas contra ese logger de
  boot, crea el logger definitivo del módulo (`<module_name>.log`, nivel `DEBUG`), y devuelve config
  y logger por los parámetros de salida (`out_config`, `out_logger`). Devuelve `false` ante
  cualquier fallo (logger de boot, config inexistente, validación, logger principal).
- **Dónde se usa: en ningún lado.** No la llama ningún `main.c` — cada módulo tiene su propio
  `<modulo>_config_create` (p. ej. `cpu_config_create` en `cpu/src/main.c:30`,
  `kernel_scheduler_config_create` en `kernel_scheduler/src/main.c:31`, y así con los 6) que hace lo
  mismo a mano, sin pasar por acá.
- **Para qué (diseño, no en uso):** en teoría sería el punto de entrada común de todo el sistema,
  garantizando que ningún módulo siga ejecutando con un config incompleto o sin logger funcionando
  — pero en la práctica cada módulo resuelve esto por su cuenta.
- **Cómo funciona:** usa un **logger de boot** (`t_log` separado, `LOG_LEVEL_INFO`, siempre en
  archivo `boot.log`) exclusivamente para las 5 etapas del arranque en sí; una vez que el módulo
  tiene su logger propio (`<module_name>.log`) listo, destruye el logger de boot (`log_destroy`) —
  de ahí en más el módulo loguea con el suyo. Cada rama de error libera lo que ya se había creado
  (config, boot_logger) antes de devolver `false`, evitando leaks incluso en los caminos de falla.
  Nota: `num_keys == 0` es un caso válido (sin claves obligatorias) que se loguea distinto pero no
  es un error.

---

### `protocolo.c` — nombres legibles de los códigos del protocolo

`protocolo.h` define los tres enums que son el contrato de comunicación entre TODOS los módulos
(ver sección de structs/enums más abajo). `protocolo.c` sólo aporta las funciones de logging para
esos códigos.

#### `const char* codigo_mensaje_to_string(t_codigo_mensaje codigo)` — protocolo.c:3
- **Qué hace:** switch exhaustivo que traduce cada valor de `t_codigo_mensaje` (24 opcodes) a su
  nombre en texto (p. ej. `MENSAJE_CREAR_SEGMENTO` → `"CREAR_SEGMENTO"`); `default` devuelve
  `"DESCONOCIDO"`.
- **Dónde se usa:** **en ningún lado del código actual.** Grep completo del repo (excluyendo
  `obj/`/`bin/`) no encuentra ningún llamador fuera de su propia declaración/definición. Es función
  muerta hoy: está pensada para loguear el nombre del opcode de mensaje recibido/enviado, pero
  ningún módulo la invoca — todos loguean directamente con literales de texto o con
  `codigo_syscall_to_string` (que sí se usa).
- **Para qué (intención de diseño):** dar un nombre legible a los 24 valores de `t_codigo_mensaje`
  para los logs de bajo nivel del protocolo (equivalente a lo que sí se hace con syscalls e
  instrucciones).
- **Cómo funciona:** switch plano de constante a string literal, sin estado ni efectos secundarios.
- **Nota:** vale la pena señalarlo al equipo — o se empieza a usar en los puntos donde se loguea la
  llegada/envío de un `t_codigo_mensaje` crudo, o es candidato a eliminar si nunca se necesitó.

#### `const char* codigo_syscall_to_string(t_codigo_syscall codigo)` — protocolo.c:34
- **Qué hace:** switch exhaustivo que traduce cada valor de `t_codigo_syscall` (11 valores) a texto
  (p. ej. `SYSCALL_MEM_ALLOC` → `"MEM_ALLOC"`).
- **Dónde se usa:** `kernel_scheduler/src/conexiones/conexiones.c:203` y
  `cpu/src/conexiones/conexiones.c:61`, en ambos casos dentro de un `log_info` con el formato
  obligatorio de la cátedra: `"## (PID) - Solicitó syscall: %s"`. Es decir, se usa exactamente en
  los dos lados de la misma syscall: cuando la CPU la envía y cuando el KS la recibe.
  documentar `<log obligatorio>`.
- **Para qué:** cumplir el formato de log exigido por la consigna cada vez que una CPU solicita una
  syscall al kernel.
- **Cómo funciona:** switch plano igual que su par de mensajes.

---

### `instrucciones.c` — nombres legibles y serialización de instrucciones de proceso

Complementa `instrucciones.h` (que define `t_codigo_instruccion` y `t_instruccion`, ver abajo) con
utilidades de logging usadas por el ciclo de instrucción de la CPU.

#### `const char* cod_instruccion_to_string(t_codigo_instruccion codigo)` — instrucciones.c:6
- **Qué hace:** switch de los 19 valores de `t_codigo_instruccion` a su nombre en texto (p. ej.
  `INST_COPY_MEM` → `"COPY_MEM"`). Nota: `INST_WAIT` y `INST_SIGNAL` están declarados en el enum
  (`instrucciones.h:21-22`) pero **no tienen `case` en este switch** — si alguna vez se decodifica
  una instrucción con ese código, cae en `default` y loguea `"UNKNOWN"` en vez de `"WAIT"`/`"SIGNAL"`.
  Grepeando el repo, `INST_WAIT`/`INST_SIGNAL` tampoco aparecen manejados en
  `cpu/src/instrucciones/instrucciones.c` (`cpu_execute`), así que son códigos declarados pero sin
  implementación de ejecución ni de logging todavía.
- **Dónde se usa:** `cpu/src/instrucciones/instrucciones.c:141` (log de "## PID: X - Ejecutando: ...")
  y `cpu/src/cpu/cpu.c:99` (log de "## PID: X - DECODE completado..."). Ambos son logs obligatorios
  de la cátedra que se emiten en cada ciclo de instrucción de la CPU.
- **Para qué:** loguear qué instrucción está ejecutando cada proceso, en el formato pedido por la
  consigna.
- **Cómo funciona:** switch plano de constante a string literal.

#### `char* parametros_to_string(t_instruccion* inst)` — instrucciones.c:30
- **Qué hace:** concatena los `cantidad_parametros` strings de `inst->parametros[]` separados por
  un espacio, en un buffer estático de 256 bytes, y devuelve un puntero a ese buffer.
- **Dónde se usa:** mismos dos call-sites que `cod_instruccion_to_string` (`cpu/instrucciones.c:141`
  y `cpu/cpu.c:99`) — siempre van juntas en el mismo log, para mostrar "instrucción + sus
  parámetros" en una sola línea.
- **Para qué:** los logs de ejecución necesitan mostrar los parámetros de la instrucción (p. ej.
  `SUM AX BX`) como un solo string, no como un array.
- **Cómo funciona:** si `cantidad_parametros == 0` devuelve `""` directamente. Si no, usa un
  `static char buffer[256]` (compartido entre llamadas: **no es reentrante ni thread-safe** — si dos
  hilos de CPU llamaran a esta función "simultáneamente" el buffer se pisaría; en la práctica no es
  un problema porque cada proceso CPU es un ejecutable propio con un solo hilo de ejecución de
  instrucciones). Concatena con `strcat` en un loop, agregando `" "` entre parámetros salvo antes
  del primero.

---

### `estados.h` — nombre legible de estado (única función en un header)

#### `static inline const char* estado_to_string(t_estado_proceso estado)` — estados.h:21
- **Qué hace:** switch de los 7 estados de `t_estado_proceso` a texto (p. ej. `ESTADO_SUSP_BLOCK` →
  `"SUSP. BLOCK"`, con el punto y espacio tal cual lo pide el formato de log de la cátedra).
- **Dónde se usa:** exclusivamente en `kernel_scheduler/src/planificador/planificador.c`, en las 9
  transiciones de estado del PCB (líneas 423, 448, 478, 491, 552, 590, 633, 670, 775) — cada
  transición loguea `"## (PID) Pasa del estado <anterior> al estado <nuevo>"`, el log obligatorio
  de cambio de estado.
- **Para qué:** el kernel_scheduler es el único módulo que maneja la máquina de estados del proceso
  (NEW/READY/EXEC/BLOCK/SUSP_BLOCK/SUSP_READY/EXIT), por eso es el único que necesita nombrar esos
  estados en logs.
- **Cómo funciona:** switch plano. Está declarada `static inline` directamente en el header (no hay
  un `estados.c`) — es decir, cada `.c` que incluye `estados.h` obtiene su propia copia de la
  función inlineada por el compilador; es una función tan chica que no vale la pena un `.c` aparte
  ni generar un símbolo externo.

---

### Qué NO tiene funciones (structs y enums — el contrato compartido)

Estos headers no ejecutan lógica: son el **vocabulario común** que todos los módulos serializan
"campo a campo" a mano al mandar/recibir por socket (no hay una capa de paquetes/serialización
genérica en este protocolo — cada `send`/`recv` empaqueta a mano los campos en el orden acordado).

#### `t_codigo_mensaje` (protocolo.h) — qué mensaje viaja por el socket
Primer campo de **todo** mensaje entre módulos (después del handshake inicial). 24 valores, en 3
grupos:
- **Base (CP1/CP2):** `HANDSHAKE`, `ENVIAR_PID`, `PEDIR_INSTRUCCION`, `DEVOLVER_INSTRUCCION`,
  `SYSCALL`, `FIN_IO`, `INTERRUPCION`, `CONTEXTO`, `OK`, `ERROR`, `LEER_MEMORIA`,
  `ESCRIBIR_MEMORIA`, `ESPACIO_LIBRE`, `ACTUALIZAR_CONTEXTO` — el ciclo de instrucción básico,
  IO y acceso a memoria.
- **CP3 (memoria real + multi-stick):** `CREAR_SEGMENTO`/`ELIMINAR_SEGMENTO`/`FINALIZAR_PROCESO`
  (KS→KM), `COMPACTACION`/`DESALOJO_OK` (ida y vuelta KM↔KS para compactar), `INFORMAR_TAMANIO`
  (Stick→KM), `TOPOLOGIA_STICKS` (KM↔CPU), `MEMORIA_CORRUPTA`/`MAS_MEMORIA` (notificaciones
  asíncronas KM→KS).
- **Entrega final (mediano plazo + SWAP):** `SUSPENDER_PROCESO`/`DESSUSPENDER_PROCESO` (KS→KM).

Los comentarios inline en el enum (protocolo.h:36-47) documentan emisor→receptor y el payload
esperado de cada uno — es la referencia más confiable de "quién manda qué a quién" en todo el repo.

#### `t_codigo_syscall` (protocolo.h) — qué syscall pidió un proceso
11 valores que la CPU envía al KS dentro de un mensaje `MENSAJE_SYSCALL` cuando el proceso que está
ejecutando llega a una instrucción que requiere intervención del kernel: `SLEEP`, `STDIN`,
`STDOUT`, `MUTEX_CREATE/LOCK/UNLOCK`, `INIT_PROC`, `EXIT`, `MEM_ALLOC`, `MEM_FREE`, `SEG_FAULT`
(este último no es una syscall que "pida" el proceso, sino que la dispara la MMU de la CPU cuando
detecta un acceso inválido).

#### `t_codigo_instruccion` / `t_instruccion` (instrucciones.h)
`t_codigo_instruccion`: 19 opcodes que puede tener una línea del archivo de instrucciones de un
proceso (`NOOP`, `SET`, `MOV_IN/OUT`, `SUM`, `SUB`, `JNZ`, `COPY_MEM`, `SLEEP`, `WAIT`, `SIGNAL`,
`MUTEX_CREATE/LOCK/UNLOCK`, `MEM_ALLOC/FREE`, `IO_STDIN/STDOUT`, `INIT_PROC`, `EXIT`). `WAIT` y
`SIGNAL` están definidos pero sin manejo en `cod_instruccion_to_string` ni en el `cpu_execute` de
la CPU (ver arriba) — parecen reservados para semáforos, no implementados aún.

`t_instruccion`: struct que arma la CPU en la etapa *Decode* — `codigo` (el opcode) +
`parametros[INSTRUCCION_MAX_PARAMS][INSTRUCCION_PARAM_LEN]` (hasta 3 parámetros de hasta 64 bytes
cada uno, como strings crudos que cada instrucción interpreta a su manera — p. ej. `SUM` los lee
como nombres de registro, `MEM_ALLOC` como números) + `cantidad_parametros`.

#### `t_estado_proceso` (estados.h)
7 estados de la máquina de estados del proceso: `NEW → READY → EXEC → {BLOCK, EXIT}`,
`BLOCK → {READY, SUSP_BLOCK}`, `SUSP_BLOCK → SUSP_READY → READY`. El prefijo `ESTADO_` es
deliberado para no colisionar con la macro `EXIT` de la libc (`estados.h:6`). Sólo lo usa
`kernel_scheduler`, que es dueño exclusivo de la máquina de estados — ningún otro módulo transiciona
procesos.

#### `t_registros_cpu` (registros.h)
Banco de registros de una CPU ejecutando un proceso: `pc` (Program Counter, 32 bits),
4 registros de propósito general de 8 bits (`ax/bx/cx/dx`), 4 de 32 bits (`eax/ebx/ecx/edx`), y
`si`/`di` (Source/Destination Index, para operaciones tipo `COPY_MEM`). Es el **contexto de
ejecución** de un proceso: la CPU lo mantiene mientras ejecuta, y se lo manda al KM (mensaje
`ACTUALIZAR_CONTEXTO`) para que quede persistido cuando el proceso se desaloja — así, cuando otra
CPU (o la misma) retoma ese proceso más tarde, arranca desde donde quedó.

#### `t_segmento` (segmento.h)
Descriptor de un segmento de memoria asignado a un proceso: `id_segmento`, `base` (dirección física
de inicio), `limite` y `tamanio`. Es la unidad que maneja `kernel_memory` en su tabla de segmentos
por PID (esquema de segmentación pura, no paginación) y la que usa la CPU/MMU para validar que una
dirección lógica caiga dentro de los límites antes de traducirla a física.
