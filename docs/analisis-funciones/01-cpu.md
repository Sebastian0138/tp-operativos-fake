# Módulo cpu — Análisis detallado

Ejecuta el ciclo de instrucción (fetch-decode-execute) de cada proceso que le despacha el
`kernel_scheduler` (KS), traduce direcciones lógicas a físicas (MMU) contra la tabla de
segmentos que le manda `kernel_memory` (KM), y lee/escribe bytes reales conectándose
**directamente** a los `memory_stick` (no pasa por KM para el R/W físico).

> Nota: varios nombres de función (`conectar_a_kernel_scheduler`, `conectar_a_kernel_memory`)
> se repiten en otros módulos (`io`, `kernel_scheduler`, `memory_stick`) con firmas distintas.
> Son funciones **independientes** (cada módulo compila su propio binario); se aclara en cada
> caso para no confundirlo con una función compartida.

---

### `main.c`

Punto de entrada del binario. Levanta boot logger → config → logger definitivo → conecta a
KM y KS → crea el contexto de CPU → pide la topología de sticks → entra al loop principal.

#### `main(argc, argv)` — main.c:9
- **Qué hace:** Orquesta el arranque completo de la CPU: valida argumentos (`config` e
  `id_cpu`), carga config, crea logger, se conecta a KM (recibe `segment_max_size`) y a KS,
  crea el struct `t_cpu`, pide la topología inicial de sticks y entra en el loop de atención
  de KS. Al salir del loop (KS se desconectó), destruye todo.
- **Dónde se usa:** Entry point del ejecutable `cpu`, invocado por `start_all.sh` / línea de
  comandos como `./bin/cpu [config] [id_cpu]`. No lo llama otra función.
- **Para qué:** Bootstrap del proceso CPU.
- **Cómo funciona:** Requiere exactamente 3 argv (`argv[0]`=binario, `argv[1]`=config,
  `argv[2]`=id). Si falla cualquier paso de arranque (config, logger) libera lo creado hasta
  ahí y sale con `EXIT_FAILURE`. El orden de conexión es primero KM (para tener
  `segment_max_size` antes de operar) y después KS. La llamada a `actualizar_topologia_sticks`
  antes del loop asegura que la CPU ya conozca los sticks existentes al arrancar, aunque
  también se refresca en cada despacho de proceso (ver `cpu.c`).

---

### `config/cpu_config.c` — carga de configuración

#### `tiene_todas_las_propiedades(config)` — cpu_config.c:6 *(static)*
- **Qué hace:** Verifica que el `.cfg` tenga las 5 claves obligatorias: `LOG_LEVEL`,
  `IP_KERNEL_SCHEDULER`, `PORT_KERNEL_SCHEDULER`, `IP_KERNEL_MEMORY`, `PORT_KERNEL_MEMORY`.
- **Dónde se usa:** Solo `cpu_config_create` (mismo archivo, línea 21).
- **Para qué:** Falla rápido y con mensaje claro si el `.cfg` está incompleto, en vez de
  segfaultear más adelante con un `strdup(NULL)`.
- **Cómo funciona:** Cadena de `config_has_property` (de la librería `commons`) unidas por
  `&&` — cortocircuita en la primera que falte.

#### `cpu_config_create(path, logger)` — cpu_config.c:14
- **Qué hace:** Abre el archivo de config en `path`, valida claves, y si todo OK reserva un
  `t_cpu_config` y copia (con `strdup`) cada valor como string.
- **Dónde se usa:** `main.c:30`.
- **Para qué:** Punto único de carga de configuración de la CPU.
- **Cómo funciona:** `config_create` → si es `NULL` loguea error y devuelve `NULL`. Si
  `tiene_todas_las_propiedades` falla, destruye el config y devuelve `NULL`. Si todo bien,
  hace `malloc(sizeof(t_cpu_config))` y duplica cada string (todos quedan como `char*`, incluso
  el puerto, porque se usan tal cual en `crear_conexion` que espera strings). Libera el
  `t_config* config` de la librería (ya no lo necesita, los valores están copiados).

#### `cpu_config_log(config, logger)` — cpu_config.c:39
- **Qué hace:** Loguea los 5 valores cargados con `log_info`.
- **Dónde se usa:** `main.c:37`.
- **Para qué:** Trazabilidad de arranque — poder confirmar en el log con qué config arrancó la
  CPU.
- **Cómo funciona:** Secuencia lineal de `log_info`, sin lógica.

#### `cpu_config_destroy(config)` — cpu_config.c:48
- **Qué hace:** Libera los 5 strings duplicados y el struct.
- **Dónde se usa:** `main.c:42` (en el camino de error de logger) y `main.c:57` (camino normal,
  apenas terminó de usarse para conectar).
- **Para qué:** Evitar leaks — la config solo se necesita durante el arranque, así que se
  destruye apenas se usó.
- **Cómo funciona:** Chequea `NULL`, hace `free` de cada campo y del struct. Nótese que la
  config se destruye **antes** de que arranque el loop principal (main.c:57 va antes de
  `cpu_create`), a diferencia de otros módulos donde vive todo el proceso.

---

### `logger/cpu_logger.c`

