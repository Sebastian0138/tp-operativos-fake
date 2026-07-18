# Módulo kernel_memory — Infraestructura (main, config, logger, gestor_procesos, conexiones, atencion_cpu, atencion_scheduler)

> Cubre todo `kernel_memory/src/` **excepto** `gestor_memoria/gestor_memoria.c` (documentado aparte). Verificado
> contra el código fuente real al momento de este análisis — no se asumió nada del `DOCUMENTACION_FUNCIONES.md`
> preexistente.

## Panorama del módulo

Kernel Memory (KM) es el servidor TCP que administra la memoria de usuario: mantiene el registro de procesos
(`gestor_procesos`), delega toda la lógica de segmentación/compactación/SWAP en `gestor_memoria` (ver documento
aparte) y expone esa lógica a través de dos handlers de protocolo — uno para CPU (`atencion_cpu`) y otro para
Kernel Scheduler (`atencion_scheduler`) — más un tercero implícito para Memory Sticks y SWAP dentro de
`conexiones.c`. Cada cliente que se conecta se identifica con un `int32_t` de **handshake** y se despacha a un
hilo dedicado (`pthread_create` + `pthread_detach`), o sea que KM atiende N CPUs, 1 KS (dos conexiones: control +
notificaciones), M Memory Sticks y 1 SWAP simultáneamente, todos sobre el mismo puerto de escucha.

---

### `main.c`
Responsabilidad: orquestar el arranque — cargar config, crear logger, crear `gestor_procesos` y `gestor_memoria`,
y bloquear el proceso principal dentro del loop `accept()` del servidor.

#### `main(argc, argv)` — main.c:9
- **Qué hace:** valida que se pase el path del archivo de config como único argumento; crea el boot logger, la
  config, el logger definitivo, el `gestor_procesos` y el `gestor_memoria` (en ese orden, con `EXIT_FAILURE` y
  liberación en cascada si cualquiera falla); llama a `iniciar_servidor_kernel_memory`, que bloquea corriendo el
  servidor para siempre; al retornar (nunca en la práctica, solo si el `accept` inicial falla) libera todo.
- **Dónde se usa:** entry point del binario `kernel_memory`, invocado por el sistema operativo al ejecutar
  `./bin/kernel_memory <config>` (ver `start_all.sh` / `scripts_modulos/kernel_memory.sh`).
- **Para qué:** es el bootstrap estándar que siguen los 7 módulos del TP.
- **Cómo funciona:** secuencia estrictamente lineal de creación con chequeo de `NULL` en cada paso; nótese que
  `gestor_memoria_create` recibe el `gestor_procesos` ya creado porque internamente lo necesita (para poblar la
  tabla de segmentos por proceso al crear/eliminar segmentos).

---

### `config/kernel_memory_config.c` / `.h`
Responsabilidad: cargar y validar `kernel_memory.cfg` usando la librería `commons` de la cátedra.

Claves obligatorias (`tiene_todas_las_propiedades`): `LOG_LEVEL`, `SEGMENT_MAX_SIZE`, `ALLOCATION_STRATEGY`,
`INSTRUCTION_DELAY`, `COMPACTION_DELAY`, `SCRIPTS_BASEPATH`, `LISTEN_IP`, `LISTEN_PORT`.

#### `tiene_todas_las_propiedades(t_config* config)` *(static)* — kernel_memory_config.c:6
- **Qué hace:** `&&` en cadena de `config_has_property` para las 8 claves obligatorias.
- **Dónde se usa:** solo dentro de `kernel_memory_config_create` (mismo archivo, línea 25). Es `static`, no
  exportada en el `.h`.
- **Para qué:** fail-fast si falta una clave en el `.cfg`, antes de intentar leerla y crashear.
- **Cómo funciona:** cortocircuito booleano simple, sin loop.

#### `kernel_memory_config_create(char* path, t_log* logger)` — kernel_memory_config.c:18
- **Qué hace:** abre el archivo con `config_create`, valida claves, y si están todas, hace `malloc` del struct
  `t_kernel_memory_config` y copia cada valor (`strdup` para strings, `config_get_int_value` para enteros).
