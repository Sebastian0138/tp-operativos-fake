# Plug & Pray — Consigna ↔ Resolución

Recorre el enunciado (`Plug & Pray.pdf` v1.1) en su mismo orden. Por cada requisito:
la cita textual, cómo se resuelve, y las funciones que intervienen.

Complemento: `MAPA_TP.md` (índice de búsqueda rápida por funcionalidad).

---

# MÓDULO: KERNEL SCHEDULER

## KS-1. Lineamiento e implementación

> «El Kernel Scheduler contará con un primer parámetro que será el Path al Proceso Inicial, el cual será el PID 0 del sistema, que siempre tendrá prioridad máxima, y a partir del cual se crearán los demás Procesos.»

**Cómo se resuelve.** `main()` valida que lleguen exactamente 3 argumentos (`argv[0]` binario, `argv[1]` config, `argv[2]` path del proceso inicial) y aborta si no. El PID 0 se crea a mano, fuera del flujo normal de `INIT_PROC`: se instancia el PCB con `pcb_create(0, argv[2])` y se lo encola directamente en NEW. El contador `proximo_pid` del planificador arranca en **1** justamente para reservarle el 0 al proceso inicial.

La "prioridad máxima" sale gratis: `pcb_create()` inicializa `prioridad_original` y `prioridad_actual` en 0, y en este TP **prioridad 0 es la más alta** (`cola_de()` mapea prioridad→índice de cola, y `desencolar_ready()` recorre las colas de menor índice a mayor, así que la cola 0 se vacía primero).

| Función | Ubicación |
|---|---|
| `main()` — validación de argumentos | `kernel_scheduler/src/main.c:21-25` |
| `pcb_create(0, argv[2])` + encolado | `kernel_scheduler/src/main.c:78-79` |
| `proximo_pid = 1` | `planificador.c:184` |
| `cola_de()` — prioridad → índice de cola | `planificador.c:18` |
| `desencolar_ready()` — recorre de la cola 0 hacia arriba | `planificador.c:97-107` |

---

> «Al iniciar el módulo, el mismo se deberá conectar con el módulo Kernel Memory y, una vez que establezca dicha conexión, deberá crear un servidor multihilo para atender de forma concurrente las peticiones de las CPUs y de los módulos de IO.»

**Cómo se resuelve.** El orden en `main()` es estricto y respeta la dependencia del diagrama de arquitectura: primero `conectar_a_kernel_memory()` (como cliente), después se construye el planificador, y **al final** se levanta el servidor, que es bloqueante y no retorna nunca.

El servidor es un `accept` en loop infinito: por cada conexión entrante lanza un `pthread` desprendido con `manejar_cliente()`, que lee el `int32` de handshake y ramifica. Si es `HANDSHAKE_CPU`, ese mismo hilo se queda para siempre atendiendo a esa CPU (`atender_peticiones_cpu()`); si es `HANDSHAKE_IO`, registra el dispositivo en el pool y el hilo termina — el fd queda en manos de los hilos de IO asíncrona, que lo toman bajo demanda.

**Detalle de diseño:** el KS abre **dos** conexiones contra KM, no una. La principal (`HANDSHAKE_SCHEDULER`) es request/response sincrónico serializado por `mutex_socket_memoria`. La segunda (`HANDSHAKE_SCHEDULER_NOTIF`) es un canal de push unidireccional KM→KS. Sin esa separación, un `MEMORIA_CORRUPTA` o un `MAS_MEMORIA` empujado por KM en cualquier momento se intercalaría con una respuesta pendiente del canal principal y desincronizaría el protocolo para siempre.

| Función | Ubicación |
|---|---|
| `conectar_a_kernel_memory()` | `kernel_scheduler/src/conexiones/conexiones.c:446` |
| `conectar_canal_notificaciones_km()` | `conexiones.c:492` |
| `iniciar_servidor_kernel_scheduler()` — loop de accept | `conexiones.c:20`, loop en `:33-46` |
| `manejar_cliente()` — dispatcher de handshake | `conexiones.c:49` |
| Orden de arranque | `kernel_scheduler/src/main.c:55-82` |

---

> «A lo largo de la ejecución se podrán desconectar y conectar nuevas CPUs, por lo que se deberá mantener la escucha a nuevas conexiones siempre activa.»

**Cómo se resuelve.** El `while(1)` de `accept` nunca sale, así que la escucha es permanente. La desconexión se detecta en `atender_peticiones_cpu()`: cuando el `recv` del código de mensaje devuelve `<= 0`, se llama a `planificador_remover_cpu()` y el hilo termina.

Lo importante de `planificador_remover_cpu()` es que **no pierde el proceso**: si la CPU tenía un PCB ejecutando (`!cpu->libre`), lo saca de `lista_exec` y lo reencola en READY. Si la CPU estaba libre, hace `sem_trywait(&sem_cpus_libres)` para decrementar el semáforo y que el corto plazo no crea que hay una CPU disponible que ya no existe.

| Función | Ubicación |
|---|---|
| Detección de la baja | `conexiones.c:160-165` |
| `planificador_remover_cpu()` | `planificador.c:832` |
| Recupero del proceso en ejecución | `planificador.c:839-844` |
| Ajuste del semáforo si estaba libre | `planificador.c:845-848` |

---

## KS-2. Planificación de Largo Plazo

> «Se encargará de ingresar Procesos de NEW a READY. Dado que los Procesos inician sin memoria asignada, este pasaje se podrá realizar sin ninguna limitación.»

**Cómo se resuelve.** Hay un hilo dedicado, `hilo_planificador_largo_plazo()`, que corre un `while(1)` bloqueado en `sem_wait(&sem_procesos_new)`. Cada vez que alguien encola un proceso en NEW hace `sem_post`, y el hilo despierta, hace `queue_pop` bajo `mutex_cola_new` y lo transiciona.

La frase *"sin ninguna limitación"* se traduce en que **no hay ningún chequeo de memoria** antes de pasar a READY: no se consulta espacio libre, no se reserva nada. Lo único que se hace en el medio es registrar el proceso en Kernel Memory con `MENSAJE_ENVIAR_PID`, que crea su contexto de ejecución (registros en 0 y tabla de segmentos vacía) y asocia el PID a su archivo de pseudocódigo. Eso no puede fallar por falta de memoria porque el proceso todavía no tiene ni un segmento.

Recién después de recibir el OK de KM se llama a `planificador_transicionar_new_a_ready()`, que cambia el estado, sella el timestamp de llegada a READY y encola.

| Función | Ubicación |
|---|---|
| `hilo_planificador_largo_plazo()` | `planificador.c:249` |
| Bloqueo en el semáforo | `planificador.c:254` |
| `planificador_encolar_new()` (productor) | `planificador.c:434` |
| Registro en KM (`MENSAJE_ENVIAR_PID`) | `planificador.c:264-279` |
| Recepción del lado de KM | `kernel_memory_atencion_scheduler.c:24-45` |
| Creación del contexto vacío | `kernel_memory_gestor_procesos.c:29-42` |
| `planificador_transicionar_new_a_ready()` | `planificador.c:443` |

---

### ★ BSOD — el caso que pediste en detalle

> «En algún momento de la ejecución es posible que se reciba una notificación del Kernel Memory de que se detectó corrupción en una parte de la memoria. Ante este evento, se deberán finalizar todos los Procesos y finalizar el Kernel Scheduler con motivo de Blue Screen of Death (BSOD).»

Desglosemos la consigna en sus tres partes, porque cada una se resuelve en un lugar distinto:

1. *"se reciba una notificación del Kernel Memory"* → **cómo llega el aviso**
2. *"que se detectó corrupción en una parte de la memoria"* → **quién y cómo la detecta**
3. *"finalizar todos los Procesos y finalizar el Kernel Scheduler"* → **qué se hace al recibirla**

#### Parte 2 (cronológicamente primero): la detección, en Kernel Memory

El enunciado define la corrupción en la sección de Kernel Memory: *«Existe la posibilidad de que en medio de una ejecución un Memory Stick se desconecte. Dado el caso, se deberá notificar al Kernel Scheduler que la memoria se encuentra corrupta.»*

Cuando un Memory Stick hace handshake contra KM, el hilo que lo atiende (`atender_memory_stick()`) hace tres cosas: recibe el `INFORMAR_TAMANIO`, registra el stick, y después **se queda colgado en un `recv` que nunca debería retornar**:

```c
char descarte;
while (recv(fd, &descarte, 1, MSG_WAITALL) > 0);
gestor_memoria_stick_desconectado(_gestor_memoria, id_stick);
```

Esa es la clave del mecanismo. El stick no vuelve a escribir jamás por esa conexión de control (las lecturas y escrituras van por otro socket, el canal de datos que KM abre *hacia* el stick). Entonces el `recv` bloquea indefinidamente mientras el stick esté vivo. Si el proceso del stick muere o cierra el socket, el `recv` devuelve 0, el `while` corta, y la línea siguiente es la notificación de corrupción. Es detección de caída por EOF de socket, sin polling ni heartbeat.

`gestor_memoria_stick_desconectado()` entonces marca el stick como `conectado = false`, cierra su fd de datos y llama a `notificar_ks(gm, MENSAJE_MEMORIA_CORRUPTA, NULL)`.

| Función | Ubicación |
|---|---|
| `atender_memory_stick()` — el `recv` centinela | `kernel_memory/src/conexiones/conexiones.c:83-86` |
| `gestor_memoria_stick_desconectado()` | `gestor_memoria.c:127` |
| Marcado del stick como caído | `gestor_memoria.c:131-136` |

#### Parte 1: el transporte de la notificación

`notificar_ks()` escribe el código de mensaje en `gm->fd_notif_ks`, que es el fd del **canal de notificaciones**, no el canal principal. Ese fd se registró al arranque: cuando el KS se conectó por segunda vez con `HANDSHAKE_SCHEDULER_NOTIF`, KM llamó a `gestor_memoria_set_notif_ks()` y devolvió `NULL` sin cerrar el socket, dejándolo guardado en el gestor de memoria para uso futuro.

El `send` va protegido por `mutex_notif` porque por este mismo canal también viaja `MENSAJE_MAS_MEMORIA`, y dos sticks conectándose/desconectándose en simultáneo podrían intercalar sus escrituras.

Del lado del KS, `hilo_notificaciones_km()` es un hilo dedicado que sólo hace `recv` en loop sobre ese fd y ramifica según el código. Al recibir `MENSAJE_MEMORIA_CORRUPTA` llama a `planificador_bsod()` — y el comentario del código lo aclara: `// no retorna`.

| Función | Ubicación |
|---|---|
| `notificar_ks()` | `gestor_memoria.c:78` |
| Registro del fd de notificaciones | `gestor_memoria_set_notif_ks()` `gestor_memoria.c:72`, llamada en `kernel_memory/src/conexiones/conexiones.c:114` |
| `hilo_notificaciones_km()` (lado KS) | `kernel_scheduler/src/conexiones/conexiones.c:459` |
| Case `MENSAJE_MEMORIA_CORRUPTA` | `conexiones.c:479-481` |

#### Parte 3: la ejecución del BSOD

`planificador_bsod()` hace, en orden:

1. **Loguea el error** y llama a `pausar_planificacion()`, que levanta el flag `compactacion_en_curso`. Reusa el mismo mecanismo del desalojo por compactación: cualquier hilo que esté por despachar un proceso queda esperando en `cond_compactacion` dentro de `despachar()` y no llega a mandar nada a ninguna CPU. Es la forma de congelar el sistema antes de empezar a finalizar procesos.

2. **Recorre las 6 colas** emitiendo el log obligatorio de fin de proceso con motivo BSOD por cada PCB. El enunciado dice *"todos los Procesos"*, así que no alcanza con los que están ejecutando: se barren NEW, las N colas de READY (loop sobre `cant_colas`, porque en CMN hay una por nivel de prioridad), EXEC, BLOCK, SUSP. BLOCK y SUSP. READY. Cada cola se toma bajo su propio mutex.

   Hay dos helpers porque las estructuras difieren: `bsod_finalizar_lista()` opera sobre `t_list*` (READY y EXEC) y `bsod_finalizar_cola()` sobre `t_queue*`, accediendo a su campo `->elements` que es la lista interna.

3. **Finaliza el Kernel Scheduler**: loguea el motivo, destruye el logger y hace `exit(EXIT_FAILURE)`. Es una salida deliberadamente abrupta — no se destruye el planificador ni se cierran sockets ordenadamente, porque el sistema ya está en un estado inconsistente (hay memoria física que desapareció bajo los pies de procesos que la tenían asignada). El código de salida es de error, no 0.

| Función | Ubicación |
|---|---|
| `planificador_bsod()` | `planificador.c:1083` |
| `pausar_planificacion()` | `planificador.c:955` |
| Barrera efectiva en el despacho | `planificador.c:341-344` |
| `bsod_finalizar_lista()` / `bsod_finalizar_cola()` | `planificador.c:1072` / `:1079` |
| Barrido NEW | `planificador.c:1088-1090` |
| Barrido READY (todas las colas) | `planificador.c:1092-1096` |
| Barrido EXEC | `planificador.c:1098-1100` |
| Barrido BLOCK | `planificador.c:1102-1104` |
| Barrido SUSP. BLOCK + SUSP. READY | `planificador.c:1106-1109` |
| `exit(EXIT_FAILURE)` | `planificador.c:1113` |

