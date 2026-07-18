# memory_stick/ y swap/ — documentación por archivo

Cobertura completa, archivo por archivo, de los dos módulos que representan almacenamiento
físico puro (RAM simulada y disco de SWAP). Ninguno de los dos tiene lógica de negocio: solo
saben leer/escribir bytes en un rango que ya viene validado o resuelto por quien los llama
(CPU y Kernel Memory, respectivamente).

# memory_stick/

## `memory_stick/src/main.c` (92 líneas)

**Propósito:** punto de entrada. Boot estándar (logger de boot → config → logger principal),
reserva el bloque de RAM simulado, abre el socket de escucha, se conecta a Kernel Memory como
cliente para anunciarse, y entra al loop bloqueante de atención de conexiones.

### `int main(int argc, char** argv)`
Qué hace y cómo, en orden estricto:
1. Crea el boot logger (`memory_stick_boot_logger_create`).
2. Valida `argc == 3` — uso esperado: `./bin/memory_stick [Archivo Config] [Tamaño]`. El
   tamaño del stick se pasa por **línea de comandos**, no por archivo de config.
3. Parsea `argv[2]` a `uint32_t` con `strtoul`; si da `0` (incluye el caso de que no sea un
   número válido, porque `strtoul` devuelve 0 en error), aborta.
4. Carga la config (`memory_stick_config_create`), la loguea (`memory_stick_config_log`).
5. Crea el logger principal y destruye el boot logger.
6. Reserva el bloque físico: `memoria_stick_create(tamanio)`.
7. **Orden crítico, documentado en un comentario del propio archivo:** primero
   `crear_escucha_memory_stick` (bind+listen) y recién después `conectar_a_kernel_memory`.
   Razón: apenas Kernel Memory recibe el `MENSAJE_INFORMAR_TAMANIO`, abre una conexión de
   datos *hacia* este stick — si el stick todavía no estuviera escuchando, esa conexión
   fallaría. Es una dependencia de orden de arranque real, no cosmética.
8. Entra a `atender_conexiones_memory_stick(fd_escucha)`, que es un loop infinito — el
   `main` nunca llega naturalmente a las líneas de `destroy` de después salvo que ese loop
   termine (lo cual no ocurre en operación normal).

**Dónde se usa:** es el entry point del binario, no lo llama nadie.
**Para qué:** orquestar el arranque de un nodo de memoria física independiente.

## `memory_stick/src/config/memory_stick_config.c` (63 líneas) y `.h` (21 líneas)

**Propósito:** carga y valida el `.cfg` del stick contra la librería `commons`.

Claves obligatorias (`tiene_todas_las_propiedades`): `LOG_LEVEL`, `MEMORY_DELAY`,
`LISTEN_IP`, `LISTEN_PORT`, `IP_KERNEL_MEMORY`, `PORT_KERNEL_MEMORY`. Nótese que el
**tamaño del stick no está acá** — viene por `argv`, no por config (confirmado en `main.c`).

`t_memory_stick_config` (struct, todos los campos con propósito):
- `log_level` (string) — nivel de log runtime.
- `memory_delay` (int, ms) — demora artificial que simula latencia de acceso a memoria; se
  aplica en cada lectura/escritura real (ver `conexiones.c`).
- `listen_ip` / `listen_port` (string) — dónde escucha este stick su socket de datos.
- `ip_kernel_memory` / `port_kernel_memory` (string) — a dónde conectarse para anunciarse.

### `t_memory_stick_config* memory_stick_config_create(char* path, t_log* logger)`
Abre el `.cfg` con `config_create`, valida presencia de las 6 claves, y si están todas
completas hace `malloc` + copia (`strdup` para strings, `config_get_int_value` para el int).
Libera el `t_config* config` de commons ni bien terminó de copiar los valores (no lo retiene).
Devuelve `NULL` en cualquier fallo (archivo inexistente o claves faltantes), logueando el motivo.
**Dónde se usa:** `main.c:34`. **Para qué:** único punto de construcción de la config del módulo.

### `void memory_stick_config_log(t_memory_stick_config* config, t_log* logger)`
Vuelca las 6 claves al logger, una por línea, con formato `CLAVE=valor`. **Dónde se usa:**
`main.c:41`. **Para qué:** trazabilidad de con qué parámetros arrancó el nodo (requisito
típico de cátedra: loguear la config al boot).

