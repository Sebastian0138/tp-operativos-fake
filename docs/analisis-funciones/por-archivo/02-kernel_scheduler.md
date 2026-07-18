# kernel_scheduler/ — documentación por archivo

## `kernel_scheduler/src/main.c` (93 líneas)

Punto de entrada del proceso. Orquesta el boot completo: logger → config → logger definitivo → conexión a Kernel Memory (KM) → creación del planificador → canal de notificaciones KM→KS → arranque de los hilos de largo y corto plazo → encolado del proceso inicial (PID 0) → arranque del servidor (bloqueante) → limpieza al cerrar.

### `int main(int argc, char** argv)`
**Qué hace y cómo:** Valida `argc == 3` (uso: `./kernel_scheduler [Config] [ProcesoInicial]`). Crea boot logger → carga config con `kernel_scheduler_config_create` → crea logger definitivo con `kernel_scheduler_logger_create` → destruye el boot logger. Se conecta a KM como cliente (`conectar_a_kernel_memory`, guarda el fd en `socket_memoria`). Crea el planificador con `planificador_create(config, logger, socket_memoria)`. Abre el segundo canal hacia KM dedicado a notificaciones async (`conectar_canal_notificaciones_km`). Lanza `hilo_planificador_largo_plazo` y `hilo_planificador_corto_plazo` como hilos detached. Crea el PCB del proceso inicial con `pcb_create(0, argv[2])` y lo encola con `planificador_encolar_new`. Finalmente llama `iniciar_servidor_kernel_scheduler`, que es bloqueante — el proceso vive ahí hasta que ese loop termine (en la práctica, nunca, salvo error al crear el socket de escucha). El código después de esa llamada (`planificador_destroy`, cierres, `log_destroy`) es limpieza que solo se ejecuta si el servidor no pudo levantarse.
**Dónde se usa:** Es el entrypoint del binario `kernel_scheduler`.
**Para qué:** Es la secuencia de arranque completa del módulo — todo el resto del código depende de que estos pasos se hayan hecho en este orden (config antes que logger, logger antes que conexiones, planificador antes que los hilos).

**Dependencias:** `config/kernel_scheduler_config.h`, `logger/kernel_scheduler_logger.h`, `conexiones/conexiones.h`, `planificador/planificador.h`.

---

## `kernel_scheduler/src/config/kernel_scheduler_config.c` (85 líneas) + `.h` (25 líneas)

Carga y valida el archivo de configuración del módulo usando la librería `commons` de la cátedra.

### `t_kernel_scheduler_config` (struct, en el `.h`)
Campos: `log_level` (str), `planification_algorithm` (str: `"FIFO"`/`"RR"`/`"CMN"`), `queues_algorithms` (`char**` — array de algoritmos por cola, solo relevante si `planification_algorithm == "CMN"`), `rr_quantum` (int, ms), `queue_preemption` (str `"TRUE"`/`"FALSE"`, se parsea a bool en otra parte del código), `suspension_timeout` (int, ms — tiempo bloqueado en IO antes de pasar a SUSP_BLOCK), `listen_ip`/`listen_port` (str — donde escucha el propio KS), `ip_memory`/`port_memory` (str — dónde conectarse a Kernel Memory).

### `static bool tiene_todas_las_propiedades(t_config* config)`
**Qué hace:** Chequea con `config_has_property` que estén las 10 claves obligatorias: `LOG_LEVEL`, `PLANIFICATION_ALGORITHM`, `QUEUES_ALGORITHMS`, `RR_QUANTUM`, `QUEUE_PREEMPTION`, `SUSPENSION_TIMEOUT`, `LISTEN_IP`, `LISTEN_PORT`, `IP_MEMORY`, `PORT_MEMORY`.
**Dónde se usa:** Solo dentro de `kernel_scheduler_config_create` (línea 28).
**Para qué:** Falla rápido y con un log claro si al archivo de config le falta una clave, en vez de crashear más adelante con un puntero nulo.

### `t_kernel_scheduler_config* kernel_scheduler_config_create(char* path, t_log* logger)`
**Qué hace y cómo:** Abre el archivo con `config_create(path)`; si falla, loguea error y devuelve `NULL`. Verifica que tenga todas las propiedades (función anterior). Reserva el struct y copia cada valor (`strdup` para strings, `config_get_int_value` para enteros, `config_get_array_value` para el array de algoritmos por cola). Destruye el `t_config* config` de la librería (ya copió todo lo que necesita a su propio struct) y devuelve el struct propio.
**Dónde se usa:** `main.c:31`.
**Para qué:** Es el único punto donde el resto del código toca el archivo de configuración; después de esta llamada todo el módulo trabaja con el struct tipado `t_kernel_scheduler_config`, no con la API genérica de `commons/config`.

### `void kernel_scheduler_config_log(t_kernel_scheduler_config* config, t_log* logger)`
**Qué hace:** Loguea cada parámetro cargado (incluyendo un loop sobre `queues_algorithms` hasta encontrar `NULL`, ya que `config_get_array_value` devuelve un array NULL-terminated).
**Dónde se usa:** `main.c:38`.
**Para qué:** Trazabilidad de arranque — permite confirmar en el log qué configuración efectivamente tomó el proceso.

### `void kernel_scheduler_config_destroy(t_kernel_scheduler_config* config)`
**Qué hace:** Libera cada campo string (incluye `string_array_destroy` para el array de algoritmos) y el struct.
**Dónde se usa:** `main.c:90` (en la ruta de limpieza final, y también en las rutas de error tempranas de `main.c` cuando falla la creación del logger o del planificador).
**Para qué:** Evita leaks del único struct de configuración que vive durante toda la corrida.

---

## `kernel_scheduler/src/logger/kernel_scheduler_logger.c` (19 líneas) + `.h` (10 líneas)

Wrapper mínimo sobre `commons/log` para crear los dos loggers del módulo.

### `t_log* kernel_scheduler_boot_logger_create(void)`
**Qué hace:** `log_create("logs/kernel_scheduler_boot.log", "KERNEL_SCHEDULER_BOOT", true, LOG_LEVEL_INFO)` — logger de nivel fijo `INFO`, usado solo durante el arranque antes de tener la config cargada (por eso no puede usar el `LOG_LEVEL` del archivo de config: ese archivo todavía no se leyó).
**Dónde se usa:** `main.c:12`.
**Para qué:** Loguear los pasos de arranque (incluyendo errores de config) antes de que exista el logger "real".

### `t_log* kernel_scheduler_logger_create(t_kernel_scheduler_config* config)`
**Qué hace:** `log_create("logs/kernel_scheduler.log", "KERNEL_SCHEDULER", true, log_level_from_string(config->log_level))` — usa el nivel configurado por el usuario en `LOG_LEVEL`.
**Dónde se usa:** `main.c:40`.
**Para qué:** Es el logger que usa todo el resto del sistema (`planificador.c`, `conexiones.c`) una vez arrancado — se pasa como puntero global.

---

## `kernel_scheduler/src/pcb/pcb.c` (46 líneas) + `pcb.h` (61 líneas)

Define y administra el Process Control Block, la estructura central del proceso desde el punto de vista del Kernel Scheduler.

### `t_pcb` (struct)
Campos: `pid` (uint32_t), `path_instrucciones` (char*, ruta al archivo de instrucciones), `estado` (`t_estado_proceso`, de `utils/estados.h`), `prioridad_original`/`prioridad_actual` (int — separadas para poder restaurar tras herencia de prioridades), `cpu_asignada` (int, `-1` si ninguna), `motivo_bloqueo` (char*, ej. `"IO_SLEEP"`, `"MUTEX"`, `"MEMORIA"` — se usa tanto para logging como, en el caso `"MEMORIA"`, para decidir si aplica el timer de suspensión), `recurso_bloqueado` (char*, nombre del mutex si el bloqueo es por `MUTEX_LOCK`), `bloqueo_id` (uint32_t — contador que se incrementa en cada entrada a BLOCK; existe específicamente para invalidar timers de suspensión obsoletos cuando un proceso se desbloquea y vuelve a bloquearse antes de que expire un timer viejo), `lista_mutex_tomados` (`t_list*` de `char*` — nombres de mutex que el proceso tiene tomados, para poder liberarlos todos si el proceso termina), `registros` (`t_registros_cpu`, el contexto de ejecución guardado cada vez que la CPU devuelve el proceso), y cuatro `timespec` (`ts_llegada_ready`, `ts_inicio_exec`, `ts_inicio_block`, `ts_suspension`) para métricas/criterios de desempate (la única que se usa activamente en lógica de negocio es `ts_suspension`, para el desempate por antigüedad al des-suspender).

### `t_pcb* pcb_create(uint32_t pid, char* path_instrucciones)`
**Qué hace y cómo:** `malloc` del struct, copia el PID, `strdup` del path, inicializa `estado = ESTADO_NEW`, prioridades en 0, `cpu_asignada = -1`, punteros de motivo/recurso en `NULL`, `bloqueo_id = 0`, crea `lista_mutex_tomados` vacía, y pone en cero (`memset`) tanto los registros de CPU como los cuatro timestamps.
**Dónde se usa:** `main.c:78` (proceso inicial PID 0) y `conexiones.c:391` (dentro del handler de `SYSCALL_INIT_PROC`, para cada proceso nuevo que se crea en runtime).
**Para qué:** Es el único constructor de PCB del sistema — garantiza que todo proceso arranca en un estado consistente conocido.

### `void pcb_destroy(t_pcb* pcb)`
**Qué hace:** Libera `path_instrucciones`, `motivo_bloqueo` y `recurso_bloqueado` si no son `NULL`, destruye `lista_mutex_tomados` con `list_destroy_and_destroy_elements(..., free)`, y libera el struct.
**Dónde se usa:** Vía punteros a función `(void*)pcb_destroy` en `planificador_destroy` (`planificador.c:200-210`, para vaciar las 8 colas/listas de procesos al cerrar el planificador). No se llama individualmente en ningún otro lugar del flujo normal — un proceso que llega a EXIT permanece en `lista_exit` hasta que el planificador se destruye (no hay liberación temprana de PCBs finalizados).
**Para qué:** Evitar leaks del PCB y de sus strings asociados.

**Dependencias:** `utils/estados.h` (para `t_estado_proceso`), `utils/registros.h` (para `t_registros_cpu`), `commons/collections/list.h`.

---

## `kernel_scheduler/src/mutex_kernel/mutex_kernel.c` (19 líneas) + `.h` (29 líneas)

Estructura de datos del mutex de usuario (el semáforo tipo Mutex del lenguaje de scripting del TP). Nótese que la herencia de prioridades NO vive acá — el comentario del propio header lo aclara ("sin herencia de prioridades"): esta estructura es pasiva, la lógica de herencia está en `planificador.c`.