#### Recorrido completo, de punta a punta

```
[Memory Stick muere]
   │
   ▼  EOF en el socket de control
atender_memory_stick()              kernel_memory/src/conexiones/conexiones.c:85
   │  el while(recv) corta
   ▼
gestor_memoria_stick_desconectado() gestor_memoria.c:127
   │  marca conectado=false, cierra fd_datos
   ▼
notificar_ks(MEMORIA_CORRUPTA)      gestor_memoria.c:139 → :78
   │  send por fd_notif_ks  ══════════════ canal _NOTIF ══════════════╗
   │                                                                  ║
   ╚══════════════════════ KERNEL MEMORY ════╤═══ KERNEL SCHEDULER ═══╝
                                             ▼
                              hilo_notificaciones_km()   conexiones.c:459
                                             │  case MENSAJE_MEMORIA_CORRUPTA :479
                                             ▼
                              planificador_bsod()        planificador.c:1083
                                             │
                        ┌────────────────────┼────────────────────┐
                        ▼                    ▼                    ▼
              pausar_planificacion()  barrido de las 6 colas   exit(EXIT_FAILURE)
                 planificador.c:955      :1088 a :1109            :1113
```

> ⚠ **Desvío detectado.** El enunciado pide finalizar *los Procesos*, y el código sólo
> emite el log por cada uno — no avisa a Kernel Memory que libere sus segmentos ni
> cierra las conexiones con las CPUs. En la práctica no cambia el resultado observable
> (el KS muere de inmediato y el sistema se cae entero), pero conceptualmente los
> procesos no se "finalizan", se abandonan. Si en el coloquio te preguntan por qué no
> se llama a `planificador_finalizar_proceso_km()` para cada PID, la respuesta honesta
> es que la memoria ya está corrupta y liberarla ordenadamente no aporta nada.

---

## KS-3. Planificación de Mediano Plazo

> «Para realizar la transición del estado BLOCK a SUSP. BLOCK, por cada Proceso que entre al estado BLOCK, se deberá esperar un tiempo determinado por archivo de configuración. Si al transcurrir dicho tiempo el Proceso aún continúa en estado BLOCK, se lo deberá mover al estado SUSP. BLOCK.»

**Cómo se resuelve.** *"Por cada Proceso que entre al estado BLOCK"* se implementa literal: `planificador_transicionar_exec_a_block()` lanza un hilo desprendido (`hilo_timer_suspension()`) en cada entrada a BLOCK. El hilo duerme `SUSPENSION_TIMEOUT` milisegundos y después chequea si corresponde suspender.

El problema fino acá es la condición *"si aún continúa en estado BLOCK"*. No alcanza con preguntar por el estado, porque un proceso puede haberse desbloqueado y **vuelto a bloquear** por otra razón mientras el primer timer todavía dormía: ese timer viejo suspendería un bloqueo que no le corresponde. La solución es el campo `bloqueo_id` del PCB, un contador que se incrementa en cada entrada a BLOCK (`planificador.c:581`) y también en cada salida (`:626`). El timer captura el `bloqueo_id` vigente al nacer y sólo actúa si al despertar el PCB sigue teniendo *ese mismo* valor. Un timer obsoleto no encuentra coincidencia y termina sin hacer nada.

La transición en sí se hace **atómica**: se toma `mutex_cola_block`, se busca y extrae el PCB, se toma `mutex_cola_susp`, se lo empuja a `cola_susp_block`, y recién ahí se sueltan ambos. Si se soltara el primero antes de tomar el segundo, un fin de IO concurrente podría buscar el PCB y no encontrarlo en ninguna de las dos colas.

Recién con el proceso ya en SUSP. BLOCK se le pide a KM que mueva sus segmentos a SWAP (`MENSAJE_SUSPENDER_PROCESO`), y como eso liberó memoria principal, se dispara `planificador_evento_memoria_liberada()`.

**Excepción propia del grupo:** los procesos bloqueados con motivo `"MEMORIA"` (un `MEM_ALLOC` que no entró y quedó esperando) **no** reciben timer. Están bloqueados esperando que se libere memoria, y swapearlos no tendría sentido: no tienen nada que swapear todavía, y el pedido pendiente lo resuelve el gestor de memoria por otra vía.

| Función | Ubicación |
|---|---|
| Lanzamiento del timer | `planificador_transicionar_exec_a_block()` `planificador.c:596-605` |
| `hilo_timer_suspension()` | `planificador.c:513` |
| Espera del timeout | `planificador.c:520` |
| Guarda por `bloqueo_id` | `planificador.c:530`; incrementos en `:581` y `:626` |
| Transición atómica entre las dos colas | `planificador.c:525-549` |
| Pedido de swap a KM | `planificador.c:555-567` |
| Excepción de los bloqueados por memoria | `planificador.c:596` |

---

> «En caso de que se libere memoria, se agregue un memory stick nuevo, o se compacte la misma, se recorrerán todos los Procesos suspendidos por orden de prioridad y se irán des-suspendiendo si cuentan con espacio disponible para re-crear todos sus segmentos sin disparar una compactación. En caso de contar con más de un Proceso suspendido con la misma prioridad, se evaluará primero el que lleve mayor tiempo suspendido.»

**Cómo se resuelve.** Los tres disparadores del enunciado (y algunos más) convergen en **una sola función**: `planificador_evento_memoria_liberada()`. Es el patrón central del mediano plazo — en vez de replicar la lógica en cada sitio que libera memoria, todos llaman al mismo punto de entrada.

Los sitios que la invocan:

| Evento del enunciado | Dónde se dispara |
|---|---|
| "se libere memoria" — `MEM_FREE` | `conexiones.c:355` |
| "se libere memoria" — fin de proceso | `conexiones.c:154` |
| "se libere memoria" — suspensión de otro proceso | `planificador.c:570` |
| "se agregue un memory stick nuevo" | `conexiones.c:476` (case `MENSAJE_MAS_MEMORIA`) |
| "se compacte la misma" | `conexiones.c:332` |
| (extra) fin de IO de un proceso suspendido | `planificador.c:1277` |
| (extra) mutex liberado hacia un suspendido | `planificador.c:1471` |

La función arranca tomando `mutex_evento_memoria`, que **serializa** todo el recorrido: si dos hilos liberan memoria a la vez, el segundo espera y vuelve a evaluar sobre el estado ya actualizado por el primero. Sin eso, dos hilos podrían decidir en paralelo des-suspender dos procesos que individualmente entran pero juntos no.

El *"por orden de prioridad... y a igual prioridad, el que lleve mayor tiempo suspendido"* se implementa copiando la cola a un array de `t_candidato_dessusp` (pid, prioridad, timestamp de suspensión) y ordenándolo con `qsort` y `comparar_candidatos()`: primero compara prioridad (menor número gana), y ante empate compara `ts_suspension` (más viejo gana). El timestamp se sella con `clock_gettime(CLOCK_MONOTONIC)` en el momento exacto de la suspensión (`planificador.c:544`).

El *"si cuentan con espacio para re-crear **todos** sus segmentos **sin disparar una compactación**"* no se evalúa en el KS — se delega a KM, que es quien conoce el estado real de la memoria. `dessuspender_en_km()` manda `MENSAJE_DESSUSPENDER_PROCESO` y KM responde OK o ERROR. Del lado de KM, `gestor_memoria_dessuspender_proceso()` implementa el "todos o ninguno": reserva huecos para todos los segmentos con el algoritmo configurado y, si alguno no entra, **revierte las reservas ya hechas** y devuelve `false` sin haber tocado nada. Nunca compacta.

Sólo si KM confirma, el KS mueve el PCB de SUSP. READY a READY.

La segunda mitad de la función reintenta los `MEM_ALLOC` que habían quedado en `lista_espera_memoria`, en orden de llegada; los que siguen sin entrar quedan esperando el próximo evento.

| Función | Ubicación |
|---|---|
| `planificador_evento_memoria_liberada()` | `planificador.c:733` |
| Serialización con `mutex_evento_memoria` | `planificador.c:736` |
| `comparar_candidatos()` (prioridad, luego antigüedad) | `planificador.c:709` |
| `qsort` de candidatos | `planificador.c:751` |
| Sellado de `ts_suspension` | `planificador.c:544` |
| `dessuspender_en_km()` | `planificador.c:719` |
| Lógica "todos o ninguno" en KM | `gestor_memoria_dessuspender_proceso()` `gestor_memoria.c:777-806` |
| Transición SUSP. READY → READY | `planificador.c:756-777` |
| Reintento de `MEM_ALLOC` pendientes | `planificador.c:783-803` |

---

## KS-4. Planificación de Corto Plazo

> «Vamos a estar utilizando uno de los tres posibles algoritmos de planificación definidos, los cuales van a ser FIFO, RR y Colas Multinivel (no retroalimentadas). El algoritmo a utilizar será elegible por archivo de configuración y no cambiará a lo largo de una prueba.»

**Cómo se resuelve.** La abstracción elegida unifica los tres casos: READY **siempre** es un arreglo de listas (`colas_ready`), sólo cambia cuántas hay.

- Con `FIFO` o `RR`: `cant_colas = 1`, y `alg_por_cola[0]` apunta al string de `PLANIFICATION_ALGORITHM`.
- Con `CMN`: `cant_colas` = cantidad de entradas de `QUEUES_ALGORITHMS`, y cada `alg_por_cola[i]` apunta al algoritmo de esa cola.

Eso se decide una sola vez en `planificador_create()` y no cambia más — coherente con el *"no cambiará a lo largo de una prueba"*. El resto del código no pregunta nunca "¿qué algoritmo global es?", sino "¿qué cola le toca a este proceso?" (`cola_de()`) y "¿esta cola usa RR?" (`cola_usa_rr()`).

`desencolar_ready()` recorre las colas de índice 0 hacia arriba y saca el primer elemento de la primera cola no vacía. Con una sola cola eso es FIFO puro; con N colas es prioridad estricta entre niveles y FIFO dentro de cada nivel.

| Función | Ubicación |
|---|---|
| Armado de las colas según config | `planificador_create()` `planificador.c:118-140` |
| `es_cmn()` | `planificador.c:12` |
| `cola_de()` | `planificador.c:18` |
| `cola_usa_rr()` | `planificador.c:26` |
| `desencolar_ready()` | `planificador.c:97` |
| `hilo_planificador_corto_plazo()` | `planificador.c:382` |

---

> «Las prioridades comenzarán en 0, y no se deberán planificar Procesos que tengan un nivel de prioridad fuera del rango definido dentro del archivo de configuración. Por ejemplo, si se definen 4 colas en la configuración, solamente se planificarán prioridades de la 0 a la 3.»

**Cómo se resuelve.** Se valida en el punto de creación, que es la syscall `INIT_PROC`. Si el algoritmo es CMN y la prioridad recibida cae fuera de `[0, cant_colas)`, **no se crea el PCB**: se loguea un error y el proceso simplemente no existe. El proceso llamante sigue su curso normal (vuelve a READY).

La validación se saltea si el algoritmo no es CMN, porque el enunciado aclara que *"en los algoritmos de FIFO y RR la prioridad no tendrá injerencia alguna"*.

| Función | Ubicación |
|---|---|
| Validación de rango | `conexiones.c:381-384` |
| Rechazo y log | `conexiones.c:396-399` |
| Clamp defensivo en `cola_de()` | `planificador.c:20-22` |

---

> «El algoritmo (FIFO o RR) a utilizar dentro de cada cola estará especificado dentro del archivo de configuración.»

**Cómo se resuelve.** El quantum no se decide globalmente sino **por proceso, según la cola a la que pertenece**. Al final de `despachar()`:

```c
if (cola_usa_rr(pl, cola_de(pl, pcb))) { /* lanzar timer */ }
```

Si la cola del proceso es FIFO, no se lanza timer y el proceso corre hasta que se bloquee, termine o lo desalojen por prioridad. Si es RR, se lanza `hilo_timer_quantum()`.

El timer tiene el mismo problema que el de suspensión: puede quedar obsoleto. Acá el escenario concreto es el **retorno directo de syscalls** — un proceso hace `MUTEX_UNLOCK`, vuelve a la misma CPU, y ahora hay dos timers vivos para el mismo par (PID, CPU). La solución es el contador `rafaga` de `t_cpu_conectada`, que se incrementa en cada despacho; el timer captura su valor y sólo interrumpe si al despertar la CPU sigue en esa misma ráfaga. Además chequea que la CPU siga ocupada con ese PID, que esté `en_rafaga` (ejecutando instrucciones, no estacionada en una syscall) y que no tenga ya otra interrupción pendiente.

El `send` de la interrupción se hace **dentro** de `mutex_cpus` para que no se intercale con otro hilo escribiendo al mismo socket.

