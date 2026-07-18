# cpu/ — documentación por archivo

Nota sobre "dónde se usa": los 7 módulos son binarios independientes que solo comparten
`utils/` (biblioteca estática). Ningún módulo linkea el `.o` de otro. Por lo tanto, salvo que
se indique lo contrario, todos los callers de las funciones de `cpu/` están dentro del propio
binario `cpu` (verificado con grep sobre los 7 módulos: ninguna función no-`static` de `cpu/`
es invocada desde otro módulo).

## `cpu/src/main.c` (70 líneas)

Punto de entrada del binario. Orquesta boot → config → conexión → ejecución → limpieza, sin
lógica propia (todo delega a otros archivos del módulo).

### `main(int argc, char** argv)`
- Crea el boot logger (`cpu_boot_logger_create`) — si falla, aborta con `EXIT_FAILURE` antes de
  poder loguear nada (no hay logger para reportar el propio fallo del logger).
- Valida `argc != 3`: exige exactamente `./bin/cpu [Archivo Config] [Identificador]`, tal como
  pide el enunciado. Si no, loguea el uso correcto y aborta.
- `id_cpu = atoi(argv[2])` — el identificador de CPU se toma tal cual de línea de comandos, sin
  validar que sea numérico (un `atoi` sobre texto no numérico da `0` silenciosamente).
- Carga config con `cpu_config_create`; si falla, destruye el boot logger y aborta.
- Crea el logger definitivo con `cpu_logger_create` a partir del nivel de log de la config.
- **Conexiones, en este orden fijo:** primero `conectar_a_kernel_memory` (obtiene
  `segment_max_size`, indispensable para la MMU), después `conectar_a_kernel_scheduler`. Si
  KM se cae en el medio, el programa igual intenta conectar a KS con `segment_max_size` sin
  inicializar (bug potencial si `conectar_a_kernel_memory` devuelve `-1`: no se chequea el
  resultado antes de usarlo en `cpu_create`).
- Libera la config inmediatamente después de leer los dos sockets (ya no la necesita).
- `cpu_create(...)` — arma el struct `t_cpu`. Nota en el comentario del código: los Memory
  Sticks **no** se pasan por config, se descubren en runtime vía `actualizar_topologia_sticks`.
- Llama una vez `actualizar_topologia_sticks` antes de entrar al loop principal (para tener al
  menos la topología inicial), y entra a `atender_peticiones_kernel_scheduler`, que es el loop
  infinito de la CPU (solo retorna cuando el KS desconecta).
- Al salir del loop: `cpu_destroy` + `log_destroy`. No hay manejo de señales (SIGINT/SIGTERM) —
  el corte es siempre por desconexión del socket del lado del KS.

**Dónde se usa:** es `main`, el entry point del ejecutable — no tiene callers.

---

## `cpu/src/config/cpu_config.c` (59 líneas) y `cpu_config.h` (20 líneas)

Carga y valida el archivo de configuración de la CPU usando `commons/config` de la cátedra.

### `t_cpu_config` (struct, en el `.h`)
| Campo | Tipo | Para qué |
|---|---|---|
| `log_level` | `char*` | Nivel de log (`LOG_LEVEL`), pasado a `cpu_logger_create`. |
| `ip_kernel_scheduler` / `port_kernel_scheduler` | `char*` | Dirección del KS para `conectar_a_kernel_scheduler`. |
| `ip_kernel_memory` / `port_kernel_memory` | `char*` | Dirección de KM para `conectar_a_kernel_memory`. |

Todos los campos son `strdup` de lo que devuelve `commons/config` — son propiedad de
`t_cpu_config` y se liberan en `cpu_config_destroy`.

### `tiene_todas_las_propiedades(t_config* config)` — `static`, línea 6
Chequea con `config_has_property` que las 5 claves obligatorias (`LOG_LEVEL`,
`IP_KERNEL_SCHEDULER`, `PORT_KERNEL_SCHEDULER`, `IP_KERNEL_MEMORY`, `PORT_KERNEL_MEMORY`) estén
presentes. **Dónde se usa:** `cpu_config_create` (línea 21), único caller.