### `t_mutex_kernel` (struct)
Campos: `nombre` (char*), `pid_duenio` (uint32_t, `0` si libre — nótese que `0` es un valor de PID válido en teoría ya que el proceso inicial tiene PID 0, pero en la práctica `pid_duenio == 0` se usa como centinela de "sin dueño" porque el proceso PID 0 nunca hace `MUTEX_LOCK` en los casos de prueba típicos de la cátedra), `tomado` (bool), `cola_bloqueados` (`t_queue*` de `uint32_t*` — PIDs esperando, en orden FIFO real de solicitud).

### `t_mutex_kernel* mutex_kernel_create(char* nombre)`
**Qué hace:** `malloc`, `strdup` del nombre, `pid_duenio = 0`, `tomado = false`, `cola_bloqueados = queue_create()` (vacía).
**Dónde se usa:** `planificador.c:1376` (`planificador_crear_mutex`, ante `SYSCALL_MUTEX_CREATE`) y `planificador.c:1391` (dentro de `planificador_lock_mutex`, creación on-demand si el mutex no existía todavía — ver más abajo).
**Para qué:** Constructor único del tipo.

### `void mutex_kernel_destroy(t_mutex_kernel* mutex)`
**Qué hace:** Libera `nombre` y destruye `cola_bloqueados` con `queue_destroy_and_destroy_elements(..., free)` (libera cada `uint32_t*` pendiente), luego libera el struct.
**Dónde se usa:** Vía puntero a función `(void*)mutex_kernel_destroy` en `planificador_destroy` (`planificador.c:213`), para vaciar `lista_mutex` al cerrar.
**Para qué:** Limpieza al cierre del proceso.

**Dependencias:** `commons/collections/queue.h`.

---

## `kernel_scheduler/src/io_scheduler/io_scheduler.h` (29 líneas — sin `.c`)

Header puro de datos, sin implementación propia — define el vocabulario que usan tanto `conexiones.c` como `planificador.c` para hablar de dispositivos y pedidos de IO.

### `t_tipo_io` (enum): `IO_SLEEP`, `IO_STDIN`, `IO_STDOUT`
**Para qué:** Los 3 tipos de IO que pide el enunciado. Se usa para tipar tanto los dispositivos conectados (`t_io_conectada.tipo` en `planificador.h`) como los pedidos asíncronos (`t_io_async_args.tipo` en `planificador.c`).

### `t_pedido_io` (struct)
Campos: `pid`, `tipo`, `tiempo_ms` (solo para `IO_SLEEP`), `direccion_logica` (solo para `IO_STDIN`/`IO_STDOUT`), `tamanio`, `contenido` (char*, puede ser `NULL`).
**Hallazgo:** Este struct está declarado pero **no se instancia en ningún lugar del código real** (verificado con grep — cero usos fuera de la propia declaración). El pedido de IO asíncrono que sí se usa en la práctica es `t_io_async_args`, definido localmente (`static`, no exportado) dentro de `planificador.c:1206-1212`, con un layout de campos equivalente pero distinto (no reutiliza este tipo). `t_pedido_io` es código muerto — una estructura que probablemente se pensó como la interfaz pública para pasar pedidos de IO y terminó reemplazada por el struct interno de `planificador.c` sin borrar la declaración original.

**Dependencias:** `utils/protocolo.h` (incluido pero sin uso directo visible en este header — probablemente arrastrado de una versión anterior).

---

## `kernel_scheduler/src/conexiones/conexiones.c` (511 líneas) + `.h` (20 líneas)

El servidor TCP del Kernel Scheduler y el dispatcher completo de syscalls de CPU. Es el archivo que traduce entre "mensajes de red" y "llamadas al planificador". Usa tres punteros globales `static` (`_logger`, `_planificador`, `_config`) inicializados una sola vez en `iniciar_servidor_kernel_scheduler`, porque los hilos que atienden clientes (`manejar_cliente`, `atender_peticiones_cpu`) no reciben esos punteros como parámetro — los necesitan disponibles globalmente porque se lanzan con `pthread_create` pasando solo el fd.

### `void iniciar_servidor_kernel_scheduler(const char* puerto, t_planificador* planificador, t_kernel_scheduler_config* config, t_log* logger)`
**Qué hace y cómo:** Guarda los tres punteros en las globales estáticas. Crea el socket de escucha con `crear_servidor(puerto)` (de `utils/sockets`). Entra en un loop infinito de `accept()`: por cada conexión entrante, reserva un `int*` en el heap con el fd (para poder pasarlo como `void*` al hilo sin que se pise con la siguiente iteración), lanza `manejar_cliente` en un hilo detached.
**Dónde se usa:** `main.c:82` — es la llamada bloqueante final de `main`.
**Para qué:** Es el servidor único del módulo: acepta tanto CPUs como IOs por el mismo puerto, distinguiéndolos por handshake dentro de `manejar_cliente`.

### `void* manejar_cliente(void* arg)`
**Qué hace y cómo:** Castea `arg` a `int*`, extrae el fd y libera el puntero temporal. Recibe 4 bytes de handshake (`recv` con `MSG_WAITALL`). Si la conexión se cerró antes del handshake (`bytes == 0`) o hubo error (`bytes == -1`), loguea y cierra. Si el handshake es `HANDSHAKE_CPU`: recibe el `id_cpu` (int32); si falla la recepción, genera un ID secuencial de fallback (`list_size(cpus_conectadas) + 1`, bajo `mutex_cpus`) — luego llama `planificador_registrar_cpu` y entra en el loop bloqueante `atender_peticiones_cpu` (este hilo queda dedicado a esa CPU para siempre, hasta que se desconecte). Si el handshake es `HANDSHAKE_IO`: recibe un nombre length-prefixed (`nombre_len` + bytes), lo mapea a `t_tipo_io` comparando contra los literales `"STDIN"`/`"STDOUT"`/`"SLEEP"` (si no matchea ninguno, rechaza la conexión con un warning), y llama `planificador_registrar_io`. A diferencia del caso CPU, **este hilo termina acá** — el fd de la IO queda "en préstamo" al pool de IOs del planificador; son los hilos `hilo_ejecutar_io` (lanzados desde `planificador_ejecutar_io_async`) los que después leen/escriben ese fd bajo demanda. Cualquier otro valor de handshake se rechaza con warning y cierre de conexión.
**Dónde se usa:** Lanzada por `pthread_create` desde `iniciar_servidor_kernel_scheduler:44` — no se llama directamente en ningún otro punto, siempre vía el puntero a función pasado a `pthread_create`.
**Para qué:** Es el único punto de entrada de conexiones del módulo; decide si un socket entrante pertenece al mundo "CPU" (dedicado, bloqueante, dueño de un hilo propio) o al mundo "IO" (recurso compartido, tomado y liberado bajo demanda).

### `static void liberar_cpu(t_cpu_conectada* cpu)`
**Qué hace:** Bajo `mutex_cpus`, marca `cpu->libre = true`, `pid_ejecutando = 0`, `en_rafaga = false`; fuera del lock, hace `sem_post(&_planificador->sem_cpus_libres)`.
**Dónde se usa:** Se llama 6 veces dentro de `atender_peticiones_cpu` — en todas las ramas de syscall salvo las que hacen `planificador_despachar_directo` (mutex create/lock-exitoso/unlock, mem_alloc exitoso, mem_free) — y una vez más en el handler de `MENSAJE_INTERRUPCION` (línea 437).
**Para qué:** Es el único punto donde una CPU vuelve a estar disponible para el hilo de corto plazo; separar esto en una función evita duplicar la secuencia lock→marcar→unlock→`sem_post` en cada rama del switch.

### `static void finalizar_proceso(t_pcb* pcb, const char* motivo)`
**Qué hace y cómo:** Llama `planificador_transicionar_exec_a_exit`. Luego, en un loop, va sacando el primer elemento de `pcb->lista_mutex_tomados` (nombre de mutex) y llama `planificador_unlock_mutex` para cada uno hasta vaciar la lista — así ningún mutex queda tomado eternamente por un proceso que ya terminó. Avisa a KM con `planificador_finalizar_proceso_km` para que libere los segmentos y estructuras del proceso en memoria. Finalmente llama `planificador_evento_memoria_liberada` porque la liberación de los segmentos del proceso puede haber dejado espacio para des-suspender a otros o satisfacer `MEM_ALLOC`s pendientes.
**Dónde se usa:** Dentro de `atender_peticiones_cpu`, en las ramas `SYSCALL_EXIT` (motivo `"SUCCESS"`) y `SYSCALL_SEG_FAULT` (motivo `"SEG_FAULT"`).
**Para qué:** Centraliza la secuencia completa de "terminar un proceso correctamente" (que no es solo cambiar el estado, sino también soltar recursos) para no duplicarla entre las dos rutas que pueden terminar un proceso.

### `void atender_peticiones_cpu(int socket_cpu, int32_t id_cpu)`
El corazón del archivo — dispatcher central de syscalls de una CPU específica. Es un loop infinito de un solo hilo dedicado a esa CPU.

**Qué hace y cómo, paso a paso por iteración:**
1. `recv` bloqueante de un `t_codigo_mensaje` (4 bytes). Si `bytes <= 0` (desconexión), llama `planificador_remover_cpu` y cierra el socket — sale del loop (el hilo termina).
2. Busca la `t_cpu_conectada*` por `id_cpu` (`planificador_obtener_cpu_por_id`); si no existe, cierra y sale.
3. Bajo `mutex_cpus`, "cosecha" el estado que dejó la CPU al devolver el control: `pid_ejecutando`, `motivo_interrupcion`, `pid_preemptor`, `prioridad_preemptor`; y limpia `en_rafaga = false`, `motivo_interrupcion = INT_NINGUNA` — la CPU deja de estar "en ráfaga" apenas devuelve el control, independientemente de qué haga después el dispatcher.
4. Busca el PCB real en `lista_exec` por ese PID (`pcb_ejecutando`, puede quedar `NULL` en casos borde de timing, y todas las ramas chequean por `NULL` antes de tocarlo).
5. `switch (codigo)`:
   - **`MENSAJE_SYSCALL`**: recibe el `t_codigo_syscall`, loguea `"## (pid) - Solicitó syscall: <nombre>"`, y despacha a un segundo `if/else if` por tipo de syscall (ver detalle abajo).
   - **`MENSAJE_INTERRUPCION`**: recibe los registros actualizados, y según `motivo_int` decide cómo reencolar: `INT_COMPACTACION` → `planificador_transicionar_exec_a_ready_frente` (al principio de su cola, para no perder terreno por un desalojo que no fue "culpa" del proceso) con log `"Desalojado por compactación"`; `INT_PREEMPCION` → `planificador_transicionar_exec_a_ready` normal (el log ya se emitió al mandar la interrupción, en `chequear_desalojo_por_prioridad`); `INT_QUANTUM`/default → `planificador_transicionar_exec_a_ready` con log `"Desalojado por fin de quantum"`. Siempre termina con `liberar_cpu(cpu)`.