#### `cpu_boot_logger_create()` — cpu_logger.c:3
- **Qué hace:** Crea un logger fijo (`logs/cpu_boot.log`, tag `CPU_BOOT`, nivel `INFO`,
  también a consola por el `true`) usado solo durante el arranque, antes de tener la config
  cargada (porque el nivel de log real viene del `.cfg`).
- **Dónde se usa:** `main.c:10`.
- **Para qué:** Poder loguear errores de carga de config (que todavía no existe) sin
  depender de ella.
- **Cómo funciona:** Wrapper directo de `log_create` de `commons` con parámetros hardcodeados.

#### `cpu_logger_create(config)` — cpu_logger.c:12
- **Qué hace:** Crea el logger definitivo (`logs/cpu.log`, tag `CPU`) con el nivel que indica
  `config->log_level` (parseado con `log_level_from_string`).
- **Dónde se usa:** `main.c:39`.
- **Para qué:** Logger que se usa durante toda la vida del proceso, con el nivel que el
  usuario configuró.
- **Cómo funciona:** Wrapper de `log_create`, la única diferencia con el boot logger es el
  nivel dinámico y el archivo destino.

---

### `cpu/cpu.c` — estructura central y loop principal

Define el struct `t_cpu` (registros, sockets, lista de sticks conocidos, tabla de segmentos
del proceso actual) y el bucle que atiende al KS.

#### `cpu_create(socket_kernel, socket_memoria, id_cpu, segment_max_size)` — cpu.c:14
- **Qué hace:** Reserva y arma el struct `t_cpu`: pone los registros en cero, guarda los dos
  sockets (KS y KM) ya conectados, crea las listas vacías `sticks` y `tabla_segmentos`,
  marca `ejecutando=false` y `pid_actual=0`.
- **Dónde se usa:** `main.c:61`, una sola vez al arrancar.
- **Para qué:** Constructor del contexto de ejecución de la CPU, que viaja por parámetro a
  todas las funciones de `instrucciones.c`, `conexiones.c` y `registros_cpu.c`.
- **Cómo funciona:** `malloc` + inicialización campo a campo. `list_create()` de `commons`
  para las dos listas dinámicas.

#### `stick_cpu_destroy(elemento)` — cpu.c:31 *(static)*
- **Qué hace:** Destructor de un `t_stick_cpu*`: cierra su `fd` (si no es -1), libera `ip` y
  `puerto`, libera el struct.
- **Dónde se usa:** Solo como callback pasado a `list_destroy_and_destroy_elements` dentro de
  `cpu_destroy` (cpu.c:45).
- **Para qué:** Cerrar prolijamente las conexiones directas a cada stick al terminar el
  proceso CPU.
- **Cómo funciona:** Cast de `void*` a `t_stick_cpu*` y liberación de sus 3 recursos (fd,
  ip, puerto) más el struct en sí.

#### `cpu_destroy(cpu)` — cpu.c:39
- **Qué hace:** Libera `tabla_segmentos` (con `free` simple por elemento, son `t_segmento*`
  planos) y `sticks` (con `stick_cpu_destroy`, que además cierra sockets), y el struct `cpu`.
- **Dónde se usa:** `main.c:66`, al terminar el `while(1)` de `atender_peticiones_kernel_scheduler`
  (KS se desconectó).
- **Para qué:** Liberación final de todos los recursos de la CPU antes de salir del proceso.
- **Cómo funciona:** Chequeos de `NULL` por robustez, luego dos `list_destroy_and_destroy_elements`
  con destructores distintos según el tipo de elemento de cada lista.

#### `atender_peticiones_kernel_scheduler(cpu, logger)` — cpu.c:50
- **Qué hace:** Es el **loop principal** de la CPU. Bloqueado en `recv` esperando un
  `t_codigo_mensaje` del KS. Ante `MENSAJE_ENVIAR_PID`: recibe el PID a ejecutar, refresca la
  topología de sticks, pide el contexto a KM, y corre el ciclo fetch→decode→execute hasta que
  el proceso termine, se bloquee, se desaloje o falle. Ante `MENSAJE_INTERRUPCION` "suelta"
  (solo loguea; el manejo real de interrupciones ocurre dentro del ciclo vía
  `cpu_check_interrupt`, no acá). Si `recv` devuelve `<=0`, el KS se desconectó y sale del
  `while(1)`.
- **Dónde se usa:** `main.c:63`, una sola vez — es el loop de vida del proceso.
- **Para qué:** Es el corazón de la CPU: implementa el ciclo de instrucción completo y decide
  cuándo la CPU queda libre para el próximo PID.
- **Cómo funciona:**
  1. Recibe PID, marca `ejecutando=true`, refresca sticks (por si se conectó uno nuevo desde
     el último despacho) y pide el contexto (registros + tabla de segmentos) a KM vía
     `solicitar_contexto_a_km`.
  2. Loop interno `while (!termino)`:
     - **FETCH:** `cpu_fetch` pide a KM la instrucción en el PC actual. Si devuelve `NULL`,
       corta con error (no debería pasar en un flujo sano).
     - **DECODE:** `cpu_decode` parsea el string a `t_instruccion`. Si falla, corta.
     - **EXECUTE:** `cpu_execute` ejecuta y devuelve `0` (seguir) o `1` (la instrucción ya
       liberó la CPU — syscall, EXIT o SEG_FAULT).
     - Si `resultado == 1`: `termino = true`.
     - Si no: incrementa `PC` **excepto** si la instrucción ya lo modificó ella misma (`JNZ`
       que saltó, o `SET PC`) — el comentario en el código explica que esto es crítico: si no
       se hace antes de chequear la interrupción, al reanudar el proceso se re-ejecutaría la
       misma instrucción en loop infinito. Después llama a `cpu_check_interrupt`; si hay
       interrupción pendiente del KS (desalojo por quantum o compactación), también corta el
       loop.
  3. Al salir del loop interno, resetea `ejecutando=false` y `pid_actual=0` para quedar libre
     para el próximo despacho.