| Función | Ubicación |
|---|---|
| Decisión de lanzar timer | `planificador.c:364-374` |
| `hilo_timer_quantum()` | `planificador.c:298` |
| Guardas de validez del timer | `planificador.c:313-315` |
| Envío de la interrupción bajo `mutex_cpus` | `planificador.c:322-323` |
| Campo `rafaga` | `planificador.h:46`, incremento en `planificador.c:350` |
| Recepción del desalojo por quantum | `conexiones.c:430-434` |

---

> «...al momento de llegar un Proceso, y si el desalojo entre colas está habilitado, se deberá validar si se encuentra ejecutando un Proceso con menor prioridad. En caso de que así sea, se deberá desalojar al Proceso menos prioritario para darle lugar al más prioritario que acaba de llegar.»

**Cómo se resuelve.** *"Al momento de llegar un Proceso"* se implementa enganchando el chequeo al final de `encolar_en_ready()`, que es el embudo por el que pasa **toda** llegada a READY, venga de donde venga (NEW, fin de IO, desalojo, des-suspensión).

`chequear_desalojo_por_prioridad()` sale de inmediato si no es CMN o si `QUEUE_PREEMPTION` no es `TRUE`. Después recorre `lista_exec` buscando la **víctima**: el proceso en ejecución con la peor prioridad (número más alto), y estrictamente peor que el que acaba de llegar. Si hay varias CPUs ejecutando, se elige la peor de todas — no la primera que aparezca.

Encontrada la víctima, se busca su CPU y se le manda `MENSAJE_INTERRUPCION` marcando `motivo_interrupcion = INT_PREEMPCION`. El log obligatorio se emite en este momento, no cuando la CPU devuelve el proceso, porque acá es donde se tienen a mano los cuatro datos (PID desalojado, su prioridad, PID preemptor, su prioridad).

Cuando la CPU devuelve el proceso, `atender_peticiones_cpu()` lee el motivo guardado y lo reencola al **final** de su cola (a diferencia del desalojo por compactación, que va al frente).

Un detalle: si `al_frente` es `true` (o sea, estamos en pleno desalojo por compactación), el chequeo **no** se ejecuta. No tiene sentido desalojar procesos mientras se está desalojando a todos.

| Función | Ubicación |
|---|---|
| `chequear_desalojo_por_prioridad()` | `planificador.c:32` |
| Enganche en `encolar_en_ready()` | `planificador.c:90-93` |
| Filtro CMN + `QUEUE_PREEMPTION` | `planificador.c:33` |
| Búsqueda de la peor víctima | `planificador.c:41-50` |
| Log obligatorio + interrupción | `planificador.c:63-68` |
| Retorno del proceso desalojado | `conexiones.c:424-429` |

---

## KS-5. Atención de Syscalls

> «Las Syscalls relacionadas a la memoria tendrán la particularidad de que una vez finalizadas deberán volver a enviar el mismo proceso a la CPU que realizó la llamada.»
>
> «Las operaciones de creación y liberación de mutex, deberán volver a enviar el mismo proceso a la CPU que realizó la llamada.»

**Cómo se resuelve.** Este requisito es el que motiva `planificador_despachar_directo()`. El proceso **no pasa por READY**: no se lo saca de `lista_exec`, no se encola, no compite con nadie. Se lo reenvía directo a la misma `t_cpu_conectada` desde la que llegó la syscall.

La función es un wrapper de `despachar()`, lo cual es importante porque hereda tres comportamientos: (a) actualiza el contexto en KM antes de mandar, (b) respeta la pausa por compactación esperando en `cond_compactacion`, y (c) relanza el timer de quantum con una `rafaga` nueva si la cola es RR.

Se usa en las 5 syscalls que lo requieren:

| Syscall | Dónde retorna directo |
|---|---|
| `MUTEX_CREATE` | `conexiones.c:263` |
| `MUTEX_LOCK` (sólo si lo adquirió) | `conexiones.c:284` |
| `MUTEX_UNLOCK` | `conexiones.c:309` |
| `MEM_ALLOC` (sólo si hubo espacio) | `conexiones.c:328` |
| `MEM_FREE` | `conexiones.c:353` y `:358` |

Notar que la CPU **nunca se libera** en estos caminos: no se llama a `liberar_cpu()`, porque el proceso vuelve a ella inmediatamente. En cambio, en las ramas que sí bloquean (`MUTEX_LOCK` que no pudo tomar, `MEM_ALLOC` sin espacio) sí se libera.

| Función | Ubicación |
|---|---|
| `planificador_despachar_directo()` | `planificador.c:377` |
| `despachar()` (implementación real) | `planificador.c:338` |

---

> «Al momento de atender la creación de segmentos puede ser que se cuente con el espacio suficiente para crear el segmento pero el mismo no se encuentre contiguo, esto deberá disparar una Compactación.»

Ver **KS-6**, que lo desarrolla completo.

---

## KS-6. Desalojo por compactación

> «Al recibir este pedido, el Kernel Scheduler enviará la solicitud de desalojo a todas las CPU y no enviará ningún otro Proceso a ejecutar hasta que reciba la confirmación por parte del Kernel Memory que finalizó la compactación. Los Procesos desalojados, excepcionalmente, serán colocados al principio de la cola de READY.»

**Cómo se resuelve.** Es el protocolo más largo del TP y va todo sobre la conexión principal KS↔KM, de forma sincrónica. Lo dispara siempre un `MEM_ALLOC`.

**Paso 1 — KM decide que hace falta compactar.** `gestor_memoria_crear_segmento()` intenta `buscar_hueco()`. Si falla, compara el espacio libre total contra el tamaño pedido: si el total alcanza pero no hubo hueco contiguo, devuelve `CREAR_SEGMENTO_NECESITA_COMPACTACION`. Si ni el total alcanza, devuelve `CREAR_SEGMENTO_SIN_ESPACIO` — que es un caso distinto y **no** compacta.

**Paso 2 — KM pide el desalojo.** En vez de responder OK/ERROR, `atender_scheduler()` responde `MENSAJE_COMPACTACION` y se queda bloqueado esperando `MENSAJE_DESALOJO_OK`.

**Paso 3 — KS congela la planificación.** `planificador_crear_segmento_km()` ve la respuesta `MENSAJE_COMPACTACION` y llama a `pausar_planificacion()`. El flag hace que cualquier hilo dentro de `despachar()` quede esperando en `pthread_cond_wait(&cond_compactacion)`. Esto cubre el *"no enviará ningún otro Proceso a ejecutar"*.

**Paso 4 — KS desaloja todas las CPUs.** `interrumpir_cpus_para_compactar()` recorre las CPUs, manda `MENSAJE_INTERRUPCION` a las que estén `en_rafaga` y devuelve cuántas siguen ejecutando. El KS la llama en loop con `usleep(5ms)` hasta que devuelva 0.

Dos detalles finos acá: (a) la función es **idempotente** — no reinterrumpe una CPU que ya tiene `INT_COMPACTACION` marcado, así que llamarla en loop es seguro; (b) sólo espera a las CPUs `en_rafaga`, es decir, ejecutando instrucciones. Una CPU cuyo proceso está "estacionado" en una syscall no está tocando memoria y no hace falta esperarla.

**Paso 5 — las CPUs devuelven sus procesos.** Cada CPU detecta la interrupción en `cpu_check_interrupt()` y manda el contexto de vuelta. `atender_peticiones_cpu()` lee `motivo_int == INT_COMPACTACION` y llama a `planificador_transicionar_exec_a_ready_frente()` — que es el *"excepcionalmente, al principio de la cola"*: internamente hace `list_add_in_index(cola, 0, pcb)` en vez de `list_add`.

**Paso 6 — KS confirma.** Manda `MENSAJE_DESALOJO_OK` y se queda esperando el resultado final.

**Paso 7 — KM compacta y reintenta.** `gestor_memoria_compactar()` amontona todo al principio, y después KM **vuelve a intentar** la creación del segmento, ahora sí con memoria contigua. Recién ahí responde OK o ERROR.

**Paso 8 — KS reanuda.** `reanudar_planificacion()` baja el flag y hace `pthread_cond_broadcast()`, despertando a todos los hilos que estaban esperando para despachar. Eso cubre el *"una vez finalizada la misma se procederá a replanificar todos los Procesos de acuerdo al algoritmo definido"*: los procesos están al frente de sus colas y el corto plazo los vuelve a repartir normalmente.

Como paso extra (no exigido explícitamente pero coherente con el mediano plazo), tras una compactación exitosa se llama a `planificador_evento_memoria_liberada()`, porque el reordenamiento pudo habilitar des-suspensiones.

**Nota sobre el lock:** todo el protocolo transcurre con `mutex_socket_memoria` tomado, desde el `send` inicial hasta la respuesta final. Es intencional — la conexión es sincrónica y no puede haber otra operación intercalada — pero implica que cualquier otro hilo que quiera hablar con KM queda bloqueado durante toda la compactación.

| Paso | Función | Ubicación |
|---|---|---|
| 1 | `gestor_memoria_crear_segmento()` → `NECESITA_COMPACTACION` | `gestor_memoria.c:282-292` |
| 2 | KM envía `MENSAJE_COMPACTACION` | `kernel_memory_atencion_scheduler.c:63-68` |
| 3 | `pausar_planificacion()` | `planificador.c:955`, llamada en `:1011` |
| 3 | Barrera en el despacho | `planificador.c:341-344` |
| 4 | `interrumpir_cpus_para_compactar()` | `planificador.c:970`, loop en `:1016-1018` |
| 5 | `cpu_check_interrupt()` + `enviar_contexto_a_ks()` | `instrucciones.c:337`, `cpu/src/conexiones/conexiones.c:97` |
| 5 | Reencolado al frente | `conexiones.c:419-423` → `planificador.c:499` → `:82` |
| 6 | `MENSAJE_DESALOJO_OK` | `planificador.c:1021-1022` |
| 7 | `gestor_memoria_compactar()` + reintento | `kernel_memory_atencion_scheduler.c:77-78` |
| 8 | `reanudar_planificacion()` | `planificador.c:961`, llamada en `:1029` |
| extra | Evento de memoria post-compactación | `conexiones.c:329-333` |

---

## KS-7. Mutex

> «Al momento de liberar un mutex, en caso de que se cuente con procesos esperando por dicho mutex, los mismos se irán liberando en el orden en el que solicitaron el mutex.»

**Cómo se resuelve.** Cada `t_mutex_kernel` tiene una `t_queue* cola_bloqueados` con los PIDs (no los PCBs) de quienes esperan. `planificador_lock_mutex()`, cuando el mutex está tomado, hace `queue_push` del PID; `planificador_unlock_mutex()` hace `queue_pop`. Cola FIFO = orden de solicitud, que es exactamente lo que pide la aclaración de la v1.1 del enunciado.

Se guardan PIDs y no punteros a PCB a propósito: el PCB puede haber migrado entre colas (BLOCK → SUSP. BLOCK) mientras esperaba, así que se lo vuelve a buscar por PID al despertarlo.

El `unlock` transfiere la propiedad **directamente** al siguiente de la cola: el mutex nunca queda libre en el medio, pasa de un dueño al siguiente. Si no hay nadie esperando, queda `tomado = false` y `pid_duenio = 0`.

`planificador_desbloquear()` maneja el caso de que el que esperaba se haya suspendido: si estaba en BLOCK va a READY, si estaba en SUSP. BLOCK va a SUSP. READY (con el mutex ya tomado), y en ese caso se dispara el evento de memoria por si ya puede des-suspenderse.

| Función | Ubicación |
|---|---|
| `planificador_crear_mutex()` | `planificador.c:1373` |
| `planificador_lock_mutex()` | `planificador.c:1384` |
| Encolado FIFO del que espera | `planificador.c:1402-1406` |
| `planificador_unlock_mutex()` | `planificador.c:1423` |
| Desencolado y transferencia directa | `planificador.c:1447-1454` |
| Despertar al siguiente | `planificador.c:1458-1472` |
| Liberación forzada al finalizar un proceso | `conexiones.c:144-149` |

---

> «Ante este evento llamado Inversión de Prioridades... este último heredará temporalmente la prioridad del Proceso con mayor prioridad que se encuentre bloqueado esperando el Mutex. Una vez que el Proceso de baja prioridad libere el Mutex, volverá a tener su prioridad original.»

**Cómo se resuelve.** El PCB tiene dos campos separados: `prioridad_original` (la que se le asignó al crearlo, inmutable) y `prioridad_actual` (la efectiva, que puede estar heredada). Todo el planificador usa `prioridad_actual` — `cola_de()`, `chequear_desalojo_por_prioridad()`, `comparar_candidatos()` — así que la herencia tiene efecto real e inmediato sobre la planificación, no es cosmética.

La herencia se aplica en `planificador_lock_mutex()`, **después** de soltar `mutex_lista_mutex` (para no anidar locks). Si el que se bloquea es más prioritario que el dueño (`pcb->prioridad_actual < duenio->prioridad_actual`, recordando que menor número = mayor prioridad), se llama a `cambiar_prioridad()`.

`cambiar_prioridad()` hace algo más que asignar un `int`: si el proceso está en READY y el algoritmo es CMN, tiene que **moverlo físicamente de cola**, porque su cola actual ya no le corresponde. Lo saca de donde esté y lo agrega al final de la cola nueva. Y emite el log obligatorio de cambio de prioridad.

