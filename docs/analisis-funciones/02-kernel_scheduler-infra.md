# Módulo kernel_scheduler — Infraestructura (main, config, logger, pcb, mutex, conexiones)

> Cubre todo `kernel_scheduler/src/` **excepto** `planificador/planificador.c` y `planificador/planificador.h`
> (el núcleo de planificación se documenta aparte). El foco de este documento es: cómo arranca el proceso,
> cómo se representa un proceso (PCB) y un mutex, y sobre todo **cómo el servidor TCP atiende a las CPUs y
> a IO** — que es el archivo más denso (`conexiones.c`, 511 líneas).

---

## `main.c`

Responsabilidad: punto de entrada del binario `kernel_scheduler`. Orquesta el arranque completo y no
contiene lógica de negocio propia.

#### `main(argc, argv)` — main.c:11
- **Qué hace:** valida argumentos (`config` + `proceso inicial`), crea logger de boot, carga la config,
  crea el logger definitivo, se conecta a Kernel Memory, crea el planificador, abre el canal de
  notificaciones KM→KS, lanza los 2 hilos de planificación (largo y corto plazo), crea el PCB del PID 0
  (proceso inicial) y lo encola en NEW, y finalmente llama a `iniciar_servidor_kernel_scheduler` (bloqueante).
- **Dónde se usa:** es `main`, entry point del binario. No tiene callers en código.
- **Para qué:** es el "director de orquesta" del arranque: nada en el resto del módulo se ejecuta si esto
  no corre en este orden.
- **Cómo funciona:** orden estricto y con salida temprana (`EXIT_FAILURE`) ante cualquier fallo de config o
  logger. Notar que **conecta a Kernel Memory antes de crear el planificador** (`socket_memoria` se le pasa
  a `planificador_create`) — el planificador necesita ese socket para pedirle memoria a KM más adelante
  (crear/eliminar segmentos, actualizar contexto). Los hilos de planificación se lanzan `pthread_detach`
  antes de levantar el servidor, así que ya están corriendo (compitiendo por PCBs en las colas) cuando
  entra el primer cliente. El servidor (`iniciar_servidor_kernel_scheduler`) es un `while(1)` que nunca
  vuelve en operación normal; el código de limpieza después de la llamada es en la práctica inalcanzable
  salvo que el socket de escucha falle al crearse (ver conexiones.c).

---

## `config/kernel_scheduler_config.c` + `.h`

Responsabilidad: cargar y exponer el `.cfg` del KS como struct tipado (`t_kernel_scheduler_config`).

Campos del struct: `log_level`, `planification_algorithm` (ej. "CMN"), `queues_algorithms` (array de
strings, uno por nivel de cola — ej. `["FIFO","RR","RR"]`), `rr_quantum`, `queue_preemption` ("TRUE"/"FALSE"),
`suspension_timeout`, `listen_ip`/`listen_port` (propio), `ip_memory`/`port_memory` (de Kernel Memory).

#### `tiene_todas_las_propiedades(t_config* config)` *(static)* — kernel_scheduler_config.c:7
- **Qué hace:** chequea con `config_has_property` que estén las 10 claves obligatorias del `.cfg`.
- **Dónde se usa:** solo dentro de `kernel_scheduler_config_create` (mismo archivo, línea 28). Es `static`.
- **Para qué:** falla rápido y con log claro si falta una clave, en vez de crashear más adelante con un
  `strdup(NULL)`.
- **Cómo funciona:** una cadena de `&&` que corta en corto (short-circuit) — no dice cuál falta específicamente,
  solo que falta alguna.

#### `kernel_scheduler_config_create(char* path, t_log* logger)` — kernel_scheduler_config.c:21
- **Qué hace:** abre el archivo de config (`config_create` de commons), valida claves, y si están todas
  aloca y llena el struct `t_kernel_scheduler_config` con `strdup`/`config_get_int_value`/`config_get_array_value`.
- **Dónde se usa:** `main.c:31` (una sola vez, al arrancar).
- **Para qué:** punto único de lectura de configuración; todo el resto del módulo lee del struct, no del
  archivo directamente.