---

### `instrucciones/instrucciones.c` — MMU + ciclo de instrucción

El archivo más denso de la CPU: implementa la traducción de direcciones (MMU) y las 3 etapas
del ciclo de instrucción (fetch/decode/execute) más el chequeo de interrupciones no
bloqueante.

#### `mmu_traducir(cpu, dir_logica, tamanio_lectura, logger)` — instrucciones.c:23 *(static)*
- **Qué hace:** Traduce una dirección **lógica** (relativa al espacio de direcciones del
  proceso) a una dirección **física global** (absoluta, sobre el conjunto de sticks),
  validando que el acceso no se salga del segmento correspondiente.
- **Dónde se usa:** *(static, solo dentro de instrucciones.c)* Llamada desde `cpu_execute` en
  `INST_MOV_IN` (instrucciones.c:183), `INST_MOV_OUT` (:205), `INST_COPY_MEM` (dos veces, origen
  y destino, :226 y :233), `INST_IO_STDIN` (:278) e `INST_IO_STDOUT` (:290).
- **Para qué:** Es la MMU de la CPU: sin esto, cualquier lectura/escritura de memoria del
  proceso de usuario sería insegura (podría leer/escribir memoria de otro proceso).
- **Cómo funciona:**
  1. Calcula `num_segmento = dir_logica / segment_max_size` y
     `desplazamiento = dir_logica % segment_max_size` (esquema de **segmentación paginada
     simple**: el espacio lógico se divide en bloques de tamaño fijo `segment_max_size`, cada
     uno es "el segmento N").
  2. Busca en `cpu->tabla_segmentos` (la que mandó KM en `solicitar_contexto_a_km`) un
     `t_segmento` cuyo `id_segmento == num_segmento`.
  3. Si no lo encuentra → `SEG_FAULT` (el proceso intentó acceder a un segmento que nunca creó
     o que ya no existe), loguea y devuelve `UINT32_MAX` como código de error.
  4. Si lo encuentra, valida que `desplazamiento + tamanio_lectura <= seg->tamanio` — si se
     pasa, también `SEG_FAULT`.
  5. Si todo OK, devuelve `seg->base + desplazamiento` (la dirección física global, que
     después `operar_fisico` en `conexiones.c` rutea al stick correcto).
  - `UINT32_MAX` se usa como sentinel de error porque `uint32_t` no tiene forma nativa de
    devolver "inválido" de otra manera sin un parámetro de salida extra.

#### `cpu_fetch(cpu, logger)` — instrucciones.c:60
- **Qué hace:** Le pide a KM la instrucción del proceso actual en la posición `PC` actual.
- **Dónde se usa:** `cpu.c:83`, dentro del loop de `atender_peticiones_kernel_scheduler`
  (etapa FETCH de cada iteración).
- **Para qué:** Primera etapa del ciclo de instrucción — la CPU no tiene las instrucciones
  localmente, viven en KM (`kernel_memory_gestor_procesos`).
- **Cómo funciona:** Envía `MENSAJE_PEDIR_INSTRUCCION` + `pid_actual` + `pc` por
  `socket_memoria`. Recibe primero un `uint32_t len`; si `len == 0` o `recv` falla, devuelve
  `NULL` (fin de instrucciones o error). Si no, reserva `len+1` bytes, recibe el string y lo
  null-termina. El caller (`cpu.c`) es responsable de hacer `free` de ese string.

#### `cpu_decode(instrucciones, instruccion_out)` — instrucciones.c:81
- **Qué hace:** Parsea un string tipo `"MOV_IN EAX"` o `"SET PC 5"` a un struct
  `t_instruccion` con su `codigo` (enum) y sus parámetros como array de strings.
- **Dónde se usa:** `cpu.c:94`, etapa DECODE del ciclo.
- **Para qué:** Traducir el texto plano recibido de KM a una representación que
  `cpu_execute` pueda ejecutar con un `switch`.
- **Cómo funciona:** `string_split` por espacios. El primer token se compara contra 18
  mnemónicos posibles (`NOOP`, `SET`, `MOV_IN`, `MOV_OUT`, `SUM`, `SUB`, `JNZ`, `COPY_MEM`,
  `SLEEP`, `MUTEX_CREATE/LOCK/UNLOCK`, `MEM_ALLOC/FREE`, `STDIN/STDOUT`, `INIT_PROC`, `EXIT`)
  con una cadena de `if/else strcmp`; si no matchea ninguno, devuelve `-1`. Los tokens
  restantes se copian con `strncpy` (truncando a `INSTRUCCION_PARAM_LEN-1`) a
  `instruccion_out->parametros[i]`, cortando el loop en el primer `NULL` del array
  (importante: el comentario en el código aclara que iterar hasta `INSTRUCCION_MAX_PARAMS`
  sin chequear `NULL` primero leería memoria fuera del array devuelto por `string_split`
  quando hay menos parámetros que el máximo).

