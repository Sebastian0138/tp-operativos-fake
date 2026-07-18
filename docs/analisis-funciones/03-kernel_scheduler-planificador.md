# Módulo kernel_scheduler — planificador.c (núcleo)

Archivos analizados: `kernel_scheduler/src/planificador/planificador.c` (1474 líneas) y
`planificador.h` (215 líneas). Es el corazón del Kernel Scheduler: colas multinivel,
desalojo, herencia de prioridades, mutex de usuario, compactación, suspensión/SWAP
(mediano plazo) e IO asíncrona.

> Todas las funciones `static` son privadas del archivo; sus "callers" están listados
> con línea exacta. Las públicas (declaradas en `planificador.h`) se invocan casi
> exclusivamente desde `kernel_scheduler/src/conexiones/conexiones.c` (el despachador de
> mensajes de red) y `main.c` — se indica el syscall/evento de protocolo que dispara cada
> una.

---

### Estructuras clave

**`t_cpu_conectada`** (una por CPU conectada al KS):
- `id_cpu`, `socket_cpu`: identidad y canal de comunicación.
- `libre` / `pid_ejecutando`: si está ocupada y con qué proceso.
- `en_rafaga`: `true` desde que se despacha un PID hasta que la CPU lo devuelve. Una CPU
  puede tener un proceso "estacionado" en una syscall (no ejecutando instrucciones) sin
  estar `en_rafaga` — esto es clave para la compactación, que solo necesita esperar a las
  CPUs que están ejecutando de verdad.
- `rafaga`: contador incremental. Sirve para invalidar timers de quantum viejos cuando el
  mismo PID vuelve rápido a la misma CPU (por ejemplo, tras un `MUTEX_LOCK` exitoso que
  hace `despachar_directo`): el timer viejo compara su `rafaga` capturada contra la actual
  y, si no coincide, no interrumpe.
- `motivo_interrupcion` (`INT_NINGUNA/QUANTUM/PREEMPCION/COMPACTACION`): por qué se le
  mandó `MENSAJE_INTERRUPCION`, para saber qué transición aplicar cuando la CPU devuelva
  el proceso.
- `pid_preemptor` / `prioridad_preemptor`: metadata del proceso que causó un desalojo por
  cola más prioritaria (solo informativo/log).

**`t_io_conectada`**: `fd`, `tipo` (STDIN/STDOUT/SLEEP), `nombre`, `libre` y
`desconectado` (se marcó para remover pero seguía en uso — ver `planificador_remover_io`).

**`t_planificador`**: el objeto central.
- `cola_new` (`t_queue`): procesos recién creados, esperando al largo plazo.
- `colas_ready` (arreglo de `t_list*`) + `cant_colas` + `alg_por_cola`: con FIFO/RR hay una
  sola cola (índice 0); con CMN hay una por nivel de prioridad, cada una con su propio
  algoritmo leído de `QUEUES_ALGORITHMS`.
- `queue_preemption`: si `TRUE` y el algoritmo es CMN, un proceso más prioritario que
  llega a READY desaloja al que esté corriendo con menor prioridad.
- `lista_exec`: lista (no cola) porque puede haber múltiples CPUs ejecutando a la vez.
- `cola_block`, `cola_susp_block`, `cola_susp_ready`, `lista_exit`: resto de los estados
  del proceso.
- `cpus_conectadas`, `ios_conectados`, `lista_mutex`: recursos registrados.
- Un mutex por cada colección (`mutex_cola_new`, `mutex_cola_ready`, `mutex_lista_exec`,
  `mutex_cola_block`, `mutex_lista_exit`, `mutex_cpus`, `mutex_ios`, `mutex_lista_mutex`) —
  granularidad fina, cada estructura se protege independiente.
- Semáforos de conteo (`sem_procesos_new`, `sem_procesos_ready`, `sem_cpus_libres`,
  `sem_io_stdin/stdout/sleep_libres`): implementan el patrón productor-consumidor de los
  hilos de largo/corto plazo y de la toma de dispositivos IO.
- `compactacion_en_curso` + `mutex_compactacion` + `cond_compactacion`: mecanismo de
  pausa global de la planificación mientras KM compacta memoria.
- `mutex_cola_susp`, `lista_espera_memoria` + `mutex_espera_memoria`,
  `mutex_evento_memoria`: soporte de mediano plazo (suspensión a SWAP y reintento de
  `MEM_ALLOC` pendientes).
- `socket_memoria` + `mutex_socket_memoria`: el socket hacia KM es compartido por todos
  los hilos, así que cada operación (`send`+`recv`) se hace bajo lock para que las
  respuestas no se crucen entre hilos concurrentes.
- `proximo_pid` + `mutex_pid`: generador atómico de PIDs.

---

### Ciclo de vida / estructura

#### `t_planificador* planificador_create(t_kernel_scheduler_config* config, t_log* logger, int socket_memoria)` — planificador.c:113
- **Qué hace:** reserva y arma toda la estructura `t_planificador`: decide cuántas colas
  READY crear (1 para FIFO/RR, N para CMN según `QUEUES_ALGORITHMS`), inicializa todas las
  colas/listas vacías, todos los mutex y semáforos en 0, y arranca `proximo_pid` en 1 (el
  PID 0 lo crea `main.c` a mano para el proceso inicial).
- **Dónde se usa:** `kernel_scheduler/src/main.c:58`, al arrancar el módulo.
- **Para qué:** punto de entrada único que deja el planificador listo para que los hilos
  de largo/corto plazo y el servidor de conexiones operen sobre él.
- **Cómo funciona:** si `planification_algorithm == "CMN"`, cuenta las entradas de
  `config->queues_algorithms` (array `NULL`-terminado) para fijar `cant_colas`, y si viene
  vacío loguea error y aborta la creación (`free(pl); return NULL`). Si no es CMN, fuerza
  `cant_colas = 1` y usa el algoritmo único de la config como "algoritmo de la cola 0".
  No toma ningún lock (nadie más tiene el puntero todavía).

#### `void planificador_destroy(t_planificador* pl)` — planificador.c:197
- **Qué hace:** libera cada cola/lista destruyendo sus elementos con el destructor
  correspondiente (`pcb_destroy`, `io_conectada_destroy`, `mutex_kernel_destroy`, o `free`
  simple para CPUs), destruye todos los mutex/semáforos y libera la estructura.
- **Dónde se usa:** `kernel_scheduler/src/main.c:85` (shutdown ordenado, si se llega a
  ejecutar — el flujo normal del KS corre en un loop infinito).
- **Para qué:** liberar memoria de forma prolija al apagar el módulo.
- **Cómo funciona:** secuencia lineal de `queue_destroy_and_destroy_elements` /
  `list_destroy_and_destroy_elements` por cada colección, sin locks (asume que ya no hay
  hilos activos usando `pl`).

#### `static void io_conectada_destroy(void* elemento)` — planificador.c:190
- **Qué hace:** cierra el `fd` de una IO conectada y libera su `nombre` y el struct.
- **Dónde se usa:** `planificador_destroy` (destructor de `ios_conectados`),
  `planificador_remover_io` (líneas 1160/1198) y `planificador_liberar_io` (línea 1198)
  cuando una IO se desconecta.
- **Para qué:** liberar recursos de un dispositivo IO que ya no se usa.
- **Cómo funciona:** simple, sin locks — el caller ya sacó el elemento de la lista antes
  de invocarla.

---

### Hilos planificadores

#### `void* hilo_planificador_largo_plazo(void* arg)` — planificador.c:249
- **Qué hace:** loop infinito que espera procesos en NEW (`sem_wait(&sem_procesos_new)`),
  los saca de `cola_new`, le informa a Kernel Memory el nuevo proceso (le manda el PID y
  el path del archivo de instrucciones para que cree su contexto) y los pasa a READY.
- **Dónde se usa:** lanzado una vez como hilo por `main.c:72`
  (`pthread_create(&thread_largo, NULL, hilo_planificador_largo_plazo, planificador)`).
- **Para qué:** implementa la transición NEW→READY (admisión de procesos), que en este
  diseño no tiene ninguna restricción adicional de memoria (los procesos arrancan sin
  segmentos asignados).
- **Cómo funciona:** `sem_wait` bloquea el hilo hasta que `planificador_encolar_new` haga
  `sem_post`. Extrae el PCB bajo `mutex_cola_new`. Si hay conexión a KM
  (`socket_memoria != -1`), toma `mutex_socket_memoria`, manda
  `MENSAJE_ENVIAR_PID` + PID + longitud del path + el path, y espera la respuesta antes de
  soltar el lock — esto serializa contra cualquier otro hilo que use el mismo socket hacia
  KM (por ejemplo el corto plazo actualizando contexto). Termina llamando a
  `planificador_transicionar_new_a_ready`.

#### `void* hilo_timer_quantum(void* arg)` — planificador.c:298
- **Qué hace:** hilo de vida corta que duerme `RR_QUANTUM` milisegundos y, si al despertar
  la CPU objetivo sigue ejecutando exactamente el mismo PID en la misma "ráfaga", le manda
  `MENSAJE_INTERRUPCION` marcando `motivo_interrupcion = INT_QUANTUM`.