La restauración está en `planificador_unlock_mutex()`: si `prioridad_actual != prioridad_original`, se vuelve a la original.

Para encontrar al dueño, `buscar_pcb_por_pid()` lo busca en EXEC, luego en todas las colas de READY, luego en BLOCK — porque el dueño del mutex puede estar en cualquiera de esos tres estados.

| Función | Ubicación |
|---|---|
| Campos `prioridad_original` / `prioridad_actual` | `pcb.h:22-23` |
| Detección de la inversión | `planificador.c:1411-1418` |
| `buscar_pcb_por_pid()` | `planificador.c:1310` |
| `cambiar_prioridad()` | `planificador.c:1348` |
| Reubicación de cola en CMN | `planificador.c:1352-1368` |
| Restauración al liberar | `planificador.c:1435-1437` |

> ⚠ **Limitación conocida.** La restauración es incondicional: si el proceso tiene
> **dos** mutex tomados y libera sólo uno, vuelve igual a su prioridad original aunque
> siga habiendo un proceso prioritario bloqueado esperando el otro. Un manejo estricto
> requeriría recalcular la prioridad heredada a partir de todos los mutex que aún tiene
> (`pcb->lista_mutex_tomados`). Vale la pena tenerlo a mano para el coloquio.

---

## KS-8. IO

> «En todos los casos, al momento de que llegue la petición de la CPU, el proceso pasará del estado EXEC al estado BLOCK y el Kernel Scheduler deberá enviar un nuevo Proceso a ejecutar si lo hubiera.»

**Cómo se resuelve.** Las tres syscalls de IO siguen el mismo patrón en `atender_peticiones_cpu()`: guardar los registros recibidos en el PCB, `planificador_transicionar_exec_a_block()`, `planificador_ejecutar_io_async()`, y `liberar_cpu()`.

El *"enviar un nuevo Proceso a ejecutar si lo hubiera"* es consecuencia automática de `liberar_cpu()`, que hace `sem_post(&sem_cpus_libres)`. El hilo de corto plazo está bloqueado en `sem_wait` sobre ese semáforo; despierta y despacha al siguiente de READY si hay alguno.

La IO se ejecuta en un **hilo aparte** (`hilo_ejecutar_io()`) y no en el hilo de atención de la CPU. Eso es necesario porque la IO puede tardar (un SLEEP de 25 segundos, un STDIN esperando que el usuario tipee) y el hilo de la CPU tiene que quedar libre para recibir la próxima syscall del próximo proceso que le despachen.

`planificador_tomar_io_libre()` bloquea con `sem_wait` sobre el semáforo del tipo pedido hasta que haya un dispositivo de ese tipo disponible. Hay un semáforo por tipo (`sem_io_stdin_libres`, `sem_io_stdout_libres`, `sem_io_sleep_libres`), así que un STDOUT ocupado no frena a un SLEEP.

| Función | Ubicación |
|---|---|
| Atención de `SLEEP` | `conexiones.c:218-231` |
| Atención de `STDIN` / `STDOUT` | `conexiones.c:232-248` |
| `planificador_transicionar_exec_a_block()` | `planificador.c:574` |
| `liberar_cpu()` + `sem_post` | `conexiones.c:128-135` |
| `planificador_ejecutar_io_async()` | `planificador.c:1284` |
| `hilo_ejecutar_io()` | `planificador.c:1226` |
| `planificador_tomar_io_libre()` | `planificador.c:1175` |
| `sem_de_tipo_io()` | `planificador.c:1120` |

---

> «Para las IO de tipo STDIN, se recibirán por parte de la CPU un tamaño total a leer y la dirección física donde se deberá guardar el contenido leído. Se deberá solicitar a la IO de tipo STDIN que le solicite al usuario ingresar el total a leer y luego, cuando este retorne los caracteres leídos, se deberán enviar al Módulo Kernel Memory para que este las escriba en donde corresponda.»

**Cómo se resuelve.** El flujo es: KS pide bytes a la IO → IO devuelve los bytes → KS se los manda a KM para que los escriba.

```c
ok = io_enviar_pid_y_param(io->fd, pcb->pid, a->tamanio)
     && recv(io->fd, buffer, a->tamanio, MSG_WAITALL) > 0;
if (ok) ok = planificador_escribir_memoria_en_km(pl, pcb->pid, a->param, a->tamanio, buffer);
```

**Desvío deliberado sobre el enunciado:** la consigna dice "dirección física", pero acá se maneja **dirección lógica**. El enunciado lo habilita explícitamente en la sección de Kernel Memory: *«Está permitido también que el parámetro sea una dirección lógica y que la traducción la realice el Kernel Memory»*.

La razón es de correctitud, no de comodidad. Entre que la CPU emite la syscall y que la IO devuelve los bytes pueden pasar segundos, y en el medio puede haber ocurrido una **compactación** (que movió el segmento a otra base) o una **suspensión** (que lo mandó a SWAP). Una dirección física calculada al inicio quedaría apuntando a cualquier lado. Mandando la lógica, KM la traduce recién en el momento de escribir, contra el estado actual — y si el segmento está en SWAP, `gestor_memoria_io_rw()` opera directamente sobre los bloques.

| Función | Ubicación |
|---|---|
| Rama STDIN de `hilo_ejecutar_io()` | `planificador.c:1246-1255` |
| `planificador_escribir_memoria_en_km()` | `planificador.c:932` |
| Recepción en KM | `kernel_memory_atencion_scheduler.c:139` |
| Traducción tardía + fallback a SWAP | `gestor_memoria_io_rw()` `gestor_memoria.c:895` |
| Lado IO | `io/src/main.c:80-115` |

---

> «En el caso de las IO de tipo STDOUT, el Kernel Scheduler recibirá de parte de la CPU la dirección física y el tamaño a leer. Una vez que este le retorne todos los bytes, deberá pedirle esta información al Kernel Memory para luego, enviársela a la IO de tipo STDOUT.»

**Cómo se resuelve.** Espejo del anterior, en orden inverso: KS pide el contenido a KM → lo recibe → lo manda a la IO → espera el OK.

Mismo criterio de dirección lógica, misma justificación.

| Función | Ubicación |
|---|---|
| Rama STDOUT de `hilo_ejecutar_io()` | `planificador.c:1235-1244` |
| `planificador_leer_memoria_en_km()` | `planificador.c:904` |
| Recepción en KM | `kernel_memory_atencion_scheduler.c:110` |
| Lado IO | `io/src/main.c:117-157` |

---

# MÓDULO: KERNEL MEMORY

## KM-1. Lineamiento e implementación

> «Al iniciar creará un servidor multihilo que se encargará de gestionar de manera concurrente las peticiones del Kernel Scheduler y las CPUs, así como también esperará las conexiones de los diferentes Memory Sticks y del SWAP.»

**Cómo se resuelve.** Mismo patrón que el KS: `accept` en loop, un `pthread` desprendido por conexión, dispatcher por handshake. La diferencia es que KM atiende **cinco** tipos de cliente distintos, cada uno con su tratamiento del fd:

| Handshake | Qué hace el hilo | Destino del fd |
|---|---|---|
| `HANDSHAKE_CPU` | `atender_cpu()` — loop permanente | el hilo lo tiene hasta que la CPU corta |
| `HANDSHAKE_SCHEDULER` | `atender_scheduler()` — loop permanente | idem |
| `HANDSHAKE_SCHEDULER_NOTIF` | registra el fd y **retorna sin cerrarlo** | queda guardado en el gestor de memoria para push |
| `HANDSHAKE_MEMORY_STICK` | recibe el tamaño, registra, y queda de centinela en `recv` | detección de caída |
| `HANDSHAKE_SWAP` | recibe tamaños, registra, y **retorna sin cerrar** | queda en el gestor, se usa bajo `mutex_swap` |

Los dos casos que retornan `NULL` antes del `close(fd_conexion)` final son deliberados: esos sockets no se leen más desde el hilo de conexión, pero tienen que seguir vivos porque otros hilos escriben en ellos bajo demanda.

| Función | Ubicación |
|---|---|
| `iniciar_servidor_kernel_memory()` | `kernel_memory/src/conexiones/conexiones.c:20` |
| `manejar_cliente()` — dispatcher | `conexiones.c:89` |
| `atender_cpu()` | `kernel_memory_atencion_cpu.c:10` |
| `atender_scheduler()` | `kernel_memory_atencion_scheduler.c:9` |
| Registro del canal de notificaciones | `conexiones.c:109-116` |
| Registro del SWAP | `conexiones.c:120-136` |

---

## KM-2. Contexto de Ejecución

> «Por cada PID del sistema se deberá almacenar un contexto de ejecución, el cual consiste en una copia de todos los registros de la CPU y la tabla de segmentos asociada a dicho Proceso. Este módulo deberá mantener el contexto de ejecución de cada Proceso del sistema. Deberá ser capaz de enviar y/o recibir el contexto de ejecución hacia/desde el módulo CPU cuando este se lo solicite.»

**Cómo se resuelve.** `t_contexto_ejecucion` contiene exactamente las dos cosas que pide el enunciado: un `t_registros_cpu` (struct compartido en `utils`, así que CPU y KM lo serializan como bloque binario sin conversión) y una `t_list*` de `t_segmento*`.

Hay tres operaciones sobre el contexto:

- **Envío a la CPU** (`MENSAJE_CONTEXTO`): manda los registros, luego un `uint32` con la cantidad de segmentos, luego cada `t_segmento`. Todo el envío se hace **con `gestor_lock()` tomado**, no sólo la lectura: otro hilo (por ejemplo, `atender_scheduler` procesando un `MEM_ALLOC`) podría estar agregando entradas a la tabla mientras se arma la respuesta, y se mandaría un contexto inconsistente.
- **Recepción desde el KS** (`MENSAJE_ACTUALIZAR_CONTEXTO`): el KS es quien persiste los registros, no la CPU. Ocurre en cada despacho, dentro de `despachar()`.
- **Actualización de la tabla de segmentos**: la hacen las operaciones de memoria (`tabla_proceso_agregar()`, la eliminación, la compactación).

Si el PID no existe, se manda un contexto vacío (registros en 0, cero segmentos) en vez de cortar la conexión — evita desincronizar el protocolo.

| Función | Ubicación |
|---|---|
| `t_contexto_ejecucion` | `kernel_memory/src/contexto/contexto.h` |
| `t_registros_cpu` (compartido) | `utils/src/utils/registros.h:10-25` |
| Envío del contexto | `kernel_memory_atencion_cpu.c:31-61` |
| Envío bajo lock | `kernel_memory_atencion_cpu.c:46-57` |
| Recepción desde el KS | `kernel_memory_atencion_scheduler.c:188-204` |
| Emisor del lado KS | `planificador_actualizar_contexto_en_km()` `planificador.c:888` |
| Recepción del lado CPU | `solicitar_contexto_a_km()` `cpu/src/conexiones/conexiones.c:158` |

---

## KM-3. Archivos de pseudocódigo

> «Los archivos de pseudocódigo serán archivos de texto, los cuales contendrán las instrucciones que ejecutan las CPUs, estando estas separadas por el caracter '\n'. Por cada PID del sistema se tendrá un archivo de pseudocódigo... El Kernel Memory deberá ser capaz de enviar la instrucción correspondiente a cada pedido de la CPU sin haber una restricción específica de cómo se deben obtener las mismas.»

**Cómo se resuelve.** La asociación PID ↔ archivo se guarda en `t_proceso_km->path_instrucciones`, armado al crear el proceso concatenando `SCRIPTS_BASEPATH` con el path relativo que mandó el KS (eso resuelve el *"relativo al path base configurado"* de la consigna de Creación de Proceso).

El enunciado deja libre la implementación, y acá se eligió la más simple: **no se cachea nada**. `gestor_obtener_instruccion()` abre el archivo con `fopen`, lo recorre con `fgets` contando líneas hasta llegar a la número `pc`, le saca el `\n`, hace `strdup` y cierra. Una lectura de disco por cada fetch.

Es O(n) por instrucción y notoriamente ineficiente, pero es correcto y el enunciado no exige otra cosa. Si en el coloquio preguntan por optimización, la respuesta natural es cachear el archivo parseado como array de strings en el `t_proceso_km` la primera vez que se pide.

Antes de responder se aplica `INSTRUCTION_DELAY` con `usleep`, y se emite el log obligatorio de obtención de instrucción.

| Función | Ubicación |
|---|---|
| Armado del path con `SCRIPTS_BASEPATH` | `kernel_memory_atencion_scheduler.c:35-37` |
| `gestor_agregar_proceso()` | `kernel_memory_gestor_procesos.c:29` |
| `gestor_obtener_instruccion()` | `kernel_memory_gestor_procesos.c:84` |
| Recorrido por líneas | `kernel_memory_gestor_procesos.c:101-108` |
| `INSTRUCTION_DELAY` | `kernel_memory_atencion_cpu.c:68` |

---

## KM-4. Esquema de memoria y estructuras

> «La memoria de usuario de nuestro Kernel Memory utilizará el esquema de segmentación pura, por lo que se deberá almacenar para cada PID del sistema una estructura que permita conocer todos los segmentos correspondientes a dicho PID con su base y su límite.»

