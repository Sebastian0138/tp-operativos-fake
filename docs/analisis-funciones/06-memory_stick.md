# Módulo memory_stick — Análisis detallado

Nodo de memoria física real. No tiene lógica de negocio: es un bloque de RAM
plano (`malloc` de N bytes) protegido por un mutex, con un servidor TCP que
atiende operaciones de lectura/escritura por dirección **local** (0..tamaño-1).
No conoce PIDs, segmentos ni direcciones globales — toda la traducción
(dirección global → stick + offset local) la hace quien lo consulta: la CPU
(para el ciclo de instrucción) o Kernel Memory (para SWAP, STDIN/STDOUT y
compactación).

**Corrección sobre `DOCUMENTACION_FUNCIONES.md`:** ese documento cita un TODO
en `conexiones.c:71` ("atender al CPU") como pendiente. Eso **ya no es así**:
el archivo actual (182 líneas) tiene el loop de atención completo
(`atender_peticiones`) y distingue explícitamente entre cliente CPU y cliente
Kernel Memory en `manejar_cliente` (líneas 131-145). No hay TODOs ni mocks en
el módulo — está 100% implementado.

**Confirmado con evidencia:** el stick acepta conexiones de **dos tipos de
cliente** distintos, verificado por handshake (`memory_stick/src/conexiones/conexiones.c:131-145`):
- `HANDSHAKE_CPU` (`cpu/src/conexiones/conexiones.c`, vía `stick_operar`): la CPU se conecta **directo** a cada stick para el R/W físico del ciclo de instrucción (MOV_IN/MOV_OUT/COPY_MEM), sin pasar por KM.
- `HANDSHAKE_KERNEL_MEMORY`: KM se conecta al mismo puerto para sus propias operaciones físicas (`gestor_memoria_leer_fisico`/`escribir_fisico`, usadas en SWAP, STDIN/STDOUT y compactación).

Ambos hablan el mismo protocolo de datos (`MENSAJE_LEER_MEMORIA` /
`MENSAJE_ESCRIBIR_MEMORIA`) sobre el mismo loop `atender_peticiones` — el
stick no diferencia el origen una vez pasado el handshake.

---

### `main.c`
Orquesta el arranque: logger de boot → config → logger definitivo → reserva el
bloque de memoria → abre el socket de escucha **antes** de conectarse a KM
(comentario explícito en el código, línea 65-66: KM abre su conexión de datos
hacia el stick apenas recibe `MENSAJE_INFORMAR_TAMANIO`, así que el stick ya
tiene que estar escuchando) → se conecta a KM informando tamaño → entra al
loop bloqueante de atención.

#### `main(int argc, char** argv)` — main.c:9
- **Qué hace:** punto de entrada. Valida argumentos (`config` + `tamaño` por CLI, no por archivo — a diferencia de los demás módulos el tamaño se pasa como segundo argumento de línea de comandos), y encadena el arranque completo con manejo de error en cada paso (libera lo ya creado y sale con `EXIT_FAILURE` si algo falla).
- **Dónde se usa:** entry point del binario `memory_stick`, invocado por `scripts_modulos/memory_stick.sh` / `start_all.sh`.
- **Para qué:** levantar un nodo de RAM física operativo y conectado a KM.
- **Cómo funciona:** 1) crea boot logger, 2) valida `argc == 3` (`./bin/memory_stick config tamaño`), 3) parsea `tamaño` con `strtoul` (falla si es 0), 4) carga config, 5) crea logger definitivo, 6) `memoria_stick_create(tamanio)`, 7) `crear_escucha_memory_stick` (bind+listen), 8) `conectar_a_kernel_memory` (handshake + informar tamaño/ip/puerto), 9) `atender_conexiones_memory_stick` (bloquea para siempre aceptando clientes).

---

### `config/memory_stick_config.c` — carga de configuración
Responsabilidad: leer el `.cfg` (vía `commons/config`), validar que estén las
6 claves obligatorias y exponerlas en un struct tipado.

#### `tiene_todas_las_propiedades(t_config* config)` — memory_stick_config.c:6 *(static)*
- **Qué hace:** valida presencia de `LOG_LEVEL`, `MEMORY_DELAY`, `LISTEN_IP`, `LISTEN_PORT`, `IP_KERNEL_MEMORY`, `PORT_KERNEL_MEMORY`.
- **Dónde se usa:** solo `memory_stick_config_create` (mismo archivo, línea 23).
- **Para qué:** fail-fast si el `.cfg` está incompleto, antes de intentar leer valores inexistentes.
- **Cómo funciona:** cadena de `config_has_property` con `&&`, corto-circuito.