- **Dónde se usa:** `main.c:24`.
- **Para qué:** punto único de acceso a la configuración tipada del módulo (evita repetir `config_get_string_value`
  por todo el código).
- **Cómo funciona:** libera el `t_config* config` de `commons` con `config_destroy` al final — solo sobrevive la
  copia tipada en el struct propio. Si falla la apertura o faltan claves, devuelve `NULL` sin loggear stack trace
  (el log ya lo hizo antes de retornar).

#### `kernel_memory_config_log(t_kernel_memory_config* config, t_log* logger)` — kernel_memory_config.c:46
- **Qué hace:** un `log_info` por cada campo de la config.
- **Dónde se usa:** `main.c:30`, inmediatamente tras crear la config (con el `boot_logger`, antes de tener el
  logger definitivo).
- **Para qué:** dejar constancia en el log de arranque de con qué parámetros corrió esa instancia (típico
  requisito de la cátedra: "loguear los parámetros leídos").
- **Cómo funciona:** trivial, solo `log_info` por campo.

#### `kernel_memory_config_destroy(t_kernel_memory_config* config)` — kernel_memory_config.c:58
- **Qué hace:** `free` de los 5 campos `char*` (`log_level`, `allocation_strategy`, `scripts_basepath`,
  `listen_ip`, `listen_port`) y del struct.
- **Dónde se usa:** `main.c:35,46,55,64` — en los 3 caminos de error temprano y en el cierre normal al final.
- **Para qué:** evitar leaks de las 5 strings duplicadas con `strdup`.
- **Cómo funciona:** chequea `config == NULL` primero (permite llamarla defensivamente).

---

### `logger/kernel_memory_logger.c` / `.h`

#### `kernel_memory_boot_logger_create(void)` — kernel_memory_logger.c:3
- **Qué hace:** crea un `t_log*` fijo a `logs/kernel_memory_boot.log`, tag `KERNEL_MEMORY_BOOT`, nivel `INFO` fijo
  (no depende de la config, porque todavía no se cargó).
- **Dónde se usa:** `main.c:10`, antes de leer la config.
- **Para qué:** poder loguear errores de arranque (ej. "no se pudo abrir el archivo de configuración") aun sin
  config cargada.
- **Cómo funciona:** wrapper directo sobre `log_create` de `commons`.

#### `kernel_memory_logger_create(t_kernel_memory_config* config)` — kernel_memory_logger.c:12
- **Qué hace:** crea el `t_log*` definitivo a `logs/kernel_memory.log`, tag `KERNEL_MEMORY`, con el nivel que
  indique `config->log_level` (vía `log_level_from_string`).
- **Dónde se usa:** `main.c:32`.
- **Para qué:** logger que usan todos los hilos del módulo durante toda su vida.
- **Cómo funciona:** el `t_log*` de `commons` es thread-safe internamente (usa un mutex propio), por eso puede
  compartirse entre los N hilos de `manejar_cliente` sin sincronización adicional del lado de KM.

---

### `gestor_procesos/kernel_memory_gestor_procesos.c` / `.h`
Responsabilidad: tabla PID → proceso (contexto de registros + tabla de segmentos + path al archivo de
instrucciones), con acceso concurrente protegido por un único mutex. Es el registro "quién existe", separado de
`gestor_memoria` que administra "dónde vive físicamente cada uno".

Structs clave: `t_proceso_km { t_contexto_ejecucion* contexto; char* path_instrucciones; }` y
`t_gestor_procesos { t_list* lista_procesos; pthread_mutex_t mutex; }`.

#### `destruir_proceso_km(void* elemento)` *(static)* — kernel_memory_gestor_procesos.c:6
- **Qué hace:** libera `path_instrucciones`, destruye `tabla_segmentos` (lista de `t_segmento*` con `free` como
  destructor de elemento), libera el `contexto` y el `t_proceso_km` mismo.