**Cómo se resuelve.** Hay **dos representaciones en paralelo** de la misma información, y entender por qué es clave:

1. **`gm->segmentos`** — lista global de `t_segmento_fisico` (pid, id_segmento, base, tamanio). Es la vista del *administrador de memoria*: ordenada por base permite derivar los huecos, buscar dónde entra algo nuevo y compactar.
2. **`proc->contexto->tabla_segmentos`** — lista de `t_segmento` (id_segmento, base, límite, tamanio) por proceso. Es la vista del *proceso*, y es la que se le manda a la CPU para que su MMU traduzca.

Toda operación que modifica memoria tiene que actualizar **las dos**, y por eso las funciones vienen de a pares: `gestor_memoria_crear_segmento()` agrega a `gm->segmentos` y llama a `tabla_proceso_agregar()`; la eliminación borra de las dos; la compactación actualiza bases en las dos.

El campo `limite` se calcula como `base + tamanio - 1` y existe sólo por fidelidad conceptual con la teoría de segmentación — la MMU de la CPU en realidad valida contra `tamanio`.

| Función | Ubicación |
|---|---|
| `t_segmento_fisico` | `gestor_memoria.h:32-37` |
| `t_segmento` (entrada de tabla) | `utils/src/utils/segmento.h:6-11` |
| `tabla_proceso_agregar()` | `gestor_memoria.c:253` |
| Cálculo de `limite` | `gestor_memoria.c:261` |

---

> «Para el almacenamiento de los datos se utilizarán los módulos Memory Stick, los cuales al momento de conectarse deberán informarle su tamaño al módulo Kernel Memory. Una vez recibido el tamaño, se deberá añadir al final de la lista de Memory Sticks conectados, ampliando el tamaño máximo de la memoria total. Al momento de que se conecte un nuevo Memory Stick y se amplíe el tamaño total de la memoria, se deberá notificar al Kernel Scheduler que se dispone de más memoria.»

**Cómo se resuelve.** Los sticks forman un **espacio de direcciones global contiguo**. Cada `t_stick` tiene un `base` que es la suma de los tamaños de todos los sticks anteriores, y su rango es `[base, base + tamanio)`. Un stick nuevo se agrega al final: `base = memoria_total` actual, y después `memoria_total += tamanio`. Todo bajo `gm->mutex`.

Eso hace que las direcciones físicas sean globales y no relativas a cada stick — coherente con el cambio de la v1.1 del enunciado (*"Se elimina la restricción de que las direcciones físicas sean relativas a cada stick"*). La conversión global→local se hace recién al operar: `dir_local = dir - stick->base`.

Antes de publicar el stick, KM **abre una conexión de datos hacia él** (`crear_conexion` + `HANDSHAKE_KERNEL_MEMORY`). Ese canal es el que usa KM para leer/escribir durante compactación y suspensión. Por eso el Memory Stick tiene que hacer `bind`+`listen` **antes** de conectarse a KM (ver `memory_stick/src/main.c:65-67`): si no estuviera escuchando, este `crear_conexion` fallaría.

La notificación de más memoria sale por el canal `_NOTIF` con el nuevo total como payload. Del lado del KS, dispara `planificador_evento_memoria_liberada()` — más memoria puede habilitar des-suspensiones o desbloquear `MEM_ALLOC` en espera.

| Función | Ubicación |
|---|---|
| `gestor_memoria_registrar_stick()` | `gestor_memoria.c:93` |
| Apertura del canal de datos KM→stick | `gestor_memoria.c:95-101` |
| Cálculo de `base` y ampliación del total | `gestor_memoria.c:110-116` |
| Notificación `MAS_MEMORIA` | `gestor_memoria.c:123` |
| Recepción en el KS | `kernel_scheduler/src/conexiones/conexiones.c:471-477` |
| Orden listen-antes-de-conectar | `memory_stick/src/main.c:65-76` |

---

## KM-5. Desconexión de un Memory Stick

> «Existe la posibilidad de que en medio de una ejecución un Memory Stick se desconecte. Dado el caso, se deberá notificar al Kernel Scheduler que la memoria se encuentra corrupta.»

Desarrollado completo en **KS-2 (BSOD)**, parte 2.

Resumen: el hilo que atendió el alta del stick queda bloqueado en un `recv` centinela sobre la conexión de control; cuando devuelve 0 (EOF), llama a `gestor_memoria_stick_desconectado()`, que marca el stick como caído, cierra su canal de datos y empuja `MENSAJE_MEMORIA_CORRUPTA` por el canal de notificaciones.

| Función | Ubicación |
|---|---|
| `recv` centinela | `kernel_memory/src/conexiones/conexiones.c:85` |
| `gestor_memoria_stick_desconectado()` | `gestor_memoria.c:127` |

---

## KM-6. Creación de Proceso

> «Para esta operación se va a recibir un PID y el path de un archivo de instrucciones, relativo al path base configurado por archivo de configuración. Con los datos recibidos, el Módulo Kernel Memory deberá crear el contexto de ejecución del Proceso, inicializando todos los registros asociados con el valor 0.»

**Cómo se resuelve.** El KS manda `MENSAJE_ENVIAR_PID` + pid + longitud del path + path. KM concatena con `SCRIPTS_BASEPATH` en un buffer de 512 bytes usando `snprintf` (que trunca en vez de desbordar), y llama a `gestor_agregar_proceso()`.

Ahí se arma el contexto: `memset(&contexto->registros, 0, sizeof(t_registros_cpu))` pone **todos** los registros en 0 de un saque (incluido el PC, que es lo que hace que el proceso arranque por la instrucción 0), y `list_create()` deja la tabla de segmentos vacía — coherente con *"los Procesos inician sin memoria asignada"* del largo plazo.

| Función | Ubicación |
|---|---|
| Recepción del mensaje | `kernel_memory_atencion_scheduler.c:24-45` |
| Concatenación del path | `kernel_memory_atencion_scheduler.c:35-37` |
| `gestor_agregar_proceso()` | `kernel_memory_gestor_procesos.c:29` |
| Registros en 0 | `kernel_memory_gestor_procesos.c:32` |
| Tabla de segmentos vacía | `kernel_memory_gestor_procesos.c:33` |

---

## KM-7. Creación de Segmento y algoritmos de huecos

> «Al momento de crear un segmento el Kernel Memory recibirá como parámetro el PID del Proceso, un ID de segmento y el tamaño que debe tener el nuevo segmento.»
>
> «Los Algoritmos a implementar son Best Fit y Worst Fit, y la elección del mismo se hará por medio del archivo de configuración del Kernel Memory.»

**Cómo se resuelve.** `gestor_memoria_crear_segmento()` valida primero contra `SEGMENT_MAX_SIZE` (y rechaza tamaño 0), después busca hueco, y si encuentra da de alta en las dos estructuras.

Lo interesante es cómo se representan los huecos: **no se guardan**. No hay lista de huecos libres que haya que mantener sincronizada. Se **derivan** en cada búsqueda a partir de los segmentos ocupados: `segmentos_ordenados()` devuelve un array ordenado por base, y `buscar_hueco()` lo recorre mirando los espacios que quedan entre el fin de un segmento y el comienzo del siguiente, más el espacio final hasta `memoria_total`. El loop va de `0` a `cantidad_segmentos` **inclusive** (`i <= cantidad_segmentos`) justamente para incluir ese último hueco.

Esto es O(n log n) por búsqueda pero elimina toda una clase de bugs de sincronización entre dos estructuras.

Best Fit vs Worst Fit se resuelve con dos funciones chicas y bien separadas:

- `estrategia_es_best_fit()` lee `ALLOCATION_STRATEGY`. **Best Fit es el default**: sólo se usa Worst Fit si el valor es exactamente `"WORST"`.
- `hueco_es_mejor_que_el_actual()` es el único lugar donde difieren los dos algoritmos: si no hay candidato previo, cualquiera que entre sirve; con Best Fit gana el más chico; con Worst Fit, el más grande.

El resto del recorrido es idéntico para ambos. Es la separación correcta: la política está aislada del mecanismo.

Cuando no se encuentra hueco, se distingue entre dos situaciones **muy** distintas comparando el espacio libre total contra el tamaño pedido:
- libre total ≥ tamaño → `CREAR_SEGMENTO_NECESITA_COMPACTACION` (hay lugar, pero fragmentado)
- libre total < tamaño → `CREAR_SEGMENTO_SIN_ESPACIO` (no hay lugar, compactar no serviría)

Esa distinción es la que dispara (o no) todo el protocolo de compactación.

| Función | Ubicación |
|---|---|
| `gestor_memoria_crear_segmento()` | `gestor_memoria.c:272` |
| Validación de `SEGMENT_MAX_SIZE` | `gestor_memoria.c:273-277` |
| `buscar_hueco()` | `gestor_memoria.c:191` |
| `segmentos_ordenados()` + `comparar_por_base()` | `gestor_memoria.c:158` / `:150` |
| Recorrido de huecos (incluye el final) | `gestor_memoria.c:207-228` |
| `estrategia_es_best_fit()` | `gestor_memoria.c:172` |
| `hueco_es_mejor_que_el_actual()` | `gestor_memoria.c:180` |
| Distinción compactar vs sin espacio | `gestor_memoria.c:282-292` |
| `espacio_libre_sin_lock()` | `gestor_memoria.c:236` |

---

## KM-8. Compactación

> «Una vez recibida la confirmación por parte del Kernel de que se desalojaron las CPUs comenzará el proceso de compactación, en el cual se tendrán que reordenar todos los segmentos de memoria al principio de la misma. Esto puede resultar en que algunos segmentos se muevan parcial o totalmente de Memory Stick, debiendo actualizar las tablas de segmentos de todos los Procesos.»

**Cómo se resuelve.** El algoritmo es directo: ordenar todos los segmentos por base y amontonarlos al principio sin dejar huecos. Se lleva un cursor `proxima_base_libre` que arranca en 0; para cada segmento, si su base actual no coincide con el cursor, está desplazado y hay que moverlo; después el cursor avanza su tamaño.

`mover_segmento()` hace el traslado en tres pasos:
1. Lee el segmento completo a un buffer local en RAM
2. Lo reescribe en la nueva base
3. Actualiza la tabla de segmentos del proceso dueño y el registro físico

El *"parcial o totalmente de Memory Stick"* de la consigna es lo que resuelve `copiar_segmento_entre_sticks()`: en vez de asumir que un segmento vive en un solo stick, avanza de a "tramos", tomando en cada vuelta sólo lo que entra en el stick actual (`min(bytes_restantes, bytes_hasta_fin_del_stick)`). Un segmento que arranca en el stick 0 y termina en el stick 1 se copia en dos operaciones, transparentemente. Lo mismo al escribirlo en el destino, que puede tener un reparto distinto.

El *"debiendo actualizar las tablas de segmentos de todos los Procesos"* lo cubre `actualizar_base_de_segmento_en_proceso()`, que busca la entrada por id y le recalcula `base` y `limite`. Se llama por cada segmento movido, así que al final todas las tablas quedan consistentes. Esto es indispensable: la próxima vez que la CPU pida el contexto de cualquiera de esos procesos, va a recibir las bases nuevas.

Toda la operación corre con `gm->mutex` tomado. Es seguro porque las CPUs ya están desalojadas y no hay nadie leyendo memoria.

Al final se aplica `COMPACTION_DELAY` con `usleep`, y se emiten los logs de inicio y fin.

| Función | Ubicación |
|---|---|
| `gestor_memoria_compactar()` | `gestor_memoria.c:566` |
| Amontonado con cursor | `gestor_memoria.c:577-587` |
| `mover_segmento()` | `gestor_memoria.c:551` |
| `copiar_segmento_entre_sticks()` | `gestor_memoria.c:500` |
| Avance por tramos | `gestor_memoria.c:506-523` |
| `actualizar_base_de_segmento_en_proceso()` | `gestor_memoria.c:528` |
| `COMPACTION_DELAY` | `gestor_memoria.c:593` |

---

## KM-9. Eliminación de Segmento

> «Al momento de eliminar un segmento se recibirá como parámetro el PID y el ID del segmento a eliminar y se procederá simplemente a eliminar dicha entrada en la tabla de segmentos del proceso. Por último, se marcará ese espacio como libre en la memoria.»

**Cómo se resuelve.** Las dos acciones de la consigna se corresponden con las dos estructuras (ver KM-4), en orden inverso al que las nombra el enunciado:

1. *"marcar el espacio como libre"* → sacar el `t_segmento_fisico` de `gm->segmentos`. Como los huecos se derivan de esa lista, remover el nodo **es** marcar el espacio como libre; no hay ninguna estructura de huecos que tocar.
2. *"eliminar la entrada en la tabla de segmentos del proceso"* → sacar el `t_segmento` de `proc->contexto->tabla_segmentos`.

Si el segmento no existe en la lista global, se loguea un warning y se devuelve `false` sin tocar nada, lo que el KS convierte en `MENSAJE_ERROR`.

| Función | Ubicación |
|---|---|
| `gestor_memoria_eliminar_segmento()` | `gestor_memoria.c:310` |
| Liberación del espacio físico | `gestor_memoria.c:312-322` |
| Baja de la tabla del proceso | `gestor_memoria.c:330-342` |
| Handler del mensaje | `kernel_memory_atencion_scheduler.c:87-97` |