**Detalle de cada syscall dentro de `MENSAJE_SYSCALL`:**
- `SYSCALL_EXIT` → `finalizar_proceso(pcb, "SUCCESS")` + `liberar_cpu`.
- `SYSCALL_SEG_FAULT` → `finalizar_proceso(pcb, "SEG_FAULT")` + `liberar_cpu`.
- `SYSCALL_SLEEP` → recibe `tiempo_ms` y los registros actualizados, `planificador_transicionar_exec_a_block(pcb, "SLEEP")`, `planificador_ejecutar_io_async(pcb, IO_SLEEP, tiempo_ms, 0)`, `liberar_cpu`.
- `SYSCALL_STDIN`/`SYSCALL_STDOUT` → recibe `dir_logica`, `tam` y registros; transiciona a BLOCK con motivo `"STDIN"`/`"STDOUT"`; `planificador_ejecutar_io_async` con el tipo correspondiente; `liberar_cpu`.
- `SYSCALL_MUTEX_CREATE` → recibe nombre length-prefixed y registros; `planificador_crear_mutex`; **no libera la CPU** — llama `planificador_despachar_directo(pcb, cpu)` para que el mismo proceso siga en la misma CPU (cumple textualmente lo que pide el enunciado: create/lock-exitoso/unlock no pasan por READY).
- `SYSCALL_MUTEX_LOCK` → recibe nombre y registros; `bool adquirido = planificador_lock_mutex(...)`. Si `adquirido`: loguea `"Toma el Mutex"`, agrega el nombre a `lista_mutex_tomados`, y `planificador_despachar_directo` (misma CPU). Si no: guarda el nombre en `pcb->recurso_bloqueado`, transiciona a BLOCK con motivo `"MUTEX"`, `liberar_cpu` (el proceso queda bloqueado — se lo despertará desde `planificador_unlock_mutex` cuando el dueño lo libere).
- `SYSCALL_MUTEX_UNLOCK` → recibe nombre y registros; loguea `"Libera el Mutex"`; `planificador_unlock_mutex`; `planificador_despachar_directo` (misma CPU).
- `SYSCALL_MEM_ALLOC` → recibe `id_segmento`, `tamanio` y registros; llama `planificador_crear_segmento_km(..., &hubo_compactacion)`. Si `ok`: `planificador_despachar_directo` (misma CPU) y, si `hubo_compactacion`, además `planificador_evento_memoria_liberada` (la compactación pudo haber liberado huecos que permitan des-suspender a otros). Si no `ok` (sin espacio y sin poder compactar): transiciona a BLOCK con motivo `"MEMORIA"`, `planificador_bloquear_por_memoria` (encola el pedido pendiente), `liberar_cpu`.
- `SYSCALL_MEM_FREE` → recibe `id_segmento` y registros; `planificador_eliminar_segmento_km`. Si tuvo éxito: `planificador_despachar_directo` + `planificador_evento_memoria_liberada` (se liberó memoria). Si falló: loguea warning y **igual** hace `planificador_despachar_directo` (no deja al proceso trabado por un `MEM_FREE` inválido).
- `SYSCALL_INIT_PROC` → recibe path y prioridad length-prefixed, y (si `pcb_ejecutando != NULL`) los registros. Valida rango de prioridad **solo si el algoritmo configurado es `"CMN"`** (`prioridad >= 0 && prioridad < cant_colas`); con FIFO/RR cualquier prioridad es aceptada (no se usa para nada). Si es válida: toma un PID nuevo bajo `mutex_pid` (`proximo_pid++`), crea el PCB con `pcb_create`, le setea `prioridad_original`/`prioridad_actual`, y lo encola con `planificador_encolar_new`. Si no es válida: solo loguea error, **no crea el proceso**. En cualquier caso, el proceso que hizo la syscall vuelve a READY (`planificador_transicionar_exec_a_ready`, no a la misma CPU — a diferencia de mutex/memoria, `INIT_PROC` sí compite de nuevo por CPU) y se libera la CPU actual.

**Dónde se usa:** Llamada desde `manejar_cliente:83`, una vez por cada CPU que completa el handshake — no retorna hasta que la CPU se desconecta.
**Para qué:** Es la traducción completa entre "lo que la CPU manda por el protocolo" y "lo que el planificador debe hacer" — el único lugar del código que conoce el detalle byte a byte de cada syscall.

### `int conectar_a_kernel_memory(const char* ip, const char* puerto, t_log* logger)`
**Qué hace:** `crear_conexion(ip, puerto)`; si tiene éxito, loguea y manda el handshake `HANDSHAKE_SCHEDULER` (4 bytes); si falla, loguea error. Devuelve el fd (o `-1`).
**Dónde se usa:** `main.c:55`.
**Para qué:** Establece el canal principal (síncrono, de comandos) hacia KM — el mismo fd se usa después para `MENSAJE_ENVIAR_PID`, `MENSAJE_CREAR_SEGMENTO`, `MENSAJE_ACTUALIZAR_CONTEXTO`, etc., desde `planificador.c` (a través de `pl->socket_memoria`).

### `static void* hilo_notificaciones_km(void* arg)`
**Qué hace y cómo:** Castea `arg` a fd (via `intptr_t`). Loop infinito de `recv` de un `t_codigo_mensaje`. Si `bytes <= 0`, loguea y rompe el loop (el canal se cerró). Si el código es `MENSAJE_MAS_MEMORIA`: recibe el nuevo total de bytes, loguea, y llama `planificador_evento_memoria_liberada` (más memoria total puede significar que ahora entren des-suspensiones o allocs pendientes). Si es `MENSAJE_MEMORIA_CORRUPTA`: llama `planificador_bsod` (que no retorna — termina el proceso). Cualquier otro código: warning.
**Dónde se usa:** Lanzado por `pthread_create` desde `conectar_canal_notificaciones_km`.
**Para qué:** Es el receptor del canal *asíncrono* KM→KS — deliberadamente separado del canal síncrono de comandos (`socket_memoria`) para que un `recv` bloqueante esperando la respuesta de, por ejemplo, `MENSAJE_CREAR_SEGMENTO` no reciba por error una notificación async y la interprete como esa respuesta.

### `int conectar_canal_notificaciones_km(const char* ip, const char* puerto, t_planificador* planificador, t_log* logger)`
**Qué hace:** Guarda `_logger`/`_planificador` (redundante con lo que ya hace `iniciar_servidor_kernel_scheduler`, pero se llama antes en `main.c`, así que hace falta acá también). `crear_conexion`, manda handshake `HANDSHAKE_SCHEDULER_NOTIF`, lanza `hilo_notificaciones_km` detached con el fd.
**Dónde se usa:** `main.c:68`.
**Para qué:** Abre y deja corriendo en background el canal de notificaciones descrito arriba.

**Dependencias:** `utils/sockets/sockets.h`, `utils/protocolo.h`, `planificador/planificador.h` (vía el header), `config/kernel_scheduler_config.h`.

---

## `kernel_scheduler/src/planificador/planificador.h` (215 líneas)

El header más grande del módulo — expone toda la API pública del planificador y define las tres estructuras centrales.

### `t_motivo_interrupcion` (enum): `INT_NINGUNA`, `INT_QUANTUM`, `INT_PREEMPCION`, `INT_COMPACTACION`
**Para qué:** Permite que, cuando una CPU devuelve un proceso tras `MENSAJE_INTERRUPCION`, el dispatcher (`atender_peticiones_cpu`) sepa **por qué** fue interrumpido y decida la transición correcta (compactación → al frente de la cola; quantum/preempción → al final).

### `t_cpu_conectada` (struct)
Campos: `id_cpu`, `socket_cpu`, `libre` (bool), `pid_ejecutando` (0 si libre), `en_rafaga` (bool — true desde que se despacha un PID hasta que la CPU lo devuelve; un proceso "estacionado" en una syscall que vuelve a la misma CPU sin pasar por `en_rafaga=false→true` de nuevo no cuenta como en ráfaga para efectos de compactación... en realidad `en_rafaga` se pone en `false` cada vez que la CPU devuelve el control en `atender_peticiones_cpu`, y se vuelve a poner en `true` en `despachar()`, incluso para los redespachos directos por mutex/memoria — así que si el proceso vuelve a la CPU vía `planificador_despachar_directo`, sigue contando como en ráfaga para compactación, correctamente), `rafaga` (uint32_t, contador que se incrementa en cada `despachar()` — invalida timers de quantum obsoletos: un timer lanzado para la ráfaga N no debe interrumpir la ráfaga N+1 si el mismo PID volvió rápido a la misma CPU vía un redespacho directo), `motivo_interrupcion`, `pid_preemptor`/`prioridad_preemptor` (para el log de desalojo por cola).

### `t_io_conectada` (struct)
Campos: `fd`, `tipo` (`t_tipo_io`), `nombre`, `libre` (bool), `desconectado` (bool — "zombie flag": la conexión se cerró pero un hilo `hilo_ejecutar_io` todavía la estaba usando; se remueve realmente recién cuando ese hilo la libera).

### `t_planificador` (struct) — la estructura central de todo el módulo
Agrupa: `cola_new` (`t_queue*`), `colas_ready` (`t_list**`, array de `cant_colas` listas — una sola con FIFO/RR, una por prioridad con CMN), `cant_colas`, `alg_por_cola` (`char**`, apunta a strings del config, no copia), `queue_preemption` (bool, parseado de `"TRUE"`/`"FALSE"`), `lista_exec` (`t_list*`, lista y no cola porque puede haber múltiples CPUs ejecutando en paralelo), `cola_block`, `cola_susp_block`, `cola_susp_ready` (las tres `t_queue*`), `lista_exit` (`t_list*`), `cpus_conectadas`/`ios_conectados`/`lista_mutex` (`t_list*`), un mutex por cada colección (9 mutexes de colas/listas más `mutex_pid` y `mutex_socket_memoria`), 6 semáforos de conteo (`sem_procesos_new`, `sem_procesos_ready`, `sem_cpus_libres`, y uno por tipo de IO: `sem_io_stdin_libres`/`sem_io_stdout_libres`/`sem_io_sleep_libres` — patrón productor/consumidor), el trío de compactación (`compactacion_en_curso` bool + `mutex_compactacion` + `cond_compactacion`), el trío de mediano plazo (`mutex_cola_susp` protege ambas colas de suspendidos, `lista_espera_memoria` de `t_espera_memoria*` para `MEM_ALLOC`s pendientes + `mutex_espera_memoria`, y `mutex_evento_memoria` que serializa el recorrido completo de des-suspensión/reintento), y los datos globales `config`, `logger`, `socket_memoria`, `proximo_pid` + `mutex_pid`.

**Para qué (el struct):** Es el estado completo del scheduler — se pasa por puntero a prácticamente todas las funciones del módulo. El diseño de "un mutex por colección" en vez de un lock global permite que, por ejemplo, un hilo IO libere un proceso a READY mientras otro hilo simultáneamente despacha otro proceso desde READY a EXEC, sin bloquearse mutuamente salvo en el instante exacto de tocar la misma colección.