- **Dónde se usa:** creado (`pthread_create` + `pthread_detach`) dentro de `despachar()`
  cada vez que se despacha un proceso a una cola que usa Round Robin
  (`cola_usa_rr(pl, cola_de(pl, pcb))`).
- **Para qué:** implementa el corte de quantum de Round Robin / colas RR dentro de CMN.
- **Cómo funciona:** recibe por argumento `pid`, `id_cpu` y `rafaga` capturados en el
  momento del despacho. Tras el `usleep`, recorre `cpus_conectadas` bajo `mutex_cpus` y
  solo interrumpe si **todas** estas condiciones siguen valiendo: mismo `id_cpu`, la CPU
  no está libre, sigue `en_rafaga`, mismo `pid_ejecutando`, **mismo `rafaga`** (si el
  proceso volvió y salió de nuevo de la CPU — p. ej. por un `MUTEX_LOCK` fallido seguido de
  otro despacho — el contador cambió y este timer viejo queda invalidado) y
  `motivo_interrupcion == INT_NINGUNA` (no pisa una interrupción ya en curso, p. ej. por
  compactación). El `send` se hace con `mutex_cpus` tomado para no intercalarse con otro
  hilo escribiendo al mismo socket.

#### `static void* hilo_timer_suspension(void* arg)` — planificador.c:513
- **Qué hace:** hilo de vida corta que duerme `SUSPENSION_TIMEOUT` ms y, si el proceso
  sigue bloqueado con el mismo `bloqueo_id`, lo pasa de BLOCK a SUSP_BLOCK y le pide a KM
  que mueva sus segmentos a SWAP.
- **Dónde se usa:** creado en `planificador_transicionar_exec_a_block` (línea 603), pero
  **solo si el motivo de bloqueo no es `"MEMORIA"`** (un proceso esperando un `MEM_ALLOC`
  no se suspende: su propio mecanismo de reintento en `planificador_evento_memoria_liberada`
  lo resuelve, y suspenderlo generaría un estado inconsistente con el pedido pendiente).
- **Para qué:** implementa la planificación de mediano plazo (SUSPENSIÓN por timeout).
- **Cómo funciona:** tras el sleep, recorre `cola_block` completa (popeando todo y
  re-encolando lo que no matchea) buscando `pid` **y** `bloqueo_id` iguales a los
  capturados al crear el timer — el `bloqueo_id` se incrementa en cada entrada a BLOCK, así
  que si el proceso se desbloqueó y volvió a bloquearse en el medio, este timer viejo no
  encuentra nada y no hace nada (`return NULL` temprano). Si lo encuentra: cambia el estado
  a `ESTADO_SUSP_BLOCK`, lo mueve a `cola_susp_block` (ambas colas bajo lock en la misma
  sección crítica, para que un fin de IO concurrente no lo pierda entre medio), loguea la
  transición, y si hay conexión a memoria manda `MENSAJE_SUSPENDER_PROCESO` + PID a KM
  (espera respuesta bloqueante). Termina llamando a `planificador_evento_memoria_liberada`
  porque la suspensión acaba de liberar memoria principal, lo que puede permitir
  des-suspender a otro proceso o satisfacer un `MEM_ALLOC` pendiente.

---

### Selección de cola / desalojo (helpers `static`)

#### `static bool es_cmn(t_planificador* pl)` — planificador.c:12
- **Qué hace:** compara `pl->config->planification_algorithm` contra `"CMN"`.
- **Dónde se usa:** `cola_de` (línea 19), `chequear_desalojo_por_prioridad` (línea 33),
  `cambiar_prioridad` (línea 1353).
- **Para qué:** condición repetida para saber si el sistema corre con colas multinivel
  (con prioridades reales) o con un único algoritmo FIFO/RR.
- **Cómo funciona:** comparación de strings directa, sin cachear el resultado.

#### `static int cola_de(t_planificador* pl, t_pcb* pcb)` — planificador.c:18
- **Qué hace:** devuelve el índice de `colas_ready` que le corresponde a un PCB.
- **Dónde se usa:** `encolar_en_ready` (línea 78), `despachar` (línea 364, para decidir si
  aplica RR), `cambiar_prioridad` (línea 1361, indirectamente vía el mismo clamp).
- **Para qué:** traduce prioridad de proceso → índice de cola física.
- **Cómo funciona:** si no es CMN, siempre `0`. Si es CMN, usa `pcb->prioridad_actual`
  clampeado al rango `[0, cant_colas - 1]` (protección ante prioridades fuera de rango).

#### `static bool cola_usa_rr(t_planificador* pl, int cola)` — planificador.c:26
- **Qué hace:** compara `alg_por_cola[cola]` contra `"RR"`.
- **Dónde se usa:** `despachar` (línea 364), para decidir si arma el timer de quantum.
- **Para qué:** cada cola CMN tiene su propio algoritmo interno (FIFO o RR); esto
  desacopla "es CMN" de "esta cola en particular usa RR".
- **Cómo funciona:** comparación de string directa.

#### `static void chequear_desalojo_por_prioridad(t_planificador* pl, t_pcb* nuevo)` — planificador.c:32
- **Qué hace:** implementa el desalojo entre colas (`QUEUE_PREEMPTION=TRUE`): si el
  proceso que acaba de entrar a READY es más prioritario que algún proceso en EXEC, envía
  `MENSAJE_INTERRUPCION` a la CPU de la víctima.
- **Dónde se usa:** `encolar_en_ready` (línea 92), pero **no** cuando se encola "al
  frente" (desalojo por compactación) — evita cascadas de desalojo durante ese proceso.
- **Para qué:** política de desalojo apropiativo por prioridad de CMN, log obligatorio
  `"Desalojado por cola más prioritaria"`.
- **Cómo funciona:** si no es CMN o `queue_preemption` es falso, retorna enseguida. Si no,
  primero busca bajo `mutex_lista_exec` la "víctima": el proceso en EXEC con **peor**
  prioridad (número más alto) que sea estrictamente peor que la del que llega — recorre
  toda `lista_exec` guardando el peor visto (no corta apenas encuentra uno, sigue buscando
  el peor de todos). Si no hay víctima, termina. Si hay, recorre `cpus_conectadas` bajo
  `mutex_cpus` buscando la CPU que tiene exactamente ese `pid_ejecutando`, ocupada,
  `en_rafaga` (si no está en ráfaga está "estacionada" en una syscall, no tiene sentido
  desalojarla ahí) y sin una interrupción ya pendiente (`motivo_interrupcion ==
  INT_NINGUNA`, para no pisar una compactación en curso). Marca
  `motivo_interrupcion = INT_PREEMPCION`, guarda PID/prioridad del preemptor para el log
  que se emite en el otro extremo (`conexiones.c:421-434`), loguea de inmediato el mensaje
  obligatorio y manda `MENSAJE_INTERRUPCION` por el socket de esa CPU.

#### `static void encolar_en_ready(t_planificador* pl, t_pcb* pcb, bool al_frente)` — planificador.c:77
- **Qué hace:** inserta el PCB en la cola READY que le corresponde (`cola_de`), al final o
  al principio según `al_frente`, hace `sem_post` de `sem_procesos_ready` y dispara el
  chequeo de desalojo (salvo que sea `al_frente`).
- **Dónde se usa:** `planificador_transicionar_new_a_ready`, `exec_a_ready` (usada por
  `planificador_transicionar_exec_a_ready` y `..._exec_a_ready_frente`),
  `planificador_evento_memoria_liberada` (al des-suspender un proceso), `hilo_planificador_corto_plazo` (línea 409, reencolado defensivo si no había CPU libre pese al semáforo).
- **Para qué:** único punto de entrada a READY, mantiene el invariante de "insertar + avisar
  al corto plazo + evaluar desalojo" siempre junto.
- **Cómo funciona:** todo bajo `mutex_cola_ready`. `al_frente=true` usa
  `list_add_in_index(..., 0, pcb)` (usado únicamente por el desalojo por compactación, para
  que el proceso reordenado no pierda su lugar en la fila).

#### `static t_pcb* desencolar_ready(t_planificador* pl)` — planificador.c:97
- **Qué hace:** recorre las colas READY en orden de índice (0 = más prioritaria) y saca el
  primer PCB disponible de la primera cola no vacía.
- **Dónde se usa:** `hilo_planificador_corto_plazo` (línea 390).
- **Para qué:** selección del próximo proceso a ejecutar, respetando la prioridad de cola
  en CMN (siempre se vacía la cola 0 antes de tocar la cola 1, etc.) y el orden interno
  FIFO/RR de cada cola individual (`list_remove(..., 0)`, siempre el primero insertado).
- **Cómo funciona:** todo bajo `mutex_cola_ready`, un solo lock/unlock para toda la
  búsqueda.

---

### Despacho y quantum

#### `static void despachar(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu)` — planificador.c:338
- **Qué hace:** envía efectivamente un PID a una CPU libre y, si corresponde, arranca el
  timer de quantum.
- **Dónde se usa:** `hilo_planificador_corto_plazo` (línea 425) y, indirectamente, todo
  caller de `planificador_despachar_directo` (ver abajo).
- **Para qué:** único punto real donde se manda `MENSAJE_ENVIAR_PID` por el socket de una
  CPU — centraliza el orden de locks para evitar tanto despachar durante una compactación
  como intercalar escrituras en el socket.