### `void memory_stick_config_destroy(t_memory_stick_config* config)`
`free` de los 5 punteros a string más el struct; no-op si `config == NULL`. **Dónde se usa:**
`main.c:60,70,79,88` — en cada camino de salida de `main` (éxito y todos los `EXIT_FAILURE`).

## `memory_stick/src/logger/memory_stick_logger.c` (19 líneas) y `.h` (10 líneas)

**Propósito:** dos constructores de `t_log*` de la librería `commons/log`, uno para boot
(nivel fijo `INFO`, antes de tener config) y otro definitivo (nivel leído de la config).

### `t_log* memory_stick_boot_logger_create(void)`
`log_create("logs/memory_stick_boot.log", "MEMORY_STICK_BOOT", true, LOG_LEVEL_INFO)`.
**Dónde se usa:** `main.c:10`. **Para qué:** loguear errores de arranque (archivo de config
inválido, etc.) *antes* de tener un logger configurado — no puede depender de la config
porque su fallo es justamente lo que este logger necesita reportar.

### `t_log* memory_stick_logger_create(t_memory_stick_config* config)`
`log_create("logs/memory_stick.log", "MEMORY_STICK", true, log_level_from_string(...))`.
**Dónde se usa:** `main.c:43`. **Para qué:** logger operativo del resto de la vida del proceso.

## `memory_stick/src/memoria/memoria.c` (60 líneas) y `memoria.h` (27 líneas)

**Propósito:** el corazón real del módulo — el bloque de RAM simulado y sus operaciones
atómicas de lectura/escritura. Sin ninguna noción de PIDs, segmentos ni direcciones globales:
opera exclusivamente con direcciones **locales al stick** (`0..tamanio-1`).

`t_memoria_stick` (struct):
- `bloque` (`void*`) — el buffer real, reservado con `calloc` (arranca en cero).
- `tamanio` (`uint32_t`) — tamaño del bloque en bytes.
- `mutex` (`pthread_mutex_t`) — protege cada acceso individual de lectura/escritura (no hay
  una sección crítica más amplia que el propio `memcpy`).

### `t_memoria_stick* memoria_stick_create(uint32_t tamanio)`
`malloc` del struct + `calloc(1, tamanio)` para el bloque (inicializa todo en cero — un stick
recién creado siempre empieza "limpio"). Si cualquiera de los dos `malloc`/`calloc` falla,
libera lo que ya haya reservado y devuelve `NULL` (no deja memoria a medio inicializar).
Inicializa el mutex con `pthread_mutex_init`. **Dónde se usa:** `main.c:57`. **Para qué:**
única fuente de la RAM simulada del proceso.

### `void memoria_stick_destroy(t_memoria_stick* memoria)`
`free(bloque)` + `pthread_mutex_destroy` + `free(memoria)`; no-op si `memoria == NULL`.
**Dónde se usa:** `main.c:69,78,87` (los tres caminos de salida tras haberla creado).

### `static bool rango_valido(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio)`
Valida que `[dir, dir+tamanio)` caiga dentro de `[0, memoria->tamanio)`. **Cómo, exactamente**
(documentado con un comentario extenso en el propio archivo): en vez de comparar
`dir + tamanio <= memoria->tamanio` — que podría desbordar un `uint32_t` si `dir` y `tamanio`
son ambos grandes, dando un resultado chico que pasaría el chequeo incorrectamente — se
verifica en dos pasos que individualmente nunca desbordan: (1) `dir <= memoria->tamanio`
(el inicio no se pasa del bloque) y (2) `tamanio <= memoria->tamanio - dir` (lo que resta del
bloque desde `dir` alcanza para `tamanio` bytes). Es el mismo patrón de defensa contra
overflow que aparece también en la validación de rangos del lado de `kernel_memory`.
**Dónde se usa:** internamente por `memoria_stick_leer` (línea 45) y `memoria_stick_escribir`
(línea 54). **Para qué:** que un pedido de lectura/escritura corrupto o malicioso jamás
provoque un acceso fuera del bloque reservado (que sería un buffer overflow/underflow real
en C, no solo un "path incorrecto").