### `cpu_config_create(char* path, t_log* logger)` — línea 14
Abre el archivo con `config_create`; si no existe o es inválido, loguea error y devuelve `NULL`.
Valida propiedades obligatorias con `tiene_todas_las_propiedades`; si falta alguna, destruye el
`t_config` intermedio y devuelve `NULL`. Si todo está, hace `malloc` del `t_cpu_config` y copia
cada valor con `strdup`. Libera el `t_config` de `commons` (ya no lo necesita, todo está
copiado). **Dónde se usa:** `main.c:30`.

### `cpu_config_log(t_cpu_config* config, t_log* logger)` — línea 39
Loguea las 5 claves leídas, una por una, con `log_info`. Solo efectos de logging, no valida ni
transforma nada. **Dónde se usa:** `main.c:37`, inmediatamente después de cargar la config.

### `cpu_config_destroy(t_cpu_config* config)` — línea 48
`NULL`-safe (chequea `config == NULL` antes de tocar nada). Libera los 5 `char*` con `free` y
después el struct. **Dónde se usa:** `main.c:57` (dos veces en el código: también en los early
returns de fallo de logger, línea 42).

---

## `cpu/src/logger/cpu_logger.c` (19 líneas) y `cpu_logger.h` (10 líneas)

Dos constructores finos sobre `commons/log`, sin lógica propia.

### `cpu_boot_logger_create(void)` — línea 3
`log_create("logs/cpu_boot.log", "CPU_BOOT", true, LOG_LEVEL_INFO)` — logger fijo a nivel INFO,
usado solo durante el arranque (antes de tener la config, por lo tanto no puede usar el nivel
configurado por el usuario). **Dónde se usa:** `main.c:10`.

### `cpu_logger_create(t_cpu_config* config)` — línea 12
`log_create("logs/cpu.log", "CPU", true, log_level_from_string(config->log_level))` — el nivel
sí sale de la config, ya cargada en este punto. **Dónde se usa:** `main.c:39`.

---

## `cpu/src/cpu/cpu.c` (147 líneas) y `cpu.h` (40 líneas)

El corazón estructural del módulo: define `struct t_cpu`, su ciclo de vida, y el loop principal
que dialoga con el Kernel Scheduler.

### `t_stick_cpu` (struct, en el `.h`, línea 15)
Representa un Memory Stick tal como lo conoce la CPU (copia local de lo que informa KM vía
`MENSAJE_TOPOLOGIA_STICKS`).
| Campo | Tipo | Para qué |
|---|---|---|
| `id` | `uint32_t` | Identificador del stick, para no reconectarse dos veces al mismo (ver `buscar_stick_por_id`). |
| `base` | `uint32_t` | Dirección física global donde arranca este stick — clave para `stick_para_direccion`. |
| `tamanio` | `uint32_t` | Tamaño del stick, junto con `base` define el rango `[base, base+tamanio)`. |
| `ip` / `puerto` | `char*` | Datos de conexión, informados por KM. |
| `fd` | `int` | Socket ya conectado directamente CPU↔stick (`-1` si no aplica; en la práctica siempre queda conectado apenas se descubre). |

### `struct t_cpu` (en el `.h`, línea 24)
| Campo | Tipo | Para qué |
|---|---|---|
| `registros` | `t_registros_cpu` | Los 11 registros del proceso en ejecución (ver `utils/registros.h`). |
| `socket_kernel` | `int` | Conexión al Kernel Scheduler (dispatch, syscalls, interrupciones). |
| `socket_memoria` | `int` | Conexión a Kernel Memory (fetch, contexto, topología). |
| `sticks` | `t_list*` de `t_stick_cpu*` | Topología de Memory Sticks conocida, agregada en orden de descubrimiento. |
| `id_cpu` | `uint32_t` | Identificador de esta CPU (para logs y handshakes). |
| `segment_max_size` | `uint32_t` | Tamaño máximo de segmento, informado por KM al conectar — parámetro de la fórmula de la MMU. |
| `ejecutando` | `bool` | `true` mientras hay un PID despachado en esta CPU. |
| `pid_actual` | `uint32_t` | PID del proceso que se está ejecutando (`0` si ninguno). |
| `tabla_segmentos` | `t_list*` de `t_segmento*` | Copia local de la tabla de segmentos del proceso actual, recibida en `solicitar_contexto_a_km`. |