- **Cómo funciona (orden de locks, documentado en el propio código):** (1) actualiza el
  contexto del proceso en KM llamando a `planificador_actualizar_contexto_en_km` **sin
  ningún lock propio tomado todavía** (evita deadlock, porque esa función toma
  `mutex_socket_memoria` internamente); (2) toma `mutex_compactacion` y hace
  `pthread_cond_wait` en un loop mientras `compactacion_en_curso` sea verdadero — esto es
  lo que impide que se despache cualquier proceso mientras KM está compactando; (3) con la
  compactación garantizada inactiva, toma `mutex_cpus`, marca la CPU ocupada
  (`libre=false`), setea `pid_ejecutando`, `en_rafaga=true`, incrementa `rafaga`, resetea
  `motivo_interrupcion`, y manda `MENSAJE_ENVIAR_PID` + el PID por el socket; libera
  `mutex_cpus` y luego `mutex_compactacion`. Finalmente, si la cola del proceso usa RR,
  lanza `hilo_timer_quantum` con los datos capturados (incluyendo el `rafaga` justo
  incrementado, para que el timer pueda auto-invalidarse después).

#### `void planificador_despachar_directo(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu)` — planificador.c:377
- **Qué hace:** wrapper público de `despachar()`, sin lógica adicional.
- **Dónde se usa:** `conexiones.c` en 6 puntos, todos en `atender_peticiones_cpu`, para las
  syscalls que **devuelven el proceso a la misma CPU** sin pasar por READY:
  `SYSCALL_MUTEX_CREATE` (línea 263), `SYSCALL_MUTEX_LOCK` cuando lo adquiere (línea 284),
  `SYSCALL_MUTEX_UNLOCK` (línea 309), `SYSCALL_MEM_ALLOC` cuando hay éxito (línea 328),
  `SYSCALL_MEM_FREE` en ambas ramas éxito/fallo (líneas 353 y 358).
- **Para qué:** la consigna del TP exige que las syscalls de mutex y memoria devuelvan el
  proceso a la **misma** CPU que lo pidió (no vuelve a pasar por la cola READY general).
- **Cómo funciona:** delega 1:1 en `despachar`.

#### `void* hilo_planificador_corto_plazo(void* arg)` — planificador.c:382
- **Qué hace:** loop infinito: espera que haya un proceso listo **y** una CPU libre
  (`sem_wait` sobre ambos semáforos), saca el proceso más prioritario de READY, transiciona
  a EXEC y lo despacha.
- **Dónde se usa:** lanzado como hilo único por `main.c:73`.
- **Para qué:** implementa la transición READY→EXEC (corto plazo).
- **Cómo funciona:** `sem_wait(&sem_procesos_ready)` y `sem_wait(&sem_cpus_libres)` (en ese
  orden fijo — posible punto de análisis de deadlock, pero como son los únicos dos
  semáforos que este hilo espera en cascada y nadie más los espera en orden inverso, no hay
  inversión). Llama `desencolar_ready`; si por alguna razón da `NULL` (no debería, dado el
  semáforo, pero el código lo cubre por seguridad) hace `sem_post` de vuelta sobre
  `sem_cpus_libres` y continúa. Busca la primera CPU con `libre=true` recorriendo
  `cpus_conectadas`; si tampoco la encuentra (mismo caso defensivo) reencola el PCB **al
  frente** con `encolar_en_ready(pl, pcb, true)` y repone el semáforo. Si todo está bien:
  cambia `pcb->estado = ESTADO_EXEC`, timestampea `ts_inicio_exec`, agrega el PCB a
  `lista_exec` bajo `mutex_lista_exec`, loguea la transición y llama a `despachar`.

---

### Transiciones de estado (API pública)

#### `void planificador_encolar_new(t_planificador* pl, t_pcb* pcb)` — planificador.c:434
- **Qué hace:** empuja el PCB a `cola_new`, loguea `"Se crea el proceso - Estado: NEW"` y
  hace `sem_post(sem_procesos_new)`.
- **Dónde se usa:** `main.c:79` (proceso inicial, PID 0) y
  `conexiones.c:395` (`SYSCALL_INIT_PROC`, creación de un proceso nuevo desde otro proceso
  en ejecución).
- **Para qué:** punto de entrada de todo proceso al sistema.
- **Cómo funciona:** trivial, bajo `mutex_cola_new`.

#### `void planificador_transicionar_new_a_ready(t_planificador* pl, t_pcb* pcb)` — planificador.c:443
- **Qué hace:** cambia estado a READY, timestampea `ts_llegada_ready`, loguea la
  transición y llama `encolar_en_ready(pl, pcb, false)`.
- **Dónde se usa:** solo `hilo_planificador_largo_plazo` (después de registrar el proceso
  en KM).
- **Para qué:** cierre de la admisión de largo plazo.
- **Cómo funciona:** directo, sin locks propios más allá de los que toma `encolar_en_ready`.

#### `static t_pcb* remover_de_exec(t_planificador* pl, uint32_t pid)` — planificador.c:453
- **Qué hace:** busca por PID en `lista_exec` y lo remueve si lo encuentra.
- **Dónde se usa:** `planificador_transicionar_exec_a_exit`, `exec_a_ready`,
  `planificador_transicionar_exec_a_block`, `planificador_remover_cpu` (para recuperar el
  proceso de una CPU que se desconectó a mitad de ejecución).
- **Para qué:** helper común a toda transición que sale de EXEC.
- **Cómo funciona:** recorrido lineal bajo `mutex_lista_exec`.

#### `void planificador_transicionar_exec_a_exit(t_planificador* pl, t_pcb* pcb, const char* motivo)` — planificador.c:467
- **Qué hace:** saca el PCB de `lista_exec`, lo pasa a EXIT, lo agrega a `lista_exit` y
  loguea tanto la transición de estado como el motivo de finalización.
- **Dónde se usa:** exclusivamente `finalizar_proceso` en `conexiones.c:140` (que a su vez
  se llama para `SYSCALL_EXIT` con motivo `"SUCCESS"` y `SYSCALL_SEG_FAULT` con motivo
  `"SEG_FAULT"`).
- **Para qué:** cierre definitivo de un proceso.
- **Cómo funciona:** usa `remover_de_exec`; si no lo encuentra (no debería pasar en uso
  normal) usa el `pcb` recibido como fallback para no perder el log.

#### `static void exec_a_ready(t_planificador* pl, t_pcb* pcb, bool al_frente)` — planificador.c:482
- **Qué hace:** helper común: saca de `lista_exec`, pasa a READY, resetea `cpu_asignada =
  -1`, timestampea, loguea y reencola (al frente o al final según el parámetro).
- **Dónde se usa:** internamente por `planificador_transicionar_exec_a_ready` (línea 496,
  `al_frente=false`) y `planificador_transicionar_exec_a_ready_frente` (línea 500,
  `al_frente=true`).
- **Para qué:** evitar duplicar la lógica entre el caso normal (fin de quantum / desalojo
  por prioridad) y el caso especial (desalojo por compactación, que debe ir al frente).
- **Cómo funciona:** ver arriba, delega en `remover_de_exec` + `encolar_en_ready`.

#### `void planificador_transicionar_exec_a_ready(t_planificador* pl, t_pcb* pcb)` — planificador.c:495
- **Dónde se usa:** `conexiones.c` tres veces: `SYSCALL_INIT_PROC` (el que crea un proceso
  hijo vuelve a READY, línea 405), y en `MENSAJE_INTERRUPCION` para los motivos
  `INT_PREEMPCION` y `INT_QUANTUM` (líneas 428 y 433). También indirectamente vía
  `planificador_remover_cpu` cuando una CPU se cae con un proceso corriendo.
- **Qué hace / cómo funciona:** `exec_a_ready(pl, pcb, false)` — va al final de su cola.

#### `void planificador_transicionar_exec_a_ready_frente(t_planificador* pl, t_pcb* pcb)` — planificador.c:499
- **Dónde se usa:** `conexiones.c:422`, único caso: `MENSAJE_INTERRUPCION` con
  `motivo_int == INT_COMPACTACION`.
- **Qué hace / cómo funciona:** `exec_a_ready(pl, pcb, true)` — va al **frente** de su
  cola, porque fue desalojado sin haber terminado su quantum/ráfaga por una razón ajena a
  él (compactación), no debería perder su lugar relativo en la fila.

#### `void planificador_transicionar_exec_a_block(t_planificador* pl, t_pcb* pcb, const char* motivo_bloqueo)` — planificador.c:574
- **Qué hace:** saca de EXEC, pasa a BLOCK, guarda el motivo textual, incrementa
  `bloqueo_id`, timestampea `ts_inicio_block`, encola en `cola_block`, loguea, y — salvo
  que el motivo sea `"MEMORIA"` — arranca `hilo_timer_suspension`.
- **Dónde se usa:** `conexiones.c` para `SYSCALL_SLEEP` (motivo `"SLEEP"`, línea 227),
  `SYSCALL_STDIN`/`SYSCALL_STDOUT` (motivo dinámico, línea 243), `SYSCALL_MUTEX_LOCK`
  fallido (motivo `"MUTEX"`, línea 288), `SYSCALL_MEM_ALLOC` sin espacio (motivo
  `"MEMORIA"`, línea 337).