- **Cómo funciona:** si `config_create` falla (archivo no existe / mal formado) o si falta una propiedad,
  loguea el error, destruye lo que haya alocado (`config_destroy`) y devuelve `NULL` — el caller (`main`)
  corta la ejecución. Notar que `queues_algorithms` se guarda como el array crudo que devuelve
  `config_get_array_value` (no se copia elemento a elemento), y se libera después con
  `string_array_destroy` (no con `free` simple).

#### `kernel_scheduler_config_log(t_kernel_scheduler_config* config, t_log* logger)` — kernel_scheduler_config.c:51
- **Qué hace:** vuelca todos los campos del config al logger dado (típicamente el boot logger), incluyendo
  un loop para listar cada elemento de `queues_algorithms`.
- **Dónde se usa:** `main.c:38`, inmediatamente después de cargar la config.
- **Para qué:** trazabilidad — poder confirmar en el log de arranque con qué parámetros efectivamente
  levantó el módulo (típico requisito de la cátedra).
- **Cómo funciona:** el loop de `queues_algorithms` recorre hasta encontrar `NULL` (el array viene
  NULL-terminated, convención de `commons`).

#### `kernel_scheduler_config_destroy(t_kernel_scheduler_config* config)` — kernel_scheduler_config.c:71
- **Qué hace:** libera cada string del struct (`free`) y el array de algoritmos con
  `string_array_destroy`, y por último el struct mismo.
- **Dónde se usa:** `main.c:43`, `main.c:63` (paths de error temprano) y `main.c:90` (limpieza final,
  técnicamente inalcanzable en operación normal porque el servidor no retorna).
- **Para qué:** evitar leaks del único config vivo durante toda la corrida del proceso.
- **Cómo funciona:** `if (config == NULL) return;` primero, así es seguro llamarla incluso si
  `kernel_scheduler_config_create` falló a mitad de camino (aunque en ese caso ya se limpia internamente
  antes de devolver `NULL`, por lo que este `NULL`-guard es defensivo más que necesario en el flujo actual).

---

## `logger/kernel_scheduler_logger.c` + `.h`

Responsabilidad: crear las dos instancias de `t_log*` (de la librería `commons`) que usa el módulo.

#### `kernel_scheduler_boot_logger_create(void)` — kernel_scheduler_logger.c:3
- **Qué hace:** crea un logger fijo a `logs/kernel_scheduler_boot.log`, tag `KERNEL_SCHEDULER_BOOT`,
  consola habilitada (`true`), nivel `LOG_LEVEL_INFO` fijo (no configurable).
- **Dónde se usa:** `main.c:12`, antes de que exista la config (por eso es fijo y no lee `log_level`).
- **Para qué:** poder loguear errores de carga de configuración, que ocurren *antes* de tener un config
  válido del cual leer el nivel de log real.
- **Cómo funciona:** delega directo en `log_create` de la librería `commons/log.h`.

#### `kernel_scheduler_logger_create(t_kernel_scheduler_config* config)` — kernel_scheduler_logger.c:12
- **Qué hace:** crea el logger definitivo a `logs/kernel_scheduler.log`, tag `KERNEL_SCHEDULER`, con el
  nivel que indique `config->log_level` (convertido con `log_level_from_string`).
- **Dónde se usa:** `main.c:40`. Es el logger que se pasa a todo el resto del sistema (planificador,
  conexiones, etc.) para el resto de la ejecución.
- **Para qué:** logger "real" del módulo, respeta el nivel configurado por la cátedra (INFO/DEBUG/etc.)
  para no inundar el log en producción.
- **Cómo funciona:** una sola línea, wrapper de `log_create`.

---

## `pcb/pcb.c` + `.h`

Responsabilidad: define `t_pcb`, la estructura central que representa un proceso dentro del KS, y su
ciclo de vida básico (alta/baja). **No** contiene la máquina de estados (eso vive en `planificador.c`).

Campos relevantes del struct (para entender el resto del sistema): `pid`, `path_instrucciones`, `estado`
(`t_estado_proceso` de `utils/estados.h`), `prioridad_original`/`prioridad_actual` (para herencia de
prioridades), `cpu_asignada`, `motivo_bloqueo`/`recurso_bloqueado` (strings descriptivos, ej. "MUTEX" /
nombre del mutex), `bloqueo_id` (contador que invalida timers de suspensión obsoletos si el proceso se
desbloqueó y volvió a bloquearse antes de que venza un timer viejo), `lista_mutex_tomados` (lista de
nombres, para poder liberarlos todos si el proceso termina con mutexes tomados), `registros`
(`t_registros_cpu`, el contexto de ejecución guardado al desalojar) y varios `timespec` de timestamps
(llegada a ready, inicio de exec, inicio de block, suspensión) — probablemente usados para métricas/logs
de tiempos.