### Firmas declaradas (implementación documentada en `planificador.c` más abajo)
`planificador_create`, `planificador_destroy`, `hilo_planificador_largo_plazo`, `hilo_planificador_corto_plazo`, `planificador_encolar_new`, `planificador_transicionar_new_a_ready`, `planificador_transicionar_exec_a_exit`, `planificador_transicionar_exec_a_ready`, `planificador_transicionar_exec_a_ready_frente`, `planificador_transicionar_exec_a_block`, `planificador_transicionar_block_a_ready`, `planificador_desbloquear`, `planificador_bloquear_por_memoria`, `planificador_evento_memoria_liberada`, `planificador_registrar_cpu`, `planificador_remover_cpu`, `planificador_obtener_cpu_por_socket`, `planificador_obtener_cpu_por_id`, `planificador_despachar_directo`, `planificador_registrar_io`, `planificador_remover_io`, `planificador_tomar_io_libre`, `planificador_liberar_io`, `planificador_ejecutar_io_async`, `planificador_actualizar_contexto_en_km`, `planificador_crear_segmento_km`, `planificador_eliminar_segmento_km`, `planificador_finalizar_proceso_km`, `planificador_bsod`, `planificador_crear_mutex`, `planificador_lock_mutex`, `planificador_unlock_mutex`.

**Dependencias:** `pcb/pcb.h`, `../io_scheduler/io_scheduler.h`, `../mutex_kernel/mutex_kernel.h`, `config/kernel_scheduler_config.h`, `commons/collections/list.h`, `commons/collections/queue.h`, `pthread.h`, `semaphore.h`.

---

## `kernel_scheduler/src/planificador/planificador.c` (1474 líneas — el archivo más denso del repo)

### Helpers de colas READY multinivel (líneas 1–107)

#### `static bool es_cmn(t_planificador* pl)`
**Qué hace:** `strcmp(pl->config->planification_algorithm, "CMN") == 0`.
**Dónde se usa:** `cola_de`, `chequear_desalojo_por_prioridad`, `planificador_create` (indirectamente vía el mismo `strcmp`), `cambiar_prioridad`. Es el chequeo base que decide si el resto de la lógica de colas múltiples aplica.
**Para qué:** Punto único de verdad para "¿estamos en modo CMN?" — evita repetir el `strcmp` con errores de tipeo en distintos lugares.

#### `static int cola_de(t_planificador* pl, t_pcb* pcb)`
**Qué hace y cómo:** Si no es CMN, devuelve siempre `0` (una sola cola). Si es CMN, usa `pcb->prioridad_actual` como índice, clampeado a `[0, cant_colas-1]` por seguridad (nunca debería salir de rango porque `INIT_PROC` ya valida esto, pero el clamp evita un acceso fuera de límites si algo cambia la prioridad a un valor inesperado).
**Dónde se usa:** `encolar_en_ready`, `despachar` (para decidir si aplica RR), `cambiar_prioridad`.
**Para qué:** Traduce "prioridad del proceso" a "índice del array `colas_ready`".

#### `static bool cola_usa_rr(t_planificador* pl, int cola)`
**Qué hace:** `strcmp(pl->alg_por_cola[cola], "RR") == 0`.
**Dónde se usa:** `despachar` (línea 364), para decidir si lanzar el timer de quantum.
**Para qué:** Con CMN, cada cola tiene su propio algoritmo (`QUEUES_ALGORITHMS`); esta función mira el algoritmo de la cola específica del proceso, no el algoritmo global.

#### `static void chequear_desalojo_por_prioridad(t_planificador* pl, t_pcb* nuevo)`
**Qué hace y cómo:** Si no es CMN o `queue_preemption` está apagado, no hace nada (return inmediato). Si no: recorre `lista_exec` buscando el proceso en ejecución con la **peor** prioridad (número más alto) que sea estrictamente peor que la del proceso que acaba de llegar (`p->prioridad_actual > victima_prio`, arrancando `victima_prio` en la prioridad del recién llegado — así solo encuentra víctimas *peores*, nunca iguales o mejores). Si encuentra una víctima, busca su `t_cpu_conectada` (debe estar ocupada, en ráfaga, ejecutando ese PID, y sin una interrupción ya pendiente) y le marca `motivo_interrupcion = INT_PREEMPCION` + guarda quién la desalojó (`pid_preemptor`, `prioridad_preemptor`), loguea el desalojo con el formato exacto que espera la cátedra, y manda `MENSAJE_INTERRUPCION` por el socket de esa CPU.
**Dónde se usa:** `encolar_en_ready` (solo si no es un reencolado "al frente", es decir, no durante compactación — línea 91-93).
**Para qué:** Implementa el desalojo entre colas de CMN que pide el enunciado: "si el desalojo entre colas está habilitado, se deberá validar si se encuentra ejecutando un Proceso con menor prioridad [que el que llega] ... se deberá desalojar".

#### `static void encolar_en_ready(t_planificador* pl, t_pcb* pcb, bool al_frente)`
**Qué hace y cómo:** Calcula la cola destino con `cola_de`. Bajo `mutex_cola_ready`, inserta al frente (`list_add_in_index(..., 0, ...)`) o al final (`list_add`) según `al_frente`. Hace `sem_post(&pl->sem_procesos_ready)` (despierta al hilo de corto plazo si estaba esperando). Si **no** es al frente, dispara `chequear_desalojo_por_prioridad` — el reencolado al frente es exclusivamente el caso de compactación, donde desalojar de nuevo no tendría sentido (ya se está desalojando todo por otra razón).
**Dónde se usa:** `planificador_transicionar_new_a_ready`, `exec_a_ready` (con ambos valores de `al_frente`), `planificador_transicionar_block_a_ready`, `planificador_evento_memoria_liberada` (al des-suspender).
**Para qué:** Es el único punto de inserción real en las colas READY — centraliza la lógica de "a qué cola, en qué extremo, y si corresponde desalojar".

#### `static t_pcb* desencolar_ready(t_planificador* pl)`
**Qué hace y cómo:** Bajo `mutex_cola_ready`, recorre las colas en orden de índice (0 = más prioritaria) y devuelve el primer elemento de la primera cola no vacía que encuentra (`list_remove(colas_ready[q], 0)`).
**Dónde se usa:** `hilo_planificador_corto_plazo` (línea 390).
**Para qué:** Es el único punto de extracción de READY — implementa el orden de prioridad entre colas (siempre se vacía primero la cola 0, la de mayor prioridad) y dentro de cada cola respeta el orden de inserción (FIFO natural de `t_list` con `list_add`/`list_remove(...,0)`, que es exactamente cómo se comporta también el algoritmo RR ya que el reencolado tras el quantum vuelve a entrar por el final).

### Creación / destrucción (líneas 109–243)

#### `t_planificador* planificador_create(t_kernel_scheduler_config* config, t_log* logger, int socket_memoria)`
**Qué hace y cómo:** Arma el array de colas READY: si `PLANIFICATION_ALGORITHM == "CMN"`, cuenta cuántos algoritmos hay en `QUEUES_ALGORITHMS` (hasta el primer `NULL`) y usa eso como `cant_colas` (si es 0, es un error de configuración fatal — loguea y devuelve `NULL`); si no es CMN, `cant_colas = 1` y `alg_por_cola[0]` apunta directamente al string `planification_algorithm` (así FIFO y RR "planos" reutilizan la misma infraestructura de `alg_por_cola`/`cola_usa_rr` que CMN, sin caso especial). Reserva y crea las `cant_colas` listas vacías. Parsea `queue_preemption` de string a bool. Crea el resto de las colecciones (`cola_new`, `lista_exec`, `cola_block`, `cola_susp_block`, `cola_susp_ready`, `lista_exit`, `cpus_conectadas`, `ios_conectados`, `lista_mutex`) todas vacías. Inicializa los 9 mutexes de colecciones + `mutex_pid`. Inicializa los 6 semáforos en 0. Inicializa el trío de compactación (`compactacion_en_curso = false`, mutex y condvar). Inicializa el trío de mediano plazo. Guarda `config`, `logger`, `socket_memoria`, y arranca `proximo_pid = 1` (el PID 0 ya se crea manualmente en `main.c` antes de este momento — el planificador no necesita saberlo, solo asume que el próximo PID libre es 1).
**Dónde se usa:** `main.c:58`.
**Para qué:** Constructor único del planificador — deja todo listo para que los hilos de largo/corto plazo y el servidor empiecen a operar sobre un estado consistente.

#### `static void io_conectada_destroy(void* elemento)`
**Qué hace:** Castea a `t_io_conectada*`, cierra el fd, libera `nombre` y el struct.
**Dónde se usa:** Como callback de `list_destroy_and_destroy_elements(pl->ios_conectados, io_conectada_destroy)` dentro de `planificador_destroy`.
**Para qué:** Limpieza específica de IO al cerrar (necesita cerrar el fd, a diferencia de un `free` genérico).

#### `void planificador_destroy(t_planificador* pl)`
**Qué hace:** Destruye, en orden, cada una de las 9 colecciones (con `pcb_destroy`, `io_conectada_destroy` o `mutex_kernel_destroy` según corresponda como destructor de elementos), libera los arrays auxiliares (`colas_ready`, `alg_por_cola`), destruye los 11 mutexes, los 6 semáforos, la condvar de compactación, y finalmente el propio struct.
**Dónde se usa:** `main.c:85` (en la ruta de limpieza que solo se alcanza si `iniciar_servidor_kernel_scheduler` retorna, lo cual en la práctica no ocurre en una corrida normal).
**Para qué:** Liberación simétrica y completa de todo lo creado en `planificador_create`.

### Hilo de Largo Plazo — NEW → READY (líneas 245–285)

#### `void* hilo_planificador_largo_plazo(void* arg)`
**Qué hace y cómo:** Loop infinito: `sem_wait(&sem_procesos_new)` (bloquea hasta que haya al menos un proceso en NEW), saca el primero de `cola_new` bajo `mutex_cola_new`. Si el socket a KM está activo, registra el proceso en KM enviando `MENSAJE_ENVIAR_PID` + PID + path de instrucciones length-prefixed, y espera la respuesta (crea el contexto/tabla de segmentos vacía del proceso del lado de KM) — todo bajo `mutex_socket_memoria` porque el socket es compartido con otras funciones que también le hablan a KM. Finalmente llama `planificador_transicionar_new_a_ready`.
**Dónde se usa:** Lanzado como hilo detached desde `main.c:72`.
**Para qué:** Es el planificador de largo plazo: decide **cuándo** un proceso recién creado entra al sistema. En este TP la política es trivial (entra apenas se registra en KM, sin límite de multiprogramación) — la única razón de ser un hilo separado (en vez de hacerlo inline donde se crea el PCB) es desacoplar la creación del proceso (que puede pasar desde `main` o desde una syscall `INIT_PROC` en cualquier hilo de CPU) de la actualización de las estructuras de planificación, serializada a través de este único hilo consumidor.