- **Dónde se usa:** como destructor pasado a `list_destroy_and_destroy_elements` en `gestor_procesos_destroy`
  (línea 24) y directamente en `gestor_remover_proceso` (línea 50).
- **Para qué:** único punto de liberación completa de un `t_proceso_km`, para no duplicar la lógica de free en dos
  lugares.
- **Cómo funciona:** el orden importa — primero libera la lista interna de segmentos (que son punteros propios del
  proceso) y recién después el contexto que la contiene.

#### `gestor_procesos_create()` — kernel_memory_gestor_procesos.c:14
- **Qué hace:** `malloc` del struct, `list_create()` para la lista de procesos, `pthread_mutex_init`.
- **Dónde se usa:** `main.c:43`.
- **Para qué:** inicializar el registro global de procesos de KM.
- **Cómo funciona:** trivial; devuelve `NULL` si el `malloc` falla (chequeado en el caller).

#### `gestor_procesos_destroy(t_gestor_procesos* gestor)` — kernel_memory_gestor_procesos.c:22
- **Qué hace:** destruye la lista completa con `destruir_proceso_km` como destructor, destruye el mutex, libera el
  struct.
- **Dónde se usa:** `main.c:54,63` (en el camino de error de `gestor_memoria_create` y en el cierre normal).
- **Para qué:** limpieza total al apagar KM.
- **Cómo funciona:** chequea `NULL` primero.

#### `gestor_agregar_proceso(t_gestor_procesos* gestor, uint32_t pid, char* path)` — kernel_memory_gestor_procesos.c:29
- **Qué hace:** crea un `t_contexto_ejecucion` con `pid`, registros en cero (`memset`) y `tabla_segmentos` vacía;
  arma el `t_proceso_km` con ese contexto y `path` copiado (`strdup`); lo agrega a la lista bajo el mutex.
- **Dónde se usa:** `atencion_scheduler.c:40`, dentro del handler de `MENSAJE_ENVIAR_PID` — o sea, se dispara cada
  vez que el Kernel Scheduler crea un proceso nuevo (syscall `INIT_PROC` del lado CPU, o el proceso inicial al
  arrancar el KS) y se lo informa a KM.
- **Para qué:** dar de alta el proceso en KM ANTES de que la CPU pueda pedir su primera instrucción o contexto —
  es el primer mensaje que KM recibe de un proceso nuevo.
- **Cómo funciona:** el `path` que recibe ya viene resuelto a ruta completa (`SCRIPTS_BASEPATH` + nombre de
  archivo) por el caller en `atencion_scheduler.c`; acá no se toca el filesystem, solo se guarda el string.

#### `gestor_remover_proceso(t_gestor_procesos* gestor, uint32_t pid)` — kernel_memory_gestor_procesos.c:44
- **Qué hace:** recorre la lista linealmente buscando `pid`, y si lo encuentra lo saca de la lista
  (`list_remove`) y lo destruye (`destruir_proceso_km`).
- **Dónde se usa:** `gestor_memoria.c:376`, dentro de `gestor_memoria_finalizar_proceso` — se llama cuando KS
  manda `MENSAJE_FINALIZAR_PROCESO` (fin de proceso por EXIT o syscall de error).
- **Para qué:** dar de baja completamente a un proceso terminado, liberando su entrada del registro.
- **Cómo funciona:** búsqueda O(n) por `list_size`/`list_get`; corta en el primer match con `break`.

#### `gestor_buscar_proceso_sin_lock(t_gestor_procesos* gestor, uint32_t pid)` — kernel_memory_gestor_procesos.c:57
- **Qué hace:** recorre la lista sin tomar el mutex y devuelve el `t_proceso_km*` con ese PID, o `NULL`.
- **Dónde se usa:** `gestor_buscar_proceso` (mismo archivo, línea 71, que sí toma el lock antes de llamarla);
  directamente (bajo lock ya tomado por el caller) en `gestor_memoria.c:255,331,533,739`,
  `atencion_cpu.c:36` (handler `MENSAJE_CONTEXTO`) y `atencion_scheduler.c:195` (handler
  `MENSAJE_ACTUALIZAR_CONTEXTO`).
