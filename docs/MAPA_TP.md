# Mapa TP "Plug & Pray" — Enunciado ↔ Código

Trazabilidad entre cada funcionalidad del enunciado (`Plug & Pray.pdf`, v1.1) y las funciones
que la implementan en cada módulo. Formato de referencia: `archivo:línea`.

Módulos: `kernel_scheduler` (KS), `kernel_memory` (KM), `cpu`, `memory_stick` (MS), `io`, `swap`, `utils`.

---

## 0. Contrato común (utils)

Todo el sistema habla el mismo protocolo. Antes de leer cualquier funcionalidad, mirar acá.

| Concepto | Definición |
|---|---|
| Códigos de handshake | `utils/src/utils/protocolo.h:8-14` |
| Códigos de mensaje `t_codigo_mensaje` | `utils/src/utils/protocolo.h:20-48` |
| Códigos de syscall `t_codigo_syscall` | `utils/src/utils/protocolo.h:54-67` |
| Estados del proceso (7 estados) | `utils/src/utils/estados.h:8-16`, `estado_to_string()` en `:21` |
| Registros de CPU (contrato CPU↔KM) | `utils/src/utils/registros.h:10-25` |
| Entrada de tabla de segmentos | `utils/src/utils/segmento.h:6-11` |
| Códigos de instrucción | `utils/src/utils/instrucciones.h:11-32` |
| Sockets (`crear_servidor`, `crear_conexion`) | `utils/src/utils/sockets/sockets.c` |

### Mapa de handshakes (quién se conecta a quién)

| Handshake | Cliente → Servidor | Se atiende en |
|---|---|---|
| `HANDSHAKE_CPU` = 1 | CPU → KS | `kernel_scheduler/src/conexiones/conexiones.c:71` |
| `HANDSHAKE_CPU` = 1 | CPU → KM | `kernel_memory/src/conexiones/conexiones.c:103` → `atender_cpu()` |
| `HANDSHAKE_CPU` = 1 | CPU → Memory Stick | `memory_stick/src/conexiones/conexiones.c:132` |
| `HANDSHAKE_IO` = 2 | IO → KS | `kernel_scheduler/src/conexiones/conexiones.c:86` |
| `HANDSHAKE_SCHEDULER` = 3 | KS → KM (canal principal) | `kernel_memory/src/conexiones/conexiones.c:106` → `atender_scheduler()` |
| `HANDSHAKE_MEMORY_STICK` = 4 | MS → KM | `kernel_memory/src/conexiones/conexiones.c:117` → `atender_memory_stick()` |
| `HANDSHAKE_SWAP` = 5 | SWAP → KM | `kernel_memory/src/conexiones/conexiones.c:120` |
| `HANDSHAKE_SCHEDULER_NOTIF` = 6 | KS → KM (2ª conexión, canal de push KM→KS) | `kernel_memory/src/conexiones/conexiones.c:109` |
| `HANDSHAKE_KERNEL_MEMORY` = 7 | KM → Memory Stick (canal de datos) | `memory_stick/src/conexiones/conexiones.c:142` |

> **Decisión de diseño clave:** el KS abre **dos** conexiones a KM. La principal es
> request/response sincrónico bajo `mutex_socket_memoria`; la segunda (`_NOTIF`) es
> unidireccional KM→KS y es la que transporta `MAS_MEMORIA` y `MEMORIA_CORRUPTA`.
> Sin esa separación, el BSOD y el aviso de stick nuevo se intercalarían con respuestas
> pendientes del canal principal y romperían el protocolo.

---

## 1. Arranque y conexión inicial (CP1)

| Módulo | `main()` | Args | Conexión |
|---|---|---|---|
| KS | `kernel_scheduler/src/main.c:11` | `[config] [proceso inicial]` | `conectar_a_kernel_memory()` `:55` + `conectar_canal_notificaciones_km()` `:68`, luego `iniciar_servidor_kernel_scheduler()` `:82` |
| KM | `kernel_memory/src/main.c:9` | `[config]` | `iniciar_servidor_kernel_memory()` `:60` |
| CPU | `cpu/src/main.c:9` | `[config] [id]` | `conectar_a_kernel_memory()` `:54`, `conectar_a_kernel_scheduler()` `:55`, `actualizar_topologia_sticks()` `:62` |
| MS | `memory_stick/src/main.c:9` | `[config] [tamaño]` | `crear_escucha_memory_stick()` `:67` **antes** de `conectar_a_kernel_memory()` `:76` |
| IO | `io/src/main.c:13` | `[config] [tipo]` | `conectar_a_kernel_scheduler()` `:68` |
| SWAP | `swap/src/main.c:8` | `[config]` | `conectar_y_atender_kernel_memory()` `:59` |

Servidores multihilo (accept → `pthread_create(manejar_cliente)` → `pthread_detach`):
- KS: `kernel_scheduler/src/conexiones/conexiones.c:20` (loop `:33`), dispatcher `manejar_cliente()` `:49`
- KM: `kernel_memory/src/conexiones/conexiones.c:20` (loop `:38`), dispatcher `manejar_cliente()` `:89`
- MS: `memory_stick/src/conexiones/conexiones.c:34`, dispatcher `manejar_cliente()` `:117`

Config y logger por módulo: `*/src/config/*_config.c` y `*/src/logger/*_logger.c`.
Helper genérico de boot: `utils/src/utils/boot_loader/boot_loader.c:17` (`system_boot`).

---

## 2. KERNEL SCHEDULER

### 2.1 Estructuras base

| Qué | Dónde |
|---|---|
| `t_planificador` (todas las colas, mutex y semáforos) | `kernel_scheduler/src/planificador/planificador.h:73-128` |
| `t_pcb` | `kernel_scheduler/src/pcb/pcb.h:16-48`, `pcb_create()` en `pcb.c` |
| `t_cpu_conectada` | `planificador.h:33-52` |
| `t_io_conectada` | `planificador.h:58-64` |
| `t_mutex_kernel` | `kernel_scheduler/src/mutex_kernel/mutex_kernel.h:12-17` |
| `t_tipo_io` | `kernel_scheduler/src/io_scheduler/io_scheduler.h:11-15` |
| Creación / destrucción | `planificador_create()` `planificador.c:113`, `planificador_destroy()` `:197` |