### Despacho y quantum (líneas 287–428)

#### `void* hilo_timer_quantum(void* arg)`
**Qué hace y cómo:** Recibe (y libera) un `t_timer_args*` con `pl`, `target_pid`, `target_cpu`, `target_rafaga`. Duerme `rr_quantum` milisegundos (`usleep`). Al despertar, bajo `mutex_cpus`, busca la CPU por `id_cpu == target_cpu` y verifica **las cinco condiciones simultáneas** de que el timer siga siendo válido: no libre, en ráfaga, mismo PID ejecutando, **mismo número de ráfaga** (`cpu->rafaga == target_rafaga` — esta es la comprobación clave que evita que un timer viejo interrumpa una ráfaga nueva del mismo proceso en la misma CPU, algo que puede pasar si el proceso hizo un `MUTEX_LOCK` exitoso y volvió directo a la misma CPU antes de que expire el timer original), y sin una interrupción ya en curso. Si todo coincide: marca `motivo_interrupcion = INT_QUANTUM` y manda `MENSAJE_INTERRUPCION` (el `send` se hace todavía bajo `mutex_cpus`, deliberadamente, para que no se entrelace con otro hilo escribiendo al mismo socket).
**Dónde se usa:** Lanzado como hilo detached desde `despachar()`, solo si la cola del proceso usa RR.
**Para qué:** Implementa el corte de quantum de Round Robin.

#### `static void despachar(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu)`
La función de envío real de un PID a una CPU — todo despacho del sistema (desde READY o directo por syscall) pasa por acá.
**Qué hace y cómo, en el orden exacto que documenta el propio comentario del código:**
1. `planificador_actualizar_contexto_en_km` — sincroniza los registros actuales del PCB con KM, **sin tener ningún lock propio tomado todavía** (evita mantener locks mientras se espera una respuesta de red).
2. Toma `mutex_compactacion` y hace `pthread_cond_wait` en loop mientras `compactacion_en_curso` sea true — así ningún despacho ocurre mientras Kernel Memory está compactando.
3. Toma `mutex_cpus`, marca la CPU ocupada (`libre = false`), le asigna el PID, `en_rafaga = true`, incrementa el contador `rafaga`, limpia `motivo_interrupcion`, y manda `MENSAJE_ENVIAR_PID` + el PID por el socket — todo bajo el mismo lock, para que el envío no se entrelace con otro hilo. Libera ambos mutexes (`mutex_cpus` primero, luego `mutex_compactacion`).
4. Fuera de los locks: setea `pcb->cpu_asignada`, y si la cola del proceso usa RR, lanza `hilo_timer_quantum` con los datos capturados (incluyendo el número de ráfaga recién incrementado, para el chequeo de validez descrito arriba).
**Dónde se usa:** `planificador_despachar_directo` (wrapper público, línea 377-379) y `hilo_planificador_corto_plazo` (línea 425).
**Para qué:** Es el único punto real de envío de `MENSAJE_ENVIAR_PID` a una CPU en todo el sistema — garantiza que ese envío nunca ocurra durante una compactación y que el temporizador de quantum, si aplica, arranque en el momento correcto.

#### `void planificador_despachar_directo(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu)`
**Qué hace:** Llama directamente a `despachar` — es un wrapper público de una función `static`.
**Dónde se usa:** `conexiones.c`, en las 5 ramas de syscall que devuelven el proceso a la misma CPU sin pasar por READY: `MUTEX_CREATE`, `MUTEX_LOCK` exitoso, `MUTEX_UNLOCK`, `MEM_ALLOC` exitoso, `MEM_FREE` exitoso o fallido.
**Para qué:** Es la API que usa `conexiones.c` para pedir "reenviar este proceso a la CPU de la que vino" sin exponer la función `static` `despachar` fuera del archivo.

#### `void* hilo_planificador_corto_plazo(void* arg)`
El planificador de corto plazo: READY → EXEC.
**Qué hace y cómo:** Loop infinito con doble `sem_wait`: primero `sem_procesos_ready` (hay al menos un proceso listo), después `sem_cpus_libres` (hay al menos una CPU libre) — el orden importa poco en la práctica porque ambos deben cumplirse, pero evita tomar una CPU si no hay ningún proceso, y viceversa. Saca el proceso más prioritario con `desencolar_ready`. Si por alguna razón de timing el resultado es `NULL` (no debería pasar dado el semáforo, pero el código es defensivo), libera el `sem_cpus_libres` que ya había tomado y reintenta (`continue`). Busca la primera CPU libre en `cpus_conectadas` (recorrido lineal bajo `mutex_cpus`); si por algún motivo no encuentra ninguna (de nuevo, defensivo — el semáforo debería garantizarlo), reencola el proceso al frente de su cola y reintenta. Transiciona el PCB a `ESTADO_EXEC` (con timestamp `ts_inicio_exec`), lo agrega a `lista_exec`, loguea el cambio de estado, y llama `despachar`.
**Dónde se usa:** Lanzado como hilo detached desde `main.c:73`.
**Para qué:** Es el motor de la planificación de corto plazo — el único consumidor real de las colas READY.

### Métodos de transición de estado (líneas 430–676)

#### `void planificador_encolar_new(t_planificador* pl, t_pcb* pcb)`
**Qué hace:** Encola el PCB en `cola_new` bajo `mutex_cola_new`, loguea `"Se crea el proceso - Estado: NEW"`, `sem_post(&sem_procesos_new)`.
**Dónde se usa:** `main.c:79` (proceso inicial) y `conexiones.c:395` (`SYSCALL_INIT_PROC`).
**Para qué:** Único punto de entrada al sistema para un proceso nuevo.

#### `void planificador_transicionar_new_a_ready(t_planificador* pl, t_pcb* pcb)`
**Qué hace:** Cambia estado a `ESTADO_READY`, marca `ts_llegada_ready`, loguea la transición, llama `encolar_en_ready(pl, pcb, false)`.
**Dónde se usa:** `hilo_planificador_largo_plazo` únicamente.
**Para qué:** Transición NEW→READY.

#### `static t_pcb* remover_de_exec(t_planificador* pl, uint32_t pid)`
**Qué hace:** Bajo `mutex_lista_exec`, busca linealmente por PID y remueve (`list_remove`) el primer match.
**Dónde se usa:** `planificador_transicionar_exec_a_exit`, `exec_a_ready`, `planificador_transicionar_exec_a_block`, `planificador_remover_cpu` (si la CPU desconectada tenía un proceso en ejecución).
**Para qué:** Toda transición que saca un proceso de EXEC pasa por acá primero — centraliza el patrón "buscar y remover de la lista de ejecución".

#### `void planificador_transicionar_exec_a_exit(t_planificador* pl, t_pcb* pcb, const char* motivo)`
**Qué hace:** Remueve de `lista_exec` (con fallback al puntero recibido si por algún motivo no estaba ahí — defensivo). Cambia a `ESTADO_EXIT`, lo agrega a `lista_exit`, loguea la transición y **además** un segundo log obligatorio con el motivo (`"finalizó su ejecución con motivo de %s"`).
**Dónde se usa:** `finalizar_proceso` en `conexiones.c` (única invocación).
**Para qué:** Transición final EXEC→EXIT.

#### `static void exec_a_ready(t_planificador* pl, t_pcb* pcb, bool al_frente)`
**Qué hace:** Remueve de `lista_exec`, cambia a `ESTADO_READY`, limpia `cpu_asignada = -1`, marca `ts_llegada_ready`, loguea, y `encolar_en_ready(pl, real_pcb, al_frente)`.
**Dónde se usa:** Vía los dos wrappers públicos siguientes.
**Para qué:** Implementación común de ambas variantes de "volver a READY desde EXEC".

#### `void planificador_transicionar_exec_a_ready(t_planificador* pl, t_pcb* pcb)` / `..._ready_frente(...)`
**Qué hacen:** Wrappers de `exec_a_ready` con `al_frente = false` / `true` respectivamente.
**Dónde se usan:** La variante normal, en `conexiones.c` (`INT_PREEMPCION`, `INT_QUANTUM`, `SYSCALL_INIT_PROC` del proceso llamante) y en `planificador_remover_cpu` (reencolar el proceso de una CPU que se desconectó). La variante "al frente", solo en `conexiones.c` para `INT_COMPACTACION`.
**Para qué:** Distinguir el caso normal del caso "el desalojo no fue culpa del proceso" (compactación), que no debería hacerle perder su lugar en la cola.

#### `static void* hilo_timer_suspension(void* arg)`
**Qué hace y cómo:** Recibe `pl`, `pid`, `bloqueo_id` (libera el arg de entrada). Duerme `suspension_timeout` ms. Al despertar, bajo `mutex_cola_block`, recorre **toda** `cola_block` sacando y volviendo a meter cada elemento (patrón "drain and requeue", porque `t_queue` no tiene búsqueda por índice), reteniendo solo el que matchea `pid` **y** `bloqueo_id` — el chequeo de `bloqueo_id` es lo que invalida el timer si el proceso se desbloqueó y volvió a bloquearse (con un `bloqueo_id` incrementado) en el medio. Si no lo encuentra, libera el lock y termina sin hacer nada (el proceso ya se desbloqueó). Si lo encuentra: cambia a `ESTADO_SUSP_BLOCK`, marca `ts_suspension`, lo mueve a `cola_susp_block` (bajo `mutex_cola_susp`, tomado *dentro* del lock de `cola_block` — el comentario del código aclara que el orden block→susp es intencional para que la transición sea atómica de punta a punta), loguea el cambio de estado. Después, si hay conexión a KM, manda `MENSAJE_SUSPENDER_PROCESO` + PID y espera la respuesta (mueve los segmentos del proceso a SWAP, liberando memoria principal) — si KM responde algo distinto de `MENSAJE_OK`, solo loguea un warning (no revierte la transición de estado; el proceso queda lógicamente suspendido aunque KM no haya podido mover su memoria, lo cual sería un estado inconsistente, pero el código no maneja ese caso de error más allá del log). Finalmente llama `planificador_evento_memoria_liberada` porque suspender un proceso libera memoria principal.
**Dónde se usa:** Lanzado como hilo detached desde `planificador_transicionar_exec_a_block`, salvo si el motivo es `"MEMORIA"`.
**Para qué:** Implementa la planificación de mediano plazo: `SUSPENSION_TIMEOUT` del enunciado.