#### `pcb_create(uint32_t pid, char* path_instrucciones)` — pcb.c:5
- **Qué hace:** alloca un `t_pcb`, copia el path con `strdup`, inicializa `estado = ESTADO_NEW`,
  prioridades en 0, `cpu_asignada = -1`, punteros de motivo/recurso en `NULL`, `bloqueo_id = 0`, crea la
  lista de mutex tomados vacía, y pone en cero (`memset`) tanto los registros como los 4 timestamps.
- **Dónde se usa:** `main.c:78` (crea el PCB del proceso inicial, PID 0) y
  `kernel_scheduler/src/conexiones/conexiones.c:391` (dentro del handler de `SYSCALL_INIT_PROC`, cuando un
  proceso en ejecución pide crear un hijo).
- **Para qué:** es el único punto de construcción de un PCB — garantiza que todo proceso nuevo arranca en
  un estado consistente y conocido.
- **Cómo funciona:** simple malloc + inicialización campo a campo; no valida `pid` duplicado ni nada — esa
  responsabilidad es de quien la llama (en `SYSCALL_INIT_PROC`, el PID se genera de forma atómica con
  `mutex_pid` en el planificador antes de invocar esta función).

#### `pcb_destroy(t_pcb* pcb)` — pcb.c:31
- **Qué hace:** libera `path_instrucciones`, `motivo_bloqueo` y `recurso_bloqueado` si no son `NULL`,
  destruye `lista_mutex_tomados` con `list_destroy_and_destroy_elements(..., free)`, y libera el struct.
- **Dónde se usa:** exclusivamente dentro de `planificador_destroy` (`planificador.c:200-210`), como
  destructor pasado a `queue_destroy_and_destroy_elements`/`list_destroy_and_destroy_elements` para cada
  una de las colas del planificador (`cola_new`, `colas_ready[i]`, `lista_exec`, `cola_block`,
  `cola_susp_block`, `cola_susp_ready`, `lista_exit`) al cerrar el programa.
- **Para qué:** liberar toda la memoria de un PCB, incluyendo sus sub-estructuras, sin leaks.
- **Cómo funciona:** el `(void*)pcb_destroy` cast en los callers es porque las funciones de destrucción de
  `commons/collections` esperan un `void (*)(void*)` genérico. Es null-safe (`if (pcb == NULL) return;`).

> Nota: `pcb_destroy` **no** se llama en el flujo normal de fin de proceso (`finalizar_proceso` en
> `conexiones.c`) — ahí el PCB se mueve a `lista_exit`, no se libera. Solo se libera masivamente al destruir
> el planificador completo (shutdown del programa).

---

## `mutex_kernel/mutex_kernel.c` + `.h`

Responsabilidad: define `t_mutex_kernel`, el mutex de usuario que el KS administra para las syscalls
`MUTEX_CREATE`/`MUTEX_LOCK`/`MUTEX_UNLOCK`. Es una estructura de datos pura (create/destroy); la lógica de
tomar/liberar/bloquear vive en `planificador.c` (`planificador_crear_mutex`, `planificador_lock_mutex`,
`planificador_unlock_mutex`).

Campos: `nombre`, `pid_duenio` (0 si libre), `tomado` (bool), `cola_bloqueados` (`t_queue*` de `uint32_t*`
con los PIDs esperando, en orden FIFO de llegada).

#### `mutex_kernel_create(char* nombre)` — mutex_kernel.c:5
- **Qué hace:** alloca el struct, copia el nombre, inicializa `pid_duenio = 0`, `tomado = false`, y crea
  la cola de bloqueados vacía.
- **Dónde se usa:** `planificador.c:1376` y `planificador.c:1391`, ambos dentro de
  `planificador_crear_mutex` — el primero cuando el mutex no existía (`SYSCALL_MUTEX_CREATE` explícita),
  el segundo aparentemente como fallback de creación implícita al hacer `lock` de un mutex inexistente
  (ver análisis de `planificador.c` para el detalle del flujo).