#### `cpu_execute(cpu, inst, logger)` — instrucciones.c:139
- **Qué hace:** Ejecuta la instrucción decodificada. Devuelve `0` si la CPU debe seguir
  ejecutando instrucciones del mismo proceso, o `1` si la CPU quedó liberada (syscall
  enviada al KS, SEG_FAULT, o EXIT).
- **Dónde se usa:** `cpu.c:103`, etapa EXECUTE del ciclo — una vez por instrucción.
- **Para qué:** Es el intérprete del ISA (instruction set) del TP.
- **Cómo funciona (por instrucción):**
  - `NOOP`: no hace nada, devuelve `0`.
  - `SET reg valor`: escribe `valor` (parseado con `strtoul`) en `reg` vía `set_registro_32`
    (que internamente decide si es de 8 o 32 bits). Devuelve `0`.
  - `SUM/SUB dest orig`: lee ambos registros de 8 bits, suma/resta y guarda en `dest`. `0`.
  - `JNZ reg destino`: si `reg != 0`, hace `cpu->registros.pc = destino` y hace `return 0`
    **inmediatamente** (sin el `break` normal) — esto es lo que le indica al loop de `cpu.c`
    que el PC ya fue modificado y no debe incrementarse.
  - `MOV_IN reg`: lee `tamanio_registro(reg)` bytes desde `cpu->registros.si` (dirección
    lógica origen, registro `SI`), traduce con `mmu_traducir`, si falla manda `SEG_FAULT` y
    `return 1`; si no, `leer_memoria_fisica` y guarda el valor en `reg`.
  - `MOV_OUT reg`: análogo pero escribe el valor de `reg` en `cpu->registros.di` (dirección
    lógica destino).
  - `COPY_MEM tamanio_reg`: copia `tamanio` bytes de `SI` a `DI` (ambos traducidos por
    separado con `mmu_traducir`), leyendo a un buffer intermedio y volviendo a escribir.
    Si cualquiera de las dos traducciones o de las dos operaciones físicas falla, `SEG_FAULT`.
  - `SLEEP tiempo`: manda `enviar_syscall_sleep` y `return 1` (libera la CPU, el proceso se
    bloquea en el KS).
  - `EXIT`: `enviar_syscall_exit`, `return 1`.
  - `STDIN dir tam` / `STDOUT dir tam`: valida el rango con `mmu_traducir` (solo para
    detectar SEG_FAULT temprano — la traducción real la vuelve a hacer KM al momento de
    escribir/leer, por si hubo compactación entretanto, según el comentario del código),
    manda la syscall correspondiente con la dirección **lógica** y `return 1`.
  - `MUTEX_CREATE/LOCK/UNLOCK nombre`: manda la syscall correspondiente con el nombre del
    mutex, `return 1`.
  - `MEM_ALLOC id tam` / `MEM_FREE id`: manda la syscall correspondiente, `return 1`.
  - `INIT_PROC archivo prioridad`: manda `enviar_syscall_init_proc`, `return 1`.
  - default: loguea instrucción desconocida, `return 0` (no debería ocurrir tras un decode
    exitoso).

#### `cpu_check_interrupt(cpu, logger)` — instrucciones.c:337
- **Qué hace:** Chequea de forma **no bloqueante** (`MSG_DONTWAIT`) si el KS mandó una
  `MENSAJE_INTERRUPCION` por el socket. Si la hay, manda el contexto actual de vuelta al KS y
  devuelve `true`.
- **Dónde se usa:** `cpu.c:126`, al final de cada iteración del ciclo de instrucción (solo
  si la instrucción no liberó ya la CPU).
- **Para qué:** Es el mecanismo de **desalojo**: cuando el KS decide que se venció el
  quantum (Round Robin) o que necesita desalojar CPUs para compactar memoria, manda
  `MENSAJE_INTERRUPCION` de forma asíncrona; la CPU la detecta acá entre instrucción e
  instrucción (no puede interrumpir a mitad de una instrucción, es "cooperativo" a nivel
  instrucción).
- **Cómo funciona:** `recv` con `MSG_DONTWAIT` — si no hay datos, devuelve `0` bytes leídos
  inmediatamente sin bloquear. Si `bytes > 0` y el código es `MENSAJE_INTERRUPCION`, llama
  `enviar_contexto_a_ks` (que además manda de vuelta ese mismo código como parte del
  protocolo) y devuelve `true`.

---

### `registros_cpu/registros_cpu.c` — banco de registros

Abstrae el acceso a los 11 registros de la CPU (`AX/BX/CX/DX` de 8 bits;
`EAX/EBX/ECX/EDX/SI/DI/PC` de 32 bits) por nombre de string, para que `instrucciones.c` no
tenga que hacer `switch` sobre nombres en cada instrucción.

#### `get_registro(cpu, nombre)` — registros_cpu.c:6
- **Qué hace:** Devuelve el valor (8 bits) del registro `AX/BX/CX/DX` cuyo nombre coincide.
- **Dónde se usa:** `instrucciones.c` en `INST_SUM` (:157, :158) e `INST_SUB` (:164, :165).
- **Para qué:** Acceso genérico a registros de 8 bits para aritmética simple.
- **Cómo funciona:** Cadena de `strcmp`; si no matchea ninguno devuelve `0` (silencioso, no
  hay manejo de error — asume que el decode ya garantizó un nombre válido).