#### `void planificador_transicionar_exec_a_block(t_planificador* pl, t_pcb* pcb, const char* motivo_bloqueo)`
**Qué hace y cómo:** Remueve de `lista_exec`, cambia a `ESTADO_BLOCK`, limpia `cpu_asignada`, **incrementa `bloqueo_id`** (invalida cualquier timer de suspensión previo asociado a un bloqueo anterior), guarda `motivo_bloqueo` (con `free` del anterior si había), marca `ts_inicio_block`, encola en `cola_block`, loguea la transición. Si el motivo **no** es `"MEMORIA"`, lanza `hilo_timer_suspension` — los bloqueos por `MEM_ALLOC` sin espacio están explícitamente excluidos del timer de suspensión: el comentario explica que ese caso ya lo resuelve el gestor de memoria (reintentos vía `planificador_evento_memoria_liberada`) y que "swapearlos generaría un estado inconsistente" (un proceso que está esperando memoria para poder crear un segmento no puede, a la vez, tener ese mismo segmento movido a SWAP).
**Dónde se usa:** `conexiones.c`, en `SYSCALL_SLEEP`, `SYSCALL_STDIN`/`STDOUT`, `SYSCALL_MUTEX_LOCK` (bloqueo), `SYSCALL_MEM_ALLOC` (bloqueo por falta de memoria, motivo `"MEMORIA"`).
**Para qué:** Transición EXEC→BLOCK, con el efecto colateral (salvo excepción) de armar el timer de mediano plazo.

#### `t_pcb* planificador_transicionar_block_a_ready(t_planificador* pl, t_pcb* pcb)`
**Qué hace y cómo:** Patrón "drain and requeue" sobre `cola_block` buscando por PID (nota: a diferencia de `hilo_timer_suspension`, **no** chequea `bloqueo_id` acá — cualquier instancia bloqueada con ese PID se despierta). Si lo encuentra: cambia a `ESTADO_READY`, **incrementa `bloqueo_id`** (esto es lo que realmente invalida cualquier timer de suspensión pendiente para este bloqueo — el timer, al despertar, no va a encontrar coincidencia de `bloqueo_id` y no hará nada), limpia `motivo_bloqueo`, marca `ts_llegada_ready`, loguea, `encolar_en_ready`.
**Dónde se usa:** Directamente, solo dentro de `planificador_desbloquear` (ver siguiente). Es la función que realmente busca en `cola_block`.
**Para qué:** Es el "caso feliz" de desbloqueo: el proceso todavía estaba en BLOCK (no llegó a suspenderse).

#### `t_pcb* planificador_desbloquear(t_planificador* pl, uint32_t pid, bool* estaba_suspendido)`
**Qué hace y cómo:** Primero intenta `planificador_transicionar_block_a_ready` con un PCB "de búsqueda" (struct local solo con el PID seteado, cero-inicializado — un truco para reusar la función de búsqueda por PID sin tener el puntero real). Si encuentra algo, devuelve eso y `*estaba_suspendido = false`. Si no (el proceso ya no estaba en BLOCK, probablemente porque se suspendió mientras tanto), busca en `cola_susp_block` con el mismo patrón drain-and-requeue; si lo encuentra, cambia a `ESTADO_SUSP_READY` (**no** a READY — sigue en SWAP), lo mueve a `cola_susp_ready`, loguea, y marca `*estaba_suspendido = true`.
**Dónde se usa:** `hilo_ejecutar_io` (al terminar una IO), `planificador_unlock_mutex` (al despertar al siguiente en la cola de un mutex).
**Para qué:** Es la función genérica de "despertar a un proceso por PID sin saber de antemano si está en BLOCK o ya se suspendió" — encapsula la ambigüedad de timing entre "la IO/el mutex se liberó" y "el timer de suspensión ya disparó".

### Espera de memoria y des-suspensiones (líneas 678–806)

#### `void planificador_bloquear_por_memoria(t_planificador* pl, t_pcb* pcb, uint32_t id_segmento, uint32_t tamanio)`
**Qué hace:** Empaqueta `pcb`, `id_segmento`, `tamanio` en un `t_espera_memoria*` y lo agrega a `lista_espera_memoria` bajo `mutex_espera_memoria`; loguea.
**Dónde se usa:** `conexiones.c`, en la rama `SYSCALL_MEM_ALLOC` cuando `planificador_crear_segmento_km` falla.
**Para qué:** Registra un `MEM_ALLOC` pendiente para reintentarlo más adelante.

#### `static int comparar_candidatos(const void* a, const void* b)`
**Qué hace:** Comparador para `qsort` sobre `t_candidato_dessusp` (pid, prioridad, `ts_suspension`): ordena primero por `prioridad` ascendente (menor número = mayor prioridad primero), y en caso de empate, por `ts_suspension` ascendente (el que lleva más tiempo suspendido, es decir, el timestamp más viejo, primero).
**Dónde se usa:** `planificador_evento_memoria_liberada`.
**Para qué:** Implementa textualmente el criterio del enunciado: "se recorrerán todos los Procesos suspendidos por orden de prioridad ... en caso de empate ... el que lleve mayor tiempo suspendido".

#### `static bool dessuspender_en_km(t_planificador* pl, uint32_t pid)`
**Qué hace:** Manda `MENSAJE_DESSUSPENDER_PROCESO` + PID a KM bajo `mutex_socket_memoria`, espera respuesta; devuelve `true` solo si la respuesta fue `MENSAJE_OK`.
**Dónde se usa:** `planificador_evento_memoria_liberada`, una vez por cada candidato ordenado.
**Para qué:** Le pide a KM que intente reservar espacio para **todos** los segmentos del proceso suspendido (KM decide todo-o-nada); si no entra, sigue en SUSP_READY y se reintentará en el próximo evento de liberación de memoria.

#### `void planificador_evento_memoria_liberada(t_planificador* pl)`
La función que centraliza toda reacción a "se liberó memoria" (fin de proceso, `MEM_FREE`, compactación, nuevo stick, suspensión de otro proceso).
**Qué hace y cómo:**
1. Toma `mutex_evento_memoria` para todo el cuerpo — serializa: si dos eventos de liberación ocurren casi a la vez (por ejemplo, dos procesos terminan juntos), el segundo espera a que el primero termine de recorrer y reevalúa sobre el estado ya actualizado, en vez de correr en paralelo sobre una foto potencialmente inconsistente.
2. **Fase 1 — des-suspensión:** copia `cola_susp_ready` completa a un array de `t_candidato_dessusp` (bajo `mutex_cola_susp`), lo ordena con `qsort`+`comparar_candidatos`, y para cada candidato en ese orden llama `dessuspender_en_km`; si tiene éxito, saca al proceso real de `cola_susp_ready` (drain-and-requeue de nuevo) y lo transiciona a `ESTADO_READY` + `encolar_en_ready`.
3. **Fase 2 — reintento de allocs pendientes:** toma un snapshot (`list_duplicate`) de `lista_espera_memoria` bajo lock, y fuera del lock recorre ese snapshot en **orden de llegada** (FIFO — no hay reordenamiento por prioridad acá, a diferencia de la fase 1) reintentando `planificador_crear_segmento_km` para cada uno; si tiene éxito, remueve el `t_espera_memoria` real de la lista, loguea, y llama `planificador_desbloquear` sobre ese PID (que puede estar en BLOCK o ya suspendido) — si estaba suspendido, ese desbloqueo por sí solo no lo pasa a READY (queda en SUSP_READY), lo cual es correcto porque un `MEM_ALLOC` recién satisfecho no implica que el resto de sus segmentos (si estaban en SWAP) ya estén restaurados.
**Dónde se usa:** `conexiones.c` (`finalizar_proceso`, `MEM_ALLOC` exitoso tras compactación, `MEM_FREE` exitoso), `hilo_notificaciones_km` (`MENSAJE_MAS_MEMORIA`), `hilo_timer_suspension` (tras suspender a alguien), `hilo_ejecutar_io` (si el proceso que terminó su IO estaba suspendido), `planificador_unlock_mutex` (si el que se despertó estaba suspendido).
**Para qué:** Es el mecanismo central de la planificación de mediano plazo — cualquier evento que pueda haber cambiado la disponibilidad de memoria termina, directa o indirectamente, en esta función.

### CPUs conectadas (líneas 808–882)

#### `void planificador_registrar_cpu(t_planificador* pl, int socket_cpu, int id_cpu)`
**Qué hace:** Crea un `t_cpu_conectada` inicializado en estado libre/sin ráfaga, lo agrega a `cpus_conectadas` bajo `mutex_cpus`, loguea `"CPU %d Conectada"`, `sem_post(&sem_cpus_libres)`.
**Dónde se usa:** `conexiones.c:82`, tras el handshake `HANDSHAKE_CPU`.
**Para qué:** Alta de una CPU en el pool de recursos del planificador.

#### `void planificador_remover_cpu(t_planificador* pl, int socket_cpu)`
**Qué hace y cómo:** Busca la CPU por socket bajo `mutex_cpus`, la remueve de la lista, loguea la desconexión. Si tenía un proceso ejecutando (`!cpu->libre`), lo recupera con `remover_de_exec` y lo reencola vía `planificador_transicionar_exec_a_ready` (el proceso no se pierde ni se da por terminado — simplemente pierde su CPU). Si estaba libre, hace `sem_trywait` sobre `sem_cpus_libres` para mantener el contador del semáforo consistente con la cantidad real de CPUs libres restantes.
**Dónde se usa:** `conexiones.c:162`, cuando `atender_peticiones_cpu` detecta que el `recv` devolvió `<= 0` (la CPU se desconectó).
**Para qué:** Baja de una CPU, con recuperación del trabajo en curso si tenía uno.

#### `t_cpu_conectada* planificador_obtener_cpu_por_socket(t_planificador* pl, int socket_cpu)`
**Qué hace:** Búsqueda lineal por socket bajo `mutex_cpus`.
**Dónde se usa:** **Ningún caller en todo el repo** (verificado con grep — solo aparece la declaración en el header y la definición acá).
**Hallazgo:** Código muerto. Función completa, correctamente implementada, pero nunca invocada — probablemente una API pensada originalmente para identificar la CPU por su fd de socket (como sí se hace en varias partes de `manejar_cliente`/`atender_peticiones_cpu` de otros módulos) y luego reemplazada por `planificador_obtener_cpu_por_id`, que es la que realmente usa `atender_peticiones_cpu`, sin borrar esta.

#### `t_cpu_conectada* planificador_obtener_cpu_por_id(t_planificador* pl, int id_cpu)`
**Qué hace:** Búsqueda lineal por `id_cpu` bajo `mutex_cpus`.
**Dónde se usa:** `conexiones.c:168`, en cada iteración del loop de `atender_peticiones_cpu` — es cómo el dispatcher recupera el struct de la CPU a partir del `id_cpu` capturado en el handshake.
**Para qué:** Traduce el identificador lógico de CPU (el que manda la propia CPU al conectarse) a su struct de control interno.

### Sincronización de contexto con Kernel Memory (líneas 884–949)