- **Para qué:** variante sin lock para componer operaciones atómicas más grandes — el caller toma
  `gestor_lock`/`gestor_unlock` alrededor de varias operaciones (ej. leer el contexto Y mandar la respuesta antes
  de soltar el lock, para que nadie lo mute en el medio).
- **Cómo funciona:** misma búsqueda lineal que `gestor_remover_proceso`, pero de solo lectura.

#### `gestor_buscar_proceso(t_gestor_procesos* gestor, uint32_t pid)` — kernel_memory_gestor_procesos.c:69
- **Qué hace:** toma el mutex, delega en `gestor_buscar_proceso_sin_lock`, libera el mutex.
- **Dónde se usa:** `gestor_obtener_instruccion` (mismo archivo, línea 85).
- **Para qué:** versión "de un solo uso" para callers que solo necesitan el puntero momentáneamente (no van a leer
  campos mutables después de soltar el lock en el mismo paso atómico).
- **Cómo funciona:** wrapper de conveniencia sobre la versión sin lock.

#### `gestor_obtener_instruccion(t_gestor_procesos* gestor, uint32_t pid, uint32_t pc, t_log* logger)` — kernel_memory_gestor_procesos.c:84
- **Qué hace:** busca el proceso por PID; si existe, abre su `path_instrucciones` con `fopen`, lee línea por línea
  con `fgets` hasta llegar a la línea número `pc` (0-indexed), le saca el `\n` final y la devuelve como string
  nuevo (`strdup`); si `pc` excede el archivo, devuelve `NULL`.
- **Dónde se usa:** `atencion_cpu.c:70`, dentro del handler de `MENSAJE_PEDIR_INSTRUCCION` — se dispara cada vez
  que una CPU hace fetch de la instrucción en el PC actual del proceso que está ejecutando.
- **Para qué:** es el "fetch" real de instrucciones (reemplaza el mock de CP2 que hacía `memoria_instrucciones_fetch`
  en memoria — ver nota sobre código muerto más abajo). Loguea `"## PID: %u - Obtener instrucción: %u -
  Instrucción: %s"`, uno de los logs obligatorios de la cátedra.
- **Cómo funciona:** **relee el archivo completo desde disco en cada fetch** (no cachea las instrucciones en
  memoria); es O(pc) por llamada porque tiene que iterar línea por línea hasta la deseada — para los tamaños de
  test del TP es aceptable, pero no escala. Devuelve memoria que el caller (`atencion_cpu.c`) debe `free`.

#### `gestor_lock(t_gestor_procesos* gestor)` / `gestor_unlock(t_gestor_procesos* gestor)` — kernel_memory_gestor_procesos.c:76,80
- **Qué hace:** wrappers directos de `pthread_mutex_lock`/`unlock` sobre `gestor->mutex`.
- **Dónde se usa:** `atencion_scheduler.c:194,199` y `atencion_cpu.c:35,39,57` (para envolver
  `gestor_buscar_proceso_sin_lock` + lectura/escritura de campos mutables en una sola sección crítica);
  `gestor_memoria.c:254,264,330,342,531,545,738,750` (`gestor_memoria_crear_segmento`,
  `gestor_memoria_eliminar_segmento`/`finalizar_proceso`, `gestor_memoria_suspender_proceso`,
  `gestor_memoria_dessuspender_proceso` — todas necesitan tocar `tabla_segmentos` del proceso de forma atómica).
- **Para qué:** exponer el mutex interno del gestor a otros módulos (`gestor_memoria.c`) sin romper el
  encapsulamiento del struct (`t_gestor_procesos` no expone su mutex directamente en el header, solo estas dos
  funciones).
- **Cómo funciona:** un único mutex global del gestor protege tanto la lista de procesos como el contenido mutable
  de cada `t_proceso_km` (registros + tabla de segmentos) — diseño simple pero significa que un fetch de
  instrucción largo (que no toma este lock, ver arriba) no bloquea otras operaciones, mientras que crear/eliminar
  segmentos sí serializa contra el resto.