### 2.2 Planificación de Largo Plazo (NEW → READY)

Enunciado p.10: "ingresar Procesos de NEW a READY... sin ninguna limitación".

| Paso | Función |
|---|---|
| Hilo dedicado | `hilo_planificador_largo_plazo()` — `planificador.c:249` |
| Encolar en NEW + log de creación | `planificador_encolar_new()` — `planificador.c:434` |
| Registrar el proceso en KM (crea contexto) | `planificador.c:264-279` → envía `MENSAJE_ENVIAR_PID` |
| Recepción en KM | `atender_scheduler()` case `MENSAJE_ENVIAR_PID` — `kernel_memory/src/atencion_scheduler/kernel_memory_atencion_scheduler.c:24` |
| Transición | `planificador_transicionar_new_a_ready()` — `planificador.c:443` |
| PID 0 (proceso inicial) | `kernel_scheduler/src/main.c:78-79` |
| PIDs siguientes | `proximo_pid` bajo `mutex_pid` — `conexiones.c:387-389` (syscall `INIT_PROC`) |

### 2.3 Planificación de Corto Plazo (READY → EXEC)

| Concepto | Función |
|---|---|
| Hilo dedicado | `hilo_planificador_corto_plazo()` — `planificador.c:382` |
| Selección de cola (FIFO/RR = 1 cola; CMN = N colas) | `cola_de()` `planificador.c:18`, `es_cmn()` `:12` |
| Algoritmo por cola (`QUEUES_ALGORITHMS`) | `cola_usa_rr()` — `planificador.c:26` |
| Armado de colas según config | `planificador_create()` — `planificador.c:118-140` |
| Encolar en READY | `encolar_en_ready()` — `planificador.c:77` |
| Desencolar (cola de menor índice primero = más prioritaria) | `desencolar_ready()` — `planificador.c:97` |
| Despacho efectivo a CPU | `despachar()` — `planificador.c:338` |
| Recepción del PID en la CPU | `atender_peticiones_kernel_scheduler()` case `MENSAJE_ENVIAR_PID` — `cpu/src/cpu/cpu.c:64` |

**Quantum (RR):**
- Lanzamiento del timer: `despachar()` — `planificador.c:364-374`
- Hilo del timer: `hilo_timer_quantum()` — `planificador.c:298`
- Envío de `MENSAJE_INTERRUPCION`: `planificador.c:322-323`
- Guarda anti-timer-viejo: campo `rafaga` (`planificador.h:46`), chequeo en `:313-315`
- Recepción del desalojo: `atender_peticiones_cpu()` case `INT_QUANTUM` — `conexiones.c:430-434`

**Desalojo por cola más prioritaria (CMN + `QUEUE_PREEMPTION`):**
- `chequear_desalojo_por_prioridad()` — `planificador.c:32` (llamada desde `encolar_en_ready()` `:92`)
- Elección de la víctima (peor prioridad en EXEC): `planificador.c:41-50`
- Log obligatorio + interrupción: `planificador.c:63-68`
- Retorno del proceso: `conexiones.c:424-429` (`INT_PREEMPCION`)

**Validación de prioridad fuera de rango (CMN):** `conexiones.c:381-384` y `:396-399`.

### 2.4 Planificación de Mediano Plazo

**BLOCK → SUSP. BLOCK (por `SUSPENSION_TIMEOUT`):**

| Paso | Función |
|---|---|
| Arranque del timer al entrar a BLOCK | `planificador_transicionar_exec_a_block()` — `planificador.c:574` (timer en `:596-605`) |
| Hilo del timer | `hilo_timer_suspension()` — `planificador.c:513` |
| Guarda anti-timer-viejo | campo `bloqueo_id` (`pcb.h:34`), chequeo en `planificador.c:530` |
| Transición atómica block→susp | `planificador.c:525-549` |
| Pedido de swap a KM | `MENSAJE_SUSPENDER_PROCESO` — `planificador.c:555-567` |
| Excepción: bloqueados por memoria no se suspenden | `planificador.c:596` |

**SUSP. BLOCK → SUSP. READY:** `planificador_desbloquear()` — `planificador.c:640` (rama suspendida `:650-673`).

**SUSP. READY → READY (des-suspensión):**

| Paso | Función |
|---|---|
| Punto de entrada único | `planificador_evento_memoria_liberada()` — `planificador.c:733` |
| Orden por prioridad y luego antigüedad | `comparar_candidatos()` — `planificador.c:709` + `qsort` en `:751` |
| Pedido a KM | `dessuspender_en_km()` — `planificador.c:719` (`MENSAJE_DESSUSPENDER_PROCESO`) |
| Transición a READY | `planificador.c:756-777` |

**Disparadores de `planificador_evento_memoria_liberada()`** (los 5 eventos del enunciado):
1. `MEM_FREE` → `conexiones.c:355`
2. Fin de proceso → `finalizar_proceso()` `conexiones.c:154`
3. Suspensión de otro proceso → `planificador.c:570`
4. Compactación terminada → `conexiones.c:332`
5. Memory Stick nuevo (`MENSAJE_MAS_MEMORIA`) → `conexiones.c:476`
6. Fin de IO de un suspendido → `planificador.c:1277`; mutex liberado a un suspendido → `planificador.c:1471`

### 2.5 Transiciones de estado (todas)