#### `void planificador_actualizar_contexto_en_km(t_planificador* pl, uint32_t pid, t_registros_cpu registros)`
**Qué hace:** Protocolo síncrono bajo `mutex_socket_memoria`: manda `MENSAJE_ACTUALIZAR_CONTEXTO` + PID + los registros completos; espera confirmación.
**Dónde se usa:** `despachar()` (antes de cada envío de PID a una CPU).
**Para qué:** Garantiza que KM siempre tenga los registros más recientes de un proceso antes de que una CPU vuelva a pedir su contexto — sin esto, un proceso podría reanudar con registros desactualizados tras un desalojo.

#### `static char* planificador_leer_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio)`
**Qué hace y cómo:** Manda `MENSAJE_LEER_MEMORIA` + PID + dirección lógica + tamaño; si la respuesta no es `MENSAJE_OK`, devuelve `NULL`; si es OK, reserva un buffer (mínimo 1 byte incluso si `tamanio == 0`, para no confundir un `malloc(0)` que devuelva `NULL` con un fallo de lectura) y recibe el contenido.
**Dónde se usa:** `hilo_ejecutar_io`, caso `IO_STDOUT` (lee el contenido lógico del proceso para mandárselo al dispositivo IO).
**Para qué:** Es el puente entre "dirección lógica de un proceso" y "bytes reales" para STDOUT — la traducción la hace KM (no el KS), precisamente para que siga siendo válida aunque haya habido compactación entre la syscall y la ejecución efectiva de la IO.

#### `static bool planificador_escribir_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, void* contenido)`
**Qué hace:** Simétrico al anterior: `MENSAJE_ESCRIBIR_MEMORIA` + PID + dirección + tamaño + el buffer; devuelve si la respuesta fue OK.
**Dónde se usa:** `hilo_ejecutar_io`, caso `IO_STDIN` (escribe lo leído desde el dispositivo IO a la memoria lógica del proceso).
**Para qué:** Contraparte de escritura del mecanismo anterior.

### Syscalls de memoria — crear/eliminar segmento, finalizar proceso, compactación (líneas 951–1114)

#### `static void pausar_planificacion(t_planificador* pl)` / `static void reanudar_planificacion(t_planificador* pl)`
**Qué hacen:** Setean `compactacion_en_curso` a `true`/`false` bajo `mutex_compactacion`; `reanudar_planificacion` además hace `pthread_cond_broadcast(&cond_compactacion)` para despertar a todos los hilos esperando en `despachar()`.
**Dónde se usan:** `planificador_crear_segmento_km` (cuando KM pide compactar) y `planificador_bsod`.
**Para qué:** Primitivas de pausa/reanudación del despacho, construidas sobre el trío mutex+bool+condvar.

#### `static int interrumpir_cpus_para_compactar(t_planificador* pl)`
**Qué hace y cómo:** Bajo `mutex_cpus`, recorre todas las CPUs; cuenta las que están ocupadas y en ráfaga (`en_rafaga`) — es decir, ejecutando instrucciones activamente, no "estacionadas" en una syscall. A cada una que todavía no tenga una interrupción de compactación pendiente le marca `motivo_interrupcion = INT_COMPACTACION` y le manda `MENSAJE_INTERRUPCION` (idempotente: si ya se le mandó, no se le vuelve a mandar, aunque sigue contando en el total). Devuelve cuántas siguen en ráfaga (todavía no devolvieron el control).
**Dónde se usa:** `planificador_crear_segmento_km`, dentro de un loop `while (interrumpir_cpus_para_compactar(pl) > 0) usleep(5000);` — polling cada 5ms hasta que el conteo llegue a 0.
**Para qué:** Solo las CPUs que están ejecutando instrucciones activamente necesitan ser desalojadas para una compactación segura; las que están "estacionadas" (esperando la respuesta de una syscall que las va a redespachar directo) no tienen memoria mapeada activamente en ese instante y no hace falta esperarlas.

#### `bool planificador_crear_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento, uint32_t tamanio, bool* hubo_compactacion)`
**Qué hace y cómo:** Manda `MENSAJE_CREAR_SEGMENTO` + pid + id_segmento + tamaño bajo `mutex_socket_memoria`. Si la respuesta es `MENSAJE_COMPACTACION` (KM necesita reordenar memoria antes de poder crear el segmento): marca `*hubo_compactacion = true`, loguea, `pausar_planificacion`, espera en loop a que todas las CPUs en ráfaga devuelvan el control (`interrumpir_cpus_para_compactar`), manda `MENSAJE_DESALOJO_OK` a KM (confirmando que ya no hay nadie tocando memoria) y espera el resultado final de la compactación+creación, `reanudar_planificacion`. Devuelve `true` si la respuesta final fue `MENSAJE_OK`.
**Dónde se usa:** `conexiones.c` (`SYSCALL_MEM_ALLOC`) y `planificador_evento_memoria_liberada` (reintento de allocs pendientes, con `hubo_compactacion = NULL` porque en ese contexto no importa si hubo compactación — ya se llama `planificador_evento_memoria_liberada` desde el mismo lugar que la disparó).
**Para qué:** Es el protocolo completo (mensaje de red + coordinación de desalojo) de creación de un segmento, incluyendo el caso especial de compactación descrito por el enunciado.

#### `bool planificador_eliminar_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento)`
**Qué hace:** `MENSAJE_ELIMINAR_SEGMENTO` + pid + id_segmento bajo lock; devuelve si la respuesta fue OK.
**Dónde se usa:** `conexiones.c` (`SYSCALL_MEM_FREE`).
**Para qué:** Protocolo de `MEM_FREE`.

#### `void planificador_finalizar_proceso_km(t_planificador* pl, uint32_t pid)`
**Qué hace:** `MENSAJE_FINALIZAR_PROCESO` + pid bajo lock, espera confirmación (no propaga el resultado — la finalización del lado KS ya ocurrió independientemente de si KM confirma bien).
**Dónde se usa:** `finalizar_proceso` en `conexiones.c`.
**Para qué:** Avisa a KM que libere toda la memoria/estructuras asociadas al PID que terminó.

### Blue Screen of Death (líneas 1068–1114)

#### `static void bsod_finalizar_lista(t_planificador* pl, t_list* lista)` / `static void bsod_finalizar_cola(t_planificador* pl, t_queue* cola)`
**Qué hacen:** Recorren la colección y loguean, para cada PCB, `"finalizó su ejecución con motivo de BSOD"` (`bsod_finalizar_cola` es un wrapper que accede al campo interno `->elements` de `t_queue` para reutilizar la función de lista).
**Dónde se usan:** Solo dentro de `planificador_bsod`.

#### `void planificador_bsod(t_planificador* pl)`
**Qué hace y cómo:** Loguea el error de memoria corrupta. Llama `pausar_planificacion` (para que nada se siga despachando mientras se apaga todo). Recorre y loguea el fin de **todas** las colecciones de procesos vivos: `cola_new`, las `cant_colas` colas READY, `lista_exec`, `cola_block`, `cola_susp_block`, `cola_susp_ready` (nótese que **no** incluye `lista_exit`, porque esos procesos ya finalizaron antes del BSOD). Loguea el cierre del propio Kernel Scheduler, destruye el logger, y hace `exit(EXIT_FAILURE)` — no retorna, no libera el resto de las estructuras (el proceso completo termina, el sistema operativo recupera esa memoria).
**Dónde se usa:** `hilo_notificaciones_km`, ante `MENSAJE_MEMORIA_CORRUPTA`.
**Para qué:** Implementa la política de "todo o nada" ante corrupción de memoria que documentamos en el análisis anterior — no hay forma segura de seguir operando, así que se registra el estado final de todos los procesos y se aborta.

### Dispositivos IO (líneas 1116–1295)

#### `static sem_t* sem_de_tipo_io(t_planificador* pl, t_tipo_io tipo)` / `static const char* nombre_tipo_io(t_tipo_io tipo)`
**Qué hacen:** Mapean el enum `t_tipo_io` al semáforo correspondiente / a su nombre para logging.
**Dónde se usan:** El primero en `planificador_registrar_io`, `planificador_remover_io`, `planificador_tomar_io_libre`, `planificador_liberar_io`. El segundo, solo en `hilo_ejecutar_io` (mensaje de error).
**Para qué:** Evitar un `switch` repetido en cada función que necesita el semáforo/nombre de un tipo de IO.

#### `void planificador_registrar_io(t_planificador* pl, int fd, t_tipo_io tipo, char* nombre)`
**Qué hace:** Crea un `t_io_conectada` libre, lo agrega a `ios_conectados` bajo `mutex_ios`, loguea, `sem_post` sobre el semáforo del tipo correspondiente.
**Dónde se usa:** `conexiones.c:114`, tras el handshake `HANDSHAKE_IO`.
**Para qué:** Alta de un dispositivo IO en el pool.

#### `void planificador_remover_io(t_planificador* pl, int fd)`
**Qué hace y cómo:** Busca por fd bajo `mutex_ios`. Si el dispositivo está libre (`io->libre`), lo remueve y libera ya mismo (`sem_trywait` + `io_conectada_destroy`). Si está en uso (un hilo `hilo_ejecutar_io` lo tiene tomado), no lo toca — solo marca `io->desconectado = true`, dejando que sea `planificador_liberar_io` quien lo remueva/libere realmente cuando ese hilo termine.
**Dónde se usa:** `hilo_ejecutar_io`, cuando una operación de IO falla (`!ok`).
**Para qué:** Maneja de forma segura la desconexión de un dispositivo mientras potencialmente está siendo usado por otro hilo, sin liberar memoria que ese otro hilo todavía referencia.

#### `t_io_conectada* planificador_tomar_io_libre(t_planificador* pl, t_tipo_io tipo)`
**Qué hace y cómo:** `sem_wait` sobre el semáforo del tipo (bloquea hasta que haya uno libre). Bajo `mutex_ios`, busca el primero con ese tipo, libre y no marcado como desconectado; lo marca ocupado (`libre = false`) y lo devuelve.
**Dónde se usa:** `hilo_ejecutar_io`, al arrancar.
**Para qué:** Es la primitiva de "tomar un recurso IO" — el patrón semáforo+búsqueda es el mismo que para CPUs (`sem_cpus_libres` + búsqueda lineal en `hilo_planificador_corto_plazo`), aplicado a dispositivos de IO.

#### `void planificador_liberar_io(t_planificador* pl, t_io_conectada* io)`
**Qué hace y cómo:** Bajo `mutex_ios`, si el dispositivo fue marcado como desconectado mientras estaba en uso, lo remueve realmente y lo destruye (sin volver a postear el semáforo — ya no existe). Si no, simplemente lo marca libre y postea su semáforo.
**Dónde se usa:** `hilo_ejecutar_io`, cuando la operación de IO tuvo éxito.
**Para qué:** Contraparte de `planificador_tomar_io_libre`, con el manejo del caso "se desconectó mientras lo usaba" descrito arriba.