### `cpu_create(int socket_kernel, int socket_memoria, uint32_t id_cpu, uint32_t segment_max_size)` — línea 14
`malloc` del struct; si falla devuelve `NULL` (sin loguear — el caller no chequea este `NULL`,
ver hallazgo abajo). Pone los registros en cero con `memset`, crea las dos listas vacías
(`sticks`, `tabla_segmentos`), y copia los parámetros recibidos. **Dónde se usa:** `main.c:61`.

### `stick_cpu_destroy(void* elemento)` — `static`, línea 31
Destructor de un `t_stick_cpu*` para usar como callback de `list_destroy_and_destroy_elements`:
cierra el fd si es distinto de `-1`, libera `ip` y `puerto`, libera el struct. **Dónde se usa:**
`cpu_destroy` (línea 45), como argumento de callback.

### `cpu_destroy(struct t_cpu* cpu)` — línea 39
`NULL`-safe. Destruye `tabla_segmentos` (elementos liberados con `free` directo, porque son
`t_segmento*` sin punteros internos) y `sticks` (elementos liberados con `stick_cpu_destroy`,
que sí tiene que cerrar el fd y liberar strings). Libera el struct. **Dónde se usa:**
`main.c:66`.

### `atender_peticiones_kernel_scheduler(struct t_cpu* cpu, t_log* logger)` — línea 50
El loop principal de todo el módulo. Bloquea en `recv` esperando un `t_codigo_mensaje` del KS.

- `bytes <= 0` → el KS se desconectó → loguea y rompe el loop externo (fin del programa).
- `MENSAJE_ENVIAR_PID`:
  1. Recibe el PID, marca `ejecutando = true`.
  2. Refresca topología de sticks (`actualizar_topologia_sticks`) — el comentario en el código
     aclara que pueden haberse conectado sticks nuevos desde el último despacho.
  3. Pide el contexto completo a KM (`solicitar_contexto_a_km`): registros + tabla de segmentos.
  4. Entra al sub-loop **Fetch → Decode → Execute → Check Interrupt**, instrucción por
     instrucción, hasta que `termino = true`:
     - **Fetch** (`cpu_fetch`): si devuelve `NULL`, loguea error y corta (sin avisar al KS —
       ver hallazgo).
     - **Decode** (`cpu_decode`): si devuelve `-1`, libera el string fetcheado, loguea error y
       corta (mismo caso: no hay aviso explícito al KS más allá de que la CPU deja de mandar
       nada, lo cual la deja "colgada" desde la perspectiva del KS si no hay timeout del lado
       KS — la responsabilidad de ese caso queda del lado kernel_scheduler).
     - **Execute** (`cpu_execute`): si devuelve `1`, el proceso terminó su ráfaga (syscall,
       exit o seg fault) → `termino = true`. Si devuelve `0`, sigue ejecutando en la misma CPU.
     - Si no terminó: incrementa el PC salvo que la instrucción ya lo haya modificado
       (`JNZ` que saltó, o `SET PC`) — el comentario en el código explica por qué el orden
       importa: si no se incrementa **antes** de mandar el contexto en
       `cpu_check_interrupt`, al resumir el proceso se reejecutaría la misma instrucción en
       loop infinito.
     - **Check Interrupt** (`cpu_check_interrupt`): no bloqueante; si hay interrupción pendiente
       del KS, manda el contexto actualizado y corta el sub-loop.
  5. Al salir del sub-loop: resetea `ejecutando = false` y `pid_actual = 0` — la CPU vuelve a
     estar disponible para el próximo `MENSAJE_ENVIAR_PID`.
- `MENSAJE_INTERRUPCION`: solo loguea que llegó — este caso es una interrupción que llega
  **fuera** de una ejecución activa (entre despachos), así que no hay nada más que hacer (el
  chequeo real ocurre dentro del sub-loop vía `cpu_check_interrupt`, no acá).
- `default`: loguea advertencia de mensaje desconocido y sigue el loop (no corta la conexión).

**Dónde se usa:** `main.c:63`, una única vez, es el loop de vida del programa.

**Hallazgo:** `cpu_create` puede devolver `NULL` si `malloc` falla, pero `main.c` no chequea
ese valor antes de usarlo en `actualizar_topologia_sticks`/`atender_peticiones_kernel_scheduler`
— crashearía con NULL deref en un entorno con memoria agotada (caso extremo, no bloqueante para
el TP, pero real).