> **Código muerto detectado:** `memoria_instrucciones/memoria_instrucciones.h` declara
> `memoria_instrucciones_fetch(pid, pc)`, comentado como "Mock de memoria de instrucciones para CP2", pero no
> existe ningún `.c` que la implemente ni ningún caller en todo el repo (`grep` no encontró usos reales, solo la
> declaración). Quedó reemplazada por `gestor_obtener_instruccion` y nunca se borró el header.

---

### `conexiones/conexiones.c` / `.h`
Responsabilidad: levantar el socket servidor de KM, aceptar conexiones y despachar cada una según su handshake.

#### `iniciar_servidor_kernel_memory(puerto, config, gestor, gestor_memoria, logger)` — conexiones.c:20
- **Qué hace:** guarda `config`, `gestor`, `gestor_memoria` y `logger` en variables **estáticas de módulo**
  (`_config`, `_gestor`, `_gestor_memoria`, `_logger`); crea el socket de escucha con `crear_servidor(puerto)`
  (de `utils/sockets`); entra en un loop infinito de `accept()` que por cada conexión nueva lanza un hilo
  `manejar_cliente` (`pthread_create` + `pthread_detach`, es decir, hilos "fire and forget" que se limpian solos
  al terminar).
- **Dónde se usa:** `main.c:60` — es la última llamada de `main`, bloquea el hilo principal para siempre.
- **Para qué:** loop de aceptación de conexiones del servidor de KM; el uso de globales estáticas evita tener que
  pasar 4 punteros a cada hilo manualmente (los hilos los toman directo de las variables de módulo).
- **Cómo funciona:** cada `accept()` exitoso reserva un `int*` en el heap para pasarle el fd al hilo nuevo (patrón
  típico en C con pthreads, porque no se puede pasar un `int` por valor directo a `pthread_create` sin undefined
  behavior de aliasing entre iteraciones del loop).

#### `atender_memory_stick(int fd)` *(static)* — conexiones.c:56
- **Qué hace:** espera recibir `MENSAJE_INFORMAR_TAMANIO`; si no llega ese opcode primero, rechaza la conexión
  (`log_warning` + return). Si llega, lee `tamanio` (u32), `ip_len` + `ip` (string variable), `port_len` +
  `puerto` (string variable); llama a `gestor_memoria_registrar_stick(...)` para darlo de alta; si el registro
  falla (`id_stick == -1`) corta. Si tuvo éxito, entra en un loop `recv` de 1 byte que **descarta todo lo que
  llegue** — cuando ese `recv` retorna `<= 0` (el stick se desconectó), llama a
  `gestor_memoria_stick_desconectado`.
- **Dónde se usa:** `manejar_cliente` (mismo archivo, línea 118), cuando el handshake es `HANDSHAKE_MEMORY_STICK`.
- **Para qué:** es el canal de **control** del stick (distinto del canal de **datos** que KM abre hacia el stick
  como cliente para leer/escribir bytes — ver `gestor_memoria_registrar_stick` en el otro documento). Este hilo
  vive todo el tiempo que el stick esté conectado, solo para detectar la caída.
- **Cómo funciona:** el `while (recv(...) > 0)` es la técnica estándar para "esperar a que el peer cierre la
  conexión" sin hacer polling — bloquea hasta que el socket se cierra o llega basura, momento en que se asume
  caída y se dispara la notificación de memoria corrupta hacia el KS.