| Transición | Función | Línea |
|---|---|---|
| → NEW | `planificador_encolar_new()` | `planificador.c:434` |
| NEW → READY | `planificador_transicionar_new_a_ready()` | `:443` |
| READY → EXEC | inline en `hilo_planificador_corto_plazo()` | `:414-425` |
| EXEC → EXIT | `planificador_transicionar_exec_a_exit()` | `:467` |
| EXEC → READY | `planificador_transicionar_exec_a_ready()` | `:495` |
| EXEC → READY (al **frente**, por compactación) | `planificador_transicionar_exec_a_ready_frente()` | `:499` |
| EXEC → BLOCK | `planificador_transicionar_exec_a_block()` | `:574` |
| BLOCK → READY | `planificador_transicionar_block_a_ready()` | `:608` |
| BLOCK → SUSP. BLOCK | `hilo_timer_suspension()` | `:513` |
| SUSP. BLOCK → SUSP. READY | `planificador_desbloquear()` | `:640` |
| SUSP. READY → READY | `planificador_evento_memoria_liberada()` | `:733` |
| Helper: sacar de EXEC | `remover_de_exec()` | `:453` |

### 2.6 Atención de Syscalls

**Punto de entrada único:** `atender_peticiones_cpu()` — `kernel_scheduler/src/conexiones/conexiones.c:157`
(un hilo por CPU conectada; el `switch` de syscalls arranca en `:197`).

| Syscall | Emisor (CPU) | Receptor (KS) | Comportamiento |
|---|---|---|---|
| `SLEEP` | `enviar_syscall_sleep()` `cpu/src/conexiones/conexiones.c:64` | `conexiones.c:218` | EXEC→BLOCK + IO async, libera CPU |
| `STDIN` | `enviar_syscall_stdin()` `:81` | `conexiones.c:232` | EXEC→BLOCK + IO async |
| `STDOUT` | `enviar_syscall_stdout()` `:89` | `conexiones.c:232` | EXEC→BLOCK + IO async |
| `MUTEX_CREATE` | `enviar_syscall_mutex_create()` `:106` | `conexiones.c:249` | crea + **retorno directo a la misma CPU** |
| `MUTEX_LOCK` | `enviar_syscall_mutex_lock()` `:115` | `conexiones.c:267` | si lo toma → retorno directo; si no → BLOCK |
| `MUTEX_UNLOCK` | `enviar_syscall_mutex_unlock()` `:124` | `conexiones.c:294` | libera + retorno directo |
| `MEM_ALLOC` | `enviar_syscall_mem_alloc()` `:133` | `conexiones.c:313` | crea segmento (puede compactar) + retorno directo; sin espacio → BLOCK "MEMORIA" |
| `MEM_FREE` | `enviar_syscall_mem_free()` `:141` | `conexiones.c:343` | elimina segmento + retorno directo + evento memoria |
| `INIT_PROC` | `enviar_syscall_init_proc()` `:148` | `conexiones.c:362` | crea PCB nuevo en NEW; el llamante vuelve a READY |
| `EXIT` | `enviar_syscall_exit()` `:71` | `conexiones.c:206` | `finalizar_proceso(pcb, "SUCCESS")` |
| `SEG_FAULT` | `enviar_syscall_seg_fault()` `:75` | `conexiones.c:212` | `finalizar_proceso(pcb, "SEG_FAULT")` |

Helper común de envío desde CPU (incrementa PC y manda `MENSAJE_SYSCALL` + código): `cpu/src/conexiones/conexiones.c:50`.

**Retorno directo a la misma CPU** (mutex y memoria, exigido por el enunciado p.10-11):
`planificador_despachar_directo()` — `planificador.c:377` → reusa `despachar()` `:338`.

**Liberación de CPU y finalización:**
- `liberar_cpu()` — `conexiones.c:128`
- `finalizar_proceso()` — `conexiones.c:139` (transiciona a EXIT, libera todos sus mutex, avisa a KM, dispara evento de memoria)

### 2.7 Desalojo por compactación

Flujo completo (KM pide, KS desaloja, KM compacta):

| # | Paso | Dónde |
|---|---|---|
| 1 | KM detecta que hace falta compactar | `gestor_memoria_crear_segmento()` → `CREAR_SEGMENTO_NECESITA_COMPACTACION` — `kernel_memory/src/gestor_memoria/gestor_memoria.c:285-288` |
| 2 | KM envía `MENSAJE_COMPACTACION` al KS | `kernel_memory_atencion_scheduler.c:63-68` |
| 3 | KS recibe la respuesta y pausa la planificación | `planificador_crear_segmento_km()` `planificador.c:1006-1011`; `pausar_planificacion()` `:955` |
| 4 | KS interrumpe todas las CPUs en ráfaga | `interrumpir_cpus_para_compactar()` — `planificador.c:970` (loop de espera en `:1016-1018`) |
| 5 | CPU recibe la interrupción y devuelve el contexto | `cpu_check_interrupt()` — `cpu/src/instrucciones/instrucciones.c:337`; `enviar_contexto_a_ks()` `cpu/src/conexiones/conexiones.c:97` |
| 6 | KS reencola **al frente** de READY | `INT_COMPACTACION` — `conexiones.c:419-423` → `planificador_transicionar_exec_a_ready_frente()` |
| 7 | KS confirma con `MENSAJE_DESALOJO_OK` | `planificador.c:1021-1022` |
| 8 | KM compacta | `gestor_memoria_compactar()` — `gestor_memoria.c:566` |
| 9 | KM reintenta la creación del segmento y responde | `kernel_memory_atencion_scheduler.c:77-83` |
| 10 | KS reanuda la planificación | `reanudar_planificacion()` — `planificador.c:961` |

Barrera de despacho durante la compactación: `despachar()` espera en
`cond_compactacion` — `planificador.c:341-344`.

### 2.8 Mutex y herencia de prioridades

| Operación | Función |
|---|---|
| Crear | `planificador_crear_mutex()` — `planificador.c:1373` |
| Lock | `planificador_lock_mutex()` — `planificador.c:1384` |
| Unlock | `planificador_unlock_mutex()` — `planificador.c:1423` |
| Búsqueda interna | `buscar_mutex_sin_lock()` — `planificador.c:1301` |
| Estructura + cola FIFO de bloqueados | `mutex_kernel.h:12-17`, `mutex_kernel_create()` en `mutex_kernel.c` |
| Orden de desbloqueo = orden de pedido | `queue_push` en `planificador.c:1404`, `queue_pop` en `:1447` |