### `bool memoria_stick_leer(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, void* destino)`
Valida rango; si es inválido devuelve `false` sin tocar nada. Si es válido, toma el mutex,
hace `memcpy(destino, bloque+dir, tamanio)`, libera el mutex, devuelve `true`. **Dónde se
usa:** `conexiones.c:68` (dentro del handler de `MENSAJE_LEER_MEMORIA`). **Para qué:** es la
única forma de sacar bytes del stick — no hay acceso directo al `bloque` desde fuera de este
archivo (buen encapsulamiento: el puntero es un campo del struct, pero solo estas dos
funciones lo tocan en todo el módulo).

### `bool memoria_stick_escribir(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, const void* origen)`
Simétrica a la anterior: valida rango, `memcpy(bloque+dir, origen, tamanio)` bajo mutex.
**Dónde se usa:** `conexiones.c:96`.

## `memory_stick/src/conexiones/conexiones.c` (182 líneas) y `.h` (19 líneas)

**Propósito:** toda la capa de red del stick — servidor de escucha, loop de aceptación
concurrente (un hilo por cliente), el protocolo de datos (leer/escribir memoria) y el cliente
saliente hacia Kernel Memory para anunciarse.

Variables de módulo (`static`, compartidas entre los hilos de atención, seteadas una sola vez
en `crear_escucha_memory_stick` antes de que exista ningún hilo cliente):
`_logger`, `_memoria`, `_memory_delay_ms`.

### `int crear_escucha_memory_stick(t_memory_stick_config* config, t_memoria_stick* memoria, t_log* logger)`
Setea las tres estáticas del módulo y llama a `crear_servidor(config->listen_port)` (de
`utils/sockets`) para hacer bind+listen. Devuelve el fd de escucha o `-1` si falló.
**Dónde se usa:** `main.c:67`. **Para qué:** dejar el socket de escucha listo *antes* de
anunciarse a Kernel Memory (ver la nota de orden de arranque en `main.c`).

### `void atender_conexiones_memory_stick(int fd_escucha)`
Loop infinito de `accept()`: por cada conexión entrante, reserva un `int*` en heap con el fd
(para pasarlo de forma segura al hilo nuevo sin condiciones de carrera sobre una variable de
stack compartida), lanza un hilo con `manejar_cliente` y lo hace `pthread_detach` (no se
espera su fin — el hilo libera sus propios recursos al terminar). Si `accept` falla, loguea el
error y sigue el loop (no aborta el servidor por una conexión fallida). **Dónde se usa:**
`main.c:85` — es el loop principal del proceso, nunca retorna en operación normal. **Para
qué:** atender múltiples clientes (varias CPUs + Kernel Memory) simultáneamente, cada uno con
su propio hilo y su propio fd — sin esto, un cliente lento bloquearía a todos los demás.

### `static void atender_peticiones(int fd)`
Loop de request/response sobre un fd ya autenticado por handshake. Recibe un
`t_codigo_mensaje` y despacha por switch:
- `MENSAJE_LEER_MEMORIA`: recibe `dir` y `tamanio` (dos `uint32_t`), aplica el delay
  artificial (`usleep(_memory_delay_ms * 1000)`) **antes** de leer, reserva un buffer
  (`malloc(tamanio > 0 ? tamanio : 1)` — evita `malloc(0)`, que es comportamiento indefinido/
  no confiable en cuanto a qué puntero devuelve), llama a `memoria_stick_leer`. Responde
  `MENSAJE_OK` + el buffer leído si tuvo éxito, o `MENSAJE_ERROR` (sin buffer) si el rango era
  inválido — y en ese caso loguea el motivo con `log_error` incluyendo `dir`, `tamanio` y el
  tamaño real del bloque. Libera el buffer siempre.
- `MENSAJE_ESCRIBIR_MEMORIA`: recibe `dir`, `tamanio` y luego el buffer de `tamanio` bytes
  (si `tamanio > 0`), aplica el delay artificial **después** de recibir los datos pero antes
  de escribir, llama a `memoria_stick_escribir`, responde OK/ERROR de igual manera.
- `default`: loguea un warning de mensaje desconocido y sigue el loop (no cierra la conexión
  por un mensaje inesperado, solo lo ignora).

Si cualquier `recv` intermedio devuelve `<= 0` (el cliente cortó a mitad de un mensaje), la
función retorna inmediatamente (deja de atender ese fd; el hilo termina y `manejar_cliente`
cierra el socket).