#### `set_registro(cpu, nombre, valor)` — registros_cpu.c:14
- **Qué hace:** Escribe un valor de 8 bits en `AX/BX/CX/DX`.
- **Dónde se usa:** `instrucciones.c` en `INST_SUM` (:159) e `INST_SUB` (:166).
- **Para qué:** Contraparte de escritura de `get_registro`.
- **Cómo funciona:** Igual patrón de `strcmp` + `return` temprano por cada match.

#### `get_registro_32(cpu, nombre)` — registros_cpu.c:21
- **Qué hace:** Devuelve el valor de cualquier registro (32 bits nativos para
  `EAX/EBX/ECX/EDX/SI/DI/PC`, o el valor de 8 bits promovido a `uint32_t` para `AX/BX/CX/DX`).
- **Dónde se usa:** `instrucciones.c` en `INST_JNZ` (:171), `INST_MOV_OUT` (:204), `INST_COPY_MEM`
  (:222), `INST_IO_STDIN` (:272, :273), `INST_IO_STDOUT` (:287, :288).
- **Para qué:** Acceso uniforme a registros de 32 bits, usado para direcciones lógicas
  (que siempre son de 32 bits) y para el valor a comparar en `JNZ`.
- **Cómo funciona:** Cadena de `strcmp` cubriendo los 7 registros de 32 bits primero, y como
  fallback también los de 8 bits (por si se pide un `AX` como 32 bits, devuelve el valor
  promovido). Si no matchea nada, `0`.

#### `set_registro_32(cpu, nombre, valor)` — registros_cpu.c:44
- **Qué hace:** Escribe un valor en cualquier registro; si es uno de 8 bits, trunca con
  `(uint8_t) valor`.
- **Dónde se usa:** `instrucciones.c` en `INST_SET` (:152) y en `cpu.c` indirectamente vía
  `INST_MOV_IN` (instrucciones.c:197).
- **Para qué:** Contraparte de escritura de `get_registro_32`; usado sobre todo por `SET`
  (que puede escribir **cualquier** registro incluyendo `PC`, lo cual es lo que permite que
  una instrucción `SET PC <n>` actúe como un salto absoluto) y por `MOV_IN` para guardar el
  valor leído de memoria.
- **Cómo funciona:** Mismo patrón `strcmp` en cadena, con truncamiento a `uint8_t` solo en
  las 4 ramas de los registros chicos.

#### `tamanio_registro(nombre)` — registros_cpu.c:36
- **Qué hace:** Devuelve `1` si el registro es `AX/BX/CX/DX`, o `4` para cualquier otro
  (asume que es uno de los de 32 bits, sin validar el nombre).
- **Dónde se usa:** `instrucciones.c` en `INST_MOV_IN` (:181) e `INST_MOV_OUT` (:202), para
  saber cuántos bytes leer/escribir de memoria según el registro destino/origen.
- **Para qué:** El tamaño de la operación de memoria en `MOV_IN`/`MOV_OUT` depende del
  ancho del registro involucrado (1 byte si es `AX`..`DX`, 4 bytes si es `EAX`..`PC`).
- **Cómo funciona:** Comparación directa de 4 nombres; si no matchea ninguno, asume `4`
  (fallback implícito, no hay rama de error explícita).

---

### `conexiones/conexiones.c` — sockets: KS, KM y sticks

El archivo más grande del módulo (319 líneas). Contiene el handshake con KS y KM, todas las
`enviar_syscall_*`, el manejo de contexto con KM, y todo el ruteo multi-stick.

#### `conectar_a_kernel_scheduler(ip, puerto, id_cpu, logger)` — conexiones.c:12
- **Qué hace:** Abre conexión TCP al KS, manda el handshake `HANDSHAKE_CPU` y el `id_cpu`.
- **Dónde se usa:** `main.c:55`. *(Existe una función homónima en `io/`, con firma distinta —
  módulo separado, no relacionada.)*
- **Para qué:** Establecer el canal principal CPU↔KS por el que viajan PID/despacho,
  syscalls, contexto e interrupciones.
- **Cómo funciona:** `crear_conexion` (de `utils/sockets`) → si falla, `-1`. Si conecta,
  manda dos `send`: el opcode de handshake (`int32_t`) y el `id_cpu` (`uint32_t`). No espera
  respuesta del KS en este punto (el KS simplemente registra la CPU al recibir el handshake,
  ver `kernel_scheduler/conexiones.c`).

#### `conectar_a_kernel_memory(ip, puerto, id_cpu, segment_max_size, logger)` — conexiones.c:30
- **Qué hace:** Abre conexión TCP a KM, handshake `HANDSHAKE_CPU` + `id_cpu`, y **recibe**
  de vuelta `segment_max_size` (parámetro de salida).
- **Dónde se usa:** `main.c:54`. *(Nombre repetido en `kernel_scheduler` y `memory_stick`,
  funciones distintas.)*
- **Para qué:** Canal CPU↔KM por el que viajan fetch de instrucciones, contexto (registros +
  tabla de segmentos) y topología de sticks. `segment_max_size` es indispensable para que
  `mmu_traducir` pueda calcular `num_segmento`/`desplazamiento`.