- **Para qué:** único punto de entrada a BLOCK, con el efecto colateral correcto de
  arrancar (o no) el timer de mediano plazo.
- **Cómo funciona:** el `bloqueo_id++` es el mecanismo central para que timers viejos no
  interfieran con bloqueos nuevos del mismo proceso (ver `hilo_timer_suspension`). Libera
  el `motivo_bloqueo` anterior si había uno colgado (no debería, pero por seguridad).

#### `t_pcb* planificador_transicionar_block_a_ready(t_planificador* pl, t_pcb* pcb)` — planificador.c:608
- **Qué hace:** busca por PID en `cola_block` (recorrido pop/push completo, "filtrado" de
  la cola), y si lo encuentra lo pasa a READY, incrementa `bloqueo_id` (invalida cualquier
  timer de suspensión pendiente para ese bloqueo) y limpia `motivo_bloqueo`.
- **Dónde se usa:** internamente por `planificador_desbloquear` (primer intento, caso
  "estaba en BLOCK normal").
- **Para qué:** rama "no suspendido" del desbloqueo genérico.
- **Cómo funciona:** el patrón "pop todo, comparar, re-push lo que no matchea" es cómo se
  implementa "buscar y remover por PID" sobre una `t_queue` de commons (que no tiene
  remove-by-predicate nativo). Devuelve el PCB real encontrado (no el `pcb` de búsqueda
  pasado por parámetro) o `NULL`.

#### `t_pcb* planificador_desbloquear(t_planificador* pl, uint32_t pid, bool* estaba_suspendido)` — planificador.c:640
- **Qué hace:** desbloquea un proceso por PID sin que el caller necesite saber si estaba
  en BLOCK normal o suspendido (SUSP_BLOCK). Si estaba en BLOCK, va a READY. Si estaba en
  SUSP_BLOCK, va a SUSP_READY (su memoria sigue en SWAP, no puede ejecutar todavía).
- **Dónde se usa:** `hilo_ejecutar_io` (fin de IO, línea 1271),
  `planificador_evento_memoria_liberada` (al satisfacer un `MEM_ALLOC` pendiente, línea
  799), `planificador_unlock_mutex` (al despertar al siguiente en la cola del mutex, línea
  1465).
- **Para qué:** punto único de "algo que estaba esperando este proceso terminó, hacelo
  avanzar", desacoplado de si en el medio se suspendió por timeout.
- **Cómo funciona:** intenta primero `planificador_transicionar_block_a_ready` (rama
  rápida, caso común). Si no encontró nada ahí, busca en `cola_susp_block` con el mismo
  patrón pop/push; si lo encuentra, pasa a `ESTADO_SUSP_READY`, limpia `motivo_bloqueo`, lo
  mueve a `cola_susp_ready` y setea `*estaba_suspendido = true`. Nótese que **no** hace
  nada con la memoria en este segundo caso: el proceso queda en SUSP_READY hasta que
  `planificador_evento_memoria_liberada` decida des-suspenderlo de verdad (pedirle a KM que
  restaure sus segmentos).

---

### Espera de memoria (`MEM_ALLOC` sin espacio) y des-suspensiones

#### `void planificador_bloquear_por_memoria(t_planificador* pl, t_pcb* pcb, uint32_t id_segmento, uint32_t tamanio)` — planificador.c:688
- **Qué hace:** registra en `lista_espera_memoria` un `t_espera_memoria` (PCB + id de
  segmento + tamaño pedido) y loguea.
- **Dónde se usa:** `conexiones.c:338`, inmediatamente después de
  `planificador_transicionar_exec_a_block(pcb, "MEMORIA")` cuando `planificador_crear_segmento_km`
  devuelve `false` (sin espacio, sin compactar).
- **Para qué:** dejar constancia de qué segmento le falta a un proceso bloqueado, para que
  `planificador_evento_memoria_liberada` pueda reintentar el `MEM_ALLOC` exacto más tarde.
- **Cómo funciona:** simple `malloc` + `list_add` bajo `mutex_espera_memoria`.

#### `static int comparar_candidatos(const void* a, const void* b)` — planificador.c:709
- **Qué hace:** comparador para `qsort` de candidatos a des-suspensión: ordena primero por
  prioridad (menor número = más prioritario, primero), y a igual prioridad, por quien lleva
  **más tiempo** suspendido (`ts_suspension` más antiguo primero).
- **Dónde se usa:** `planificador_evento_memoria_liberada` (línea 751).
- **Para qué:** política de des-suspensión: prioridad primero, antigüedad como
  desempate — evita starvation entre procesos de igual prioridad.
- **Cómo funciona:** comparación numérica directa de `prioridad`; si empatan, compara
  `tv_sec` y luego `tv_nsec` del timestamp.

#### `static bool dessuspender_en_km(t_planificador* pl, uint32_t pid)` — planificador.c:719
- **Qué hace:** le pide a Kernel Memory que restaure (desde SWAP) los segmentos de un
  proceso suspendido, mandando `MENSAJE_DESSUSPENDER_PROCESO` + PID, y devuelve `true` solo
  si KM respondió `MENSAJE_OK`.
- **Dónde se usa:** `planificador_evento_memoria_liberada` (línea 754).
- **Para qué:** KM es quien decide si hay espacio real para volver a crear todos los
  segmentos del proceso (sin compactar — ver comentario de la sección 1 de
  `evento_memoria_liberada`); si no entra, devuelve algo distinto de `MENSAJE_OK` y el KS
  simplemente no lo mueve de cola, reintentará en el próximo evento.
- **Cómo funciona:** send/recv síncrono bajo `mutex_socket_memoria`.

#### `void planificador_evento_memoria_liberada(t_planificador* pl)` — planificador.c:733
- **Qué hace:** la función más importante de la sección de mediano plazo. Cada vez que se
  libera memoria por cualquier motivo (MEM_FREE, fin de proceso, suspensión de otro
  proceso, compactación, nuevo stick), esta función (1) intenta des-suspender procesos en
  SUSP_READY por orden de prioridad/antigüedad, y (2) reintenta los `MEM_ALLOC` que habían
  quedado bloqueados esperando espacio.
- **Dónde se usa:** `conexiones.c` en 4 puntos — fin de proceso (`finalizar_proceso`, línea
  154), `SYSCALL_MEM_ALLOC` exitoso con compactación (línea 332), `SYSCALL_MEM_FREE`
  exitoso (línea 355), notificación `MENSAJE_MAS_MEMORIA` de KM (línea 476). Además
  internamente por `hilo_timer_suspension` (tras suspender a alguien) y por
  `hilo_ejecutar_io` / `planificador_unlock_mutex` cuando el proceso que se despierta
  **estaba** suspendido (para intentar des-suspenderlo ya mismo).
- **Para qué:** reacciona a cualquier evento que pudo haber liberado o generado más
  memoria disponible, resolviendo el backlog de procesos que dependían de eso.
- **Cómo funciona (paso a paso):** toma `mutex_evento_memoria` — esto **serializa toda la
  función**: si dos hilos disparan el evento a la vez, el segundo espera a que el primero
  termine su recorrido completo y vuelve a evaluar sobre el estado ya actualizado (evita
  que dos hilos compitan por los mismos candidatos con información desactualizada).
  **Fase 1 — des-suspensión:** copia bajo `mutex_cola_susp` los datos (pid, prioridad,
  timestamp) de todos los PCBs en `cola_susp_ready` a un array `t_candidato_dessusp[]`
  (accede directamente a `cola_susp_ready->elements`, el array interno de la `t_queue` de
  commons, para poder iterarlo con índice sin desarmar la cola). Ordena ese array con
  `qsort` + `comparar_candidatos`. Para cada candidato en orden: llama a
  `dessuspender_en_km`; si KM confirma, busca y remueve ese PID de `cola_susp_ready` (mismo
  patrón pop/push), lo pasa a `ESTADO_READY`, timestampea y lo encola en READY normal vía
  `encolar_en_ready`. Si KM no confirma (no entra sin compactar), simplemente no se toca y
  se sigue con el próximo candidato — no hay reintento inmediato, queda para el próximo
  evento. **Fase 2 — reintento de `MEM_ALLOC` pendientes:** duplica
  `lista_espera_memoria` bajo lock (`list_duplicate`, snapshot para no tener que sostener
  el lock mientras hace I/O de red) y para cada `t_espera_memoria` del snapshot llama de
  nuevo a `planificador_crear_segmento_km`. Si esta vez tiene éxito: lo remueve de la lista
  real (`list_remove_element`, bajo lock de nuevo), loguea que se satisfizo, y llama
  `planificador_desbloquear` para moverlo de BLOCK/SUSP_BLOCK a READY/SUSP_READY (nótese
  que si estaba en SUSP_BLOCK — poco probable porque el bloqueo por memoria no dispara el
  timer de suspensión, pero el código lo contempla igual — quedaría en SUSP_READY, no en
  READY directo). El orden de recorrido del snapshot es el orden de llegada
  (`list_add` siempre al final), así que los `MEM_ALLOC` se satisfacen FIFO entre sí.