**Nota de diseño explícita en el código:** el comentario de la línea 51-52 aclara que este
mismo loop atiende **tanto a CPUs como a Kernel Memory** — es el mismo protocolo de datos
para los dos tipos de cliente, el stick no distingue el origen a este nivel (si distingue, es
solo en el handshake previo, para loguear quién se conectó).

**Dónde se usa:** invocada desde `manejar_cliente` en ambas ramas (`HANDSHAKE_CPU` línea 139,
`HANDSHAKE_KERNEL_MEMORY` línea 144). **Para qué:** es el servidor de datos físico real —
todo byte que lee o escribe cualquier proceso de usuario en algún momento pasa por acá.

### `void* manejar_cliente(void* arg)`
Función de hilo (firma `pthread` estándar). Libera el `int*` recibido tras copiar el fd
localmente. Recibe el primer mensaje de la conexión: un `int32_t` de handshake. Si el `recv`
falla (cliente se desconectó antes de identificarse), loguea y cierra sin más. Si llegó,
despacha por switch:
- `HANDSHAKE_CPU`: recibe además un `int32_t id_cpu` (para loguear qué CPU es), loguea
  `"## CPU %d Conectada"`, y entra a `atender_peticiones(fd_conexion)`.
- `HANDSHAKE_KERNEL_MEMORY`: no recibe datos adicionales (es un cliente único, no hay
  múltiples "Kernel Memory" que identificar individualmente), loguea la conexión con el fd, y
  entra a `atender_peticiones`.
- `default`: handshake desconocido → loguea warning, **no** entra al loop de atención (la
  conexión se rechaza efectivamente, cae directo al `close` final).

Al salir de `atender_peticiones` (por cualquier motivo: desconexión del cliente, error de
recv), cierra el fd y retorna `NULL`.

**Dónde se usa:** lanzada como hilo desde `atender_conexiones_memory_stick` (línea 46), y
referenciada en la declaración forward de la línea 17. **Para qué:** autenticar/clasificar
cada conexión entrante antes de dejarla operar sobre la memoria real.

### `int conectar_a_kernel_memory(t_memory_stick_config* config, uint32_t tamanio, t_log* logger)`
Se conecta como cliente a `config->ip_kernel_memory:port_kernel_memory` con `crear_conexion`
(de `utils/sockets`). Manda el handshake `HANDSHAKE_MEMORY_STICK` (definido en
`utils/protocolo.h:11` como `4`). Luego manda un mensaje `MENSAJE_INFORMAR_TAMANIO` con:
`tamanio` (el tamaño del stick, el mismo que viene por `argv`), y el `listen_ip`/`listen_port`
propios **serializados con length-prefix** (primero un `uint32_t` con la longitud del string,
después el string sin `\0`) — mismo patrón de serialización que usa el resto del repo para
strings variables. Esto es lo que le permite a Kernel Memory, más adelante, abrir su propia
conexión de datos *hacia* este stick usando esa ip:puerto — el stick no le manda un fd, le
manda dónde conectarse. Devuelve el fd de esta conexión saliente (que **queda abierto** — no
se cierra tras informar el tamaño, porque este mismo socket es el canal que
`kernel_memory` puede usar más adelante, o bien KM abre uno nuevo directo al puerto de
escucha del stick — la conexión de *datos* real que después maneja `atender_peticiones` del
lado del stick es la que KM abre como cliente entrante, con handshake
`HANDSHAKE_KERNEL_MEMORY`, no esta conexión saliente inicial).

**Dónde se usa:** `main.c:76`. **Para qué:** es el único mecanismo de descubrimiento — Kernel
Memory no conoce los sticks por su propia configuración, los conoce porque cada stick se
anuncia al arrancar.

---

# swap/

## `swap/src/main.c` (66 líneas)

**Propósito:** entry point. Boot estándar, abre/crea el archivo de swap en disco, y entra
directo al loop bloqueante único de atención (a diferencia de memory_stick, **no hay hilos**:
todo el módulo corre en el hilo principal).

### `int main(int argc, char** argv)`
1. Boot logger, valida `argc == 2` (uso: `./bin/swap [Archivo Config]` — a diferencia de
   memory_stick, acá no hay un segundo argumento de tamaño; el tamaño total y de bloque vienen
   enteramente del `.cfg`).