- **Para qué:** dar de alta un recurso de exclusión mutua nombrado por el usuario.
- **Cómo funciona:** construcción directa, sin validar nombre duplicado (esa validación, si existe, está
  en `planificador_crear_mutex`, no acá).

#### `mutex_kernel_destroy(t_mutex_kernel* mutex)` — mutex_kernel.c:14
- **Qué hace:** libera el nombre, destruye la cola de bloqueados liberando cada elemento (`free`, porque
  son `uint32_t*` heap-allocados), y libera el struct.
- **Dónde se usa:** `planificador.c:213`, dentro de `planificador_destroy`, como destructor de
  `pl->lista_mutex` completa al cerrar el programa.
- **Para qué:** liberar memoria de todos los mutexes de usuario al finalizar.
- **Cómo funciona:** null-safe, delega en `queue_destroy_and_destroy_elements`.

---

## `io_scheduler/io_scheduler.h`

Solo header, sin `.c` — define tipos de datos usados por `planificador.c` para modelar pedidos de E/S.
No exporta funciones.

- `t_tipo_io` (enum): `IO_SLEEP`, `IO_STDIN`, `IO_STDOUT` — los 3 tipos de dispositivo/operación que el KS
  reconoce. Se usa en todo el módulo para tipar tanto las IOs conectadas (`t_cpu_conectada`/estructura
  análoga en `planificador.h`) como los pedidos encolados.
- `t_pedido_io` (struct): `pid`, `tipo`, `tiempo_ms` (relevante solo para `IO_SLEEP`), `direccion_logica` y
  `tamanio` (relevantes solo para `IO_STDIN`/`IO_STDOUT`), `contenido` (buffer opcional, `NULL` salvo
  cuando hay datos a escribir). Es el "pedido" que el corto plazo arma y pasa a
  `planificador_ejecutar_io_async` (en `planificador.c`) para que un hilo lo despache al dispositivo IO
  real vía socket.

---

## `conexiones/conexiones.c` + `.h`

**El archivo más importante de este análisis.** Es el servidor TCP multi-hilo del KS: acepta conexiones de
CPUs e IOs, identifica a cada cliente por handshake, y contiene el bucle que atiende **todas** las syscalls
que las CPUs disparan durante la ejecución de un proceso. También abre y atiende el canal de notificaciones
asíncronas que Kernel Memory usa para avisarle al KS eventos que no dispara ninguna CPU (más memoria
disponible, memoria corrupta).

Variables globales `static` del archivo (`_logger`, `_planificador`, `_config`): se setean una vez en
`iniciar_servidor_kernel_scheduler` (y se re-setean en `conectar_canal_notificaciones_km`) y las usan todas
las funciones internas del archivo — patrón típico de este código base (cada módulo tiene su propio
"contexto global" en vez de pasar todo por parámetro a cada hilo).

### Handshake y dispatch por tipo de cliente

#### `iniciar_servidor_kernel_scheduler(puerto, planificador, config, logger)` — conexiones.c:20
- **Qué hace:** guarda los punteros globales, crea el socket servidor con `crear_servidor(puerto)` (de
  `utils/sockets`), y entra en un `while(1)` de `accept()` infinito: por cada conexión aceptada, lanza un
  hilo `manejar_cliente` detached.
- **Dónde se usa:** `main.c:82`. Es la última llamada de `main`, bloqueante.
- **Para qué:** es el servidor TCP del KS — todo lo que le llega desde CPU o IO entra por acá.
- **Cómo funciona:** cada conexión aceptada se convierte en un `int*` heap-allocado (el fd) pasado como
  `arg` al hilo nuevo — patrón necesario porque `pthread_create` solo acepta un `void*` y el fd es un
  entero que no puede pasarse "por valor" de forma portable sin ese malloc intermedio. Si `accept()` falla
  simplemente loguea y sigue el loop (no aborta el servidor por un error puntual de aceptación).