---

## KM-10. Suspensión de Proceso

> «Al momento de suspender un Proceso se deberán mover todos sus segmentos de los Memory Sticks a bloques dentro del Módulo SWAP. Para ello se irán moviendo de a 1 segmento y se liberará la memoria una vez que el mismo se encuentra copiado.»
>
> «Es responsabilidad de cada grupo definir las estructuras necesarias para poder saber en qué bloques se encuentra cada segmento. Los bloques de SWAP se asignan por segmento, por lo que no puede existir un bloque que contenga información de 2 segmentos distintos.»

**Cómo se resuelve.** Tres decisiones de diseño:

**(a) Estructuras.** `t_segmento_swap` guarda (pid, id_segmento, tamanio, `t_list* bloques`). La lista de bloques es propia de cada segmento, y el bitmap `swap_bloques_usados` marca la ocupación global. Como cada segmento tiene su lista y los bloques se marcan usados al asignarse, **es estructuralmente imposible** que un bloque quede compartido entre dos segmentos. El requisito no se chequea, se hace inviolable por construcción.

**(b) Pre-chequeo antes de mover nada.** Antes de tocar un solo byte se calcula cuántos bloques hacen falta en total —`ceil(tamanio / block_size)` por segmento, con la fórmula entera `(tamanio + block_size - 1) / block_size`— y se compara contra los libres. Si no alcanzan, se aborta devolviendo `false` sin haber movido nada. Evita el peor caso: quedarse sin bloques a mitad de camino, con la mitad de los segmentos en SWAP y la otra mitad en memoria.

**(c) "De a 1 segmento, liberando después de copiar"** se implementa literal, en este orden por cada segmento:
1. Leer el segmento completo de memoria física a un buffer (`gestor_memoria_leer_fisico()`)
2. Partirlo en bloques y escribirlos en SWAP, marcando cada uno como usado y registrándolo en la lista
3. Recién ahora: sacar el `t_segmento_fisico` de `gm->segmentos` (liberar la memoria)
4. Sacar la entrada de la tabla de segmentos del proceso

Si el último bloque queda incompleto se rellena con `memset(bloque_buffer + tramo, 0, block_size - tramo)`, porque el módulo SWAP siempre opera con bloques completos.

Nótese que el proceso queda **sin tabla de segmentos** tras la suspensión. Es correcto: sus datos no están en memoria, y si alguien intenta traducir una dirección lógica suya, `gestor_memoria_traducir()` va a fallar y el camino de IO va a caer al fallback de SWAP.

| Función | Ubicación |
|---|---|
| `gestor_memoria_suspender_proceso()` | `gestor_memoria.c:651` |
| `t_segmento_swap` | `gestor_memoria.h:43-48` |
| Snapshot de los segmentos | `gestor_memoria.c:658-670` |
| Pre-chequeo de bloques | `gestor_memoria.c:673-685` |
| Copia y asignación de bloques | `gestor_memoria.c:704-719` |
| Relleno del último bloque | `gestor_memoria.c:709` |
| Liberación de memoria post-copia | `gestor_memoria.c:727-736` |
| Baja de la tabla del proceso | `gestor_memoria.c:738-750` |

---

## KM-11. Des-suspensión de Proceso

> «Al momento de des-suspender un Proceso se deberán restaurar todos los segmentos que se encuentran almacenados en SWAP a la memoria principal. Para ello se deberá regenerar nuevamente la tabla de segmentos que se encuentra en el contexto de ejecución del Proceso. Al momento de des-suspender los procesos, se deberá utilizar el algoritmo de búsqueda de huecos.»

**Cómo se resuelve.** El requisito implícito más fuerte viene del mediano plazo: hay que des-suspender *"si cuentan con espacio disponible para re-crear **todos** sus segmentos **sin disparar una compactación**"*. O sea: es todo o nada, y sin compactar.

La implementación usa un patrón de **reserva con rollback** en dos fases:

**Fase 1 — reservar.** Con `gm->mutex` tomado, se recorren los segmentos swapeados llamando a `buscar_hueco()` para cada uno (que es el *"se deberá utilizar el algoritmo de búsqueda de huecos"* de la consigna: el mismo BEST/WORST que en la creación). Cada hueco encontrado se materializa insertando un `t_segmento_fisico` en `gm->segmentos` y anotándolo en una lista `reservados`. Insertarlo de verdad es importante: hace que la siguiente iteración de `buscar_hueco()` ya no considere ese espacio como libre.

Si alguno no entra, se corta el loop y **se revierte**: se quitan de `gm->segmentos` y se liberan todos los reservados, se suelta el mutex y se devuelve `false` sin haber modificado nada ni haber tocado el SWAP. En ningún momento se llama a compactar.

**Fase 2 — restaurar.** Sólo si entraron todos. Por cada segmento: leer sus bloques de SWAP a un buffer, marcarlos libres en el bitmap, sacar el `t_segmento_swap` de la lista, escribir el buffer en la nueva base con `gestor_memoria_escribir_fisico()`, y regenerar la entrada en la tabla del proceso con `tabla_proceso_agregar()`.

El *"regenerar nuevamente la tabla de segmentos"* es literal: se reconstruye desde cero, con bases nuevas que probablemente no son las que tenía antes de suspenderse.

| Función | Ubicación |
|---|---|
| `gestor_memoria_dessuspender_proceso()` | `gestor_memoria.c:760` |
| Snapshot de segmentos swapeados | `gestor_memoria.c:762-768` |
| Fase 1 — reserva con `buscar_hueco()` | `gestor_memoria.c:777-794` |
| Rollback si no entra todo | `gestor_memoria.c:796-805` |
| Fase 2 — restauración de datos | `gestor_memoria.c:810-837` |
| Liberación de bloques en el bitmap | `gestor_memoria.c:826` |
| Regeneración de la tabla | `gestor_memoria.c:835` |

---

## KM-12. Finalización de Proceso

> «Esta operación se utiliza para finalizar un Proceso, por lo que solamente va a recibir un PID y a partir de este PID recibido se deberán liberar todos los segmentos asociados al Proceso y todas las estructuras asociadas al mismo.»

**Cómo se resuelve.** Un proceso puede tener memoria en **tres** lugares distintos, y hay que barrer los tres:

1. **Segmentos en memoria principal** — se recorre `gm->segmentos` **de atrás hacia adelante** (`for (i = size-1; i >= 0; i--)`) porque se remueven elementos durante el recorrido; iterando hacia adelante se saltearían elementos al correrse los índices.
2. **Bloques de SWAP** (si estaba suspendido) — mismo recorrido inverso sobre `gm->segmentos_swap`, marcando cada bloque como libre en el bitmap antes de destruir la estructura.
3. **Contexto y estructuras administrativas** — `gestor_remover_proceso()` saca el `t_proceso_km` de la lista y llama a `destruir_proceso_km()`, que libera el path, la tabla de segmentos con sus entradas, el contexto y el proceso.

Del lado del KS, `finalizar_proceso()` hace algo más que avisar a KM: primero transiciona a EXIT, después **libera todos los mutex que el proceso tuviera tomados** (recorriendo `lista_mutex_tomados` y llamando a `unlock` por cada uno, lo que despierta a los que esperaban), y al final dispara el evento de memoria liberada.

| Función | Ubicación |
|---|---|
| `gestor_memoria_finalizar_proceso()` | `gestor_memoria.c:348` |
| Liberación de segmentos en memoria | `gestor_memoria.c:350-358` |
| Liberación de bloques de SWAP | `gestor_memoria.c:361-373` |
| `gestor_remover_proceso()` | `kernel_memory_gestor_procesos.c:44` |
| `destruir_proceso_km()` | `kernel_memory_gestor_procesos.c:6` |
| `finalizar_proceso()` en el KS | `kernel_scheduler/src/conexiones/conexiones.c:139` |
| Liberación de mutex tomados | `conexiones.c:144-149` |

---

## KM-13. Lectura y Escritura de datos

> «Esta operación va estar asociada a un pedido directo del Kernel Scheduler, que va a oficiar de intermediario entre la IO de tipo STDOUT y el Kernel Memory... Va a recibir como parámetros una dirección física y el tamaño a leer. Está permitido también que el parámetro sea una dirección lógica y que la traducción la realice el Kernel Memory...»

**Cómo se resuelve.** Se tomó explícitamente la variante permitida: **dirección lógica, traducción en KM**. La justificación está en KS-8 (el proceso pudo compactarse o suspenderse mientras esperaba la IO).

Ambos caminos (lectura y escritura) convergen en `gestor_memoria_io_rw()`, que implementa un fallback en dos niveles:

1. Intenta `gestor_memoria_traducir()`. Si el segmento está en memoria principal, obtiene la física y delega en `gestor_memoria_leer_fisico()` / `gestor_memoria_escribir_fisico()`.
2. Si la traducción falla, el segmento **puede estar en SWAP** (el proceso se suspendió durante la IO). Recalcula número de segmento y desplazamiento y llama a `swap_io_rw()`, que opera directamente sobre los bloques.

`swap_io_rw()` tiene un detalle: el módulo SWAP sólo maneja bloques completos, así que para una escritura parcial hace **read-modify-write** — lee el bloque, le aplica el `memcpy` en el offset correcto, y lo reescribe entero.

El caso de swap se señala devolviendo `UINT32_MAX` en `dir_fisica_out`, y el llamador usa eso para omitir el log obligatorio de dirección física (no existe tal dirección: el dato está en disco).

El *"que se encuentra almacenada en uno o varios Memory Sticks"* lo resuelve `operar_fisico()`, que divide el pedido entre los sticks involucrados avanzando por tramos y lo consolida como una operación única.

| Función | Ubicación |
|---|---|
| `gestor_memoria_io_rw()` | `gestor_memoria.c:895` |
| `gestor_memoria_traducir()` | `gestor_memoria.c:462` |
| `swap_io_rw()` (fallback) | `gestor_memoria.c:852` |
| Read-modify-write de bloque | `gestor_memoria.c:877-880` |
| `operar_fisico()` — división entre sticks | `gestor_memoria.c:419` |
| `stick_operar()` | `gestor_memoria.c:396` |
| Handlers de los mensajes | `kernel_memory_atencion_scheduler.c:110` y `:139` |

---

# MÓDULO: CPU

## CPU-1. Lineamiento e implementación

> «Al momento de ejecutar un CPU se debe indicar por argumento cuál es su "identificador" para poder identificarla de los demás. Para cada CPU se deberá tener un archivo de log y archivo de configuración independientes...»
>
> «Las CPUs deberán conectarse al Kernel Scheduler, al Kernel Memory y a los diferentes Memory Sticks. Estos últimos se podrán ir conectando a lo largo de la ejecución, por lo que es importante que el grupo defina una estrategia entre el Kernel Memory y las CPUs para que éstas conozcan todos los Memory Sticks que existen cuando sea necesario.»

**Cómo se resuelve.** El identificador llega por `argv[2]` y viaja en el handshake tanto al KS como a KM (y también a cada Memory Stick, que loguea `## CPU <ID> Conectada`).

El orden de conexión es **KM primero, después KS**, y no es arbitrario: al conectarse a KM, la CPU recibe `SEGMENT_MAX_SIZE`, que es indispensable para que su MMU pueda hacer la división `dir_logica / segment_max_size`. Conectarse al KS antes significaría poder recibir un PID para ejecutar sin saber todavía cómo traducir direcciones.

**La estrategia de descubrimiento de Memory Sticks** que pide la consigna es un *pull* con `MENSAJE_TOPOLOGIA_STICKS`: la CPU pregunta y KM le serializa la lista de sticks conectados (id, tamaño, base global, ip, puerto). La CPU se conecta directamente a los que no conocía y guarda el fd. Se invoca en tres momentos:

1. Al arrancar, antes de atender nada (`cpu/src/main.c:62`)
2. **En cada despacho de PID** (`cpu.c:75`) — que es el punto principal: entre dos ejecuciones pudo conectarse un stick nuevo
3. Como reintento, si una dirección física no cae en ningún stick conocido (`conexiones.c:285-289`) — con guarda `reintentado` para no entrar en loop infinito

Elegir *pull* en vez de *push* evita que KM tenga que mantener una lista de CPUs a las que notificar y manejar sus desconexiones.

| Función | Ubicación |
|---|---|
| Lectura del identificador | `cpu/src/main.c:25` |
| `conectar_a_kernel_memory()` + recepción de `SEGMENT_MAX_SIZE` | `cpu/src/conexiones/conexiones.c:30`, recepción en `:44` |
| Envío de `SEGMENT_MAX_SIZE` desde KM | `kernel_memory_atencion_cpu.c:21-22` |
| `conectar_a_kernel_scheduler()` | `cpu/src/conexiones/conexiones.c:12` |
| `actualizar_topologia_sticks()` | `cpu/src/conexiones/conexiones.c:190` |
| Serialización del lado KM | `gestor_memoria_serializar_topologia()` `gestor_memoria.c:961` |

---