---

### Gestión de CPUs conectadas

#### `void planificador_registrar_cpu(t_planificador* pl, int socket_cpu, int id_cpu)` — planificador.c:812
- **Qué hace:** crea un `t_cpu_conectada` en estado libre, lo agrega a `cpus_conectadas`,
  loguea `"CPU N Conectada"` y hace `sem_post(sem_cpus_libres)`.
- **Dónde se usa:** `conexiones.c:82`, en el handler del handshake `HANDSHAKE_CPU` dentro
  de `manejar_cliente`.
- **Para qué:** alta de una CPU nueva en el pool disponible para el corto plazo.
- **Cómo funciona:** trivial, bajo `mutex_cpus`.

#### `void planificador_remover_cpu(t_planificador* pl, int socket_cpu)` — planificador.c:832
- **Qué hace:** da de baja una CPU por su socket. Si tenía un proceso ejecutando, lo
  recupera de `lista_exec` y lo reencola en READY (para que no se pierda); si estaba
  libre, decrementa `sem_cpus_libres` con `sem_trywait` (para no dejar el semáforo
  "adelantado" respecto de la cantidad real de CPUs libres).
- **Dónde se usa:** `conexiones.c:162`, cuando `recv` devuelve `<= 0` en
  `atender_peticiones_cpu` (la CPU se desconectó o cayó).
- **Para qué:** manejar caídas de CPU sin perder el proceso que estaba corriendo ahí ni
  descuadrar los contadores del pool.
- **Cómo funciona:** recorrido lineal bajo `mutex_cpus`; la llamada a
  `planificador_transicionar_exec_a_ready` ocurre **antes** de soltar el lock de CPUs
  (anidamiento de locks: `mutex_cpus` → dentro, `remover_de_exec` toma `mutex_lista_exec` y
  `encolar_en_ready` toma `mutex_cola_ready` — son mutex distintos, no hay auto-deadlock,
  pero establece un orden que hay que respetar en el resto del código para evitar deadlock
  cruzado).

#### `t_cpu_conectada* planificador_obtener_cpu_por_socket(t_planificador* pl, int socket_cpu)` — planificador.c:856
- **Dónde se usa:** declarada en el header pero **sin callers externos actuales** (no
  aparece en `conexiones.c` ni `main.c` — posible función de utilidad no usada en el flujo
  actual, o remanente de una versión anterior del protocolo).
- **Qué hace / cómo funciona:** búsqueda lineal bajo `mutex_cpus` por `socket_cpu`.

#### `t_cpu_conectada* planificador_obtener_cpu_por_id(t_planificador* pl, int id_cpu)` — planificador.c:870
- **Dónde se usa:** `conexiones.c:168`, al inicio de cada iteración de
  `atender_peticiones_cpu`, para recuperar el struct de la CPU a partir del `id_cpu` fijado
  en el handshake.
- **Para qué:** acceso a los datos de la CPU (pid ejecutando, motivo de interrupción, etc.)
  para procesar el mensaje recién recibido.
- **Cómo funciona:** búsqueda lineal bajo `mutex_cpus`.

---

### Sincronización de contexto con Kernel Memory

#### `void planificador_actualizar_contexto_en_km(t_planificador* pl, uint32_t pid, t_registros_cpu registros)` — planificador.c:888
- **Qué hace:** manda a KM `MENSAJE_ACTUALIZAR_CONTEXTO` + PID + los registros, espera
  confirmación.
- **Dónde se usa:** único caller: `despachar()` (paso 1, antes de tomar cualquier lock de
  compactación/CPU).
- **Para qué:** asegurar que KM tenga los registros más recientes del proceso justo antes
  de que la CPU lo vuelva a ejecutar (los registros se actualizan en el PCB cada vez que
  vuelve de una syscall/interrupción, en `conexiones.c`, pero eso vive solo en el KS; KM
  necesita su copia para servicios que dependen de ella).
- **Cómo funciona:** send/recv síncrono bajo `mutex_socket_memoria`.

#### `static char* planificador_leer_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio)` — planificador.c:904
- **Qué hace:** pide a KM que lea `tamanio` bytes de la dirección lógica de un proceso y
  devuelve un buffer con el contenido (o `NULL` si falló).
- **Dónde se usa:** `hilo_ejecutar_io`, caso `IO_STDOUT` (línea 1238) — el KS necesita leer
  el contenido a imprimir antes de mandarlo al dispositivo IO.
- **Para qué:** soporte de la syscall STDOUT: la dirección viaja lógica (no física) a
  propósito, así sigue siendo válida aunque haya habido una compactación entre el momento
  de la syscall y el momento en que efectivamente se ejecuta la IO (asíncrona).
- **Cómo funciona:** manda `MENSAJE_LEER_MEMORIA` + pid + dir + tamaño; si la respuesta no
  es `MENSAJE_OK` corta y devuelve `NULL`. Si es OK, reserva el buffer — con la
  particularidad de reservar como mínimo 1 byte aunque `tamanio` sea 0, documentado en el
  código: `malloc(0)` puede devolver `NULL` según la libc, lo que el caller interpretaría
  erróneamente como fallo de lectura.

#### `static bool planificador_escribir_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, void* contenido)` — planificador.c:932
- **Qué hace:** contraparte de la anterior: manda `MENSAJE_ESCRIBIR_MEMORIA` + pid + dir +
  tamaño + el contenido, y devuelve si KM confirmó con `MENSAJE_OK`.
- **Dónde se usa:** `hilo_ejecutar_io`, caso `IO_STDIN` (línea 1251) — tras recibir los
  bytes tipeados desde el dispositivo IO, se escriben en la memoria lógica del proceso.
- **Para qué:** soporte de la syscall STDIN.
- **Cómo funciona:** send/recv síncrono bajo `mutex_socket_memoria`.

---

### Syscalls de memoria (crear/eliminar segmento, finalizar proceso) y compactación

#### `static void pausar_planificacion(t_planificador* pl)` / `static void reanudar_planificacion(t_planificador* pl)` — planificador.c:955 / 961
- **Qué hacen:** setean `compactacion_en_curso = true/false` bajo `mutex_compactacion`;
  `reanudar` además hace `pthread_cond_broadcast(&cond_compactacion)` para despertar a
  todos los hilos que estén esperando en `despachar()`.
- **Dónde se usan:** `planificador_crear_segmento_km` (pausar antes de compactar, reanudar
  al final) y `planificador_bsod` (solo `pausar_planificacion`, nunca se reanuda porque el
  proceso termina con `exit()`).
- **Para qué:** el mecanismo de exclusión mutua entre "se está compactando memoria" y "se
  está despachando un proceso a una CPU" (ver `despachar`, paso 2).
- **Cómo funciona:** simple set + broadcast de condition variable.

#### `static int interrumpir_cpus_para_compactar(t_planificador* pl)` — planificador.c:970
- **Qué hace:** recorre todas las CPUs conectadas y a cada una que esté ocupada **y**
  `en_rafaga` (ejecutando instrucciones de verdad, no estacionada en una syscall) le manda
  `MENSAJE_INTERRUPCION` marcando `motivo_interrupcion = INT_COMPACTACION` — pero solo si
  no la había marcado ya (idempotente: no reenvía el mensaje si ya está marcada). Devuelve
  cuántas CPUs siguen en ráfaga (para que el caller sepa cuándo dejar de esperar).
- **Dónde se usa:** en un loop dentro de `planificador_crear_segmento_km`
  (`while (interrumpir_cpus_para_compactar(pl) > 0) usleep(5000);`).
- **Para qué:** paso 2 del protocolo de compactación: forzar a que todas las CPUs
  devuelvan sus procesos antes de que KM reorganice físicamente la memoria.
- **Cómo funciona:** el `usleep(5ms)` en el loop del caller es polling — se reconsulta el
  estado (que cambia asincrónicamente cuando `atender_peticiones_cpu` procesa el
  `MENSAJE_INTERRUPCION` de vuelta y llama `planificador_transicionar_exec_a_ready_frente`,
  lo cual saca al proceso de `lista_exec` y libera la CPU marcándola `libre` en
  `liberar_cpu`) hasta que ninguna quede `en_rafaga`.

#### `bool planificador_crear_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento, uint32_t tamanio, bool* hubo_compactacion)` — planificador.c:988
- **Qué hace:** implementa la syscall `MEM_ALLOC` contra KM, incluyendo el protocolo
  completo de compactación si KM la pide.
- **Dónde se usa:** `conexiones.c:324` (`SYSCALL_MEM_ALLOC`) y
  `planificador_evento_memoria_liberada` (reintento de pendientes, línea 790, con
  `hubo_compactacion = NULL` porque en ese contexto no importa distinguirlo).
- **Para qué:** único punto de creación de segmentos, con el manejo completo del
  protocolo de desalojo síncrono por compactación.