#### `manejar_cliente(void* arg)` — conexiones.c:49
- **Qué hace:** libera el `int*` del fd (copiando el valor), recibe el primer `int32_t` (handshake) del
  socket, y según su valor:
  - `HANDSHAKE_CPU`: recibe el `id_cpu` (con fallback a un ID secuencial si falla la recepción — genera
    `id_cpu = list_size(cpus_conectadas) + 1` bajo lock de `mutex_cpus`), registra la CPU en el
    planificador (`planificador_registrar_cpu`) y entra en el loop bloqueante
    `atender_peticiones_cpu(fd_conexion, id_cpu)` (este hilo queda dedicado a esa CPU para siempre).
  - `HANDSHAKE_IO`: recibe el nombre de la interfaz (longitud + string), lo mapea a `t_tipo_io`
    (`"STDIN"`→`IO_STDIN`, `"STDOUT"`→`IO_STDOUT`, `"SLEEP"`→`IO_SLEEP`; cualquier otro string rechaza la
    conexión con `close`), y registra la IO en el planificador (`planificador_registrar_io`). **Importante:**
    a diferencia del caso CPU, este hilo **no** entra en un loop de atención — deja el fd en manos del pool
    de IO del planificador y termina (el comentario en el código lo aclara explícitamente en la línea 111-113).
  - default: handshake desconocido, cierra la conexión.
- **Dónde se usa:** invocada por `pthread_create` desde `iniciar_servidor_kernel_scheduler:44`. Hay
  funciones homónimas en `kernel_memory/conexiones.c` y `memory_stick/conexiones.c` — mismo patrón de
  diseño replicado en cada módulo servidor, pero son funciones independientes (mismo nombre, distinto
  archivo/binario).
- **Para qué:** es el punto único donde el KS decide "quién me está hablando" y deriva el flujo. Es la
  puerta de entrada de todo el tráfico entrante.
- **Cómo funciona:** noten la asimetría clave: **una CPU ocupa un hilo dedicado indefinidamente** (loop
  bloqueante en `atender_peticiones_cpu`), mientras que **una IO no ocupa hilo propio** — su fd queda
  guardado y son los hilos que despachan operaciones de E/S (`hilo_ejecutar_io`, en `planificador.c`,
  lanzados bajo demanda) los que hacen `send`/`recv` directamente sobre ese fd cuando hace falta.

### Finalización de procesos y liberación de CPU (helpers static)

#### `liberar_cpu(t_cpu_conectada* cpu)` *(static)* — conexiones.c:128
- **Qué hace:** bajo `mutex_cpus`, marca `cpu->libre = true`, resetea `pid_ejecutando = 0` y
  `en_rafaga = false`; fuera del lock, hace `sem_post(&planificador->sem_cpus_libres)`.
- **Dónde se usa:** 8 veces dentro de `atender_peticiones_cpu` (líneas 210, 216, 230, 247, 289, 339, 407,
  437) — en cada camino donde el proceso deja de necesitar esa CPU de inmediato (exit, seg fault, sleep,
  stdin/stdout, mutex lock bloqueante, mem_alloc bloqueante, init_proc, interrupción).
- **Para qué:** avisarle al planificador de corto plazo (que espera en ese semáforo) que hay una CPU
  disponible para despachar el próximo proceso de READY.
- **Cómo funciona:** el `sem_post` es justamente la señal que despierta a `hilo_planificador_corto_plazo`
  (en `planificador.c`), que típicamente hace `sem_wait(sem_cpus_libres)` antes de desencolar y despachar.
  Notar que **no se llama** en los casos donde el proceso sigue en la misma CPU (mutex create, mutex lock
  exitoso, mutex unlock, mem_alloc exitoso, mem_free) — ahí se usa `planificador_despachar_directo` en su
  lugar, sin pasar por el semáforo de CPUs libres.

#### `finalizar_proceso(t_pcb* pcb, const char* motivo)` *(static)* — conexiones.c:139
- **Qué hace:** transiciona el PCB a EXIT (`planificador_transicionar_exec_a_exit`), libera **todos** los
  mutexes que el proceso tuviera tomados (recorriendo `pcb->lista_mutex_tomados` y llamando
  `planificador_unlock_mutex` por cada uno — esto despierta al próximo proceso en cola de cada mutex),
  avisa a Kernel Memory que libere toda su memoria (`planificador_finalizar_proceso_km`), y por último
  dispara `planificador_evento_memoria_liberada` (puede haber procesos suspendidos o bloqueados por
  memoria esperando exactamente ese espacio).