**Herencia de prioridades (inversión de prioridades):**
- Detección y aplicación al bloquearse: `planificador.c:1411-1418`
- Búsqueda del dueño en EXEC/READY/BLOCK: `buscar_pcb_por_pid()` — `planificador.c:1310`
- Cambio de prioridad efectiva (+ reubicación de cola en CMN + log obligatorio): `cambiar_prioridad()` — `planificador.c:1348`
- Restauración a la prioridad original al liberar: `planificador.c:1435-1437`
- Campos `prioridad_original` / `prioridad_actual`: `pcb.h:22-23`

**Liberación forzada al finalizar un proceso** (para no dejar mutex tomados para siempre):
`finalizar_proceso()` — `conexiones.c:144-149`.

### 2.9 IO (SLEEP / STDIN / STDOUT)

| Paso | Función |
|---|---|
| Registro del dispositivo al conectarse | `planificador_registrar_io()` — `planificador.c:1136` (handshake en `conexiones.c:86-116`) |
| Remoción / desconexión en caliente | `planificador_remover_io()` — `planificador.c:1152` |
| Tomar dispositivo libre (bloquea con semáforo por tipo) | `planificador_tomar_io_libre()` — `planificador.c:1175`; `sem_de_tipo_io()` `:1120` |
| Liberar dispositivo | `planificador_liberar_io()` — `planificador.c:1192` |
| Lanzar la IO en hilo aparte | `planificador_ejecutar_io_async()` — `planificador.c:1284` |
| Cuerpo de la IO | `hilo_ejecutar_io()` — `planificador.c:1226` |

**Por tipo:**

| Tipo | KS | IO |
|---|---|---|
| SLEEP | `planificador.c:1256-1258` (manda pid + ms, espera OK) | `io/src/main.c:159-178` (`usleep`, log obligatorio `:166`) |
| STDOUT | `planificador.c:1235-1244`: pide el contenido a KM (`planificador_leer_memoria_en_km()` `:904`) y lo manda a la IO | `io/src/main.c:117-157` (imprime por pantalla `:142` y en log `:146`) |
| STDIN | `planificador.c:1246-1255`: pide los bytes a la IO y los escribe en KM (`planificador_escribir_memoria_en_km()` `:932`) | `io/src/main.c:80-115` (lee por teclado `:97-102`, rellena con `\0` `:104-107`) |

> **Decisión:** al KS/KM se le pasa la **dirección lógica**, no la física. KM la traduce
> recién al momento de leer/escribir (`kernel_memory_atencion_scheduler.c:110` y `:139`),
> así sigue siendo válida aunque hubo compactación o suspensión mientras el proceso
> estaba en BLOCK.

Fin de IO (READY o SUSP. READY según dónde esté el proceso): `planificador.c:1268-1278`.

### 2.10 BSOD (Blue Screen of Death)

| Paso | Dónde |
|---|---|
| Origen: se desconecta un Memory Stick | `atender_memory_stick()` — `kernel_memory/src/conexiones/conexiones.c:84-86` |
| KM marca el stick caído y notifica | `gestor_memoria_stick_desconectado()` — `gestor_memoria.c:127` |
| Envío por el canal de notificaciones | `notificar_ks(gm, MENSAJE_MEMORIA_CORRUPTA, NULL)` — `gestor_memoria.c:139`; `notificar_ks()` en `:78` |
| KS escucha el canal | `hilo_notificaciones_km()` — `kernel_scheduler/src/conexiones/conexiones.c:459` (case en `:479`) |
| Ejecución del BSOD | `planificador_bsod()` — `planificador.c:1083` |
| Pausa de planificación | `pausar_planificacion()` — `planificador.c:1086` |
| Finalización de TODAS las colas | `bsod_finalizar_cola()` `:1079` / `bsod_finalizar_lista()` `:1072`, aplicados a NEW `:1089`, READY `:1094`, EXEC `:1099`, BLOCK `:1103`, SUSP.BLOCK y SUSP.READY `:1107-1108` |
| Fin del proceso KS | `exit(EXIT_FAILURE)` — `planificador.c:1113` |

### 2.11 Gestión de CPUs conectadas

| Qué | Función |
|---|---|
| Registrar | `planificador_registrar_cpu()` — `planificador.c:812` |
| Remover (desconexión en caliente, reencola su proceso) | `planificador_remover_cpu()` — `planificador.c:832` |
| Buscar por socket / por id | `planificador.c:856` / `:870` |
| Escucha permanente de nuevas conexiones | loop de `accept` en `conexiones.c:33` |

---

## 3. KERNEL MEMORY

### 3.1 Estructuras

| Qué | Dónde |
|---|---|
| `t_gestor_memoria` (sticks, segmentos, swap, notif) | `kernel_memory/src/gestor_memoria/gestor_memoria.h:63-84` |
| `t_stick` | `gestor_memoria.h:18-26` |
| `t_segmento_fisico` (segmento ocupado en memoria global) | `gestor_memoria.h:32-37` |
| `t_segmento_swap` (segmento swapeado + sus bloques) | `gestor_memoria.h:43-48` |
| `t_gestor_procesos` / `t_proceso_km` | `kernel_memory/src/gestor_procesos/kernel_memory_gestor_procesos.h` |
| `t_contexto_ejecucion` | `kernel_memory/src/contexto/contexto.h` |
| Creación / destrucción | `gestor_memoria_create()` `gestor_memoria.c:16`, `gestor_memoria_destroy()` `:55` |

### 3.2 Dispatchers

- Peticiones del **KS**: `atender_scheduler()` — `kernel_memory/src/atencion_scheduler/kernel_memory_atencion_scheduler.c:9`
- Peticiones de la **CPU**: `atender_cpu()` — `kernel_memory/src/atencion_cpu/kernel_memory_atencion_cpu.c:10`
- Alta de **Memory Sticks**: `atender_memory_stick()` — `kernel_memory/src/conexiones/conexiones.c:56`
- Alta de **SWAP**: `kernel_memory/src/conexiones/conexiones.c:120-136`

### 3.3 Memoria de instrucciones y contextos