> «Una vez establecidas todas las conexiones, la CPU se quedará a la espera de recibir un PID de parte del Kernel Scheduler. Una vez recibido, la CPU deberá solicitarle al Kernel Memory el contexto de ejecución y comenzar el primer ciclo de instrucción.»

**Cómo se resuelve.** `atender_peticiones_kernel_scheduler()` es un `while(1)` con `recv` bloqueante sobre el socket del KS. Al recibir `MENSAJE_ENVIAR_PID`: guarda el PID, refresca la topología, pide el contexto a KM con `solicitar_contexto_a_km()`, y entra al `while (!termino)` que es el ciclo de instrucción.

`solicitar_contexto_a_km()` recibe los registros (sobreescribiendo los locales) y **reconstruye la tabla de segmentos desde cero**: `list_clean_and_destroy_elements()` y después un `recv` por cada `t_segmento`. Reconstruirla en vez de actualizarla es lo correcto, porque entre dos ejecuciones pudo haber compactación (bases distintas), `MEM_ALLOC`/`MEM_FREE` (más o menos segmentos) o suspensión/des-suspensión (todas las bases distintas).

Cuando el ciclo termina, se resetea `ejecutando` y `pid_actual` y se vuelve a esperar otro PID.

| Función | Ubicación |
|---|---|
| `atender_peticiones_kernel_scheduler()` | `cpu/src/cpu/cpu.c:50` |
| Recepción del PID | `cpu/src/cpu/cpu.c:64-71` |
| `solicitar_contexto_a_km()` | `cpu/src/conexiones/conexiones.c:158` |
| Reconstrucción de la tabla | `cpu/src/conexiones/conexiones.c:168-173` |
| Loop del ciclo de instrucción | `cpu/src/cpu/cpu.c:80-131` |

---

## CPU-2. Ciclo de Instrucción

> «Fetch: La primera etapa del ciclo consiste en buscar la próxima instrucción a ejecutar. En este trabajo práctico cada instrucción deberá ser pedida al módulo Kernel Memory utilizando el Program Counter...»

**Cómo se resuelve.** `cpu_fetch()` emite el log obligatorio, manda `MENSAJE_PEDIR_INSTRUCCION` + pid + pc, recibe la longitud y después el string. Si la longitud es 0 devuelve `NULL`, que el llamador interpreta como fin de programa y corta el ciclo.

| Función | Ubicación |
|---|---|
| `cpu_fetch()` | `cpu/src/instrucciones/instrucciones.c:60` |
| Log obligatorio | `instrucciones.c:61` |
| Handler en KM | `kernel_memory_atencion_cpu.c:63` |

---

> «Decode: Esta etapa consiste en interpretar qué instrucción es la que se va a ejecutar y si la misma requiere de una traducción de dirección lógica a dirección física.»

**Cómo se resuelve.** `cpu_decode()` hace `string_split(instruccion, " ")`, mapea el token 0 contra la cadena de `strcmp` a un `t_codigo_instruccion`, y copia los tokens restantes al array `parametros[3][64]` con `strncpy` acotado.

Un detalle sutil documentado en el código: el loop de parámetros **corta apenas encuentra el `NULL`** del array devuelto por `string_split`, en vez de iterar siempre hasta `INSTRUCCION_MAX_PARAMS`. Iterar de más leería memoria fuera del arreglo en instrucciones con menos de 3 parámetros.

Si el opcode no matchea con ninguno, devuelve -1 y el ciclo corta.

| Función | Ubicación |
|---|---|
| `cpu_decode()` | `cpu/src/instrucciones/instrucciones.c:81` |
| Tabla de opcodes | `instrucciones.c:95-116` |
| Parseo acotado de parámetros | `instrucciones.c:125-134` |

---

> «Execute: En este paso se deberá ejecutar lo correspondiente a cada instrucción.»

**Cómo se resuelve.** `cpu_execute()` es un `switch` sobre el código, y usa el **valor de retorno como señal de control**: `0` = seguir el ciclo, `1` = terminar la ráfaga (syscall o SEG_FAULT). Ese es el mecanismo que implementa el *"las syscalls deberán liberar la CPU"*.

Detalle de las instrucciones de cómputo:

- **`SET`** usa `set_registro_32()`, que resuelve cualquier registro incluyendo `PC` (por eso `SET PC 5` funciona como salto) y trunca a `uint8_t` para AX..DX.
- **`SUM`/`SUB`** operan con `get_registro`/`set_registro` de 8 bits.
- **`JNZ`** lee el registro con `get_registro_32()`, y si es distinto de cero asigna el PC y hace `return 0` — el `return` temprano es relevante porque el incremento de PC lo maneja el llamador.
- **`MOV_IN`/`MOV_OUT`** determinan cuántos bytes leer/escribir con `tamanio_registro()` (1 para AX..DX, 4 para el resto), traducen con la MMU y operan contra los sticks.
- **`COPY_MEM`** hace **dos** traducciones (SI como origen, DI como destino) y valida ambas antes de mover nada; el tamaño sale del registro pasado como parámetro.

| Instrucción | Ubicación |
|---|---|
| `cpu_execute()` | `instrucciones.c:139` |
| `NOOP` | `:145` |
| `SET` | `:149` |
| `SUM` / `SUB` | `:156` / `:163` |
| `JNZ` | `:170` |
| `MOV_IN` | `:179` |
| `MOV_OUT` | `:201` |
| `COPY_MEM` | `:221` |

---

> «Las syscalls, al depender del Kernel Scheduler, deberán liberar la CPU. Una vez incrementado el Program Counter (PC) en 1, deberá actualizar el contexto de ejecución en el Kernel Memory y devolverle el PID al KS para que este realice las tareas necesarias.»

**Cómo se resuelve.** Todas las syscalls pasan por `enviar_syscall_a_ks()`, que hace exactamente lo que dice la consigna en ese orden:

```c
cpu->registros.pc++;                        // "una vez incrementado el PC en 1"
send(socket_kernel, MENSAJE_SYSCALL);       // "devolverle el PID al KS"
send(socket_kernel, syscall_tipo);
```

Después cada `enviar_syscall_*()` específica manda sus parámetros propios y **el struct de registros completo**.

Acá hay una **variante de diseño respecto de la letra del enunciado**: la consigna dice que la CPU debe *"actualizar el contexto de ejecución en el Kernel Memory"*, pero en esta implementación la CPU manda los registros **al KS**, y es el KS quien los persiste en KM (dentro de `despachar()`, vía `planificador_actualizar_contexto_en_km()`).

El efecto final es el mismo —el contexto queda actualizado en KM antes de que el proceso vuelva a ejecutar— y tiene una ventaja: el KS necesita los registros de todos modos para guardarlos en el PCB, y de esta forma se evita una ida y vuelta extra CPU↔KM. Es un punto que conviene tener preparado para el coloquio, porque se aparta de la redacción literal.

`EXIT` y `SEG_FAULT` no mandan registros (el proceso termina, no hay contexto que preservar).

| Función | Ubicación |
|---|---|
| `enviar_syscall_a_ks()` | `cpu/src/conexiones/conexiones.c:50` |
| Incremento del PC | `conexiones.c:54` |
| Envío de registros por syscall | `conexiones.c:68, 86, 94, 112, 121, 130, 138, 145, 155` |
| Persistencia en KM desde el KS | `planificador_actualizar_contexto_en_km()` `planificador.c:888` |

---

> «Es importante tener en cuenta que, al finalizar el ciclo de instrucción, el Program Counter (PC) deberá ser actualizado sumándole 1, siempre y cuando este no haya sido modificado por la instrucción ejecutada anteriormente.»

**Cómo se resuelve.** El incremento está en el llamador, no en `cpu_execute()`, y se saltea en dos casos detectados explícitamente:

```c
bool modifico_pc =
    (inst.codigo == INST_JNZ && get_registro_32(cpu, inst.parametros[0]) != 0)
 || (inst.codigo == INST_SET && strcmp(inst.parametros[0], "PC") == 0);
if (!modifico_pc) cpu->registros.pc++;
```

- `JNZ` **sólo si efectivamente saltó** (registro ≠ 0). Un `JNZ` con registro en cero no modificó el PC y sí debe incrementarse.
- `SET` cuando el destino es `PC`, que es el caso `SET PC 5` del ejemplo del enunciado.

Un punto importante que está comentado en el código: el incremento ocurre **antes** del `cpu_check_interrupt()`, incluso si hay una interrupción pendiente. La instrucción ya se ejecutó; si no se incrementara antes de mandar el contexto, al resumir el proceso volvería a ejecutar la misma instrucción, en loop infinito.

| Función | Ubicación |
|---|---|
| Lógica del incremento | `cpu/src/cpu/cpu.c:118-123` |
| Orden respecto de check interrupt | `cpu/src/cpu/cpu.c:125-128` |

---

> «Check Interrupt: se deberá chequear si el Kernel Scheduler nos envió una interrupción al PID que se está ejecutando. En caso afirmativo, se actualiza el contexto de ejecución en el Kernel Memory y se le devuelve el PID al Kernel Scheduler con el motivo de la interrupción. Caso contrario, se descarta la interrupción.»

**Cómo se resuelve.** `cpu_check_interrupt()` hace un `recv` **no bloqueante** con `MSG_DONTWAIT` sobre el socket del KS. Si no hay nada, retorna inmediatamente `false` y el ciclo sigue. Si llegó `MENSAJE_INTERRUPCION`, emite el log obligatorio, manda el contexto y devuelve `true`, cortando la ráfaga.

`enviar_contexto_a_ks()` manda **primero el código de mensaje y después los registros**. Eso está comentado en el código y es crítico: `atender_peticiones_cpu()` del lado del KS lee siempre un `t_codigo_mensaje` primero; mandar sólo los registros desincronizaría el socket permanentemente.

**El "motivo de la interrupción" no viaja en el mensaje.** Lo sabe el KS, porque él mismo lo guardó en `cpu->motivo_interrupcion` cuando envió la interrupción (`INT_QUANTUM`, `INT_PREEMPCION` o `INT_COMPACTACION`). Al recibir el contexto lo lee y actúa distinto según el caso — compactación va al frente de la cola, los otros al final. Es una decisión razonable: la CPU no tiene por qué saber por qué la interrumpieron.

| Función | Ubicación |
|---|---|
| `cpu_check_interrupt()` | `cpu/src/instrucciones/instrucciones.c:337` |
| `recv` con `MSG_DONTWAIT` | `instrucciones.c:339` |
| `enviar_contexto_a_ks()` | `cpu/src/conexiones/conexiones.c:97` |
| Enum `t_motivo_interrupcion` | `planificador.h:23-28` |
| Uso del motivo en el KS | `conexiones.c:418-435` |

---

## CPU-3. MMU

> «num_segmento = floor(dir_logica / tam_max_segmento) — desplazamiento_segmento = dir_logica % tam_max_segmento»
>
> «En algunas traducciones es posible que una petición de lectura o escritura abarque más de 1 Memory Stick. Será responsabilidad del grupo dividir estas peticiones de acuerdo a los diferentes Memory Sticks involucrados. Luego se deberá consolidar el resultado de la operación para que la misma se considere como una única operación.»
>
> «En caso de que el desplazamiento dentro del segmento sumado al tamaño a leer / escribir sea mayor al tamaño del segmento, deberá devolverse el Proceso al Kernel Scheduler para que este lo finalice con motivo de Error: Segmentation Fault (SEG_FAULT).»

**Cómo se resuelve.** `mmu_traducir()` implementa las tres cosas:

**La aritmética** es literal la del enunciado. La división entera de C ya es el `floor` para valores no negativos, así que no hace falta ninguna función especial. `segment_max_size` es el que KM le mandó en el handshake, así que CPU y KM usan exactamente el mismo divisor — indispensable para que sus traducciones coincidan.

**Las dos condiciones de SEG_FAULT** están separadas y logueadas distinto:
- el segmento no existe en la tabla del proceso → el proceso accedió a un segmento que no tiene asignado
- `desplazamiento + tamanio > seg->tamanio` → el acceso se pasa del límite, que es textual la consigna

Ambas devuelven `UINT32_MAX` como valor centinela, que el llamador interpreta llamando a `enviar_syscall_seg_fault()` y retornando 1 (fin de ráfaga). El KS lo recibe como `SYSCALL_SEG_FAULT` y finaliza el proceso con motivo `"SEG_FAULT"`.

**La división entre sticks** la hace `operar_fisico()`, no la MMU: recibe una dirección física global y un tamaño, y avanza por tramos. En cada vuelta busca el stick que contiene la dirección actual, calcula cuánto entra (`min(restante, disponible_en_stick)`), opera ese tramo y avanza los tres cursores (dirección, posición en buffer, restante). Desde afuera es una sola llamada a `leer_memoria_fisica()` / `escribir_memoria_fisica()` — el *"consolidar el resultado para que se considere una única operación"* de la consigna.

Si una dirección no cae en ningún stick conocido, refresca la topología una vez y reintenta (con guarda `reintentado` para evitar el loop).

**Nota sobre STDIN/STDOUT:** la MMU se invoca igual, pero **sólo para validar**. El resultado de la traducción se descarta y al KS se le manda la dirección lógica. Así se detecta el SEG_FAULT en el momento correcto (durante la ejecución de la instrucción, como cualquier otro acceso inválido) pero la traducción real la hace KM más tarde, contra el estado actualizado de la memoria.