---

## `cpu/src/instrucciones/instrucciones.c` (347 líneas) y `instrucciones.h` (14 líneas)

El archivo más grande del módulo: la MMU y el intérprete completo del ISA de 20 instrucciones
(18 pedidas por el enunciado + `WAIT`/`SIGNAL`, no implementadas — ver más abajo).

### `mmu_traducir(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tamanio_lectura, t_log* logger)` — `static`, línea 23
La MMU. Calcula `num_segmento = dir_logica / cpu->segment_max_size` y
`desplazamiento = dir_logica % cpu->segment_max_size` — fórmula idéntica a la que pide el
enunciado, en enteros, sin binario. Busca `num_segmento` linealmente en
`cpu->tabla_segmentos` (`O(n)` sobre la cantidad de segmentos del proceso, aceptable para los
tamaños del TP). Si no lo encuentra: `SEG_FAULT` (log + devuelve `UINT32_MAX` como código de
error, ya que `0` sería una dirección física válida). Si lo encuentra pero
`desplazamiento + tamanio_lectura > seg->tamanio`: también `SEG_FAULT` (acceso que se sale del
segmento). Si todo es válido: devuelve `seg->base + desplazamiento`, la dirección física real.
**Dónde se usa:** dentro de `cpu_execute`, en los casos `INST_MOV_IN` (línea 183),
`INST_MOV_OUT` (línea 205), `INST_COPY_MEM` (dos veces, origen y destino, líneas 226 y 233),
`INST_IO_STDIN` (línea 278) e `INST_IO_STDOUT` (línea 290) — es decir, toda instrucción que
toca memoria de usuario pasa por acá antes de tocar un stick.

### `cpu_fetch(struct t_cpu* cpu, t_log* logger)` — línea 60
Pide a KM la instrucción en la posición `cpu->registros.pc` del proceso `cpu->pid_actual`:
manda `MENSAJE_PEDIR_INSTRUCCION` + PID + PC, recibe primero un `uint32_t len` y después `len`
bytes de texto (protocolo longitud-prefijada). Si `recv` falla o `len == 0`, devuelve `NULL`
(fin de instrucciones o error de conexión — el caller no distingue ambos casos). Devuelve un
`char*` con `malloc(len+1)` null-terminado; **el caller es responsable de liberarlo** (y lo
hace, en `cpu.c:100`, tras el decode). **Dónde se usa:** `cpu.c:83`, único caller, dentro del
sub-loop de ejecución.

### `cpu_decode(const char* instrucciones, t_instruccion* instruccion_out)` — línea 81
Parsea el string crudo con `string_split(instrucciones, " ")` de `commons`. Si el split falla o
el primer token es `NULL`, libera el array y devuelve `-1`. Mapea el primer token contra los 18
nombres de instrucción conocidos (`NOOP`, `SET`, `MOV_IN`, `MOV_OUT`, `SUM`, `SUB`, `JNZ`,
`COPY_MEM`, `SLEEP`, `MUTEX_CREATE`, `MUTEX_LOCK`, `MUTEX_UNLOCK`, `MEM_ALLOC`, `MEM_FREE`,
`STDIN`, `STDOUT`, `INIT_PROC`, `EXIT`) con una cadena de `strcmp` — si no matchea ninguno,
libera el array y devuelve `-1` (instrucción desconocida). Copia los parámetros restantes a
`instruccion_out->parametros[]` con `strncpy` truncado a `INSTRUCCION_PARAM_LEN - 1`, cortando
apenas encuentra el `NULL` terminador del array de `string_split` (el comentario en el código
explica por qué: seguir iterando hasta `INSTRUCCION_MAX_PARAMS` leería memoria más allá del
array real cuando la instrucción tiene menos parámetros que el máximo). Libera el array de
tokens al final. **Dónde se usa:** `cpu.c:94`, único caller.

### `cpu_execute(struct t_cpu* cpu, t_instruccion* inst, t_log* logger)` — línea 139
El intérprete. Devuelve `0` si la CPU debe seguir ejecutando la misma ráfaga (instrucción
básica sin syscall), `1` si el proceso libera la CPU (syscall, `EXIT`, o `SEG_FAULT`). Un
`switch` sobre `inst->codigo`:

| Instrucción | Qué hace |
|---|---|
| `INST_NOOP` | No hace nada. |
| `INST_SET` | `set_registro_32` sobre el registro indicado en `parametros[0]` con el valor de `parametros[1]` (parseado con `strtoul`). Funciona para cualquier registro de 8 o 32 bits porque `set_registro_32` internamente decide el ancho. |
| `INST_SUM` / `INST_SUB` | Operan sobre registros de 8 bits (`get_registro`/`set_registro`): `dest = dest + orig` / `dest = dest - orig`, sin chequeo de overflow/underflow (wrap natural de `uint8_t`). |
| `INST_JNZ` | Si el registro de `parametros[0]` es distinto de cero, `PC = atoi(parametros[1])` y `return 0` **inmediatamente** (sin dejar que el loop de `cpu.c` incremente el PC después, porque ya lo fijó explícitamente). |
| `INST_MOV_IN` | Lee de memoria física (tamaño según ancho del registro destino) en la dirección lógica `cpu->registros.si`, traducida con `mmu_traducir`; en `SEG_FAULT`, dispara `enviar_syscall_seg_fault` y corta (`return 1`). Si lee bien, guarda el valor en el registro destino. |
| `INST_MOV_OUT` | Simétrico: escribe el valor del registro destino en la dirección lógica `cpu->registros.di`. |
| `INST_COPY_MEM` | Copia `tamanio` bytes (tomado de un registro de 32 bits) desde `si` hacia `di`, traduciendo ambas direcciones por separado; si cualquiera de las dos traducciones falla, `SEG_FAULT`. Usa un buffer intermedio (`malloc`) para la copia. |
| `INST_SLEEP` | `enviar_syscall_sleep` con el tiempo parseado de `parametros[0]`; `return 1`. |
| `INST_EXIT` | `enviar_syscall_exit`; `return 1`. |
| `INST_IO_STDIN` / `INST_IO_STDOUT` | Valida el acceso con `mmu_traducir` **antes** de mandar la syscall (si falla, `SEG_FAULT` en vez de mandarle al KS una dirección inválida); manda la dirección **lógica** (no la física) al KS — el comentario aclara que KM la vuelve a traducir al momento de escribir/leer, por si hubo compactación entre el chequeo y la ejecución real de la IO. |
| `INST_MUTEX_CREATE/LOCK/UNLOCK` | Delegan directo a `enviar_syscall_mutex_*` con el nombre del mutex; `return 1`. |
| `INST_MEM_ALLOC` / `INST_MEM_FREE` | Parsean id de segmento (y tamaño, para `ALLOC`) con `atoi`; delegan a `enviar_syscall_mem_*`; `return 1`. |
| `INST_INIT_PROC` | Parsea archivo y prioridad; delega a `enviar_syscall_init_proc`; `return 1`. |
| `default` | Instrucción desconocida (por ejemplo `INST_WAIT`/`INST_SIGNAL`, que existen en el enum pero no tienen `case` acá): solo loguea advertencia, `return 0` — la CPU seguiría al siguiente PC como si nada, lo cual en la práctica nunca ocurre porque `cpu_decode` nunca produce esos códigos (no están en la tabla de strings reconocidos). |

**Dónde se usa:** `cpu.c:103`, único caller, dentro del sub-loop de ejecución.

### `cpu_check_interrupt(struct t_cpu* cpu, t_log* logger)` — línea 337
`recv` **no bloqueante** (`MSG_DONTWAIT`) sobre `socket_kernel`. Si llegó algo y es
`MENSAJE_INTERRUPCION`, loguea, manda el contexto actualizado al KS (`enviar_contexto_a_ks`) y
devuelve `true`. Si no llegó nada (o llegó otra cosa — caso no contemplado explícitamente, se
ignora), devuelve `false`. Este es el mecanismo que le permite al Kernel Scheduler desalojar la
CPU por quantum o por compactación sin que la CPU quede bloqueada esperando una respuesta suya.
**Dónde se usa:** `cpu.c:126`, único caller, entre instrucción e instrucción del sub-loop.

**Hallazgo confirmado con el enunciado:** el ciclo Fetch→Decode→Execute→Check Interrupt está
implementado en ese orden exacto, tal como lo pide el enunciado.

---

## `cpu/src/registros_cpu/registros_cpu.c` (56 líneas) y `registros_cpu.h` (15 líneas)