- **Cómo funciona:** A diferencia de la conexión a KS, acá sí hay un `recv` bloqueante
  (`MSG_WAITALL`) esperando el `uint32_t` de `segment_max_size` que manda KM apenas ve el
  handshake — es un intercambio síncrono de un solo valor de configuración global.

#### `enviar_syscall_a_ks(cpu, syscall_tipo, logger)` — conexiones.c:50 *(static)*
- **Qué hace:** Helper común a **todas** las `enviar_syscall_*`: incrementa el PC, manda
  `MENSAJE_SYSCALL` + el tipo de syscall.
- **Dónde se usa:** *(static)* Llamada desde las 10 funciones `enviar_syscall_sleep/exit/
  seg_fault/stdin/stdout/mutex_create/mutex_lock/mutex_unlock/mem_alloc/mem_free/init_proc`
  (11 en total contando `init_proc`), cada una en este mismo archivo.
- **Para qué:** Evitar repetir 3 líneas idénticas en cada syscall.
- **Cómo funciona:** El incremento de PC ocurre **antes** de mandar nada — el comentario en
  el código explica por qué: si al reanudar el proceso el PC apuntara todavía a la
  instrucción de la syscall, se re-ejecutaría en loop infinito. Solo después manda el opcode
  y el tipo de syscall; cada función caller manda los datos adicionales específicos después
  de esta llamada.

#### `enviar_syscall_sleep(cpu, tiempo, logger)` — conexiones.c:64
- **Qué hace:** Manda `SYSCALL_SLEEP` + `tiempo` + el struct completo de registros.
- **Dónde se usa:** `instrucciones.c:262`, disparada por la instrucción `SLEEP <tiempo>`.
- **Para qué:** Pedirle al KS que bloquee el proceso por `tiempo` (probablemente asociado a
  una IO tipo `SLEEP` configurada en el KS).
- **Cómo funciona:** `enviar_syscall_a_ks` + 2 `send` adicionales (tiempo y registros). El
  envío de los registros es necesario porque el KS debe persistir el contexto actualizado del
  proceso (PC, registros de datos) para poder reanudarlo más tarde en cualquier CPU libre.

#### `enviar_syscall_exit(cpu, logger)` — conexiones.c:71
- **Qué hace:** Manda `SYSCALL_EXIT` sin datos adicionales.
- **Dónde se usa:** `instrucciones.c:267`, disparada por la instrucción `EXIT`.
- **Para qué:** Avisa al KS que el proceso terminó voluntariamente, para que lo pase a EXIT
  y libere su memoria en KM.
- **Cómo funciona:** Solo llama al helper — no manda registros porque el proceso no se va a
  reanudar.

#### `enviar_syscall_seg_fault(cpu, logger)` — conexiones.c:75
- **Qué hace:** Manda `SYSCALL_SEG_FAULT` sin datos adicionales.
- **Dónde se usa:** Las 9 ramas de fallo de MMU/física en `instrucciones.c` (`INST_MOV_IN`,
  `INST_MOV_OUT`, `INST_COPY_MEM` ×2, `INST_IO_STDIN`, `INST_IO_STDOUT`, y los `!ok` de
  lectura/escritura física dentro de `COPY_MEM`).
- **Para qué:** Notificar al KS que el proceso hizo un acceso inválido a memoria, para que lo
  finalice con motivo `SEG_FAULT`.
- **Cómo funciona:** Igual patrón; el comentario aclara que **no** importa que el PC ya se
  haya incrementado en el helper porque el proceso va a terminar de todos modos, se mantiene
  el mismo esquema de mensaje por consistencia del protocolo.

#### `enviar_syscall_stdin(cpu, dir_logica, tam, logger)` / `enviar_syscall_stdout(...)` — conexiones.c:81, :89
- **Qué hace:** Mandan `SYSCALL_STDIN`/`SYSCALL_STDOUT` + dirección lógica + tamaño +
  registros completos.
- **Dónde se usa:** `instrucciones.c:282` (`INST_IO_STDIN`) y `:294` (`INST_IO_STDOUT`).
- **Para qué:** Pedir al KS una operación de E/S (lectura/escritura) sobre una dirección de
  memoria del proceso — el KS coordina con un dispositivo `io` conectado.
- **Cómo funciona:** Se manda la dirección **lógica**, no la física — es KM quien vuelve a
  traducir al momento real de leer/escribir (por si hubo compactación entre que la CPU validó
  con `mmu_traducir` y que el KS efectivamente ejecuta la E/S).

#### `enviar_contexto_a_ks(cpu, logger)` — conexiones.c:97
- **Qué hace:** Manda `MENSAJE_INTERRUPCION` + el struct de registros completo.
- **Dónde se usa:** `instrucciones.c:343`, dentro de `cpu_check_interrupt`, cuando la CPU
  detecta que el KS pidió desalojarla.
- **Para qué:** Devolver el contexto actualizado del proceso desalojado, para que el KS lo
  pueda reencolar en READY y despachar en cualquier otra CPU más tarde.