2. Carga y loguea la config.
3. Logger principal.
4. Abre el archivo de swap con `fopen(config->swap_file_path, "w+b")`. El modo `"w+b"`
   **trunca el archivo si ya existía** — un comentario del propio código aclara que se asume
   que el swap "inicia libre" y por eso no hace falta limpiar contenido explícitamente: el
   truncado del `fopen` ya lo deja vacío. Esto implica que el estado de SWAP **no persiste**
   entre corridas del módulo — cada arranque es un swap nuevo.
5. Llama a `conectar_y_atender_kernel_memory(...)`, que es bloqueante y no retorna hasta que
   Kernel Memory se desconecta.
6. Solo entonces: `fclose`, `log_destroy`, `swap_config_destroy`.

**Dónde se usa:** entry point, no lo llama nadie. **Para qué:** arrancar el nodo de disco.

## `swap/src/config/swap_config.c` (62 líneas) y `.h` (21 líneas)

**Propósito:** carga/valida el `.cfg` de swap.

Claves obligatorias: `LOG_LEVEL`, `SWAP_FILE_PATH`, `SWAP_FILE_SIZE`, `BLOCK_SIZE`,
`IP_KERNEL_MEMORY`, `PORT_KERNEL_MEMORY`.

`t_swap_config` (struct):
- `log_level` (string).
- `swap_file_path` (string) — ruta del archivo de swap en disco.
- `swap_file_size` (int) — tamaño total del área de swap.
- `block_size` (int) — tamaño de cada bloque indexable.
- `ip_kernel_memory` / `port_kernel_memory` (string) — a dónde conectarse.

Funciones (mismo patrón exacto que `memory_stick_config.c`, solo cambian las claves):

### `t_swap_config* swap_config_create(char* path, t_log* logger)`
Igual que su equivalente de memory_stick: `config_create` + validación de las 6 claves +
copia con `strdup`/`config_get_int_value`. **Dónde se usa:** `main.c:26`.

### `void swap_config_log(t_swap_config* config, t_log* logger)`
Vuelca las 6 claves. **Dónde se usa:** `main.c:33`.

### `void swap_config_destroy(t_swap_config* config)`
Libera los 4 punteros a string (`log_level`, `swap_file_path`, `ip_kernel_memory`,
`port_kernel_memory`) más el struct. **Nota:** `swap_file_size` y `block_size` son `int`
directos, no punteros — no necesitan `free` individual, ya lo cubre el `free(config)` final.
**Dónde se usa:** `main.c:38,53,63` (los tres caminos de salida).

## `swap/src/logger/swap_logger.c` (19 líneas) y `.h` (10 líneas)

Idéntico en estructura a `memory_stick_logger.c`: `swap_boot_logger_create` (nivel fijo INFO,
archivo `logs/swap_boot.log`) y `swap_logger_create` (nivel desde config, `logs/swap.log`).
**Dónde se usan:** `main.c:9` y `main.c:35` respectivamente.

## `swap/src/conexiones/conexiones.c` (75 líneas) y `.h` (12 líneas)

**Propósito:** todo el módulo de red de swap en una sola función — no hay servidor, swap es
**cliente puro** de Kernel Memory, sin hilos, sin `accept`.

### `void conectar_y_atender_kernel_memory(t_swap_config* config, FILE* archivo_swap, t_log* logger)`
Cómo funciona, paso a paso:
1. `crear_conexion` hacia `ip_kernel_memory:port_kernel_memory`; si falla, loguea y retorna
   (el proceso sigue vivo pero no hace nada útil — no reintenta).
2. Manda handshake `HANDSHAKE_SWAP` (definido en `utils/protocolo.h:12` como `5`).
3. Informa tamaño: manda `MENSAJE_INFORMAR_TAMANIO` seguido de **dos** `uint32_t`:
   `total` (`swap_file_size`) y `block` (`block_size`) — a diferencia de memory_stick, que
   solo informa un tamaño (más ip:puerto), acá van los dos tamaños relevantes y no hay
   ip:puerto porque swap **no necesita servidor propio**: es Kernel Memory el que abre y
   mantiene esta única conexión saliente como su único canal con swap.