- **Cómo funciona (protocolo completo):** toma `mutex_socket_memoria` para **toda** la
  operación (incluida la eventual compactación — el socket hacia KM queda exclusivo de este
  hilo mientras dura). Manda `MENSAJE_CREAR_SEGMENTO` + pid + id_segmento + tamaño. Si KM
  responde `MENSAJE_COMPACTACION` (en vez de `OK`/`ERROR` directo): (1) marca
  `*hubo_compactacion = true`, (2) `pausar_planificacion` — desde acá nadie más puede
  despachar nada; (3) loop de `interrumpir_cpus_para_compactar` + `usleep(5ms)` hasta que
  todas las CPUs devuelvan sus procesos; (4) manda `MENSAJE_DESALOJO_OK` a KM confirmando
  que ya puede compactar físicamente, y espera la respuesta **final** de la operación
  original (que ahora sí puede ser `MENSAJE_OK`/`MENSAJE_ERROR`, no otra vez
  `COMPACTACION` — KM ya tiene la memoria vacía para compactar); (5)
  `reanudar_planificacion`, liberando a todos los hilos que estaban esperando en
  `despachar()`. Devuelve `respuesta == MENSAJE_OK` al final, sea que hubo compactación o
  no.

#### `bool planificador_eliminar_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento)` — planificador.c:1036
- **Qué hace:** implementa `MEM_FREE`: manda `MENSAJE_ELIMINAR_SEGMENTO` + pid + id,
  devuelve si KM confirmó OK.
- **Dónde se usa:** `conexiones.c:352` (`SYSCALL_MEM_FREE`).
- **Para qué:** liberar un segmento específico de un proceso.
- **Cómo funciona:** send/recv síncrono simple, sin protocolo de compactación (liberar
  nunca necesita compactar).

#### `void planificador_finalizar_proceso_km(t_planificador* pl, uint32_t pid)` — planificador.c:1053
- **Qué hace:** manda `MENSAJE_FINALIZAR_PROCESO` + pid a KM y espera confirmación (sin
  usar el valor de retorno).
- **Dónde se usa:** `finalizar_proceso` en `conexiones.c:151`.
- **Para qué:** avisar a KM que libere **todos** los segmentos y estructuras asociadas al
  proceso que terminó (EXIT o SEG_FAULT).
- **Cómo funciona:** send/recv síncrono simple.

---

### Blue Screen of Death (BSOD)

#### `static void bsod_finalizar_lista(t_planificador* pl, t_list* lista)` / `static void bsod_finalizar_cola(t_planificador* pl, t_queue* cola)` — planificador.c:1072 / 1079
- **Qué hacen:** loguean `"finalizó su ejecución con motivo de BSOD"` para cada PCB de una
  lista o cola (la versión de cola accede a `cola->elements`, el array interno, y delega en
  la de lista).
- **Dónde se usan:** exclusivamente dentro de `planificador_bsod`.
- **Para qué:** generar el log obligatorio de finalización para absolutamente todos los
  procesos vivos en cualquier estado, antes de terminar el proceso del Kernel Scheduler.
- **Cómo funciona:** solo logging — no liberan memoria de los PCBs ni los sacan de sus
  colas (el proceso completo va a morir con `exit()` de todas formas).

#### `void planificador_bsod(t_planificador* pl)` — planificador.c:1083
- **Qué hace:** ante memoria corrupta (stick desconectado, notificado por KM), finaliza
  **todos** los procesos del sistema en cualquier estado y termina el proceso del Kernel
  Scheduler.
- **Dónde se usa:** `hilo_notificaciones_km` en `conexiones.c:480`, al recibir
  `MENSAJE_MEMORIA_CORRUPTA` por el canal de notificaciones de KM. **No retorna** (comentario
  explícito en el caller).
- **Para qué:** política de todo-o-nada ante corrupción de memoria: no hay forma segura de
  seguir operando si un stick se cayó y podía tener segmentos de cualquier proceso.
- **Cómo funciona:** loguea el error, llama `pausar_planificacion` (nunca se reanuda — no
  tiene sentido, el proceso va a morir), y recorre **todas** las colecciones de procesos
  (`cola_new`, cada `colas_ready[q]`, `lista_exec`, `cola_block`, `cola_susp_block`,
  `cola_susp_ready`) bajo su mutex correspondiente llamando a los helpers de logging.
  Finalmente loguea el cierre del KS, hace `log_destroy` y `exit(EXIT_FAILURE)` — no libera
  memoria de las estructuras, confía en que el sistema operativo recupere todo al terminar
  el proceso.

---

### Dispositivos IO (STDIN / STDOUT / SLEEP)

#### `static sem_t* sem_de_tipo_io(t_planificador* pl, t_tipo_io tipo)` / `static const char* nombre_tipo_io(t_tipo_io tipo)` — planificador.c:1120 / 1128
- **Qué hacen:** mapean el enum `t_tipo_io` (`IO_STDIN`/`IO_STDOUT`/`IO_SLEEP`) al
  semáforo correspondiente o a su nombre legible para logs.
- **Dónde se usan:** `planificador_registrar_io`, `planificador_remover_io`,
  `planificador_tomar_io_libre`, `planificador_liberar_io`, `hilo_ejecutar_io`.
- **Para qué:** evitar un `switch` repetido en cada función que necesita el semáforo/nombre
  de un tipo de IO.
- **Cómo funcionan:** `switch` simple con `default` cubriendo `IO_SLEEP`.

#### `void planificador_registrar_io(t_planificador* pl, int fd, t_tipo_io tipo, char* nombre)` — planificador.c:1136
- **Qué hace:** crea un `t_io_conectada`, lo agrega a `ios_conectados`, loguea y hace
  `sem_post` del semáforo de su tipo.
- **Dónde se usa:** `conexiones.c:114`, en el handshake de una IO nueva (`manejar_cliente`).
- **Para qué:** alta de un dispositivo en el pool disponible.
- **Cómo funciona:** trivial, bajo `mutex_ios`.

#### `void planificador_remover_io(t_planificador* pl, int fd)` — planificador.c:1152
- **Qué hace:** da de baja una IO por su `fd`. Si estaba libre, la remueve y libera ya
  mismo (y decrementa el semáforo con `sem_trywait`). Si estaba **en uso**, solo la marca
  `desconectado = true` sin tocarla más — el hilo que la está usando (`hilo_ejecutar_io`)
  es quien la va a terminar de remover cuando la libere.
- **Dónde se usa:** `hilo_ejecutar_io` cuando una operación de IO falla (línea 1263, la IO
  se da por perdida) — nótese que **no** hay caller para desconexión detectada por
  `recv <= 0`, a diferencia de las CPUs; la única vía de detección de una IO caída en este
  código es un error durante una operación en curso.
- **Para qué:** evitar un use-after-free o una doble liberación cuando la IO se cae
  mientras un hilo la está usando activamente.
- **Cómo funciona:** el patrón de "marcar y dejar que el usuario actual limpie" es un
  handoff de responsabilidad de liberación, ver `planificador_liberar_io`.

#### `t_io_conectada* planificador_tomar_io_libre(t_planificador* pl, t_tipo_io tipo)` — planificador.c:1175
- **Qué hace:** bloquea (`sem_wait`) hasta que haya un dispositivo libre del tipo pedido,
  lo marca ocupado y lo devuelve.
- **Dónde se usa:** `hilo_ejecutar_io`, al arrancar cada operación asíncrona.
- **Para qué:** reservar en exclusiva un dispositivo IO del tipo correcto para la
  operación que está por arrancar.
- **Cómo funciona:** tras el `sem_wait`, recorre `ios_conectados` bajo `mutex_ios` buscando
  una IO del `tipo` pedido, `libre` **y no** `desconectado` (una IO recién marcada
  desconectada pero que el semáforo todavía "cree" libre — caso raro pero cubierto —
  simplemente se salta y sigue buscando otra).

#### `void planificador_liberar_io(t_planificador* pl, t_io_conectada* io)` — planificador.c:1192
- **Qué hace:** si la IO fue marcada `desconectado` mientras estaba en uso, la remueve
  definitivamente de la lista y la destruye (sin reponer el semáforo, porque ya no existe
  más). Si no, la marca libre de nuevo y repone el semáforo de su tipo.
- **Dónde se usa:** `hilo_ejecutar_io`, al terminar una operación exitosa (línea 1265).
- **Para qué:** contraparte de `planificador_tomar_io_libre`, con el caso especial de
  limpieza diferida por desconexión.
- **Cómo funciona:** todo bajo `mutex_ios`.

#### `static bool io_enviar_pid_y_param(int fd, uint32_t pid, uint32_t param)` / `static bool io_esperar_ok(int fd)` — planificador.c:1214 / 1220
- **Qué hacen:** helpers de protocolo mínimo hacia el dispositivo IO: mandar PID+parámetro,
  y esperar una respuesta `MENSAJE_OK` (cualquier otra cosa, o un `recv` fallido, se
  interpreta como error).
- **Dónde se usan:** exclusivamente dentro de `hilo_ejecutar_io`.
- **Cómo funcionan:** `send`/`recv` directos sobre el `fd` de la IO, devuelven `bool` de
  éxito.

#### `static void* hilo_ejecutar_io(void* arg)` — planificador.c:1226
- **Qué hace:** ejecuta de punta a punta una operación de IO para un proceso bloqueado:
  toma un dispositivo libre del tipo pedido, ejecuta el protocolo específico de
  STDIN/STDOUT/SLEEP, libera el dispositivo, y desbloquea al proceso (a READY o SUSP_READY
  según corresponda).