| Funcionalidad | Función |
|---|---|
| Asociar PID ↔ archivo de pseudocódigo | `gestor_agregar_proceso()` — `kernel_memory_gestor_procesos.c:29` |
| Armado del path con `SCRIPTS_BASEPATH` | `kernel_memory_atencion_scheduler.c:35-37` |
| Obtener instrucción por PC (lee la línea N del archivo) | `gestor_obtener_instruccion()` — `kernel_memory_gestor_procesos.c:84` |
| Respuesta a la CPU + `INSTRUCTION_DELAY` | `atender_cpu()` case `MENSAJE_PEDIR_INSTRUCCION` — `kernel_memory_atencion_cpu.c:63` (delay en `:68`) |
| Enviar contexto (registros + tabla de segmentos) | `atender_cpu()` case `MENSAJE_CONTEXTO` — `kernel_memory_atencion_cpu.c:31` |
| Recibir contexto actualizado desde el KS | `atender_scheduler()` case `MENSAJE_ACTUALIZAR_CONTEXTO` — `kernel_memory_atencion_scheduler.c:188` |
| Inicialización de registros en 0 | `kernel_memory_gestor_procesos.c:32` |
| Sincronización del gestor | `gestor_lock()` / `gestor_unlock()` — `kernel_memory_gestor_procesos.c:76` / `:80` |

### 3.4 Operaciones sobre memoria de usuario

| Operación (enunciado) | Función de KM | Invocada desde |
|---|---|---|
| **Creación de Proceso** | `gestor_agregar_proceso()` `kernel_memory_gestor_procesos.c:29` | `atencion_scheduler.c:24` (`MENSAJE_ENVIAR_PID`) |
| **Creación de Segmento** | `gestor_memoria_crear_segmento()` `gestor_memoria.c:272` | `atencion_scheduler.c:54` (`MENSAJE_CREAR_SEGMENTO`) |
| **Eliminación de Segmento** | `gestor_memoria_eliminar_segmento()` `gestor_memoria.c:310` | `atencion_scheduler.c:87` |
| **Finalización de Proceso** | `gestor_memoria_finalizar_proceso()` `gestor_memoria.c:348` | `atencion_scheduler.c:99` |
| **Suspensión de Proceso** | `gestor_memoria_suspender_proceso()` `gestor_memoria.c:651` | `atencion_scheduler.c:166` |
| **Des-suspensión de Proceso** | `gestor_memoria_dessuspender_proceso()` `gestor_memoria.c:760` | `atencion_scheduler.c:177` |
| **Lectura de datos** (camino STDOUT) | `gestor_memoria_io_rw(..., escribir=false)` `gestor_memoria.c:895` | `atencion_scheduler.c:110` |
| **Escritura de datos** (camino STDIN) | `gestor_memoria_io_rw(..., escribir=true)` `gestor_memoria.c:895` | `atencion_scheduler.c:139` |
| **Compactación** | `gestor_memoria_compactar()` `gestor_memoria.c:566` | `atencion_scheduler.c:77` |
| Espacio libre | `gestor_memoria_espacio_libre()` `gestor_memoria.c:245` | `atencion_scheduler.c:48` |

### 3.5 Algoritmos de selección de huecos (BEST / WORST FIT)

| Pieza | Función |
|---|---|
| Punto de entrada | `buscar_hueco()` — `gestor_memoria.c:191` |
| Lectura de `ALLOCATION_STRATEGY` | `estrategia_es_best_fit()` — `gestor_memoria.c:172` |
| Criterio de comparación (menor hueco vs mayor hueco) | `hueco_es_mejor_que_el_actual()` — `gestor_memoria.c:180` |
| Derivación de huecos a partir de los segmentos ocupados | `segmentos_ordenados()` `:158` + `comparar_por_base()` `:150`; recorrido en `:207-228` |
| Validación de `SEGMENT_MAX_SIZE` | `gestor_memoria.c:273-277` |
| Distinción "sin espacio" vs "necesita compactar" | `gestor_memoria.c:282-292` + `espacio_libre_sin_lock()` `:236` |
| Reflejo en la tabla de segmentos del proceso | `tabla_proceso_agregar()` — `gestor_memoria.c:253` |