#### `manejar_cliente(void* arg)` — conexiones.c:89
- **Qué hace:** castea `arg` a `int*`, extrae el fd y libera el puntero; recibe el primer `int32_t` (handshake) y
  hace `switch`:
  - `HANDSHAKE_CPU (1)` → `atender_cpu(...)`
  - `HANDSHAKE_SCHEDULER (3)` → `atender_scheduler(...)`
  - `HANDSHAKE_SCHEDULER_NOTIF (6)` → registra el fd como canal de notificaciones (`gestor_memoria_set_notif_ks`)
    y **retorna sin cerrar el fd** (el canal queda abierto indefinidamente para que KM empuje mensajes async).
  - `HANDSHAKE_MEMORY_STICK (4)` → `atender_memory_stick(fd)`.
  - `HANDSHAKE_SWAP (5)` → espera `MENSAJE_INFORMAR_TAMANIO` con `{total, block}`, llama a
    `gestor_memoria_registrar_swap`, y también **retorna sin cerrar el fd** (SWAP queda como conexión persistente
    que `gestor_memoria` reutiliza bajo demanda para leer/escribir bloques).
  - `default` → warning y se cierra la conexión.
  Al final del `switch` (salvo los dos casos que retornan antes), cierra el fd.
- **Dónde se usa:** disparado por `pthread_create` desde `iniciar_servidor_kernel_memory` (línea 49), una vez por
  cada `accept()`. (Nombre repetido en `kernel_scheduler` y `memory_stick` — son funciones `static`-equivalentes
  distintas por módulo, mismo patrón de diseño, no hay colisión real de símbolos porque cada una vive en su propio
  binario.)
- **Para qué:** es el despachador central que convierte "una conexión TCP nueva" en "atenderla con el handler
  correcto según quién se identificó".
- **Cómo funciona:** el diseño de handshake es deliberadamente simple — un solo entero define el rol del peer, sin
  autenticación ni versión de protocolo. Los casos `SCHEDULER_NOTIF` y `SWAP` son especiales porque el fd sobrevive
  al hilo que lo aceptó: queda guardado dentro de `t_gestor_memoria` (`fd_notif_ks`, `fd_swap`) y otros hilos
  (los que atienden al KS o a la CPU) lo usan más tarde para escribir sobre él directamente.

---

### `atencion_cpu/kernel_memory_atencion_cpu.c` / `.h`
Responsabilidad: atender el canal KM↔CPU: identificación de la CPU, envío de `SEGMENT_MAX_SIZE`, y loop de
atención de 3 tipos de pedido (contexto, fetch de instrucción, topología de sticks).

#### `atender_cpu(fd, gestor, gestor_memoria, config, logger)` — kernel_memory_atencion_cpu.c:10
- **Qué hace:**
  1. Recibe el `id_cpu` (int32) que la CPU manda después del handshake, loguea `"## CPU %d Conectada"`.
  2. Manda de vuelta `SEGMENT_MAX_SIZE` (de la config de KM) — es el límite que la CPU va a usar para validar
     `MEM_ALLOC` del lado suyo antes de pedirlo a KM.
  3. Entra en un loop `recv` de `t_codigo_mensaje` con 3 casos:
     - **`MENSAJE_CONTEXTO`**: recibe `pid`; busca el proceso bajo lock; si no existe, manda registros vacíos y
       `cant=0`; si existe, manda `contexto->registros` completo, la cantidad de segmentos de
       `tabla_segmentos`, y cada `t_segmento` uno por uno — todo dentro de la misma sección crítica
       (`gestor_lock`/`unlock`) para que nadie mute la tabla mientras se serializa. Es lo que la CPU pide antes
       de arrancar/reanudar la ejecución de un proceso (para tener su MMU poblada).
     - **`MENSAJE_PEDIR_INSTRUCCION`**: recibe `pid` y `pc`; duerme `instruction_delay` ms (`usleep`, simula
       latencia de "traer la instrucción"); llama a `gestor_obtener_instruccion`; responde `len=0` si no hay
       instrucción, o `len` + el string si sí.
     - **`MENSAJE_TOPOLOGIA_STICKS`**: serializa la topología completa de sticks conectados
       (`gestor_memoria_serializar_topologia`) y la manda entera; la CPU la usa para saber a qué IP:puerto
       conectarse por cada rango de direcciones físicas.
     - `default`: warning, sigue el loop (no corta la conexión por un mensaje desconocido).
  El loop termina cuando `recv` del código de mensaje retorna `<=0` (CPU desconectada), logueando el cierre.