- **Dónde se usa:** creado (`pthread_create` + `pthread_detach`) exclusivamente por
  `planificador_ejecutar_io_async`.
- **Para qué:** hacer la IO sin bloquear el hilo que recibió la syscall (que ya liberó la
  CPU y siguió atendiendo otros mensajes) — es el mecanismo de asincronía de IO.
- **Cómo funciona:** toma la IO con `planificador_tomar_io_libre`. Según `a->tipo`:
  **`IO_STDOUT`**: primero lee el contenido lógico desde KM
  (`planificador_leer_memoria_en_km`), y si tuvo éxito, manda PID+tamaño y el contenido al
  dispositivo IO y espera OK. **`IO_STDIN`**: manda PID+tamaño al dispositivo, recibe el
  buffer tecleado, y si eso funcionó, lo escribe en la memoria lógica del proceso vía
  `planificador_escribir_memoria_en_km`. **`IO_SLEEP`** (default): solo manda PID+tiempo y
  espera OK, no toca memoria. Si algo falló en cualquier rama (`!ok`): loguea error y llama
  `planificador_remover_io` (la IO se da de baja, asumiendo que el dispositivo quedó en mal
  estado). Si salió bien: `planificador_liberar_io` (vuelve al pool). En cualquier caso —
  éxito o fallo — termina llamando `planificador_desbloquear(pl, pcb->pid,
  &estaba_suspendido)`: el proceso puede haberse suspendido por timeout **mientras**
  esperaba la IO (la IO puede tardar más que `SUSPENSION_TIMEOUT`), así que no se asume que
  sigue en BLOCK — si `estaba_suspendido` da `true`, queda en SUSP_READY y se dispara
  `planificador_evento_memoria_liberada` para intentar des-suspenderlo ya mismo si hay
  lugar.

#### `void planificador_ejecutar_io_async(t_planificador* pl, t_pcb* pcb, t_tipo_io tipo, uint32_t param, uint32_t tamanio)` — planificador.c:1284
- **Qué hace:** wrapper público que empaqueta los argumentos y lanza `hilo_ejecutar_io` en
  un hilo separado y detached.
- **Dónde se usa:** `conexiones.c` para `SYSCALL_SLEEP` (línea 228) y
  `SYSCALL_STDIN`/`SYSCALL_STDOUT` (línea 244), siempre inmediatamente después de
  `planificador_transicionar_exec_a_block`.
- **Cómo funciona:** trivial, `malloc` de args + `pthread_create` + `pthread_detach`.

---

### Mutex del Kernel (con herencia de prioridades)

#### `static t_mutex_kernel* buscar_mutex_sin_lock(t_planificador* pl, char* nombre)` — planificador.c:1301
- **Qué hace:** búsqueda lineal en `lista_mutex` por nombre, **sin tomar ningún lock**
  (el nombre lo deja explícito).
- **Dónde se usa:** `planificador_crear_mutex`, `planificador_lock_mutex`,
  `planificador_unlock_mutex` — siempre con `mutex_lista_mutex` ya tomado por el caller.
- **Para qué:** evitar duplicar el recorrido lineal en las tres funciones públicas de
  mutex.
- **Cómo funciona:** comparación de strings, `strcmp == 0`.

#### `static t_pcb* buscar_pcb_por_pid(t_planificador* pl, uint32_t pid)` — planificador.c:1310
- **Qué hace:** busca un PCB por PID recorriendo, en orden, `lista_exec`, todas las
  `colas_ready[q]` y `cola_block`.
- **Dónde se usa:** `planificador_lock_mutex` (línea 1414), para localizar al dueño actual
  de un mutex y poder aplicarle herencia de prioridad.
- **Para qué:** la herencia de prioridad necesita modificar el PCB del dueño **donde sea
  que esté** (podría estar en READY esperando, en EXEC corriendo, o incluso en BLOCK por
  otra cosa — aunque un dueño de mutex normalmente está en EXEC o READY).
- **Cómo funciona:** tres búsquedas lineales secuenciales, cada una bajo su propio mutex
  (se libera uno antes de tomar el siguiente — no hay anidamiento de locks acá). Nota:
  **no** busca en `cola_susp_block`/`cola_susp_ready` — un proceso suspendido no puede ser
  dueño de un mutex en ejecución activa en este diseño (asunción implícita, no verificada
  explícitamente en el código).

#### `static void cambiar_prioridad(t_planificador* pl, t_pcb* pcb, int nueva)` — planificador.c:1348
- **Qué hace:** cambia `pcb->prioridad_actual` a `nueva`. Si el proceso está en READY y el
  algoritmo es CMN, además lo **mueve físicamente** de la cola vieja a la cola nueva (al
  final de la nueva). Emite el log obligatorio `"Cambio de prioridad: <anterior> <nueva>"`.
- **Dónde se usa:** `planificador_lock_mutex` (aplicar herencia, línea 1416) y
  `planificador_unlock_mutex` (restaurar prioridad original, línea 1436).
- **Para qué:** único punto que cambia la prioridad efectiva de un proceso, manteniendo
  consistente su posición en las colas READY multinivel.
- **Cómo funciona:** si `anterior == nueva` no hace nada (evita logs y trabajo
  redundante). Si está en READY y es CMN: recorre las `cant_colas` colas buscando en cuál
  está el PCB con `list_remove_element` (para en la primera que lo encuentre — devuelve
  `true`), y lo reinserta en la cola que corresponde a `nueva` (clampeada al rango válido)
  con `list_add` (al final, no al frente — pierde su lugar en la fila de la nueva cola, lo
  cual es el comportamiento esperado para un cambio de prioridad). Si no está en READY (o
  no es CMN), solo actualiza el campo sin tocar ninguna cola física.

#### `void planificador_crear_mutex(t_planificador* pl, char* nombre)` — planificador.c:1373
- **Qué hace:** implementa `SYSCALL_MUTEX_CREATE`: crea el mutex si no existe ya (si
  existe, loguea warning y lo ignora — no lo recrea).
- **Dónde se usa:** `conexiones.c:261`.
- **Cómo funciona:** todo bajo `mutex_lista_mutex`.

#### `bool planificador_lock_mutex(t_planificador* pl, t_pcb* pcb, char* nombre)` — planificador.c:1384
- **Qué hace:** implementa `SYSCALL_MUTEX_LOCK` con herencia de prioridades. Devuelve
  `true` si el proceso lo adquirió de inmediato, `false` si quedó encolado esperando.
- **Dónde se usa:** `conexiones.c:279`.
- **Para qué:** la función más "de libro de sistemas operativos" del archivo: exclusión
  mutua de usuario + protocolo de herencia de prioridad para evitar inversión de
  prioridades.
- **Cómo funciona:** busca el mutex por nombre bajo `mutex_lista_mutex`; si no existe (caso
  defensivo — "no debería pasar" según el comentario del código, ya que los scripts de
  prueba no deberían tener errores semánticos, pero por robustez lo crea on-demand en vez
  de colgar al proceso silenciosamente). Si `!m->tomado`: lo marca tomado, asigna
  `pid_duenio = pcb->pid`, `adquirido = true`. Si ya estaba tomado: encola el PID del que
  pide (`malloc` de un `uint32_t*`, `queue_push` en `m->cola_bloqueados`), guarda
  `pid_duenio` actual para después, `adquirido = false`. Libera `mutex_lista_mutex` **antes**
  de la parte de herencia (para no sostener ese lock mientras potencialmente mueve PCBs
  entre colas, que toman otros locks). Si no lo adquirió: busca al dueño actual con
  `buscar_pcb_por_pid`; si existe **y** la prioridad del que pide es numéricamente menor
  (más prioritaria) que la del dueño (`pcb->prioridad_actual < duenio->prioridad_actual`),
  llama `cambiar_prioridad(pl, duenio, pcb->prioridad_actual)` — el dueño hereda la
  prioridad del que espera, temporalmente, para que no lo desalojen procesos de prioridad
  intermedia mientras tiene el recurso tomado (inversión de prioridad clásica). Nótese que
  esto **no** es transitivo en cadena en este código (si el dueño a su vez espera otro
  mutex tomado por un tercero, no hay propagación recursiva de la herencia) y **no**
  compara contra la prioridad ya heredada del dueño de forma iterativa con múltiples
  esperas simultáneas más allá de la comparación puntual mostrada.

#### `void planificador_unlock_mutex(t_planificador* pl, t_pcb* pcb_liberador, char* nombre)` — planificador.c:1423
- **Qué hace:** implementa `SYSCALL_MUTEX_UNLOCK`: quita el mutex de la lista de tomados
  del que libera, le restaura su prioridad original si la tenía heredada, y si había
  alguien esperando en la cola del mutex, se lo asigna como nuevo dueño y lo despierta.
- **Dónde se usa:** `conexiones.c:307` (liberación normal por syscall) y
  `finalizar_proceso` en `conexiones.c:147` (liberación forzada de todos los mutex que
  tuviera un proceso que termina, uno por uno hasta vaciar `lista_mutex_tomados`).
- **Para qué:** cierre del ciclo de vida del mutex + reversión de la herencia de
  prioridad.