> `buscar_hueco()` se usa en **dos** lugares: creación de segmento (`:282`) y
> des-suspensión (`:783`), tal como pide el enunciado ("al des-suspender se deberá
> utilizar el algoritmo de búsqueda de huecos").

### 3.6 Compactación (interna)

| Paso | Función |
|---|---|
| Entrada + logs obligatorios | `gestor_memoria_compactar()` — `gestor_memoria.c:566` (logs `:567` y `:595`) |
| Recorrido y amontonado al principio | `gestor_memoria.c:577-587` |
| Mover un segmento a su nueva base | `mover_segmento()` — `gestor_memoria.c:551` |
| Copia física cruzando fronteras de sticks | `copiar_segmento_entre_sticks()` — `gestor_memoria.c:500` |
| Actualización de la tabla de segmentos del dueño | `actualizar_base_de_segmento_en_proceso()` — `gestor_memoria.c:528` |
| `COMPACTION_DELAY` | `gestor_memoria.c:593` |

### 3.7 Lectura / escritura física y traducción

| Qué | Función |
|---|---|
| División del pedido entre sticks + consolidación | `operar_fisico()` — `gestor_memoria.c:419` |
| Operación contra un stick puntual | `stick_operar()` — `gestor_memoria.c:396` |
| Resolución dirección global → stick | `stick_para_direccion()` — `gestor_memoria.c:385` |
| API pública | `gestor_memoria_leer_fisico()` `:450`, `gestor_memoria_escribir_fisico()` `:454` |
| Traducción lógica → física (lado KM) | `gestor_memoria_traducir()` — `gestor_memoria.c:462` |
| Camino IO con fallback a SWAP | `gestor_memoria_io_rw()` — `gestor_memoria.c:895`; `swap_io_rw()` `:852` |

### 3.8 SWAP (lado KM: administración de bloques)

| Qué | Función |
|---|---|
| Registro del módulo SWAP + bitmap de bloques | `gestor_memoria_registrar_swap()` — `gestor_memoria.c:602` |
| Escribir/leer un bloque | `swap_escribir_bloque()` `:615`, `swap_leer_bloque()` `:626` |
| Contar bloques libres | `swap_bloques_libres()` — `gestor_memoria.c:634` |
| Buscar segmento swapeado | `buscar_segmento_swap()` — `gestor_memoria.c:643` |
| Suspender: pre-chequeo de bloques + copia de a 1 segmento | `gestor_memoria_suspender_proceso()` — `gestor_memoria.c:651` (pre-chequeo `:673-685`, loop `:688-751`) |
| Des-suspender: reserva atómica de todos los huecos, con rollback | `gestor_memoria_dessuspender_proceso()` — `gestor_memoria.c:760` (reserva `:777-806`, restauración `:810-837`) |
| Liberación de bloques al finalizar un proceso | `gestor_memoria_finalizar_proceso()` — `gestor_memoria.c:361-373` |

> "Los bloques de SWAP se asignan por segmento, por lo que no puede existir un bloque
> que contenga información de 2 segmentos distintos" → cada `t_segmento_swap` tiene su
> propia lista de bloques (`gestor_memoria.h:47`) y nunca se comparten (`gestor_memoria.c:704-719`).

### 3.9 Memory Sticks: alta, baja y topología

| Evento | Función |
|---|---|
| Alta (recibe `INFORMAR_TAMANIO`) | `atender_memory_stick()` — `kernel_memory/src/conexiones/conexiones.c:56` |
| Registro + apertura del canal de datos KM→stick + ampliación de la memoria total | `gestor_memoria_registrar_stick()` — `gestor_memoria.c:93` |
| Notificar "más memoria" al KS | `notificar_ks(MENSAJE_MAS_MEMORIA)` — `gestor_memoria.c:123` |
| Detección de la baja (recv que retorna) | `kernel_memory/src/conexiones/conexiones.c:85` |
| Baja + notificar memoria corrupta | `gestor_memoria_stick_desconectado()` — `gestor_memoria.c:127` |
| Serializar la topología para las CPUs | `gestor_memoria_serializar_topologia()` — `gestor_memoria.c:961` (helpers `:925`, `:932`, `:943`) |
| Respuesta al pedido de la CPU | `atender_cpu()` case `MENSAJE_TOPOLOGIA_STICKS` — `kernel_memory_atencion_cpu.c:84` |

---

## 4. CPU

### 4.1 Estructura y arranque

| Qué | Dónde |
|---|---|
| `struct t_cpu` (registros, sockets, tabla de segmentos, sticks) | `cpu/src/cpu/cpu.h` |
| Creación / destrucción | `cpu_create()` `cpu/src/cpu/cpu.c:14`, `cpu_destroy()` `:39` |
| Loop principal (espera PID del KS) | `atender_peticiones_kernel_scheduler()` — `cpu/src/cpu/cpu.c:50` |
| Recepción del contexto al recibir un PID | `solicitar_contexto_a_km()` — `cpu/src/conexiones/conexiones.c:158` |
| Recepción de `SEGMENT_MAX_SIZE` desde KM | `conectar_a_kernel_memory()` — `cpu/src/conexiones/conexiones.c:44` (KM lo manda en `kernel_memory_atencion_cpu.c:21-22`) |

### 4.2 Ciclo de instrucción

| Etapa | Función | Línea |
|---|---|---|
| Loop del ciclo | `cpu/src/cpu/cpu.c:80-131` | |
| **Fetch** | `cpu_fetch()` | `cpu/src/instrucciones/instrucciones.c:60` (log obligatorio `:61`) |
| **Decode** | `cpu_decode()` | `cpu/src/instrucciones/instrucciones.c:81` (tabla de opcodes `:95-116`, parseo de params `:125-134`) |
| **Execute** | `cpu_execute()` | `cpu/src/instrucciones/instrucciones.c:139` (log obligatorio `:141`) |
| **Check Interrupt** | `cpu_check_interrupt()` | `cpu/src/instrucciones/instrucciones.c:337` (`MSG_DONTWAIT` en `:339`) |
| Incremento de PC (+ excepción si la instrucción lo modificó) | `cpu/src/cpu/cpu.c:118-123` | |
| Envío del contexto al KS por interrupción | `enviar_contexto_a_ks()` | `cpu/src/conexiones/conexiones.c:97` |

### 4.3 MMU

| Qué | Dónde |
|---|---|
| Traducción lógica → física | `mmu_traducir()` — `cpu/src/instrucciones/instrucciones.c:23` |
| `num_segmento = dir / seg_max`, `desplazamiento = dir % seg_max` | `instrucciones.c:25-26` |
| Búsqueda en la tabla de segmentos local | `instrucciones.c:30-37` |
| SEG_FAULT por segmento inexistente | `instrucciones.c:40-44` |
| SEG_FAULT por desplazamiento + tamaño > límite | `instrucciones.c:47-51` |
| División del pedido entre sticks + consolidación | `operar_fisico()` — `cpu/src/conexiones/conexiones.c:277` |
| Operación contra un stick puntual | `stick_operar()` — `cpu/src/conexiones/conexiones.c:257` |
| Resolución dirección → stick | `stick_para_direccion()` — `cpu/src/conexiones/conexiones.c:246` |
| API pública | `leer_memoria_fisica()` `:313`, `escribir_memoria_fisica()` `:317` |
| Refresco de topología (sticks nuevos) | `actualizar_topologia_sticks()` — `cpu/src/conexiones/conexiones.c:190` (llamada en cada despacho: `cpu/src/cpu/cpu.c:75`; reintento en `conexiones.c:285-289`) |

### 4.4 Registros

| Qué | Función |
|---|---|
| Get/set 8 bits (AX..DX) | `get_registro()` / `set_registro()` — `cpu/src/registros_cpu/registros_cpu.c:6` / `:14` |
| Get/set 32 bits (E**, SI, DI, PC + promoción de 8 bits) | `get_registro_32()` / `set_registro_32()` — `registros_cpu.c:21` / `:44` |
| Tamaño del registro (1 o 4 bytes) | `tamanio_registro()` — `registros_cpu.c:36` |

### 4.5 Instrucciones — dónde se ejecuta cada una

Todas dentro del `switch` de `cpu_execute()` — `cpu/src/instrucciones/instrucciones.c:144`.

**Resueltas por la CPU:**

| Instrucción | Línea | Notas |
|---|---|---|
| `NOOP` | `:145` | |
| `SET` | `:149` | usa `set_registro_32()` (resuelve cualquier registro, incluido PC) |
| `SUM` | `:156` | |
| `SUB` | `:163` | |
| `JNZ` | `:170` | modifica PC y retorna sin incrementarlo |
| `MOV_IN` | `:179` | MMU + `leer_memoria_fisica()` + log obligatorio `:195` |
| `MOV_OUT` | `:201` | MMU + `escribir_memoria_fisica()` + log obligatorio `:216` |
| `COPY_MEM` | `:221` | doble traducción (SI y DI), lee y escribe, logs `:243` y `:247` |

**Syscalls (liberan la CPU):**

| Instrucción | Línea | Handler de envío |
|---|---|---|
| `SLEEP` | `:260` | `enviar_syscall_sleep()` |
| `EXIT` | `:266` | `enviar_syscall_exit()` |
| `STDIN` | `:271` | valida con MMU, luego `enviar_syscall_stdin()` |
| `STDOUT` | `:286` | valida con MMU, luego `enviar_syscall_stdout()` |
| `MUTEX_CREATE` | `:298` | `enviar_syscall_mutex_create()` |
| `MUTEX_LOCK` | `:302` | `enviar_syscall_mutex_lock()` |
| `MUTEX_UNLOCK` | `:306` | `enviar_syscall_mutex_unlock()` |
| `MEM_ALLOC` | `:310` | `enviar_syscall_mem_alloc()` |
| `MEM_FREE` | `:316` | `enviar_syscall_mem_free()` |
| `INIT_PROC` | `:321` | `enviar_syscall_init_proc()` |

---

## 5. MEMORY STICK

| Funcionalidad | Función |
|---|---|
| Reserva del bloque con `calloc` | `memoria_stick_create()` — `memory_stick/src/memoria/memoria.c:5` |
| **Lectura de datos** | `memoria_stick_leer()` — `memoria.c:44` |
| **Escritura de datos** | `memoria_stick_escribir()` — `memoria.c:53` |
| Validación de rango (anti-overflow de `uint32`) | `rango_valido()` — `memoria.c:36` |
| Servidor de peticiones (CPU y KM comparten protocolo) | `atender_peticiones()` — `memory_stick/src/conexiones/conexiones.c:53` |
| `MEMORY_DELAY` | `conexiones.c:65` (lectura) y `:94` (escritura) |
| Informar tamaño + ip:puerto de escucha a KM | `conectar_a_kernel_memory()` — `conexiones.c:155` |

---

## 6. IO

Un solo hilo, tal como pide el enunciado v1.1. Todo el cuerpo está en `io/src/main.c`.

| Tipo | Bloque | Comportamiento clave |
|---|---|---|
| Selección del tipo por argumento | `io/src/main.c:38-50` | |
| **STDIN** | `:80-115` | lee hasta Enter `:97-102`, trunca al tamaño `:98`, rellena con `\0` `:104-107` |
| **STDOUT** | `:117-157` | imprime por pantalla `:142` y en log `:146`, responde `MENSAJE_OK` `:153-154` |
| **SLEEP** | `:159-178` | `usleep(ms * 1000)` `:169`, responde `MENSAJE_OK` `:174-175` |
| Conexión + handshake con nombre | `io/src/conexiones/conexiones.c` (`conectar_a_kernel_scheduler`) | |

---

## 7. SWAP

Sin estructuras administrativas propias (las administra KM), tal como pide el enunciado.

| Funcionalidad | Función |
|---|---|
| Crear/abrir el archivo (`SWAP_FILE_PATH`) | `swap/src/main.c:50` |
| Informar tamaño total + `BLOCK_SIZE` a KM | `conectar_y_atender_kernel_memory()` — `swap/src/conexiones/conexiones.c:23-28` |
| **Escritura de bloque** | `swap/src/conexiones/conexiones.c:39-52` (`fseek` + `fwrite` + `fflush`) |
| **Lectura de bloque** | `swap/src/conexiones/conexiones.c:53-66` (`fseek` + `fread`, relleno con ceros `:61`) |

---

## 8. Logs mínimos y obligatorios — dónde se emite cada uno

### Kernel Scheduler

| Log del enunciado | Sitio |
|---|---|
| `## Conectado a Kernel Memory` | `kernel_scheduler/src/conexiones/conexiones.c:449` |
| `## CPU <ID> Conectada` | `planificador.c:828` |
| `## (<PID>) Se crea el proceso - Estado: NEW` | `planificador.c:439` |
| `## (<PID>) - Solicitó syscall: <X>` | `conexiones.c:203` |
| `## (<PID>) Pasa del estado <A> al estado <B>` | `planificador.c:423, 448, 478, 491, 551, 590, 633, 669, 774` |
| `## (<PID>) finalizó IO y pasa a READY / SUSP. READY` | `planificador.c:1272` |
| `## (<PID>) Toma el Mutex <M>` | `conexiones.c:281` y `planificador.c:1468` |
| `## (<PID>) Libera el Mutex <M>` | `conexiones.c:306` |
| `## <PID> Cambio de prioridad: <A> - <B>` | `planificador.c:1370` ⚠ ver §9 |
| `## (<PID>) - Desalojado por fin de quantum` | `conexiones.c:432` |
| `## (<PID>) Prioridad: <P> - Desalojado por cola más prioritaria...` | `planificador.c:63-65` ⚠ ver §9 |
| `## Inicio de compactación` / `## Fin de compactación` | ⚠ ver §9 (se emiten en KM: `gestor_memoria.c:567` y `:595`) |
| `## (<PID>) finalizó su ejecución con motivo de <MOTIVO>` | `planificador.c:479` (BSOD: `:1075`) |

### Kernel Memory

| Log | Sitio |
|---|---|
| `## CPU <ID> Conectada` | `kernel_memory_atencion_cpu.c:19` |
| `## Memory Stick de <T> bytes Conectada` | `gestor_memoria.c:118` |
| `## Kernel Scheduler Conectado - FD del socket: <FD>` | `kernel_memory_atencion_scheduler.c:15` |
| `## PID: <PID> - Proceso Creado` | `kernel_memory_atencion_scheduler.c:41` |
| `## PID: <PID> - Obtener instrucción: <PC> - Instrucción: <I>` | `kernel_memory_gestor_procesos.c:113` |
| `## PID: <PID> - Segmento Creado <ID> - Tamaño: <T>` | `gestor_memoria.c:306` |
| `## PID: <PID> - Lectura - Dir. Física: <D> - Tamaño: <T>` | `kernel_memory_atencion_scheduler.c:128` |
| `## PID: <PID> - Escritura - Dir. Física: <D> - Tamaño: <T>` | `kernel_memory_atencion_scheduler.c:154` |

### CPU

| Log | Sitio |
|---|---|
| `## PID: <PID> - FETCH - Program Counter: <PC>` | `cpu/src/instrucciones/instrucciones.c:61` |
| `## Interrupción recibida` | `instrucciones.c:342` y `cpu/src/cpu/cpu.c:139` |
| `## PID: <PID> - Ejecutando: <I> - <PARAMS>` | `instrucciones.c:141` |
| `PID: <PID> - Acción: <LEER/ESCRIBIR> - Dirección Física: <D> - Valor: <V>` | `instrucciones.c:195, 216, 243, 247` |

### Memory Stick / IO / SWAP

| Log | Sitio |
|---|---|
| MS `## Conectado a Kernel Memory` | `memory_stick/src/conexiones/conexiones.c:180` |
| MS `## CPU <ID> Conectada` | `memory_stick/src/conexiones/conexiones.c:138` |
| MS `## Escritura de <N> bytes` / `## Lectura de <N> bytes` | `conexiones.c:102` / `:74` |
| IO `## Conectado a Kernel Scheduler` | `io/src/conexiones/conexiones.c:27` |
| IO `## PID: <PID> - Inicio de IO` | `io/src/main.c:86, 123, 165` |
| IO `## PID: <PID> - Fin de IO` | `io/src/main.c:112, 150, 171` |
| IO STDOUT `## PID: <PID> - <CONTENIDO>` | `io/src/main.c:146` |
| IO STDIN `## PID: <PID> - Ingrese <N> caracteres:` | `io/src/main.c:87` |
| IO SLEEP `## PID: <PID> - Haciendo sleep por <T> milisegundos.` | `io/src/main.c:166` |
| SWAP `## Conectado a Kernel Memory` | `swap/src/conexiones/conexiones.c:20` |
| SWAP `## Escritura del bloque: <N>` / `## Lectura del bloque: <N>` | `conexiones.c:48` / `:63` |

---

## 9. Desvíos detectados contra el enunciado

Encontrados al cruzar el PDF con el código. Todos son de formato de log obligatorio,
que es criterio de evaluabilidad del TP (enunciado p.7: "En caso de no cumplir con los
logs mínimos... el TP no es apto para ser evaluado").

1. ~~**Cambio de prioridad — falta el guion separador.**~~ **FALSO — verificado contra el PDF.**
   El enunciado NO lleva guion: `## <PID> Cambio de prioridad: <PRIORIDAD_ANTERIOR> <PRIORIDAD_NUEVA>`.
   El código (`planificador.c:1370`) coincide exactamente. Ejercitado y confirmado por
   `pruebas_unitarias/syscalls_mutex_herencia/`.

2. ~~**Desalojo por cola más prioritaria — falta el guion separador.**~~ **FALSO — verificado contra el PDF.**
   El enunciado NO lleva guion: `## (<PID>) Prioridad: <PRIORIDAD_DESALOJADO> Desalojado por
   cola más prioritaria por el proceso <PID> con prioridad <PRIORIDAD_NUEVA>`.
   El código (`planificador.c:64`) coincide exactamente. Ejercitado y confirmado por
   `pruebas_unitarias/pcp_cmn_con_desalojo/`.

   > Los puntos 1 y 2 estaban mal en una versión anterior de este documento: se afirmaba un
   > guion que el enunciado no pide. Corregidos tras contrastar el texto literal del PDF
   > (`pdftotext "Plug & Pray.pdf"`, sección "Logs mínimos y obligatorios" del Kernel
   > Scheduler). **Antes de "corregir" un log, leer el PDF, no este documento.**

3. **`## Inicio de compactación` / `## Fin de compactación` figuran en la lista de logs
   obligatorios del Kernel Scheduler pero se emiten sólo en el Kernel Memory**
   (`gestor_memoria.c:567` y `:595`). Éste **sí** es un desvío real: los dos strings están
   entre los "Logs mínimos y obligatorios" de la sección del Kernel Scheduler, justo antes
   de `## (<PID>) finalizó su ejecución con motivo de <MOTIVO>`. El KS loguea el desalojo
   (`planificador.c:1008`) pero no esos dos strings exactos.
   Verificado por `pruebas_unitarias/dessuspension_compactacion/`.

4. **Riesgo de desincronización del protocolo** (no es un log, es correctitud): en
   `atender_peticiones_cpu()`, para `MUTEX_*`, `MEM_ALLOC` y `MEM_FREE`, el `recv` del
   struct de registros está **dentro** del `if (pcb_ejecutando != NULL)`
   (`conexiones.c:257, 275, 302, 319, 348`). Si el PCB no se encuentra en `lista_exec`,
   esos bytes quedan sin leer en el socket y todo lo que siga se lee corrido.
   El mismo patrón en `SLEEP`/`STDIN`/`STDOUT` (`:223, 238`) tiene el mismo riesgo.