Accesores por nombre de registro (`"AX"`, `"EBX"`, etc.) sobre `struct t_cpu`. Todo por cadenas
de `strcmp` — no hay tabla/hash, dado el tamaño chico del set de registros (11) es aceptable.

### `get_registro(struct t_cpu* cpu, char* nombre)` — línea 6
Devuelve el valor de un registro de 8 bits (`AX`/`BX`/`CX`/`DX`). Si el nombre no matchea
ninguno, devuelve `0` silenciosamente (no hay log de error — asume que el caller siempre pasa
un nombre válido, ya que viene de `cpu_decode` que solo produce nombres tal cual aparecen en el
archivo de instrucciones del usuario). **Dónde se usa:** `instrucciones.c:157,158,164,165`
(`INST_SUM`/`INST_SUB`).

### `set_registro(struct t_cpu* cpu, char* nombre, uint8_t valor)` — línea 14
Simétrico de `get_registro`, para escritura. **Dónde se usa:** `instrucciones.c:159,166`.

### `get_registro_32(struct t_cpu* cpu, char* nombre)` — línea 21
Devuelve el valor de un registro de 32 bits (`EAX/EBX/ECX/EDX/SI/DI/PC`) — **y también**
soporta los de 8 bits (`AX..DX`), devolviéndolos ensanchados a `uint32_t`. Esto es lo que
permite que `INST_SET` funcione de manera uniforme para cualquier registro sin importar su
ancho real. **Dónde se usa:** `instrucciones.c:119,171,204,222,272,273,287,288` — todos los
casos que necesitan leer un registro sin saber de antemano su ancho exacto (`JNZ`, `MOV_OUT`,
`COPY_MEM`, `STDIN`/`STDOUT`).

### `tamanio_registro(char* nombre)` — línea 36
Devuelve `1` para `AX/BX/CX/DX`, `4` para cualquier otro nombre (asume que es uno de los de 32
bits, sin validar exhaustivamente). **Dónde se usa:** `instrucciones.c:181,202` (`MOV_IN`/
`MOV_OUT`, para saber cuántos bytes leer/escribir de memoria física según el ancho del registro
destino).

### `set_registro_32(struct t_cpu* cpu, char* nombre, uint32_t valor)` — línea 44
Escribe cualquier registro (8 o 32 bits) a partir de un valor de 32 bits, truncando a `uint8_t`
para los de 8 bits. **Dónde se usa:** `instrucciones.c:152,197` (`INST_SET`, `INST_MOV_IN`).

**Nota de diseño:** las cuatro funciones repiten la misma cadena de `strcmp` con distinto
cuerpo — es la única forma directa de mapear un identificador textual (que viene del archivo de
instrucciones del proceso de usuario, en texto plano) a un campo del struct sin reflexión en C;
no hay una tabla de punteros a miembro porque C no la soporta de forma segura para campos de
distinto ancho (8 vs 32 bits) sin unsafe casts.

---

## `cpu/src/conexiones/conexiones.c` (319 líneas) y `conexiones.h` (38 líneas)

Todo lo que toca sockets desde la CPU: handshakes, syscalls, contexto, topología de sticks y
ruteo físico multi-stick. Es, junto con `instrucciones.c`, el corazón del módulo.

### `conectar_a_kernel_scheduler(const char* ip, const char* puerto, uint32_t id_cpu, t_log* logger)` — línea 12
`crear_conexion` (de `utils/sockets`); si falla, loguea y devuelve `-1`. Si conecta, manda el
handshake `HANDSHAKE_CPU` (`int32_t`) seguido del `id_cpu` (`uint32_t`) — **sin** esperar
respuesta del KS (no hay ack de handshake en este sentido). **Dónde se usa:** `main.c:55`.

### `conectar_a_kernel_memory(const char* ip, const char* puerto, uint32_t id_cpu, uint32_t* segment_max_size, t_log* logger)` — línea 30
Igual patrón, pero acá sí espera respuesta: después del handshake `HANDSHAKE_CPU` + id (nota:
acá el id se manda como `int32_t`, no `uint32_t` como en la versión de KS — inconsistencia de
tipo sin impacto práctico porque el valor cabe en ambos), hace `recv` bloqueante de
`segment_max_size` — KM contesta con ese valor apenas acepta la conexión, antes de cualquier
otro intercambio. **Dónde se usa:** `main.c:54`.