- **Cómo funciona:** primero busca y remueve el `nombre` de
  `pcb_liberador->lista_mutex_tomados` (comparación de strings, `free` de la copia
  encontrada). Luego, **antes** de tocar la estructura del mutex: si
  `prioridad_actual != prioridad_original`, restaura con `cambiar_prioridad` — esto ocurre
  fuera de cualquier lock de mutex, apoyándose en los locks internos de `cambiar_prioridad`.
  Nota importante de diseño: la restauración es incondicional respecto de "cuántos mutex
  seguía teniendo tomados" — si el proceso tenía herencia por este mutex pero **todavía**
  tiene otro mutex tomado por el cual también debería tener prioridad heredada, este código
  igual lo restaura a la original (no hay un mecanismo de "prioridad heredada por el más
  restrictivo de varios mutex simultáneos"; es una simplificación válida si se asume que un
  proceso no toma múltiples mutex con distintos esperadores de prioridad distinta a la vez,
  pero es un caso borde a tener en cuenta si se lo defiende oralmente). Después, bajo
  `mutex_lista_mutex`: busca el mutex; si hay alguien en `cola_bloqueados`, lo saca
  (`queue_pop`, devuelve el PID más antiguo en espera — orden FIFO de espera, no por
  prioridad) y lo transfiere como nuevo dueño (`pid_duenio`, `tomado = true`); si no hay
  nadie, el mutex queda libre (`pid_duenio = 0`, `tomado = false`). Fuera del lock: si había
  alguien despertando, llama `planificador_desbloquear` (contempla el caso de que se haya
  suspendido mientras esperaba el mutex — pasaría a SUSP_READY con el mutex ya asignado
  como tomado, aunque su memoria siga en SWAP hasta que se des-suspenda de verdad), le
  agrega el nombre del mutex a su `lista_mutex_tomados`, loguea `"Toma el Mutex"`, y si
  estaba suspendido dispara `planificador_evento_memoria_liberada`.

---

### Flujo de herencia de prioridades paso a paso

1. Un proceso `A` (prioridad numérica baja = alta prioridad) ejecuta `SYSCALL_MUTEX_LOCK`
   sobre un mutex que ya tiene tomado el proceso `B` (prioridad numérica alta = baja
   prioridad, por ejemplo porque `B` está en una cola CMN peor que `A`).
2. En `conexiones.c`, `atender_peticiones_cpu` recibe la syscall, actualiza los registros
   de `A` en su PCB, y llama `planificador_lock_mutex(pl, A, nombre)`.
3. Dentro de `planificador_lock_mutex`: como `m->tomado` es verdadero, `A` queda encolado en
   `m->cola_bloqueados` y la función retiene `pid_duenio = B->pid`. `adquirido = false`.
4. Con `mutex_lista_mutex` ya liberado, la función busca a `B` con `buscar_pcb_por_pid`
   (puede estar en `lista_exec` si está corriendo, o en alguna `colas_ready[q]` si está
   esperando su turno).
5. Compara `A->prioridad_actual < B->prioridad_actual`. Si es cierto (A es más prioritario
   que B), llama `cambiar_prioridad(pl, B, A->prioridad_actual)`.
6. `cambiar_prioridad` actualiza `B->prioridad_actual` al valor heredado. Si `B` está en
   READY, además lo reubica físicamente a la cola CMN correspondiente a esa nueva
   prioridad (para que compita por CPU con la prioridad prestada). Emite el log obligatorio
   `"## <pid B> Cambio de prioridad: <anterior> <nueva>"`.
7. De vuelta en `conexiones.c`, como `adquirido == false`, el proceso `A` se transiciona a
   BLOCK con motivo `"MUTEX"` (`planificador_transicionar_exec_a_block`) y se libera su CPU.
   `B` sigue corriendo/esperando, ahora con la prioridad heredada — más competitivo por CPU
   de lo que le correspondería originalmente, evitando que un tercer proceso `C` de
   prioridad intermedia lo desaloje mientras retiene el recurso que `A` necesita
   (inversión de prioridad evitada).
8. Cuando `B` eventualmente ejecuta `SYSCALL_MUTEX_UNLOCK`, `conexiones.c` llama
   `planificador_unlock_mutex(pl, B, nombre)`.
9. Esa función ve que `B->prioridad_actual != B->prioridad_original` y llama
   `cambiar_prioridad(pl, B, B->prioridad_original)` — `B` vuelve a su prioridad real
   (con el mismo efecto de reubicación de cola si sigue en READY) y se loguea el cambio de
   vuelta.
10. Luego, bajo `mutex_lista_mutex`, saca a `A` de `m->cola_bloqueados` (FIFO — el primero
    en la cola de espera, que en este escenario es el único), lo asigna como nuevo
    `pid_duenio` del mutex.
11. Fuera del lock, llama `planificador_desbloquear(pl, A->pid, &estaba_suspendido)`, que
    (asumiendo que `A` no se suspendió por timeout mientras esperaba) lo saca de `cola_block`
    y lo pasa a READY. Se le agrega el nombre del mutex a `A->lista_mutex_tomados` y se
    loguea `"## (<pid A>) Toma el Mutex <nombre>"`.
12. El corto plazo eventualmente despacha a `A`, que retoma ejecución ya con el mutex en su
    poder.

---

### Flujo de compactación paso a paso

1. Un proceso ejecuta `SYSCALL_MEM_ALLOC`. En `conexiones.c`, se llama
   `planificador_crear_segmento_km(pl, pid, id_segmento, tamanio, &hubo_compactacion)`.
2. Esa función manda `MENSAJE_CREAR_SEGMENTO` a Kernel Memory y espera respuesta, todo bajo
   `mutex_socket_memoria` (el socket hacia KM queda reservado para este hilo durante toda
   la operación, incluida la compactación si se dispara).
3. KM evalúa: hay espacio libre total suficiente, pero no contiguo. Responde
   `MENSAJE_COMPACTACION` en lugar de `OK`/`ERROR`.
4. El KS marca `*hubo_compactacion = true` y llama `pausar_planificacion(pl)`: desde este
   momento, `compactacion_en_curso = true`, y cualquier hilo que intente `despachar()` un
   proceso nuevo a una CPU queda bloqueado en `pthread_cond_wait` (paso 2 de `despachar`).
5. Entra en el loop `while (interrumpir_cpus_para_compactar(pl) > 0) usleep(5ms);`. Cada
   iteración: recorre todas las CPUs conectadas; a las que están `en_rafaga` (ejecutando
   instrucciones de verdad — las que solo tienen un proceso "estacionado" en una syscall no
   cuentan) les manda `MENSAJE_INTERRUPCION` con `motivo_interrupcion = INT_COMPACTACION`
   (una sola vez cada una, es idempotente).
6. Cada CPU interrumpida, al terminar su instrucción actual, detecta la interrupción
   (`cpu_check_interrupt` del lado CPU) y devuelve el proceso al KS con
   `MENSAJE_INTERRUPCION` + sus registros actualizados.
7. En `conexiones.c`, `atender_peticiones_cpu` recibe ese `MENSAJE_INTERRUPCION`, ve que
   `motivo_int == INT_COMPACTACION`, actualiza los registros del PCB, loguea
   `"Desalojado por compactación"` y llama
   `planificador_transicionar_exec_a_ready_frente(pl, pcb)` — el proceso vuelve **al
   principio** de su cola READY (no pierde su lugar en la fila por esto, ya que no fue por
   su culpa). Luego `liberar_cpu(cpu)` marca la CPU libre y `en_rafaga = false`.
8. `interrumpir_cpus_para_compactar` vuelve a evaluarse cada 5ms desde el paso 5; cuando ya
   ninguna CPU está `en_rafaga`, el contador da 0 y el loop termina.
9. El KS manda `MENSAJE_DESALOJO_OK` a KM por el mismo socket, confirmando que ya no hay
   CPUs tocando memoria, y espera la respuesta final.
10. KM, con la certeza de que ninguna CPU está en medio de un acceso a memoria, ejecuta
    `gestor_memoria_compactar`: reordena todos los segmentos ocupados al inicio del espacio
    físico (eliminando los huecos entre ellos), actualiza las tablas de segmentos de todos
    los procesos afectados, y **ahora sí** puede crear el segmento pedido originalmente
    (ya hay espacio contiguo). Responde `MENSAJE_OK` (o `MENSAJE_ERROR` si por algún motivo
    tampoco entra ni compactando).
11. El KS recibe esa respuesta final y llama `reanudar_planificacion(pl)`:
    `compactacion_en_curso = false` + `pthread_cond_broadcast` — todos los hilos que
    estaban esperando en `despachar()` (intentando despachar otros procesos mientras tanto)
    se despiertan y proceden normalmente.
12. Libera `mutex_socket_memoria` y devuelve `respuesta == MENSAJE_OK` al caller original en
    `conexiones.c`.
13. Si fue éxito: `conexiones.c` llama `planificador_despachar_directo` (el proceso que pidió
    el `MEM_ALLOC` vuelve a la misma CPU que lo pidió, ahora con el segmento creado) y,
    como hubo compactación, además llama `planificador_evento_memoria_liberada(pl)` — la
    reorganización de memoria pudo haber generado espacio contiguo adicional que permita
    des-suspender a algún proceso en SUSP_READY que antes no entraba.