#### `memory_stick_config_create(char* path, t_log* logger)` — memory_stick_config.c:16
- **Qué hace:** abre el archivo de config, valida claves, y si todo OK arma el `t_memory_stick_config` con `strdup` de cada string y `config_get_int_value` para `MEMORY_DELAY`.
- **Dónde se usa:** `main.c:34`.
- **Para qué:** punto único de entrada de configuración del módulo.
- **Cómo funciona:** `config_create` → si `NULL` loguea y aborta; `tiene_todas_las_propiedades` → si falta algo, destruye el config y aborta; si pasa, `malloc` del struct y copia campo a campo; libera el `t_config` de commons (ya no lo necesita, los valores quedaron copiados).

#### `memory_stick_config_log(t_memory_stick_config* config, t_log* logger)` — memory_stick_config.c:42
- **Qué hace:** loguea los 6 valores cargados.
- **Dónde se usa:** `main.c:41`.
- **Para qué:** trazabilidad de arranque (ver con qué parámetros corre el stick).
- **Cómo funciona:** una línea `log_info` por campo.

#### `memory_stick_config_destroy(t_memory_stick_config* config)` — memory_stick_config.c:52
- **Qué hace:** libera los 5 strings (`log_level`, `listen_ip`, `listen_port`, `ip_kernel_memory`, `port_kernel_memory`) y el struct.
- **Dónde se usa:** `main.c` en los 5 puntos de salida (39 líneas: 46, 60, 70, 79, 88) — todos los caminos de error y el cierre normal.
- **Para qué:** evitar leaks en cualquier salida del programa.
- **Cómo funciona:** `free` guardado por null-check inicial, luego `free` de cada campo.

---

### `logger/memory_stick_logger.c` — construcción de loggers
Dos loggers: uno de "boot" (fijo en INFO, para arrancar antes de tener config)
y uno definitivo (nivel configurable vía `.cfg`).

#### `memory_stick_boot_logger_create(void)` — memory_stick_logger.c:3
- **Qué hace:** crea un `t_log*` que escribe a `logs/memory_stick_boot.log` en nivel `INFO` fijo.
- **Dónde se usa:** `main.c:10`, primera línea de `main`.
- **Para qué:** loguear el arranque (incluyendo errores de carga de config) antes de que exista el logger definitivo.
- **Cómo funciona:** wrapper directo sobre `log_create` de la librería `commons`.

#### `memory_stick_logger_create(t_memory_stick_config* config)` — memory_stick_logger.c:12
- **Qué hace:** crea el logger operativo en `logs/memory_stick.log`, con el nivel indicado por `LOG_LEVEL` del `.cfg` (`log_level_from_string`).
- **Dónde se usa:** `main.c:43`.
- **Para qué:** logging del resto de la vida del proceso (arranque completado, lecturas/escrituras).
- **Cómo funciona:** igual a la de boot pero con nivel parametrizado.

---

### `memoria/memoria.c` — el bloque de memoria física
El corazón físico del módulo: un `malloc`/`calloc` contiguo con acceso
thread-safe (mutex) y validación de rango.

#### `memoria_stick_create(uint32_t tamanio)` — memoria.c:5
- **Qué hace:** reserva el bloque de `tamanio` bytes (inicializado en cero con `calloc`) e inicializa el mutex.
- **Dónde se usa:** `main.c:57`.
- **Para qué:** representar la RAM física de este stick.
- **Cómo funciona:** `malloc` del struct contenedor, `calloc(1, tamanio)` para el bloque (si falla, libera el struct y devuelve `NULL` — no deja el struct a medio construir), `pthread_mutex_init`.

#### `memoria_stick_destroy(t_memoria_stick* memoria)` — memoria.c:20
- **Qué hace:** libera el bloque y destruye el mutex.
- **Dónde se usa:** `main.c` en 3 puntos (69, 78, 87 — errores post-creación de memoria y cierre normal).
- **Para qué:** limpieza en shutdown.
- **Cómo funciona:** null-check, `free(memoria->bloque)`, `pthread_mutex_destroy`, `free(memoria)`.