- **Dónde se usa:** `conexiones.c:208` (`SYSCALL_EXIT`) y `conexiones.c:214` (`SYSCALL_SEG_FAULT`) — los
  dos únicos motivos de finalización que llegan por syscall de CPU. (Hay otras finalizaciones — ej. BSOD —
  que no pasan por acá, sino por `planificador_bsod` directamente.)
- **Para qué:** centraliza la secuencia correcta de "cerrar" un proceso sin dejar mutexes colgados ni
  memoria huérfana — es fácil olvidarse de alguno de los 3 pasos si no estuviera encapsulado.
- **Cómo funciona:** el `while (list_size(...) > 0)` con `list_get(...,0)` + `strdup` + `unlock_mutex` es
  necesario porque `planificador_unlock_mutex` probablemente remueve el elemento de
  `lista_mutex_tomados` como side-effect (de ahí la copia con `strdup` antes de invocar, para no
  usar-después-de-liberar el string que la lista pueda haber destruido).

### El corazón del archivo: atención de syscalls de CPU

#### `atender_peticiones_cpu(int socket_cpu, int32_t id_cpu)` — conexiones.c:157
- **Qué hace:** loop bloqueante infinito, uno por cada CPU conectada (corre en el hilo que
  `manejar_cliente` le dedicó). En cada iteración: recibe un `t_codigo_mensaje` (si `recv` devuelve `<=0`,
  la CPU se desconectó: `planificador_remover_cpu` + `close` + rompe el loop); busca la `t_cpu_conectada`
  por `id_cpu` y el PCB actualmente en ejecución en esa CPU (buscando en `lista_exec` por
  `pid_ejecutando`); y despacha según el código de mensaje.
- **Dónde se usa:** llamada una única vez por CPU, desde `manejar_cliente:83`, inmediatamente después de
  `planificador_registrar_cpu`.
- **Para qué:** es el punto donde el KS reacciona a **todo** lo que una CPU le informa mientras ejecuta un
  proceso — syscalls del programa de usuario e interrupciones que la propia CPU decide devolver (fin de
  quantum, desalojo por prioridad, compactación).