- **Cómo funciona:** El comentario explica un detalle de protocolo importante: el KS, del
  lado de `atender_peticiones_cpu`, espera primero leer el código `MENSAJE_INTERRUPCION` y
  después el struct de registros — si se mandaran solo los registros sin el código antes, el
  parser del KS quedaría desincronizado permanentemente (leería bytes de registros como si
  fueran un código de mensaje).

#### `enviar_syscall_mutex_create/lock/unlock(cpu, nombre_mutex, logger)` — conexiones.c:106, :115, :124
- **Qué hace:** Mandan la syscall correspondiente + longitud del nombre + el nombre (sin
  null-terminator, se manda por longitud explícita) + registros.
- **Dónde se usa:** `instrucciones.c:299, :303, :307` (`INST_MUTEX_CREATE/LOCK/UNLOCK`).
- **Para qué:** Sincronización entre procesos de usuario vía mutexes nombrados administrados
  por el KS (`mutex_kernel.c`).
- **Cómo funciona:** Mismo patrón de longitud-prefijada para strings de tamaño variable
  (consistente con cómo se envían strings en todo el protocolo del proyecto).

#### `enviar_syscall_mem_alloc(cpu, id_segmento, tamanio, logger)` / `enviar_syscall_mem_free(cpu, id_segmento, logger)` — conexiones.c:133, :141
- **Qué hace:** Mandan `SYSCALL_MEM_ALLOC` (+ id + tamaño) o `SYSCALL_MEM_FREE` (+ id), más
  registros en ambos casos.
- **Dónde se usa:** `instrucciones.c:313, :318` (`INST_MEM_ALLOC/FREE`).
- **Para qué:** Pedir al KS que gestione la creación/eliminación de un segmento de memoria
  del proceso (el KS reenvía a KM vía `MENSAJE_CREAR_SEGMENTO`/`MENSAJE_ELIMINAR_SEGMENTO`).
- **Cómo funciona:** Igual patrón que las demás; nótese que `mem_free` no manda tamaño (no
  hace falta, KM ya sabe el tamaño del segmento por su id).

#### `enviar_syscall_init_proc(cpu, archivo, prioridad, logger)` — conexiones.c:148
- **Qué hace:** Manda `SYSCALL_INIT_PROC` + longitud y contenido del path del archivo +
  prioridad + registros.
- **Dónde se usa:** `instrucciones.c:324` (`INST_INIT_PROC`).
- **Para qué:** Permite que un proceso de usuario cree **otro** proceso (fork/spawn a nivel
  del simulador), indicando el archivo de instrucciones y la prioridad inicial.
- **Cómo funciona:** Mismo patrón longitud-prefijada para el string variable, más el
  `int32_t` de prioridad.

#### `solicitar_contexto_a_km(cpu, logger)` — conexiones.c:158
- **Qué hace:** Pide a KM el contexto completo del proceso actual: registros + tabla de
  segmentos.
- **Dónde se usa:** `cpu.c:76`, justo después de recibir un PID nuevo del KS, antes de
  arrancar el ciclo fetch/decode/execute.
- **Para qué:** La CPU no persiste estado entre despachos (`cpu_destroy`/reset implícito de
  registros no ocurre, pero cada proceso nuevo necesita SU contexto, no el del anterior) —
  este es el mecanismo por el cual la CPU "carga" el proceso antes de ejecutarlo.
- **Cómo funciona:** Manda `MENSAJE_CONTEXTO` + `pid_actual`. Recibe el struct de registros
  completo (`MSG_WAITALL`, tamaño fijo), luego un `uint32_t cant_segmentos`, y por cada uno
  recibe un `t_segmento` completo y lo agrega a `cpu->tabla_segmentos` (después de limpiar la
  tabla anterior con `list_clean_and_destroy_elements`). Esta tabla es la que después usa
  `mmu_traducir` para validar accesos.

#### `buscar_stick_por_id(cpu, id)` — conexiones.c:182 *(static)*
- **Qué hace:** Busca en `cpu->sticks` un `t_stick_cpu` con ese `id`.
- **Dónde se usa:** *(static)* Solo dentro de `actualizar_topologia_sticks` (:213), para
  evitar reconectarse a un stick ya conocido.
- **Para qué:** Idempotencia de la actualización de topología.
- **Cómo funciona:** Recorrido lineal de la lista, `O(n)` en cantidad de sticks (n pequeño en
  la práctica).

#### `actualizar_topologia_sticks(cpu, logger)` — conexiones.c:190
- **Qué hace:** Le pide a KM la lista completa de memory sticks conocidos (id, tamaño, base
  global, ip, puerto) y, para cada uno que la CPU **todavía no conoce**, abre una conexión
  TCP directa y hace handshake `HANDSHAKE_CPU`.
- **Dónde se usa:** `main.c:62` (al arrancar), `cpu.c:75` (en cada despacho de PID, por si
  se agregó un stick nuevo desde el último proceso ejecutado), y `conexiones.c:287` (dentro
  de `operar_fisico`, como reintento único si una dirección física no matchea ningún stick
  conocido — cubre el caso de que KM haya registrado un stick nuevo a mitad de ejecución).
- **Para qué:** Es el mecanismo por el cual la CPU **descubre dinámicamente** todos los
  memory sticks del sistema, sin necesidad de conocerlos de antemano por config — el diseño
  documentado en `DIVISION_CP3.md` establece que "KM define la topología, la CPU la conoce".