- **Dónde se usa:** `conexiones.c:104`, cuando el handshake es `HANDSHAKE_CPU`.
- **Para qué:** es el único punto de contacto entre una CPU individual y KM durante la ejecución de instrucciones
  — nótese que **no** atiende lectura/escritura física de memoria de usuario (eso la CPU lo hace directo contra
  los Memory Sticks, según la arquitectura del protocolo: KM solo le pasa la topología, no intermedia los bytes).
- **Cómo funciona:** un hilo por CPU conectada, vive mientras dure la conexión; todas las respuestas son
  síncronas request→response sobre el mismo fd (no hay pipelining).

---

### `atencion_scheduler/kernel_memory_atencion_scheduler.c` / `.h`
Responsabilidad: atender el canal principal KS↔KM — es el handler más grande de KM (9 opcodes), responsable de
todo el ciclo de vida de procesos y memoria que dispara el planificador.

#### `atender_scheduler(fd, gestor, gestor_memoria, config, logger)` — kernel_memory_atencion_scheduler.c:9
- **Qué hace:** loguea la conexión del KS y entra en un loop `recv` de `t_codigo_mensaje` con el siguiente switch
  completo:

  | Opcode | Origen/sentido | Qué hace el handler |
  |---|---|---|
  | `MENSAJE_ENVIAR_PID` | KS→KM, alta de proceso | Recibe `pid` + `path` (nombre relativo del archivo de instrucciones); arma `path_completo = SCRIPTS_BASEPATH + "/" + path`; llama `gestor_agregar_proceso`; loguea `"## PID: %u - Proceso Creado"`; responde `MENSAJE_OK`. |
  | `MENSAJE_ESPACIO_LIBRE` | KS→KM, consulta | Llama `gestor_memoria_espacio_libre` y devuelve el `uint32_t` directo (sin opcode de respuesta). Lo usa el planificador de largo plazo para decidir si admite un proceso de NEW a READY. |
  | `MENSAJE_CREAR_SEGMENTO` | KS→KM, syscall MEM_ALLOC | Recibe `{pid, id_segmento, tamanio}`; llama `gestor_memoria_crear_segmento`. Si el resultado es `CREAR_SEGMENTO_NECESITA_COMPACTACION`, dispara el **protocolo síncrono de compactación sobre la misma conexión**: manda `MENSAJE_COMPACTACION` al KS, bloquea esperando `MENSAJE_DESALOJO_OK` (si no llega o llega otra cosa, loguea error y **corta la atención del KS entero** con `return`), llama `gestor_memoria_compactar`, y reintenta `gestor_memoria_crear_segmento` una vez más. Responde `MENSAJE_OK`/`MENSAJE_ERROR` según el resultado final. |
  | `MENSAJE_ELIMINAR_SEGMENTO` | KS→KM, syscall MEM_FREE | Recibe `{pid, id_segmento}`; llama `gestor_memoria_eliminar_segmento`; responde OK/ERROR. |
  | `MENSAJE_FINALIZAR_PROCESO` | KS→KM, fin de proceso | Recibe `pid`; llama `gestor_memoria_finalizar_proceso` (libera todos los segmentos del proceso); responde `MENSAJE_OK` siempre. |
  | `MENSAJE_LEER_MEMORIA` | KS→KM, camino STDOUT | Recibe `{pid, dir_logica, tamanio}`; reserva buffer; llama `gestor_memoria_io_rw(..., escribir=false, ...)` que traduce la dirección lógica a física **en el momento** (importante: la traducción se hace acá, no antes, para seguir siendo válida aunque el proceso haya sido compactado o suspendido/restaurado mientras estaba bloqueado en la IO); responde OK/ERROR y, si OK, manda el buffer leído. Loguea `"## PID: %u - Lectura - Dir. Física: %u - Tamaño: %u"` solo si la dirección física es válida (no vino de un segmento en SWAP, ver `UINT32_MAX` como sentinel). |
  | `MENSAJE_ESCRIBIR_MEMORIA` | KS→KM, camino STDIN | Simétrico al anterior: recibe `{pid, dir_logica, tamanio}` + el contenido a escribir; llama `gestor_memoria_io_rw(..., escribir=true, ...)`; loguea `"## PID: %u - Escritura - ..."` y responde OK/ERROR. |
  | `MENSAJE_SUSPENDER_PROCESO` | KS→KM, mediano plazo | Recibe `pid`; llama `gestor_memoria_suspender_proceso` (mueve todos los segmentos del proceso a SWAP); responde OK/ERROR. |
  | `MENSAJE_DESSUSPENDER_PROCESO` | KS→KM, mediano plazo | Recibe `pid`; llama `gestor_memoria_dessuspender_proceso` (restaura desde SWAP a memoria real); responde OK/ERROR (ERROR si no entra sin compactar — nótese que acá KM **no** dispara el protocolo de compactación como en `MENSAJE_CREAR_SEGMENTO`, simplemente falla y es el KS quien decide reintentar más tarde vía `planificador_evento_memoria_liberada`). |
  | `MENSAJE_ACTUALIZAR_CONTEXTO` | KS→KM, tras cada ciclo de CPU | Recibe `{pid, t_registros_cpu}`; busca el proceso bajo lock y sobreescribe `proc->contexto->registros`; responde `MENSAJE_OK` siempre (no valida que el PID exista — si no existe, simplemente no hace nada y responde OK igual). |
  | `default` | — | `log_warning` y sigue el loop. |

  El loop termina cuando `recv` retorna `<=0` (KS desconectado), logueando el cierre.