- **Cómo funciona (mecanismo detallado):**
  1. Antes de mirar el código de mensaje, bajo `mutex_cpus` lee y **resetea** en la `t_cpu_conectada`:
     `pid_ejecutando` (a variable local `pid_ejec`), `motivo_interrupcion` (a `motivo_int`, y lo resetea a
     `INT_NINGUNA`), `pid_preemptor`/`prioridad_preemptor` (por si fue un desalojo por prioridad), y pone
     `en_rafaga = false`. Esto es necesario porque el flujo puede tardar en resolver la syscall y otro hilo
     podría estar mirando el estado de esa CPU mientras tanto.
  2. **`MENSAJE_SYSCALL`**: recibe el `t_codigo_syscall` y despacha por `if/else if` (no `switch`) sobre
     10 casos posibles:
     - `SYSCALL_EXIT` → `finalizar_proceso(pcb, "SUCCESS")` + `liberar_cpu`.
     - `SYSCALL_SEG_FAULT` → `finalizar_proceso(pcb, "SEG_FAULT")` + `liberar_cpu`.
     - `SYSCALL_SLEEP` → recibe `tiempo_ms` y los registros actualizados, transiciona el PCB a BLOCK
       (motivo "SLEEP"), encola un pedido async `IO_SLEEP` (`planificador_ejecutar_io_async`), libera la CPU.
     - `SYSCALL_STDIN`/`SYSCALL_STDOUT` → recibe dirección lógica + tamaño + registros, transiciona a
       BLOCK (motivo "STDIN"/"STDOUT"), encola el pedido async correspondiente, libera la CPU.
     - `SYSCALL_MUTEX_CREATE` → recibe nombre + registros, llama `planificador_crear_mutex`, y **despacha
       de nuevo a la misma CPU** (`planificador_despachar_directo`, sin pasar por `liberar_cpu`/semáforo) —
       crear un mutex no bloquea al creador.
     - `SYSCALL_MUTEX_LOCK` → recibe nombre + registros, intenta `planificador_lock_mutex`; si lo consigue,
       loguea, agrega el nombre a `lista_mutex_tomados` del PCB y re-despacha a la misma CPU; si no, guarda
       el nombre en `recurso_bloqueado`, transiciona a BLOCK (motivo "MUTEX") y libera la CPU (el proceso
       queda en la cola de bloqueados del mutex, gestionada dentro de `planificador_lock_mutex`).
     - `SYSCALL_MUTEX_UNLOCK` → recibe nombre + registros, llama `planificador_unlock_mutex` (que
       internamente puede desbloquear al próximo en cola) y re-despacha a la misma CPU.
     - `SYSCALL_MEM_ALLOC` → recibe `id_segmento` + `tamanio` + registros, pide a KM crear el segmento
       (`planificador_crear_segmento_km`, que devuelve además si hubo compactación por parámetro de
       salida); si OK, re-despacha a la misma CPU (y si hubo compactación dispara
       `planificador_evento_memoria_liberada`, porque el reordenamiento pudo liberar espacio contiguo para
       otros); si no hay espacio ni compactando, transiciona a BLOCK (motivo "MEMORIA"),
       registra el pedido pendiente (`planificador_bloquear_por_memoria`) y libera la CPU.
     - `SYSCALL_MEM_FREE` → recibe `id_segmento` + registros, pide a KM eliminarlo
       (`planificador_eliminar_segmento_km`); en ambos casos (éxito o fallo, solo logueando warning en el
       fallo) re-despacha a la misma CPU; si tuvo éxito además dispara
       `planificador_evento_memoria_liberada`.
     - `SYSCALL_INIT_PROC` → recibe path + prioridad + registros del padre; si el algoritmo es "CMN"
       valida que la prioridad esté en `[0, cant_colas)` (si no, rechaza la creación sin crashear, solo
       loguea error); si es válida, genera un PID nuevo atómicamente (`mutex_pid`), crea el PCB
       (`pcb_create`) con esa prioridad como original y actual, y lo encola en NEW
       (`planificador_encolar_new`); el proceso **padre** (llamante) siempre vuelve a READY (no a la misma
       CPU) con `planificador_transicionar_exec_a_ready` + `liberar_cpu` — a diferencia de las demás
       syscalls "no bloqueantes" de esta lista, INIT_PROC sí libera la CPU del padre.
  3. **`MENSAJE_INTERRUPCION`**: la CPU devuelve el proceso desalojado junto con sus registros actualizados.
     Según `motivo_int` (capturado en el paso 1, antes de que la CPU mandara este mensaje):
     - `INT_COMPACTACION` → vuelve a READY **al frente** de su cola (`..._exec_a_ready_frente`) — prioridad
       de reintento inmediato porque fue desalojado sin haber terminado su ráfaga por una razón ajena a él.
     - `INT_PREEMPCION` → vuelve a READY normal (el log del evento ya se emitió cuando se *envió* la
       interrupción, no acá).
     - `INT_QUANTUM` (o cualquier otro valor no contemplado, vía `default`) → vuelve a READY normal, logueando
       "Desalojado por fin de quantum".
     En los 3 casos, libera la CPU al final.
  4. Cualquier otro `t_codigo_mensaje` no reconocido: `default: break;` — se ignora silenciosamente.
- **Patrón general a notar:** las syscalls que **no** liberan la CPU (mutex create/lock-exitoso/unlock,
  mem_alloc-exitoso, mem_free) son justamente las que la consigna exige que "devuelvan el proceso a la
  misma CPU" — se resuelven de forma síncrona sin volver a pasar por el planificador de corto plazo.

### Conexión con Kernel Memory

#### `conectar_a_kernel_memory(const char* ip, const char* puerto, t_log* logger)` — conexiones.c:446
- **Qué hace:** abre una conexión TCP a KM (`crear_conexion`) y, si tiene éxito, envía el handshake
  `HANDSHAKE_SCHEDULER`.
- **Dónde se usa:** `main.c:55`. Este es el **canal principal** de comandos KS→KM (crear/eliminar
  segmento, actualizar contexto, etc. — ver `planificador.c`). Existen funciones homónimas pero
  independientes con firmas distintas en `cpu/conexiones.c` y `memory_stick/conexiones.c` (mismo nombre,
  cada módulo tiene la suya).