### `enviar_syscall_a_ks(struct t_cpu* cpu, t_codigo_syscall syscall_tipo, t_log* logger)` — `static`, línea 50
Función interna común a **todas** las syscalls. Primero incrementa `cpu->registros.pc` — el
comentario explica que esto se hace acá y no en el loop de `cpu.c`, porque toda syscall libera
la CPU (nunca vuelve a este punto para "terminar el ciclo normalmente"), así que si no se
incrementa acá, al resumir el proceso repetiría la misma syscall en loop infinito. Después manda
`MENSAJE_SYSCALL` + el tipo de syscall, y loguea. **No** manda el contexto (`t_registros_cpu`)
— eso queda a cargo de cada función `enviar_syscall_*` específica, después de esta llamada.
**Dónde se usa:** las 10 funciones `enviar_syscall_*` de este mismo archivo (líneas 65, 72, 78,
82, 90, 107, 116, 125, 134, 142, 149).

### `enviar_syscall_sleep` / `enviar_syscall_exit` / `enviar_syscall_seg_fault` / `enviar_syscall_stdin` / `enviar_syscall_stdout` / `enviar_syscall_mutex_create` / `enviar_syscall_mutex_lock` / `enviar_syscall_mutex_unlock` / `enviar_syscall_mem_alloc` / `enviar_syscall_mem_free` / `enviar_syscall_init_proc`
Todas siguen el mismo patrón: llaman a `enviar_syscall_a_ks` con su `t_codigo_syscall`
específico, y después mandan los parámetros propios de la syscall (tiempo, dirección+tamaño,
nombre de mutex longitud-prefijado, id de segmento, etc.) seguidos del struct completo de
registros (`t_registros_cpu`) — **excepto** `enviar_syscall_exit` y `enviar_syscall_seg_fault`,
que no mandan registros (el proceso termina, no hace falta preservar su contexto para
reanudarlo). `enviar_syscall_seg_fault` reutiliza el mismo esquema de mensaje que las demás
syscalls aunque conceptualmente es un error, no una syscall del usuario — el comentario del
código lo aclara explícitamente. **Dónde se usa:** todas invocadas exclusivamente desde
`cpu_execute` en `instrucciones.c` (una por cada instrucción de syscall correspondiente).

### `enviar_contexto_a_ks(struct t_cpu* cpu, t_log* logger)` — línea 97
Manda `MENSAJE_INTERRUPCION` + el struct de registros completo. El comentario en el código es
una advertencia de mantenimiento explícita: el KS espera primero el código de mensaje (lo lee
en `atender_peticiones_cpu`) y después el struct — mandar solo los registros desincronizaría el
protocolo del socket permanentemente (todos los mensajes futuros se leerían corridos). **Dónde
se usa:** `instrucciones.c:343`, dentro de `cpu_check_interrupt`, cuando el KS pide desalojar.

### `solicitar_contexto_a_km(struct t_cpu* cpu, t_log* logger)` — línea 158
Manda `MENSAJE_CONTEXTO` + PID; recibe el struct de registros completo, después la cantidad de
segmentos, y después esa cantidad de `t_segmento` uno por uno, reconstruyendo
`cpu->tabla_segmentos` desde cero (`list_clean_and_destroy_elements` primero, para no acumular
segmentos de una ejecución anterior de otro proceso). **Dónde se usa:** `cpu.c:76`, cada vez que
el KS despacha un PID nuevo a esta CPU.

### `buscar_stick_por_id(struct t_cpu* cpu, uint32_t id)` — `static`, línea 182
Búsqueda lineal en `cpu->sticks` por `id`. **Dónde se usa:** `actualizar_topologia_sticks`
(línea 213), para no reconectarse a un stick ya conocido.