#### `rango_valido(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio)` — memoria.c:36 *(static)*
- **Qué hace:** determina si el rango `[dir, dir+tamanio)` cae completo dentro del bloque `[0, memoria->tamanio)`.
- **Dónde se usa:** `memoria_stick_leer` (línea 45) y `memoria_stick_escribir` (línea 54), como guard inicial.
- **Para qué:** evitar overflow de buffer / acceso fuera de los límites del stick — es la única barrera de seguridad de memoria del módulo.
- **Cómo funciona:** el comentario del propio código (líneas 27-35) explica el detalle no trivial: **no** se calcula `dir + tamanio <= memoria->tamanio` directamente porque, siendo ambos `uint32_t`, esa suma puede desbordar (dar la vuelta) con valores grandes y colar un rango inválido como válido. En cambio valida en dos pasos que nunca overflowean: (1) `dir <= memoria->tamanio` (el inicio no se pasa del final), (2) `tamanio <= memoria->tamanio - dir` (lo que resta de bloque desde `dir` alcanza para `tamanio` bytes). Devuelve `true` solo si ambas se cumplen.

#### `memoria_stick_leer(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, void* destino)` — memoria.c:44
- **Qué hace:** copia `tamanio` bytes desde `bloque + dir` hacia `destino`.
- **Dónde se usa:** `conexiones.c:68`, dentro del handler de `MENSAJE_LEER_MEMORIA`.
- **Para qué:** servir una lectura física pedida por CPU o KM.
- **Cómo funciona:** valida rango (si falla, devuelve `false` sin tocar memoria); si es válido, toma el mutex, `memcpy`, libera el mutex. Retorna `bool` de éxito — el caller decide qué código de protocolo responder.

#### `memoria_stick_escribir(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, const void* origen)` — memoria.c:53
- **Qué hace:** copia `tamanio` bytes desde `origen` hacia `bloque + dir`.
- **Dónde se usa:** `conexiones.c:96`, dentro del handler de `MENSAJE_ESCRIBIR_MEMORIA`.
- **Para qué:** servir una escritura física pedida por CPU o KM.
- **Cómo funciona:** simétrico a `memoria_stick_leer`, mismo guard de rango y mismo mutex.

---

### `conexiones/conexiones.c` — servidor TCP y protocolo de datos
Responsabilidad: exponer el bloque de memoria por red. Usa estado a nivel de
módulo (`_logger`, `_memoria`, `_memory_delay_ms`, variables `static` seteadas
una sola vez en `crear_escucha_memory_stick`) para que los hilos de atención
no necesiten pasar esos parámetros en cada llamada.

#### `crear_escucha_memory_stick(t_memory_stick_config* config, t_memoria_stick* memoria, t_log* logger)` — conexiones.c:19
- **Qué hace:** guarda logger/memoria/delay en las estáticas del módulo y abre el socket servidor (bind+listen) en `LISTEN_PORT`.
- **Dónde se usa:** `main.c:67`, **antes** de conectarse a KM (ver nota de diseño en el header, línea 8-10: KM abre su conexión de datos hacia el stick tan pronto recibe `MENSAJE_INFORMAR_TAMANIO`, así que el stick tiene que estar escuchando primero).
- **Para qué:** dejar el stick listo para aceptar conexiones de CPU/KM.
- **Cómo funciona:** wrapper sobre `crear_servidor` de `utils/sockets`; en error loguea y devuelve `-1`.

#### `atender_conexiones_memory_stick(int fd_escucha)` — conexiones.c:34
- **Qué hace:** loop infinito de `accept()`; por cada conexión entrante lanza un hilo detached que corre `manejar_cliente`.
- **Dónde se usa:** `main.c:85`, última línea antes del shutdown (es bloqueante — no retorna en operación normal).
- **Para qué:** atender múltiples clientes (varias CPUs + KM) concurrentemente.
- **Cómo funciona:** `malloc` de un `int` para pasar el fd por puntero al hilo (evita condición de carrera si reusara una variable de loop), `pthread_create` + `pthread_detach` (no se hace `join`, el hilo se limpia solo al terminar). Si `accept` falla, loguea y continúa (no aborta el servidor).