- **Para qué:** establecer el socket que el planificador va a usar durante toda la corrida para hablar con
  KM (se lo pasa como `socket_memoria` a `planificador_create`).
- **Cómo funciona:** si `crear_conexion` falla, loguea error y devuelve `-1` — no aborta el programa acá,
  el chequeo de `-1` lo hace el caller en `main.c`.

#### `conectar_canal_notificaciones_km(ip, puerto, planificador, logger)` — conexiones.c:492
- **Qué hace:** re-setea `_logger`/`_planificador` (globales del archivo), abre una **segunda** conexión a
  KM, envía el handshake `HANDSHAKE_SCHEDULER_NOTIF` (distinto del canal principal), y lanza el hilo
  `hilo_notificaciones_km` detached sobre ese fd.
- **Dónde se usa:** `main.c:68`, después de crear el planificador.
- **Para qué:** separar el canal de **comandos síncronos** (request/response, ej. "creame este segmento")
  del canal de **notificaciones asíncronas** que KM puede emitir en cualquier momento sin que el KS se lo
  pida (más memoria disponible por un stick nuevo, memoria corrupta por caída de un stick). Si usaran el
  mismo socket, un `recv` bloqueante esperando la respuesta de un comando podría en realidad estar
  recibiendo una notificación asíncrona fuera de orden.
- **Cómo funciona:** dos handshakes distintos (`HANDSHAKE_SCHEDULER` vs `HANDSHAKE_SCHEDULER_NOTIF`) le
  permiten a KM (del lado servidor) distinguir en qué canal está cada conexión entrante y tratarlas
  distinto (ver análisis de `kernel_memory/conexiones.c`).

#### `hilo_notificaciones_km(void* arg)` *(static)* — conexiones.c:459
- **Qué hace:** loop bloqueante sobre el fd del canal de notificaciones. Por cada `t_codigo_mensaje`
  recibido:
  - `MENSAJE_MAS_MEMORIA` → recibe `nuevo_total` (uint32_t), loguea, y llama
    `planificador_evento_memoria_liberada` (un stick nuevo o una compactación en KM pudo haber liberado
    espacio contiguo suficiente para procesos suspendidos o bloqueados por memoria).
  - `MENSAJE_MEMORIA_CORRUPTA` → llama `planificador_bsod` **directamente** (comentario en el código:
    "no retorna" — finaliza todos los procesos y presumiblemente termina el programa).
  - cualquier otro código → warning de "notificación desconocida".
  Si `recv` devuelve `<=0` (KM cerró el canal), loguea warning y sale del loop, cerrando el fd.
- **Dónde se usa:** lanzado una única vez desde `conectar_canal_notificaciones_km:506`. No tiene otros
  callers (no es invocada directamente, solo vía `pthread_create`).
- **Para qué:** es el receptor de eventos que KM origina por su cuenta, no como respuesta a un pedido del
  KS. Corre en su propio hilo indefinidamente, en paralelo a `atender_peticiones_cpu` de cada CPU.
- **Cómo funciona:** el cast `(int)(intptr_t) arg` / `(void*)(intptr_t) fd_conexion` es el patrón para
  pasar un entero pequeño como puntero de hilo sin necesitar un malloc intermedio (a diferencia del fd de
  clientes en `manejar_cliente`, que sí usa un `int*` heap-allocado) — funciona porque acá solo hay una
  invocación conocida en el momento de crear el hilo, no una cola de conexiones entrantes.

---

## Resumen de responsabilidades (para ubicarse rápido)

| Archivo | Responsabilidad | Complejidad |
|---|---|---|
| `main.c` | Orquestación de arranque | Baja |
| `config/*` | Carga y expone `.cfg` | Baja |
| `logger/*` | Dos `t_log*` (boot / definitivo) | Trivial |
| `pcb/*` | Struct `t_pcb` + alta/baja | Baja |
| `mutex_kernel/*` | Struct `t_mutex_kernel` + alta/baja | Baja |
| `io_scheduler/io_scheduler.h` | Tipos de datos de pedidos de E/S (sin `.c`) | Trivial |
| `conexiones/*` | Servidor multi-hilo: handshake, dispatch de 10 syscalls + interrupciones, canal de notificaciones KM | **Alta** |