### `actualizar_topologia_sticks(struct t_cpu* cpu, t_log* logger)` — línea 190
Manda `MENSAJE_TOPOLOGIA_STICKS` a KM y recibe, por cada stick, `id`/`tamanio`/`base` y las
strings de `ip`/`puerto` (longitud-prefijadas). Para cada stick recibido: si ya lo conoce
(`buscar_stick_por_id`), libera las strings recién recibidas y sigue (no reemplaza el existente
— asume que la topología de un stick ya conocido no cambia una vez conectado). Si es nuevo:
`crear_conexion` directa CPU↔stick, handshake `HANDSHAKE_CPU` + `id_cpu`, y lo agrega al final
de `cpu->sticks`. **Dónde se usa:** `main.c:62` (una vez al boot) y `cpu.c:75` (en cada
despacho de PID, para refrescar por si se conectó un stick nuevo entre despachos) y
`operar_fisico` (línea 287, como reintento si una dirección no matchea ningún stick conocido).

### `stick_para_direccion(struct t_cpu* cpu, uint32_t dir)` — `static`, línea 246
Búsqueda lineal: devuelve el `t_stick_cpu*` tal que `dir` cae en `[stick->base, stick->base +
stick->tamanio)`. Devuelve `NULL` si ninguno matchea. **Dónde se usa:** `operar_fisico` (línea
284), en cada iteración del loop de partición.

### `stick_operar(t_stick_cpu* stick, bool escribir, uint32_t dir_local, uint32_t tamanio, void* buffer)` — `static`, línea 257
Una única operación de R/W contra **un** stick ya identificado, usando la dirección **local**
al stick (no la global). Manda el opcode correspondiente (`MENSAJE_ESCRIBIR_MEMORIA` o
`MENSAJE_LEER_MEMORIA`) + dirección + tamaño (+ buffer si es escritura), espera la respuesta
(`MENSAJE_OK` o error), y si es lectura, recibe el buffer de vuelta. Devuelve `bool` de éxito.
**Dónde se usa:** `operar_fisico` (línea 300), una vez por cada tramo en el que se particiona
una operación multi-stick.

### `operar_fisico(struct t_cpu* cpu, bool escribir, uint32_t dir_fisica, uint32_t tamanio, void* buffer, t_log* logger)` — `static`, línea 277
El ruteador multi-stick. Loop `while (restante > 0)`: por cada iteración, ubica el stick que
contiene la dirección actual (`stick_para_direccion`); si no lo encuentra y todavía no
reintentó, refresca la topología una vez (`actualizar_topologia_sticks`) por si el stick se
conectó recién y vuelve a intentar en la siguiente iteración; si ya reintentó y sigue sin
encontrarlo, loguea error y devuelve `false` — corta la operación completa (no hay resultado
parcial). Si lo encuentra, calcula `dir_local = dir - stick->base` y `tramo = min(restante,
disponible_en_este_stick)`, opera ese tramo con `stick_operar`, y avanza `dir`/`cursor`/
`restante`. El resultado queda consolidado en el mismo `buffer` que recibió el caller, como si
fuera una sola operación — la partición es completamente transparente para quien llama a
`leer_memoria_fisica`/`escribir_memoria_fisica`. **Dónde se usa:** `leer_memoria_fisica` (línea
314) y `escribir_memoria_fisica` (línea 318), únicos dos callers.

### `leer_memoria_fisica` / `escribir_memoria_fisica` — líneas 313 y 317
Wrappers públicos de `operar_fisico` con `escribir` fijo en `false`/`true`. **Dónde se usa:**
`instrucciones.c`, en `INST_MOV_IN` (línea 191), `INST_MOV_OUT` (línea 212), e `INST_COPY_MEM`
(líneas 241 y 245, una vez para cada sentido de la copia).

**Hallazgos notables de este archivo:**
- La partición multi-stick y el patrón de ruteo (`stick_para_direccion`/`operar_fisico`) están
  duplicados casi textualmente en `kernel_memory/src/gestor_memoria/gestor_memoria.c` (con
  nombres de función idénticos, ambos `static`, sin compartir código real porque son binarios
  separados) — es la misma lógica resuelta dos veces porque tanto la CPU como KM acceden a los
  sticks directamente. No es un bug, es una duplicación estructural inherente a la arquitectura
  del TP (cada módulo que toca memoria física tiene que resolver el ruteo por su cuenta).
- `conectar_a_kernel_scheduler` manda `id_cpu` como `uint32_t` pero `conectar_a_kernel_memory`
  lo manda como `int32_t` — inconsistencia de tipo sin efecto práctico (mismo tamaño en memoria,
  valores siempre positivos en este contexto), pero vale la pena unificarlo si se refactoriza.