- **Cómo funciona:** Manda `MENSAJE_TOPOLOGIA_STICKS`, recibe `cant` (cantidad de sticks) y
  por cada uno recibe `id/tamanio/base` (3 `uint32_t`) más `ip` y `puerto` como strings
  longitud-prefijados. Si el `id` ya está en `cpu->sticks` (`buscar_stick_por_id`), libera los
  strings recién recibidos y sigue (ya está conectado, no reconecta). Si es nuevo, abre
  conexión con `crear_conexion`, hace handshake y agrega un `t_stick_cpu` a la lista `sticks`
  de la CPU.

#### `stick_para_direccion(cpu, dir)` — conexiones.c:246 *(static)*
- **Qué hace:** Dado una dirección física **global**, busca en `cpu->sticks` cuál stick la
  contiene, comparando `dir` contra el rango `[stick->base, stick->base + stick->tamanio)`.
- **Dónde se usa:** *(static)* Solo dentro de `operar_fisico` (:284).
- **Para qué:** Es el paso clave del ruteo multi-stick: traduce "dirección física global" a
  "qué stick, y en qué offset local dentro de él".
- **Cómo funciona:** Recorrido lineal por rango; devuelve `NULL` si ninguno matchea (caso
  manejado en `operar_fisico` con un reintento de refresco de topología).

#### `stick_operar(stick, escribir, dir_local, tamanio, buffer)` — conexiones.c:257 *(static)*
- **Qué hace:** Ejecuta una lectura o escritura **contra un único stick puntual**, usando su
  conexión directa (`stick->fd`).
- **Dónde se usa:** *(static)* Solo dentro de `operar_fisico` (:300), una vez por cada tramo
  de la operación que cae dentro de ese stick.
- **Para qué:** Unidad atómica de comunicación con un stick — el protocolo real de
  lectura/escritura física a nivel de un solo nodo de memoria.
- **Cómo funciona:** Manda `MENSAJE_ESCRIBIR_MEMORIA` o `MENSAJE_LEER_MEMORIA` + dirección
  local (ya traducida a offset dentro del stick, no global) + tamaño; si es escritura, manda
  también el buffer. Espera `MENSAJE_OK` de respuesta (si no llega o es distinto, `false`);
  si es lectura, además recibe el contenido en `buffer`.

#### `operar_fisico(cpu, escribir, dir_fisica, tamanio, buffer, logger)` — conexiones.c:277 *(static)*
- **Qué hace:** Ejecuta una operación de lectura/escritura física de `tamanio` bytes desde
  `dir_fisica`, **partiéndola automáticamente** si cruza el límite entre dos (o más) sticks, y
  consolidando el resultado como si fuera una sola operación.
- **Dónde se usa:** *(static)* Llamada por `leer_memoria_fisica` (:314) y
  `escribir_memoria_fisica` (:318), que son las funciones públicas usadas desde
  `instrucciones.c` en `INST_MOV_IN` (:191), `INST_MOV_OUT` (:212) e `INST_COPY_MEM` (:241, :245).
- **Para qué:** Es el corazón del requisito "la CPU rutea cada dirección física al stick
  correcto, partiendo el pedido si cruza el borde de un stick" de la arquitectura del
  proyecto (ver `DIVISION_CP3.md` sección 3).
- **Cómo funciona:**
  1. Mantiene `restante` (bytes que faltan), `dir` (dirección global actual, avanza) y
     `cursor` (puntero al buffer, avanza).
  2. Loop `while (restante > 0)`:
     - Busca el stick para `dir` con `stick_para_direccion`.
     - Si no lo encuentra y **todavía no reintentó**, refresca la topología
       (`actualizar_topologia_sticks`) una vez y vuelve a intentar (`continue`) — cubre el
       caso de un stick recién conectado que la CPU aún no conocía.
     - Si sigue sin encontrarlo tras el reintento, error definitivo (`false`).
     - Calcula `dir_local = dir - stick->base` y `disponible = stick->tamanio - dir_local`
       (cuánto espacio queda en ESE stick desde `dir_local` hasta su borde).
     - `tramo = min(restante, disponible)` — el tamaño de este sub-pedido: o se completa la
       operación entera si entra en el stick, o se corta justo en el borde.
     - Llama `stick_operar` para ese tramo; si falla, corta con error.
     - Avanza `dir`, `cursor` y decrementa `restante` en `tramo`.
  3. El resultado es transparente para el caller: para el proceso de usuario, una lectura de
     100 bytes que cruza 2 sticks se ve como una sola operación exitosa (o fallida) — la
     partición ocurre solo a nivel interno de esta función.

#### `leer_memoria_fisica(cpu, dir_fisica, tamanio, destino, logger)` / `escribir_memoria_fisica(...)` — conexiones.c:313, :317
- **Qué hace:** Wrappers públicos de `operar_fisico` con `escribir=false`/`true`.
- **Dónde se usa:** `instrucciones.c` en `INST_MOV_IN` (:191), `INST_MOV_OUT` (:212),
  `INST_COPY_MEM` (:241 lectura, :245 escritura).
- **Para qué:** API limpia hacia `instrucciones.c`, que no necesita saber nada del ruteo
  multi-stick — solo pide "leé/escribí N bytes desde/hacia esta dirección física".
- **Cómo funciona:** Delegan 1:1 a `operar_fisico`.