- **Dónde se usa:** `conexiones.c:107`, cuando el handshake es `HANDSHAKE_SCHEDULER` (canal de **control**, distinto
  del canal `HANDSHAKE_SCHEDULER_NOTIF` que se resuelve directo en `conexiones.c` sin pasar por acá).
- **Para qué:** es el corazón funcional de KM del lado del Scheduler — todo lo que el planificador necesita de la
  memoria (crear/eliminar segmentos, consultar espacio libre, leer/escribir para IO, suspender/restaurar) pasa por
  este único loop, en un solo hilo por conexión de KS (el KS abre una sola conexión de control, así que este
  handler corre en un único hilo durante toda la vida del sistema).
- **Cómo funciona:** el punto más delicado es `MENSAJE_CREAR_SEGMENTO`: es la única ruta donde KM le "devuelve la
  pelota" al KS a mitad de un mensaje (pidiéndole que desaloje CPUs) **sobre la misma conexión** que se usa para
  todo lo demás — mientras dura la compactación, el KS no puede mandar ningún otro mensaje de control por ese fd
  porque este hilo está bloqueado en el `recv` de `MENSAJE_DESALOJO_OK`. Esto explica por qué existe el canal
  separado `HANDSHAKE_SCHEDULER_NOTIF`: para que KM pueda notificar cosas (memoria corrupta, más memoria) sin
  competir por el mismo fd que este protocolo síncrono de compactación usa.

---

## Notas cruzadas para el resto del equipo

- **Todo acceso a `tabla_segmentos` o `registros` de un `t_proceso_km`** debe hacerse bajo
  `gestor_lock`/`gestor_unlock` — no hay excepciones en el código actual, y ambos handlers de red
  (`atencion_cpu`, `atencion_scheduler`) lo respetan.
- **`gestor_obtener_instruccion` relee el archivo en cada fetch** — si alguien va a optimizar KM, ese es el punto
  obvio (cachear las instrucciones parseadas por proceso en vez de reabrir el `.txt` en cada PC).
- **`memoria_instrucciones.h` es código muerto** — candidato a borrar si se hace limpieza, no lo referencia nada.
- El único mensaje que KM responde **sin opcode de respuesta explícito** es `MENSAJE_ESPACIO_LIBRE` (manda el
  `uint32_t` pelado) — todo lo demás usa el patrón `MENSAJE_OK`/`MENSAJE_ERROR`.