4. Reserva un buffer de `block` bytes, reutilizado en cada operación (evita malloc/free por
   cada lectura/escritura).
5. Entra a un loop bloqueante de `recv` de `t_codigo_mensaje`, con dos casos:
   - `MENSAJE_ESCRIBIR_MEMORIA`: recibe `nro_bloque` (`uint32_t`) y luego `block` bytes de
     contenido. Hace `fseek(archivo_swap, (long) nro_bloque * block, SEEK_SET)` y
     `fwrite` + `fflush` (fuerza la escritura a disco inmediatamente, no confía en el buffer
     de `stdio` — importante para que una caída del proceso no pierda escrituras ya
     confirmadas al kernel). Loguea `"## Escritura del bloque: %u"` y responde `MENSAJE_OK`.
   - `MENSAJE_LEER_MEMORIA`: recibe `nro_bloque`, `fseek` al offset, `fread` hasta `block`
     bytes. **Detalle importante:** si `fread` devuelve menos bytes de los pedidos (bloque
     nunca escrito antes, o archivo más corto que ese offset), el resto del buffer se
     completa a mano con ceros (`for (i = leidos; i < block; i++) buffer[i] = 0`) — así un
     bloque "virgen" siempre se lee como memoria en cero, consistente con que la RAM real
     también arranca en cero (`calloc` en memory_stick). Loguea y manda los `block` bytes.
   - `default`: warning de mensaje desconocido, sigue el loop.
   - Si cualquier `recv` devuelve `<= 0`, rompe el loop (Kernel Memory se desconectó o hubo
     error).
6. Al salir del loop: libera el buffer, cierra el fd, loguea "Kernel Memory desconectado,
   finalizando SWAP".

**Sin validación de rango:** a diferencia de `memory_stick`, esta función **no valida** que
`nro_bloque` esté dentro de algún límite razonable — confía ciegamente en que Kernel Memory
(que sí administra el bitmap de bloques libres/ocupados) nunca pida un bloque fuera de rango.
Si lo hiciera, `fseek` simplemente movería el cursor más allá del final del archivo y `fwrite`
haría crecer el archivo (comportamiento estándar de POSIX para archivos con huecos), sin
error — no hay un failure path para esto en el código.

**Dónde se usa:** `main.c:59` — es el loop principal del proceso, bloqueante, no retorna hasta
que Kernel Memory cierra la conexión. **Para qué:** es el único punto de persistencia física
del contenido de un proceso suspendido — cuando `kernel_memory/gestor_memoria.c` llama a
`swap_escribir_bloque`/`swap_leer_bloque` (funciones `static` del lado de KM, en
`gestor_memoria.c:615` y `626`, usadas por la lógica de suspensión/restauración en las líneas
710, 821 y 877-880 del mismo archivo), esos mensajes de red terminan siendo atendidos acá.

## Hallazgos notables

- **memory_stick:** ninguno nuevo — se reconfirma que no hay TODOs ni mocks; el módulo está
  íntegramente implementado tal cual se documentó en `06-memory_stick.md`.
- **swap:** el archivo se trunca en cada arranque (`"w+b"`), así que el estado de swap no
  sobrevive a un restart del módulo — coincide con lo ya anotado en `07-swap.md`, confirmado
  ahora a nivel de la línea exacta (`main.c:50`).
- **swap — falta de validación de `nro_bloque`:** no es un bug per se (el enunciado no exige
  esa validación de este lado, y Kernel Memory es quien administra el espacio), pero es una
  superficie de confianza total en el módulo llamador que vale la pena tener presente si se
  hacen pruebas de estrés o inyección de mensajes malformados.
- **memory_stick — dos handshakes de entrada, uno de salida:** el módulo usa
  `HANDSHAKE_MEMORY_STICK` (4) solo para anunciarse *como cliente* saliente hacia KM;
  como *servidor*, solo reconoce `HANDSHAKE_CPU` (definido junto a los demás en
  `protocolo.h`, valor no mostrado en el grep pero usado en `cpu/src/conexiones/conexiones.c:20,38,227`)
  y `HANDSHAKE_KERNEL_MEMORY` (7). Son tres códigos distintos con tres roles distintos, aunque
  los tres cuelgan de la misma conexión TCP subyacente en direcciones opuestas según el caso.