| Función | Ubicación |
|---|---|
| `mmu_traducir()` | `cpu/src/instrucciones/instrucciones.c:23` |
| Aritmética de segmentación | `instrucciones.c:25-26` |
| SEG_FAULT por segmento inexistente | `instrucciones.c:40-44` |
| SEG_FAULT por exceso de límite | `instrucciones.c:47-51` |
| `operar_fisico()` — división y consolidación | `cpu/src/conexiones/conexiones.c:277` |
| `stick_para_direccion()` | `cpu/src/conexiones/conexiones.c:246` |
| Reintento con topología refrescada | `cpu/src/conexiones/conexiones.c:285-290` |
| Validación-sin-usar en STDIN/STDOUT | `instrucciones.c:278-281` y `:290-293` |
| `enviar_syscall_seg_fault()` | `cpu/src/conexiones/conexiones.c:75` |
| Finalización en el KS | `conexiones.c:212-217` |

---

## CPU-4. Registros

> Tabla del enunciado: PC (4 bytes), AX/BX/CX/DX (1 byte), EAX/EBX/ECX/EDX (4 bytes), SI/DI (4 bytes).

**Cómo se resuelve.** `t_registros_cpu` está en `utils` (no en `cpu`) porque es un contrato compartido: KM lo guarda en el contexto y el KS en el PCB, y los tres módulos lo mandan por socket como bloque binario. Los tipos respetan exactamente la tabla: `uint32_t` para PC, E**, SI y DI; `uint8_t` para AX..DX.

El acceso por nombre se resuelve con cuatro funciones:
- `get_registro()` / `set_registro()` — 8 bits, sólo AX..DX
- `get_registro_32()` / `set_registro_32()` — 32 bits, y además **promueven** AX..DX a `uint32_t` (o truncan al asignar). Eso permite que `SET AX 6` y `JNZ AX 4` funcionen sin casos especiales en `cpu_execute()`.
- `tamanio_registro()` — devuelve 1 para AX..DX y 4 para el resto; es lo que usan `MOV_IN`/`MOV_OUT` para saber cuántos bytes mover.

| Función | Ubicación |
|---|---|
| `t_registros_cpu` | `utils/src/utils/registros.h:10-25` |
| `get_registro()` / `set_registro()` | `registros_cpu.c:6` / `:14` |
| `get_registro_32()` / `set_registro_32()` | `registros_cpu.c:21` / `:44` |
| `tamanio_registro()` | `registros_cpu.c:36` |

---

# MÓDULO: MEMORY STICK

> «Una vez leído el tamaño de memoria que representa este Proceso, reservará con malloc() dicha cantidad de memoria y deberá conectarse al Kernel Memory. En caso de que la conexión con Kernel Memory sea exitosa, deberá quedarse a la espera de las conexiones de las diferentes CPUs...»

**Cómo se resuelve.** El tamaño llega por `argv[2]`. `memoria_stick_create()` usa `calloc(1, tamanio)` en vez de `malloc` — misma reserva pero inicializada en cero, lo que hace determinista el contenido de memoria nunca escrita.

El orden en `main()` es: reservar memoria → **`bind`+`listen`** → conectar a KM. El listen tiene que ir antes de la conexión porque, apenas KM recibe el `INFORMAR_TAMANIO`, abre inmediatamente su canal de datos *hacia* el stick (`gestor_memoria.c:95`). Si el stick no estuviera escuchando todavía, esa conexión fallaría y el registro se abortaría.

Al conectarse informa tamaño **más su ip y puerto de escucha**, que es lo que KM necesita para poder pasarle la topología a las CPUs.

| Función | Ubicación |
|---|---|
| `memoria_stick_create()` con `calloc` | `memory_stick/src/memoria/memoria.c:5-18` |
| Orden listen-antes-de-conectar | `memory_stick/src/main.c:65-76` |
| `conectar_a_kernel_memory()` | `memory_stick/src/conexiones/conexiones.c:155` |
| Envío de tamaño + ip:puerto | `conexiones.c:168-178` |

---

> «El módulo Memory Stick, solamente podrá responder a 2 operaciones que consisten en la escritura o lectura de los datos.»

**Cómo se resuelve.** `atender_peticiones()` es un loop con un `switch` de exactamente dos casos. Lo relevante es que **CPUs y Kernel Memory usan el mismo loop**: `manejar_cliente()` ramifica por handshake sólo para loguear distinto, y después ambos caen en la misma función. Es correcto porque ambos hablan el mismo protocolo de datos contra el stick.

`memoria_stick_leer()` y `memoria_stick_escribir()` son `memcpy` sobre el bloque, protegidos por un mutex propio de la memoria (varias CPUs pueden operar en paralelo desde hilos distintos).

La validación de rango merece atención: `rango_valido()` **evita deliberadamente** escribir `dir + tamanio <= memoria->tamanio`, porque ambos son `uint32_t` y esa suma puede desbordar, dar la vuelta a un número chico y **pasar el chequeo siendo inválida**. En su lugar hace dos comparaciones que no pueden desbordar: que `dir` no se pase del final, y que `tamanio` entre en lo que queda desde `dir`. Está documentado en el código.

`MEMORY_DELAY` se aplica antes de responder: en lectura antes de leer, en escritura después de recibir el contenido.

| Función | Ubicación |
|---|---|
| `atender_peticiones()` | `memory_stick/src/conexiones/conexiones.c:53` |
| `memoria_stick_leer()` | `memoria.c:44` |
| `memoria_stick_escribir()` | `memoria.c:53` |
| `rango_valido()` (anti-overflow) | `memoria.c:36-42` |
| `MEMORY_DELAY` | `conexiones.c:65` y `:94` |

---

# MÓDULO: IO

> «Los módulos IO contarán con 1 solo hilo de ejecución el cual esperará los pedidos de parte del Kernel Scheduler y contestará o realizará la operación descrita a continuación.»

**Cómo se resuelve.** Literal: no hay un solo `pthread_create` en todo el módulo. `main()` conecta al KS y entra en un `switch` por tipo de dispositivo; cada rama es un `while (recv(...) > 0)` que procesa un pedido por vez, secuencialmente. La serialización de pedidos la garantiza el KS del otro lado con su pool de semáforos por tipo.

| Función | Ubicación |
|---|---|
| `main()` — sin hilos | `io/src/main.c:13` |
| Selección del tipo | `io/src/main.c:38-50` |
| `conectar_a_kernel_scheduler()` | `io/src/conexiones/conexiones.c` |

---

> «STDIN: Recibirá una cantidad de caracteres (bytes) que deberá leer por teclado y se quedará esperando el input del usuario, el cual finalizará cuando éste pulse la tecla Enter. En caso de que la cadena de caracteres sea mayor al tamaño recibido, se deberá cortar el contenido a la cantidad de bytes solicitada. En caso de que sea menor, se completará con valores '\0' hasta alcanzar el límite indicado...»

**Cómo se resuelve.** El loop de lectura es la parte fina, y resuelve los dos casos borde de la consigna:

```c
while ((c = getchar()) != '\n' && c != EOF) {
    if (bytes_leidos < tamanio) buffer[bytes_leidos] = (char)c;
    bytes_leidos++;
}
while (bytes_leidos < tamanio) { buffer[bytes_leidos] = '\0'; bytes_leidos++; }
```

- **Corte por exceso:** el `if` guarda sólo si todavía hay lugar, pero el contador **igual avanza**. Eso consume el resto de la línea del stdin sin escribirla, dejando el stream limpio para el próximo pedido. Si se cortara con un `break`, los caracteres sobrantes quedarían en el buffer y contaminarían la siguiente lectura.
- **Relleno por defecto:** el segundo `while` completa con `\0` hasta `tamanio`.

En ambos casos se manda exactamente `tamanio` bytes, que es lo que el KS espera con `MSG_WAITALL`. STDIN no manda `MENSAJE_OK` — los datos mismos son la respuesta.

| Función | Ubicación |
|---|---|
| Rama STDIN | `io/src/main.c:80-115` |
| Lectura con corte y relleno | `io/src/main.c:97-107` |
| Logs obligatorios | `io/src/main.c:86-87` y `:112` |

---

> «STDOUT: Recibirá una cadena de caracteres (bytes) la cual deberá imprimir por pantalla y en el archivo de Log. Una vez impreso el log deberá informar al Kernel que la IO finalizó correctamente.»

**Cómo se resuelve.** Recibe pid + tamaño + los bytes, reserva `tamanio + 1` para poder cerrar con `\0`, imprime con `printf` + `fflush(stdout)` (necesario porque stdout redirigido a archivo o pipe es *full-buffered*, y sin el flush la salida no aparecería en el momento) y loguea con el formato obligatorio. Recién después manda `MENSAJE_OK`.

| Función | Ubicación |
|---|---|
| Rama STDOUT | `io/src/main.c:117-157` |
| Impresión + flush | `io/src/main.c:142-143` |
| Log obligatorio | `io/src/main.c:146` |
| Confirmación al KS | `io/src/main.c:153-154` |

---

> «SLEEP: Recibirá un tiempo en milisegundos como parámetro adicional para ejecutar un usleep() de dicho tiempo, antes de contestarle al Kernel Scheduler que la IO finalizó correctamente.»

**Cómo se resuelve.** `usleep(tiempo_ms * 1000)` — la conversión que la nota al pie del enunciado advierte (`usleep` recibe microsegundos). Loguea, duerme, loguea el fin y manda `MENSAJE_OK`.

| Función | Ubicación |
|---|---|
| Rama SLEEP | `io/src/main.c:159-178` |
| Conversión ms → µs | `io/src/main.c:169` |

---

# MÓDULO: SWAP

> «Para poder almacenar la memoria de los Procesos suspendidos, el módulo SWAP contará con un único archivo cuyo path y tamaño serán definidos por los parámetros SWAP_FILE_PATH y SWAP_FILE_SIZE... El archivo que contiene la información de los Procesos se asume que inicia siempre libre, no siendo necesario que su contenido se tenga que limpiar explícitamente.»
>
> «Dado que la administración del espacio de SWAP es tarea del Kernel Memory, este módulo no deberá contar con ninguna estructura administrativa más allá de lo necesario para leer y/o escribir el archivo.»

**Cómo se resuelve.** El archivo se abre con modo `"w+b"`, que **trunca el archivo si existía**. Eso implementa el *"se asume que inicia siempre libre"* sin necesidad de limpiarlo explícitamente.

El *"ninguna estructura administrativa"* se cumple estrictamente: en todo el módulo hay un `FILE*` y un buffer de un bloque. No hay bitmap, no hay lista de bloques usados, no hay tabla de nada. Toda esa administración vive en `t_gestor_memoria` del lado de KM (`swap_bloques_usados`, `segmentos_swap`).

Al arrancar informa `SWAP_FILE_SIZE` y `BLOCK_SIZE`, que es lo que KM necesita para dimensionar su bitmap: `swap_cant_bloques = tamanio_total / block_size`.

| Función | Ubicación |
|---|---|
| Apertura con truncado | `swap/src/main.c:50` |
| `conectar_y_atender_kernel_memory()` | `swap/src/conexiones/conexiones.c:10` |
| Informe de tamaños | `conexiones.c:23-28` |
| Dimensionado del bitmap en KM | `gestor_memoria_registrar_swap()` `gestor_memoria.c:602` |

---

> «El módulo SWAP solamente podrá responder a 2 operaciones... En ambos casos las operaciones siempre tendrán el tamaño de 1 bloque.»

**Cómo se resuelve.** El *"siempre 1 bloque"* está garantizado por construcción: el buffer se reserva **una sola vez** con `malloc(block)` fuera del loop, y todas las operaciones usan `block` como tamaño fijo. Nunca se lee ni escribe un tamaño variable.

- **Escritura:** recibe número de bloque + contenido, hace `fseek(nro * block)`, `fwrite`, `fflush` (para que los datos lleguen a disco y no queden en el buffer de stdio) y responde `MENSAJE_OK`.
- **Lectura:** recibe número de bloque, `fseek`, `fread`, y **completa con ceros lo que no se haya podido leer**. Eso cubre el caso de un bloque que nunca se escribió: el archivo recién truncado tiene tamaño 0, así que un `fread` sobre una posición más allá del fin devuelve menos bytes de los pedidos. Sin el relleno se mandaría basura del buffer anterior.

  Nótese que la lectura **no manda código de respuesta**, sólo los bytes — y del lado de KM, `swap_leer_bloque()` está escrito en consecuencia (hace `recv` directo del buffer, sin leer un `t_codigo_mensaje` previo). Los dos extremos son consistentes.

| Función | Ubicación |
|---|---|
| Buffer de un bloque | `swap/src/conexiones/conexiones.c:32` |
| Escritura de bloque | `conexiones.c:39-52` |
| Lectura de bloque | `conexiones.c:53-66` |
| Relleno con ceros | `conexiones.c:61` |
| Contrapartes en KM | `swap_escribir_bloque()` `gestor_memoria.c:615`, `swap_leer_bloque()` `:626` |