#### `static bool io_enviar_pid_y_param(int fd, uint32_t pid, uint32_t param)` / `static bool io_esperar_ok(int fd)`
**Qué hacen:** Helpers mínimos de protocolo — mandar PID+parámetro (dos `send`), y esperar un `MENSAJE_OK` (un `recv` de `t_codigo_mensaje` comparado contra `MENSAJE_OK`).
**Dónde se usan:** Dentro de `hilo_ejecutar_io`, en los tres casos (STDOUT, STDIN, SLEEP).
**Para qué:** Evitar repetir el mismo par de `send`/patrón de verificación de OK en cada rama del switch de tipos de IO.

#### `static void* hilo_ejecutar_io(void* arg)`
La ejecución asíncrona completa de un pedido de IO, en un hilo dedicado por pedido.
**Qué hace y cómo:** Toma un dispositivo libre del tipo pedido con `planificador_tomar_io_libre` (bloquea si no hay ninguno). Según el tipo:
- **`IO_STDOUT`**: lee el contenido lógico del proceso desde KM (`planificador_leer_memoria_en_km`, usando la dirección lógica — no física, para seguir siendo válida pese a compactaciones intermedias), y si tuvo éxito, manda PID+tamaño+contenido al dispositivo IO y espera OK.
- **`IO_STDIN`**: manda PID+tamaño al dispositivo, recibe el buffer leído, y lo escribe en la memoria lógica del proceso vía `planificador_escribir_memoria_en_km`.
- **`IO_SLEEP`** (default): solo manda PID+tiempo y espera OK — no toca memoria.
Si algo falló (`!ok`): loguea error y llama `planificador_remover_io` (el dispositivo se da por roto). Si tuvo éxito: `planificador_liberar_io` (vuelve al pool). En cualquiera de los dos casos, finalmente: `planificador_desbloquear(pl, pcb->pid, &estaba_suspendido)` — el proceso puede haberse suspendido por timeout mientras la IO estaba en curso, así que se usa la función genérica que contempla ambos casos (BLOCK→READY o SUSP_BLOCK→SUSP_READY); loguea a cuál de los dos pasó. Si pasó a SUSP_READY, llama además `planificador_evento_memoria_liberada` (podría ya haber lugar para des-suspenderlo del todo).
**Dónde se usa:** Lanzado como hilo detached desde `planificador_ejecutar_io_async`.
**Para qué:** Es la implementación real de "la IO ocurre en paralelo, sin bloquear al planificador" — cada pedido de IO vive en su propio hilo desde que se toma el dispositivo hasta que el proceso vuelve a ser planificable.

#### `void planificador_ejecutar_io_async(t_planificador* pl, t_pcb* pcb, t_tipo_io tipo, uint32_t param, uint32_t tamanio)`
**Qué hace:** Empaqueta los argumentos en un `t_io_async_args*` y lanza `hilo_ejecutar_io` detached.
**Dónde se usa:** `conexiones.c`, en `SYSCALL_SLEEP`, `SYSCALL_STDIN`, `SYSCALL_STDOUT`.
**Para qué:** Punto de entrada público para disparar una IO asíncrona sin bloquear al hilo que atiende la syscall.

### Mutex del Kernel con herencia de prioridades (líneas 1297–1474)

#### `static t_mutex_kernel* buscar_mutex_sin_lock(t_planificador* pl, char* nombre)`
**Qué hace:** Búsqueda lineal por nombre en `lista_mutex`, **sin tomar ningún lock** (el nombre de la función lo deja explícito — el caller es responsable de tener `mutex_lista_mutex` tomado).
**Dónde se usa:** `planificador_crear_mutex`, `planificador_lock_mutex`, `planificador_unlock_mutex` — siempre dentro de una sección ya protegida por `mutex_lista_mutex`.
**Para qué:** Evitar locks anidados/duplicados entre la búsqueda y la operación que la usa.

#### `static t_pcb* buscar_pcb_por_pid(t_planificador* pl, uint32_t pid)`
**Qué hace y cómo:** Busca secuencialmente en `lista_exec`, luego en todas las `colas_ready` (recorriendo las `cant_colas`), luego en `cola_block` (accediendo a `->elements` para poder iterar sin desencolar). Devuelve el primer match o `NULL`.
**Dónde se usa:** `planificador_lock_mutex`, para localizar al dueño actual de un mutex y aplicarle la herencia de prioridad.
**Para qué:** A diferencia de otras búsquedas del archivo (que ya sabían en qué colección buscar), esta necesita encontrar un PCB **sin saber su estado actual** — el dueño de un mutex puede estar ejecutando, listo, o incluso bloqueado por otra razón, así que recorre las tres colecciones donde un proceso "vivo y no bloqueado por este mismo mutex" podría estar. Nótese que no busca en `cola_susp_block`/`cola_susp_ready`/`lista_exit`: un proceso suspendido o terminado no puede ser dueño de un mutex en un estado consistente del sistema.

#### `static void cambiar_prioridad(t_planificador* pl, t_pcb* pcb, int nueva)`
**Qué hace y cómo:** Si la nueva prioridad es igual a la actual, no hace nada. Si el proceso está en `ESTADO_READY` y el algoritmo es CMN: bajo `mutex_cola_ready`, lo saca de la cola en la que esté (recorre las `cant_colas` buscando en cuál `list_remove_element` tiene éxito) y lo vuelve a insertar al **final** de la cola correspondiente a la nueva prioridad (clampeada). Si no está en READY (o no es CMN, donde el concepto de "cola por prioridad" no aplica), simplemente actualiza el campo sin mover nada de ninguna cola. Siempre loguea el cambio de prioridad con el formato `"## %u Cambio de prioridad: %d %d"`.
**Dónde se usa:** `planificador_lock_mutex` (para que el dueño herede) y `planificador_unlock_mutex` (para restaurar la original).
**Para qué:** Es el mecanismo real detrás de la herencia de prioridades — no es solo "cambiar un número en el struct", sino potencialmente reubicar al proceso en la cola correcta si está actualmente en READY esperando ser despachado.

#### `void planificador_crear_mutex(t_planificador* pl, char* nombre)`
**Qué hace:** Bajo `mutex_lista_mutex`, si no existe ya un mutex con ese nombre, lo crea y agrega a `lista_mutex`; si ya existía, solo loguea un warning (no falla, no duplica).
**Dónde se usa:** `conexiones.c`, `SYSCALL_MUTEX_CREATE`.
**Para qué:** Alta idempotente de un mutex nombrado.

#### `bool planificador_lock_mutex(t_planificador* pl, t_pcb* pcb, char* nombre)`
**Qué hace y cómo:** Bajo `mutex_lista_mutex`, busca el mutex; si no existe (caso que el comentario aclara que "no debería pasar" porque los scripts del TP no tienen errores semánticos, pero por robustez se maneja) lo crea on-demand. Si está libre: lo marca tomado por `pcb->pid`, `adquirido = true`. Si está tomado: encola el PID del solicitante en `cola_bloqueados` (FIFO real, `queue_push`), guarda quién es el dueño actual (`pid_duenio`), `adquirido = false`. Libera el lock de la lista de mutex. **Fuera** de ese lock (para no anidar con `mutex_lista_exec`/`mutex_cola_ready`/`mutex_cola_block` que puede tocar `buscar_pcb_por_pid`): si no se adquirió, busca el PCB del dueño actual; si el que se está bloqueando tiene **mayor** prioridad que el dueño (`pcb->prioridad_actual < duenio->prioridad_actual`, recordando que menor número = mayor prioridad), llama `cambiar_prioridad(dueño, prioridad_del_que_espera)` — la herencia. Devuelve si se adquirió o no.
**Dónde se usa:** `conexiones.c`, `SYSCALL_MUTEX_LOCK`.
**Para qué:** Implementa exclusión mutua real (cola FIFO de espera) más la herencia de prioridades tal como la describe el enunciado — nótese que la herencia solo se dispara si el solicitante es efectivamente más prioritario que el dueño actual (si el dueño ya es más prioritario o igual, no hay inversión de prioridades que resolver, y no se toca nada).

#### `void planificador_unlock_mutex(t_planificador* pl, t_pcb* pcb_liberador, char* nombre)`
**Qué hace y cómo:** Primero, fuera de cualquier lock del mutex: quita el nombre de `pcb_liberador->lista_mutex_tomados` (búsqueda lineal + `list_remove` + `free` del string). Si la prioridad actual del liberador difiere de su original (es decir, tenía una prioridad heredada), la restaura con `cambiar_prioridad` — **esto ocurre incondicionalmente al liberar el mutex, no solo si había alguien esperando**, lo cual es correcto: la herencia se debe a que este proceso *tenía tomado* el mutex, no a que alguien lo esté esperando en este preciso instante. Después, bajo `mutex_lista_mutex`: busca el mutex (si no existe, retorna sin hacer nada más); si hay alguien en `cola_bloqueados`, lo desencola (FIFO, primer solicitante servido primero) y lo convierte en el nuevo dueño (`pid_duenio`, `tomado = true`); si no hay nadie, queda libre (`pid_duenio = 0`, `tomado = false`). Libera el lock. Si había un siguiente PID: llama `planificador_desbloquear` (contempla que ese proceso pueda haberse suspendido mientras esperaba el mutex — en ese caso pasa a SUSP_READY, **con el mutex ya tomado**, algo que el comentario del código señala explícitamente); si el desbloqueo tuvo éxito, agrega el nombre del mutex a la `lista_mutex_tomados` del que se despertó y loguea que lo tomó; si estaba suspendido, además dispara `planificador_evento_memoria_liberada` (para intentar des-suspenderlo del todo, ya que quedó con el mutex resuelto).
**Dónde se usa:** `conexiones.c` (`SYSCALL_MUTEX_UNLOCK`) y `finalizar_proceso` (al terminar un proceso, para liberar todos sus mutex tomados uno por uno).
**Para qué:** Es la contraparte completa de `planificador_lock_mutex` — libera el recurso, restaura la prioridad del que lo tenía, y le pasa la posesión al siguiente en la cola FIFO si corresponde.

**Dependencias del archivo:** `planificador.h` (propio), `utils/protocolo.h` (para los `t_codigo_mensaje` que habla con KM).

---

## Hallazgos consolidados del módulo

1. **`planificador_obtener_cpu_por_socket`** (`planificador.c:856`, declarada en `planificador.h:172`) — implementada correctamente, cero callers en todo el repo. Código muerto.
2. **`t_pedido_io`** (`io_scheduler.h:20-27`) — struct declarado, nunca instanciado; el pedido de IO real que se usa en producción es el struct local `t_io_async_args` de `planificador.c:1206-1212`, con campos equivalentes pero es una definición completamente distinta. Código muerto.
3. Todas las demás funciones públicas declaradas en `planificador.h` y `conexiones.h` tienen al menos un caller real verificado — no se encontraron más divergencias entre lo declarado y lo usado.