#### `atender_peticiones(int fd)` — conexiones.c:53 *(static)*
- **Qué hace:** loop de request/response sobre una conexión ya establecida: lee un `t_codigo_mensaje` y despacha por `switch`.
- **Dónde se usa:** llamado desde `manejar_cliente` para **ambos** tipos de cliente (`HANDSHAKE_CPU` línea 139, `HANDSHAKE_KERNEL_MEMORY` línea 144) — es el mismo loop para los dos, el stick no distingue el origen a este nivel.
- **Para qué:** implementar el protocolo de datos físico del stick (los dos únicos mensajes que entiende).
- **Cómo funciona:**
  - `MENSAJE_LEER_MEMORIA`: recibe `dir` y `tamanio` (dos `uint32_t`), duerme `_memory_delay_ms` (simula latencia de memoria — notar que el delay se aplica **antes** de leer, a diferencia de escritura donde se aplica después de recibir el payload pero antes de escribir), reserva un buffer, llama a `memoria_stick_leer`. Si OK: responde `MENSAJE_OK` + los bytes leídos, loguea `## Lectura de N bytes`. Si falla (rango inválido): responde `MENSAJE_ERROR` y loguea el rango pedido vs. el tamaño real del bloque. Siempre libera el buffer.
  - `MENSAJE_ESCRIBIR_MEMORIA`: recibe `dir`, `tamanio`, y luego el payload de `tamanio` bytes; **después** aplica el delay; llama a `memoria_stick_escribir`; responde `MENSAJE_OK`/`MENSAJE_ERROR` y loguea `## Escritura de N bytes` o el error de rango.
  - Cualquier otro código: loguea `warning` "Mensaje desconocido" y sigue el loop (no cierra la conexión).
  - El loop termina (`break` externo, la función retorna) cuando `recv` devuelve `<= 0` (el cliente cerró la conexión), momento en que `manejar_cliente` cierra el fd.
  - Nota de robustez: si `recv` falla a mitad de leer los parámetros de un mensaje, la función hace `return` directo (no `break`), cortando el loop entero — una conexión con framing corrupto se da por perdida.

#### `manejar_cliente(void* arg)` — conexiones.c:117
- **Qué hace:** hilo por conexión: primero resuelve el handshake (identifica si es una CPU o Kernel Memory), luego delega el resto de la vida de la conexión a `atender_peticiones`.
- **Dónde se usa:** pasado como entry point a `pthread_create` en `atender_conexiones_memory_stick:46`.
- **Para qué:** separar la fase de identificación (handshake) del loop de datos, y loguear quién se conectó.
- **Cómo funciona:** castea `arg` a `int*`, extrae el fd y libera el `malloc` hecho por el caller. Lee un `int32_t` de handshake; si la conexión se cae antes de mandarlo, loguea y cierra. Según el valor: `HANDSHAKE_CPU` → además recibe el `id_cpu` (otro `int32_t`), loguea `## CPU <id> Conectada`, entra a `atender_peticiones`; `HANDSHAKE_KERNEL_MEMORY` → loguea la conexión (con el fd) y entra directo a `atender_peticiones` (KM no manda un id adicional, es un único cliente lógico); cualquier otro valor → `warning` "Handshake desconocido" y la conexión se descarta sin atenderla. Al volver de `atender_peticiones` (conexión cerrada por el peer), cierra el fd.

#### `conectar_a_kernel_memory(t_memory_stick_config* config, uint32_t tamanio, t_log* logger)` — conexiones.c:155
- **Qué hace:** abre una conexión **saliente** del stick hacia KM (el stick actúa de cliente acá) y le informa su tamaño + su propia ip:puerto de escucha.
- **Dónde se usa:** `main.c:76`. (Ojo: hay 4 funciones con el mismo nombre `conectar_a_kernel_memory` en el repo — una por módulo cliente de KM: `kernel_scheduler`, `cpu`, `memory_stick`. Son independientes, cada una con su propia firma; no hay colisión real porque cada módulo compila por separado.)
- **Para qué:** que KM descubra este stick, lo registre en su topología (`gestor_memoria_registrar_stick`, ver módulo `kernel_memory`) y se lo comunique después a las CPUs (`MENSAJE_TOPOLOGIA_STICKS`).
- **Cómo funciona:** 1) `crear_conexion` hacia `ip_kernel_memory:port_kernel_memory`; 2) envía el handshake `HANDSHAKE_MEMORY_STICK`; 3) envía el mensaje `MENSAJE_INFORMAR_TAMANIO` seguido de: el `tamanio` (uint32), el string `listen_ip` prefijado con su longitud (`uint32` + bytes), y el string `listen_port` de la misma forma — serialización manual campo a campo, sin capa de paquetes, consistente con el patrón del resto del protocolo. Devuelve el fd de esta conexión (que queda abierta pero el stick no la vuelve a usar activamente: es KM quien, del otro lado, abre una **segunda** conexión de datos hacia el puerto de escucha del stick para las operaciones físicas — ver `HANDSHAKE_KERNEL_MEMORY` en `manejar_cliente`).
