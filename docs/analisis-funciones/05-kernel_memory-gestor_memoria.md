# Módulo kernel_memory — gestor_memoria.c (núcleo de administración de memoria)

Archivos analizados:
- `kernel_memory/src/gestor_memoria/gestor_memoria.c` (988 líneas)
- `kernel_memory/src/gestor_memoria/gestor_memoria.h` (147 líneas)

Este es el módulo más denso de todo el TP. Administra la memoria física global del
sistema (repartida en N Memory Sticks), la segmentación pura de cada proceso, la
compactación, y el mediano plazo (SWAP). Todo el estado vive en `t_gestor_memoria`,
protegido por tres mutexes independientes (memoria, notificaciones, swap) para no
serializar operaciones que no se pisan entre sí.

---

## Estructuras clave (`gestor_memoria.h`)

### `t_stick`
Representa un Memory Stick conectado y registrado.
- `id`: índice asignado en orden de registro (0, 1, 2…).
- `tamanio`: bytes que aporta este stick.
- `base`: dirección física global donde arranca su rango — `[base, base+tamanio)`.
- `ip` / `puerto`: para reconectar/identificar.
- `fd_datos`: socket **KM → stick** abierto para lecturas/escrituras (canal de datos, distinto del socket por el que el stick se conectó inicialmente a KM).
- `conectado`: `false` si se cayó (memoria corrupta), sin borrar el registro (así se preserva el rango de direcciones ya asignado a procesos, aunque ya no se pueda operar).

### `t_segmento_fisico`
Un segmento ocupado en la memoria global (una entrada de la lista `gm->segmentos`).
`pid` + `id_segmento` lo identifican; `base` + `tamanio` delimitan su rango físico.
Es el espejo "vista global" de lo que cada proceso tiene también en su propia
`tabla_segmentos` (en `t_proceso_km->contexto`).

### `t_segmento_swap`
Un segmento que fue movido a SWAP porque su proceso se suspendió. `bloques` es una
`t_list` de `uint32_t*` con los números de bloque de SWAP que lo contienen, **en
orden** (bloque 0 de la lista = primeros `swap_block_size` bytes del segmento, etc.).
Un bloque nunca se comparte entre dos segmentos, aunque eso desperdicie el resto del
último bloque de cada segmento (ver `gestor_memoria_suspender_proceso`).

### `t_resultado_crear_segmento` (enum)
`CREAR_SEGMENTO_OK` / `CREAR_SEGMENTO_SIN_ESPACIO` / `CREAR_SEGMENTO_NECESITA_COMPACTACION`.
Esta última es la señal que usa `kernel_memory_atencion_scheduler.c` para disparar el
flujo de compactación (ver diagrama 3 en `DOCUMENTACION_FUNCIONES.md`).

### `t_gestor_memoria`
El objeto central:
- `sticks` (`t_list<t_stick*>`), `segmentos` (`t_list<t_segmento_fisico*>`), `memoria_total`, protegidos por `mutex`.
- `fd_notif_ks` + `mutex_notif`: canal asíncrono KM→KS para avisar `MENSAJE_MAS_MEMORIA` / `MENSAJE_MEMORIA_CORRUPTA` sin depender del hilo que atiende pedidos del KS.
- `fd_swap`, `swap_block_size`, `swap_cant_bloques`, `swap_bloques_usados` (bitmap), `segmentos_swap`, protegidos por `mutex_swap`: todo el estado de SWAP vive acá, no en un gestor separado.
- `config`, `gestor_procesos`, `logger`: referencias compartidas (no ownership).

**Por qué 3 mutexes y no uno solo:** una suspensión (que toca `mutex` y `mutex_swap`
en secuencia, nunca ambos a la vez) no bloquea una notificación al KS que pueda
dispararse en simultáneo, y viceversa. Es un diseño consciente de menor contención,
no accidental.

---

## Ciclo de vida / notificaciones

#### `gestor_memoria_create(config, gestor_procesos, logger)` — gestor_memoria.c:16
- **Qué hace:** reserva e inicializa un `t_gestor_memoria` vacío: listas vacías, `memoria_total=0`, `fd_notif_ks=-1`, `fd_swap=-1`, tres mutexes inicializados.
- **Dónde se usa:** `kernel_memory/src/main.c:51`, una sola vez al arrancar KM.
- **Para qué:** punto de entrada del módulo; todo el resto de las funciones reciben el puntero que devuelve.
- **Cómo funciona:** `malloc` + inicialización campo a campo, sin validar `malloc == NULL` salvo en el primer chequeo (si falla, retorna `NULL` y el resto de la inicialización directamente no ocurre — no hay chequeo de nulidad después, así que un `gm` inconsistente nunca llega a usarse porque el proceso moriría antes en `main.c` al no poder alocar).

#### `gestor_memoria_destroy(gm)` — gestor_memoria.c:55
- **Qué hace:** libera las tres listas (con sus elementos vía destructores dedicados), cierra el fd de SWAP si estaba abierto, libera el bitmap y destruye los tres mutexes.
- **Dónde se usa:** `kernel_memory/src/main.c:62`, al finalizar KM (fin de `main`).
- **Para qué:** liberar memoria de forma prolija al apagar el módulo (no es crítico en un proceso que termina, pero es buena práctica y ayuda a detectar leaks con herramientas como Valgrind).
- **Cómo funciona:** usa `list_destroy_and_destroy_elements` con `stick_destroy` / `free` / `segmento_swap_destroy` como destructor de cada lista — no hay loops manuales.

#### `stick_destroy(elemento)` *(static)* — gestor_memoria.c:41
- **Qué hace:** cierra `fd_datos` si está abierto y libera `ip`, `puerto` y el propio struct.
- **Dónde se usa:** único caller: `gestor_memoria_destroy` (como destructor pasado a `list_destroy_and_destroy_elements`).
- **Para qué:** evitar fd leaks al destruir el gestor.
- **Cómo funciona:** trivial, sin locks (se asume que ya no hay concurrencia activa en el momento de destrucción).

#### `segmento_swap_destroy(elemento)` *(static)* — gestor_memoria.c:49
- **Qué hace:** libera la lista `bloques` (con sus `uint32_t*`) y el struct del segmento swapeado.
- **Dónde se usa:** `gestor_memoria_destroy` (destructor de `segmentos_swap`), y directamente en `gestor_memoria_finalizar_proceso:370` y `gestor_memoria_dessuspender_proceso:836` cuando un segmento swapeado deja de existir (se liberó el proceso, o se restauró a memoria).
- **Para qué:** liberar correctamente la estructura anidada (lista dentro de struct).
- **Cómo funciona:** `list_destroy_and_destroy_elements(seg->bloques, free)` + `free(seg)`.

#### `gestor_memoria_set_notif_ks(gm, fd)` — gestor_memoria.c:72
- **Qué hace:** registra el fd del canal de notificaciones asíncronas hacia el KS.
- **Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:114`, cuando un cliente hace handshake con `HANDSHAKE_SCHEDULER_NOTIF` (canal secundario, distinto del canal de comandos KS→KM).
- **Para qué:** darle a `notificar_ks` un fd válido por el cual mandar avisos que el KS no pidió explícitamente (más memoria disponible, memoria corrupta).
- **Cómo funciona:** simple asignación protegida por `mutex_notif` (para que no se lea a medio escribir desde otro hilo).

#### `notificar_ks(gm, codigo, payload)` *(static)* — gestor_memoria.c:78
- **Qué hace:** envía por el canal de notificaciones un `t_codigo_mensaje` y, si se pasa, un `uint32_t` de payload.
- **Dónde se usa:** dentro del archivo: `gestor_memoria_registrar_stick` (con `MENSAJE_MAS_MEMORIA`) y `gestor_memoria_stick_desconectado` (con `MENSAJE_MEMORIA_CORRUPTA`, sin payload).
- **Para qué:** es el único punto de salida hacia el canal asíncrono — evita duplicar la lógica de lock+send en cada lugar que necesita avisar algo.
- **Cómo funciona:** toma `mutex_notif`, chequea que `fd_notif_ks != -1` (si el KS todavía no abrió ese canal, el aviso simplemente se descarta silenciosamente — no se encola ni reintenta), manda el código y opcionalmente el payload, libera el lock. No espera respuesta (fire-and-forget).

---

## Memory sticks / topología

#### `gestor_memoria_registrar_stick(gm, tamanio, ip, puerto)` — gestor_memoria.c:93
- **Qué hace:** abre el canal de datos KM→stick, hace handshake `HANDSHAKE_KERNEL_MEMORY`, crea el `t_stick`, le asigna `id` y `base` (al final de la memoria actual) y **amplía `memoria_total`**. Notifica al KS que hay más memoria.
- **Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:76`, cuando un stick se conecta a KM y manda `MENSAJE_INFORMAR_TAMANIO` (el stick es cliente en este primer contacto).
- **Para qué:** integrar dinámicamente sticks a la memoria total del sistema — el sistema soporta que se agreguen sticks después de arrancar.
- **Cómo funciona:** primero abre la conexión **antes** de tomar el lock (evita bloquear el resto del gestor mientras espera el I/O de red, que puede demorar). Recién adentro del lock calcula `id = list_size(sticks)` (el próximo índice libre) y `base = memoria_total` (el nuevo stick se apila siempre al final del espacio de direcciones ya asignado, nunca se insertan sticks "en el medio"). Esto es importante: **la base de un stick es fija una vez asignada** y no se recalcula si otro stick se desconecta después. Al final, fuera del lock (para no notificar con el mutex tomado), llama a `notificar_ks` con `MENSAJE_MAS_MEMORIA`. Si `crear_conexion` falla, loguea error y devuelve `-1` sin tocar ninguna estructura.

#### `gestor_memoria_stick_desconectado(gm, id_stick)` — gestor_memoria.c:127
- **Qué hace:** busca el stick por id, si está conectado lo marca `conectado=false`, cierra su fd y dispara `MENSAJE_MEMORIA_CORRUPTA` al KS.
- **Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:86`, en el hilo que atiende a un memory stick, cuando `recv` detecta que el socket se cerró (desconexión inesperada).
- **Para qué:** es el disparador de la política "memoria corrupta = BSOD" — si un stick con datos de procesos vivos desaparece, ya no se puede garantizar la integridad de esa memoria, así que hay que abortar todo.
- **Cómo funciona:** recorre linealmente `gm->sticks` buscando `stick->id == id_stick`. Nota de diseño: **libera el lock antes de loguear y notificar** (`pthread_mutex_unlock` en la línea 137, antes del `log_error`/`notificar_ks`), para no tener el mutex de memoria tomado mientras se hace I/O de red hacia el KS. Si el stick ya estaba marcado como no conectado (doble desconexión) o no se encuentra, no hace nada (evita notificar dos veces por el mismo stick).

#### `stick_destroy` — ver arriba (Ciclo de vida).

#### `stick_para_direccion(gm, dir)` *(static)* — gestor_memoria.c:385
- **Qué hace:** dada una dirección física global, devuelve el `t_stick*` cuyo rango `[base, base+tamanio)` la contiene, o `NULL` si está fuera de rango.
- **Dónde se usa:** dentro del archivo — `operar_fisico` y `copiar_segmento_entre_sticks` (usado en `mover_segmento`, o sea durante compactación).
- **Para qué:** es la pieza central del ruteo multi-stick: cada vez que hay que tocar una dirección física, primero hay que saber a qué stick pertenece.
- **Cómo funciona:** recorrido lineal O(n_sticks) sin asumir orden particular en la lista (aunque en la práctica los sticks se agregan en orden de `base` creciente porque `registrar_stick` siempre apila al final). **Debe llamarse con `gm->mutex` tomado** — no toma el lock internamente, lo asume del caller (evita locks anidados/relocks).

#### `stick_operar(gm, stick, escribir, dir_local, tamanio, buffer)` *(static)* — gestor_memoria.c:396
- **Qué hace:** ejecuta una única lectura o escritura contra **un** stick puntual, por su `fd_datos`, con una dirección ya traducida a local (offset dentro de ese stick).
- **Dónde se usa:** `operar_fisico` (dentro del loop que reparte tramos entre sticks) y `copiar_segmento_entre_sticks` (durante compactación).
- **Para qué:** encapsula el protocolo de mensaje puntual con el stick (código + dirección + tamaño [+ datos si es escritura], y la respuesta `MENSAJE_OK` [+ datos si es lectura]).
- **Cómo funciona:** si el stick no está `conectado` o su fd es `-1`, devuelve `false` de entrada (evita mandar a un socket cerrado). Arma el mensaje según `escribir` (`MENSAJE_ESCRIBIR_MEMORIA` o `MENSAJE_LEER_MEMORIA`), manda dirección y tamaño, y si es escritura manda también el buffer. Espera la respuesta con `recv(..., MSG_WAITALL)` — **bloquea hasta tener todos los bytes** de la respuesta o hasta que se corte la conexión. Si la respuesta no es `MENSAJE_OK`, o si algún `recv`/`send` falla (devuelve `<= 0`), retorna `false`. Si es lectura y todo salió bien, además recibe el bloque de datos hacia `buffer`.

#### `operar_fisico(gm, escribir, dir_fisica, tamanio, buffer)` *(static)* — gestor_memoria.c:419
- **Qué hace:** ejecuta una operación de lectura/escritura física que puede abarcar **más de un stick**, partiéndola en tantos tramos como sticks cruce, y consolidando el resultado.
- **Dónde se usa:** único punto de entrada de `gestor_memoria_leer_fisico` y `gestor_memoria_escribir_fisico` (son wrappers de una línea sobre esta función).
- **Para qué:** implementar el requisito de la consigna de que una operación que cruza el límite de un stick se trate como **una sola operación consolidada** desde el punto de vista del que la pidió.
- **Cómo funciona:** toma `gm->mutex` **una sola vez para toda la operación completa** (no lock por tramo — así la operación es atómica respecto a, por ejemplo, una compactación concurrente que mueva sticks/segmentos a mitad de camino). Con un cursor (`dir`, `cursor` de bytes, `restante`) va iterando: en cada vuelta busca el stick de la dirección actual (`stick_para_direccion`), calcula cuánto espacio queda disponible en *ese* stick desde `dir_local` hasta su fin (`disponible_en_stick`), y el tramo de esta vuelta es el mínimo entre lo que falta (`restante`) y lo que entra en el stick. Llama a `stick_operar` para ese tramo, avanza los tres acumuladores, y repite hasta agotar `restante` o hasta que falle un tramo (`ok=false` corta el loop). Si en algún punto la dirección cae fuera de todos los sticks conocidos, loguea error y aborta. **Importante:** si un tramo intermedio falla, la operación queda parcialmente aplicada (los tramos anteriores ya se escribieron/leyeron) — no hay rollback.

#### `gestor_memoria_leer_fisico(gm, dir_fisica, tamanio, destino)` — gestor_memoria.c:450
- **Qué hace / dónde se usa:** wrapper de `operar_fisico(gm, false, ...)`. Se usa dentro del propio archivo en `gestor_memoria_suspender_proceso` (para volcar un segmento a un buffer antes de mandarlo a SWAP) y en `gestor_memoria_io_rw` (camino de lectura lógica). **No** se llama desde ningún otro módulo — la CPU no pasa por acá para leer memoria: tiene su propia copia de la topología y habla directo con los sticks (ver `cpu/src/conexiones/conexiones.c`); esta función solo cubre el camino KS→KM (syscalls STDOUT, restauración de SWAP).
- **Para qué:** punto único de lectura física consolidada multi-stick para uso interno de KM.
- **Cómo funciona:** delega 100% en `operar_fisico`.

#### `gestor_memoria_escribir_fisico(gm, dir_fisica, tamanio, origen)` — gestor_memoria.c:454
- Análoga a la anterior con `escribir=true`. Usada en `gestor_memoria_dessuspender_proceso` (para volcar de vuelta a memoria lo restaurado desde SWAP) y en `gestor_memoria_io_rw` (camino de escritura lógica, syscall STDIN).

---

## Segmentos (crear / eliminar / huecos)

#### `comparar_por_base(a, b)` *(static)* — gestor_memoria.c:150
- **Qué hace:** comparador para `qsort` de `t_segmento_fisico**` por campo `base`, ascendente.
- **Dónde se usa:** único caller `segmentos_ordenados`.
- **Cómo funciona:** resta booleana `(sa->base > sb->base) - (sa->base < sb->base)` — patrón típico para evitar overflow de restar directamente `uint32_t`.

#### `segmentos_ordenados(gm, cant_out)` *(static)* — gestor_memoria.c:158
- **Qué hace:** arma un array `malloc`eado de punteros a los segmentos ocupados, ordenado por `base`.
- **Dónde se usa:** `buscar_hueco` y `gestor_memoria_compactar`. **Debe llamarse con `gm->mutex` tomado** (no lockea, y el caller es responsable de `free()` del array devuelto, aunque no de los `t_segmento_fisico*` que apunta, que siguen siendo dueños de `gm->segmentos`).
- **Para qué:** tanto la búsqueda de huecos como la compactación necesitan recorrer los segmentos en orden espacial, no en orden de inserción (que es como los tiene `gm->segmentos`, una `t_list` sin orden garantizado).
- **Cómo funciona:** copia los punteros de la `t_list` a un array plano y aplica `qsort`. Si `cant==0` igual reserva 1 slot (`malloc(sizeof(...) * (cant>0?cant:1))`) para no llamar `malloc(0)` (comportamiento indefinido según la implementación).

#### `estrategia_es_best_fit(gm)` *(static)* — gestor_memoria.c:172
- **Qué hace:** devuelve `true` salvo que `config->allocation_strategy` sea exactamente `"WORST"`.
- **Dónde se usa:** único caller `buscar_hueco`.
- **Cómo funciona:** un solo `strcmp`. Nótese que **cualquier valor que no sea literalmente `"WORST"` cae en Best Fit por defecto** (no hay validación explícita de `"BEST"` como único valor válido alternativo) — es una decisión de robustez, no de estricta validación de config.

#### `hueco_es_mejor_que_el_actual(best_fit, hay_candidato_previo, tamanio_candidato, tamanio_mejor_actual)` *(static)* — gestor_memoria.c:180
- **Qué hace:** función de comparación pura (sin acceso a `gm`) que decide si un hueco candidato reemplaza al mejor encontrado hasta el momento.
- **Dónde se usa:** único caller `buscar_hueco`, dentro del loop de recorrido de huecos.
- **Cómo funciona:** si no había candidato previo, cualquiera sirve. Si hay, y la estrategia es Best Fit, gana el hueco **más chico que igual entra** (minimiza desperdicio interno); si es Worst Fit, gana el **más grande** (maximiza el espacio libre restante para futuros pedidos, atrasando la fragmentación).

#### `buscar_hueco(gm, tamanio, base_out)` *(static)* — gestor_memoria.c:191
- **Qué hace:** recorre el espacio de memoria completo y encuentra, según la estrategia configurada, el mejor hueco libre donde entra `tamanio` bytes.
- **Dónde se usa:** `gestor_memoria_crear_segmento` (asignación normal) y `gestor_memoria_dessuspender_proceso` (para reservar espacio a cada segmento que vuelve de SWAP). **Debe llamarse con `gm->mutex` tomado.**
- **Para qué:** es el corazón del algoritmo de asignación de memoria (Best/Worst Fit sobre segmentación pura, sin paginación).
- **Cómo funciona paso a paso:**
  1. Obtiene los segmentos ordenados por base (`segmentos_ordenados`).
  2. Recorre desde `inicio_hueco=0` hasta el final de la memoria, con **una vuelta extra** respecto a la cantidad de segmentos (`i <= cantidad_segmentos`) para poder evaluar también el hueco que queda **después del último segmento** hasta `memoria_total`.
  3. En cada vuelta, `fin_hueco` es la base del próximo segmento (o `memoria_total` en la última vuelta). Si `fin_hueco > inicio_hueco` hay un hueco real de tamaño `fin_hueco - inicio_hueco`.
  4. Si el pedido entra en ese hueco (`tamanio_hueco >= tamanio`) y es "mejor" que el candidato actual según la estrategia, se guarda como nuevo candidato (`base_elegida`, `tamanio_elegido`).
  5. Antes de pasar a la siguiente vuelta, `inicio_hueco` se adelanta hasta el final del segmento actual (`base+tamanio`) — pero solo si eso lo hace avanzar (`if (fin_segmento > inicio_hueco)`), lo cual protege contra segmentos que se solaparan (no debería pasar en un sistema correcto, pero evita retroceder el cursor).
  6. Al final libera el array temporal y, si encontró algo, escribe `base_elegida` en `*base_out` y devuelve `true`.
  - **Complejidad:** O(n log n) por el sort + O(n) del recorrido, con `n` = segmentos ocupados totales del sistema (no solo del proceso pedido). Se ejecuta con el mutex global tomado, así que es O(n) de sección crítica por cada `MEM_ALLOC`.

#### `espacio_libre_sin_lock(gm)` *(static)* — gestor_memoria.c:236
- **Qué hace:** suma el tamaño de todos los segmentos ocupados y lo resta de `memoria_total`.
- **Dónde se usa:** `gestor_memoria_espacio_libre` (wrapper público con lock) y `gestor_memoria_crear_segmento` (para decidir si el fallo de `buscar_hueco` es "no hay espacio total" o "hay espacio pero fragmentado → compactar").
- **Cómo funciona:** O(n) sobre `gm->segmentos`, sin tocar sticks ni huecos — es simplemente `total - Σtamaños ocupados`, lo que reemplazó al viejo `free_space_mock` mencionado en la documentación histórica del proyecto (ya no existe en el código).

#### `gestor_memoria_espacio_libre(gm)` — gestor_memoria.c:245
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:49`, cuando el KS pregunta cuánta memoria libre hay (usado por el planificador de largo plazo para decidir si admite un proceso `NEW→READY`).
- **Cómo funciona:** toma el lock, delega en `espacio_libre_sin_lock`, libera el lock.

#### `tabla_proceso_agregar(gm, pid, id_segmento, base, tamanio)` *(static)* — gestor_memoria.c:253
- **Qué hace:** agrega una entrada nueva a la `tabla_segmentos` del **contexto del proceso** (estructura que vive en `gestor_procesos`, no en `gestor_memoria`), calculando también `limite = base + tamanio - 1`.
- **Dónde se usa:** `gestor_memoria_crear_segmento` (alta normal) y `gestor_memoria_dessuspender_proceso` (regenerar la entrada al volver de SWAP).
- **Para qué:** mantener sincronizadas dos vistas del mismo dato — la vista global (`gm->segmentos`, para huecos/compactación) y la vista por-proceso (`tabla_segmentos`, la que consulta la CPU vía `MENSAJE_TOPOLOGIA`/contexto para su MMU local).
- **Cómo funciona:** toma el lock del **gestor de procesos** (`gestor_lock`, un mutex distinto al de `gestor_memoria`), busca el proceso sin lock adicional (`gestor_buscar_proceso_sin_lock`), y si existe agrega el nuevo `t_segmento` a su lista. Si el proceso ya no existe (condición de carrera con una finalización concurrente), simplemente no hace nada — no es un error, el segmento de un proceso muerto no necesita tabla.

#### `coincide_segmento(pid, id_segmento, elemento)` *(static)* — gestor_memoria.c:267
- **Qué hace:** predicado simple que compara `pid` e `id_segmento` de un `t_segmento_fisico*`.
- **Dónde se usa:** único caller `gestor_memoria_eliminar_segmento`, dentro del loop de búsqueda lineal.

#### `gestor_memoria_crear_segmento(gm, pid, id_segmento, tamanio)` — gestor_memoria.c:272
- **Qué hace:** intenta crear un segmento nuevo para un proceso. Devuelve uno de los tres resultados de `t_resultado_crear_segmento`.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:61` y `:78` (esta última tras una compactación, reintentando la misma creación). Responde a la syscall `SYSCALL_MEM_ALLOC` que viaja como `MENSAJE_CREAR_SEGMENTO` KS→KM.
- **Para qué:** es el punto de entrada de la asignación de memoria de usuario.
- **Cómo funciona paso a paso:**
  1. Valida `tamanio != 0` y `tamanio <= SEGMENT_MAX_SIZE` (de config). Si falla, `CREAR_SEGMENTO_SIN_ESPACIO` directo, sin tocar el lock.
  2. Toma `gm->mutex` y llama `buscar_hueco`.
  3. Si **no** hay hueco: libera el lock, mide el espacio libre total (`espacio_libre_sin_lock`, ya fuera del lock — nótese que hay una ventana de carrera teórica acá entre el `unlock` y esta medición, aceptable porque en el peor caso solo produce un mensaje de log desactualizado, no corrompe estado) y decide: si el libre total ≥ pedido, el problema es fragmentación → `CREAR_SEGMENTO_NECESITA_COMPACTACION`; si no, no hay memoria de verdad → `CREAR_SEGMENTO_SIN_ESPACIO`.
  4. Si hay hueco: crea el `t_segmento_fisico`, lo agrega a `gm->segmentos`, libera el lock, y **fuera** del lock llama a `tabla_proceso_agregar` (evita anidar el lock de memoria con el de procesos). Loguea `"## PID: X - Segmento Creado"` (log obligatorio de la cátedra) y devuelve `CREAR_SEGMENTO_OK`.

#### `gestor_memoria_eliminar_segmento(gm, pid, id_segmento)` — gestor_memoria.c:310
- **Qué hace:** libera un segmento ocupado (lo saca de `gm->segmentos`, dejando el hueco disponible para el próximo `buscar_hueco`) y de la tabla del proceso.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:92`, respondiendo `SYSCALL_MEM_FREE` (`MENSAJE_ELIMINAR_SEGMENTO`).
- **Cómo funciona:** primero busca y remueve de `gm->segmentos` bajo `mutex` (usa `coincide_segmento` en un loop lineal, corta en el primer match con `break`). Si no lo encuentra, loguea warning y devuelve `false` (caso: `MEM_FREE` de un segmento que no existe — protección contra doble free o ids inválidos). Si lo encontró, **fuera** del lock de memoria, entra al lock de procesos y quita la entrada correspondiente de `tabla_segmentos`. No hay rollback si el segmento existía en `gm->segmentos` pero no en la tabla del proceso (inconsistencia teórica que no debería darse si ambas se mantienen siempre en sincronía).

#### `gestor_memoria_finalizar_proceso(gm, pid)` — gestor_memoria.c:348
- **Qué hace:** libera **todos** los segmentos de un proceso, tanto los que están en memoria principal como los que estén en SWAP, y destruye su contexto completo.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:103`, cuando el KS avisa `MENSAJE_FINALIZAR_PROCESO` (proceso llega a EXIT, cualquiera sea el motivo).
- **Cómo funciona:** tres fases secuenciales, cada una con su propio lock (nunca dos locks tomados a la vez):
  1. Recorre `gm->segmentos` **de atrás para adelante** (`for i = size-1; i >= 0; i--`) removiendo los que matchean `pid` — recorrer en reversa es necesario porque `list_remove` reindexa la lista, y hacerlo de adelante hacia atrás saltearía elementos.
  2. Mismo patrón sobre `gm->segmentos_swap`: además de remover el segmento swapeado, libera cada uno de sus bloques en el bitmap `swap_bloques_usados` (para que vuelvan a estar disponibles para otros procesos).
  3. Llama a `gestor_remover_proceso` (del `gestor_procesos`, otro módulo) que libera el contexto, la tabla de segmentos y la lista de instrucciones del proceso.
  - Es la única función que toca **las tres** áreas de estado (memoria, swap, procesos) en una sola llamada — el "destructor total" de un proceso desde el punto de vista de KM.

---

## Acceso físico + traducción (MMU del lado memoria)

#### `gestor_memoria_traducir(gm, pid, dir_logica, tamanio, dir_fisica_out)` — gestor_memoria.c:462
- **Qué hace:** traduce una dirección lógica de proceso a dirección física global, validando que el acceso completo (`desplazamiento + tamanio`) entre dentro del segmento correspondiente.
- **Dónde se usa:** solo dentro del archivo, en `gestor_memoria_io_rw` (primer intento, antes de probar contra SWAP).
- **Para qué:** es el análogo, del lado de KM, a `mmu_traducir` de la CPU — pero la CPU normalmente traduce con su **propia copia local** de la tabla de segmentos (recibida en el contexto), así que esta función de KM se usa específicamente para el camino **KS→KM** de I/O lógica (STDIN/STDOUT), no para el ciclo de instrucción normal de la CPU.
- **Cómo funciona:** con `SEGMENT_MAX_SIZE` de config, calcula `num_segmento = dir_logica / seg_max` y `desplazamiento = dir_logica % seg_max` (esquema de segmentación con **tamaño de segmento fijo lógico** — cada "slot" lógico de `seg_max` bytes corresponde a un id de segmento). Busca en `gm->segmentos` el que matchea `pid` y `num_segmento`. Si no existe, o si `desplazamiento + tamanio` excede el tamaño real del segmento, devuelve `false` **sin loguear error** — a propósito, porque el caller (`gestor_memoria_io_rw`) interpreta el fallo como "puede estar en SWAP" y reintenta ahí antes de considerar que es un error real.

#### `gestor_memoria_leer_fisico` / `gestor_memoria_escribir_fisico` — ver sección de sticks arriba.

#### `gestor_memoria_io_rw(gm, pid, dir_logica, tamanio, buffer, escribir, dir_fisica_out)` — gestor_memoria.c:895
- **Qué hace:** resuelve una lectura/escritura lógica de un proceso, sin importar si sus datos están en memoria principal o suspendidos en SWAP.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:121` (lectura, para STDOUT) y `:150` (escritura, para STDIN) — el camino por el cual el KS pide a KM leer/escribir memoria de usuario en nombre de un dispositivo de IO.
- **Para qué:** cubre el caso borde de que un proceso se haya **suspendido a SWAP mientras esperaba una operación de E/S** — sin esta función, una IO pendiente sobre un proceso recién suspendido fallaría.
- **Cómo funciona:** primero intenta el camino normal: `gestor_memoria_traducir` + `leer/escribir_fisico`. Si la traducción falla (el segmento no está en memoria), recalcula `num_segmento`/`desplazamiento` de la misma forma y prueba `swap_io_rw` directo sobre los bloques de SWAP. Si esto último funciona, loguea la operación explícitamente indicando que fue "en SWAP" y devuelve `*dir_fisica_out = UINT32_MAX` como valor centinela (le informa al caller que no hubo una dirección física real involucrada, para que no la use en logs "dirección física" de la cátedra).

---

## Compactación

#### `copiar_segmento_entre_sticks(gm, dir_fisica, tamanio, buffer, escribir)` *(static)* — gestor_memoria.c:500
- **Qué hace:** copia `tamanio` bytes entre la memoria física (que puede abarcar varios sticks) y un buffer en RAM local del proceso KM, en cualquier dirección (lectura o escritura según `escribir`).
- **Dónde se usa:** único caller `mover_segmento`, dos veces (una para leer el segmento de su ubicación vieja, otra para escribirlo en la nueva).
- **Para qué:** es la versión "de bajo nivel" de `operar_fisico`, pero **sin tomar el lock** (asume que ya está tomado por el caller) y **sin abortar en el primer fallo de tramo** (usa `break` si `stick_para_direccion` da `NULL`, no si `stick_operar` falla — nótese esta asimetría respecto a `operar_fisico`, que sí corta ante cualquier fallo).
- **Cómo funciona:** mismo patrón de partición en tramos que `operar_fisico` (cursor + resto + tramo = mínimo entre lo que falta y lo que entra en el stick actual), pero pensado para ser llamado ya con el mutex tomado por `gestor_memoria_compactar`.

#### `actualizar_base_de_segmento_en_proceso(gm, pid, id_segmento, nueva_base, tamanio)` *(static)* — gestor_memoria.c:528
- **Qué hace:** actualiza `base` y `limite` de la entrada correspondiente en la `tabla_segmentos` del proceso dueño, tras moverlo por compactación.
- **Dónde se usa:** único caller `mover_segmento`.
- **Cómo funciona:** toma el lock de `gestor_procesos`, busca el proceso, recorre su `tabla_segmentos` buscando el `id_segmento` que matchea, y si lo encuentra reescribe `base`/`limite`. Si el proceso no existe más (se finalizó justo durante la compactación), no hace nada — de nuevo, tolerante a esa carrera.

#### `mover_segmento(gm, seg, nueva_base)` *(static)* — gestor_memoria.c:551
- **Qué hace:** reubica físicamente un segmento entero a una nueva dirección base.
- **Dónde se usa:** único caller `gestor_memoria_compactar`, para cada segmento que está desplazado respecto de donde "debería" estar tras compactar.
- **Cómo funciona:** 1) reserva un buffer temporal del tamaño del segmento, 2) lee el segmento completo desde su base actual (`copiar_segmento_entre_sticks` con `escribir=false`), 3) lo reescribe en la nueva base (con `escribir=true` — puede caer en un stick distinto al original, la función no lo distingue, simplemente sigue el ruteo normal), 4) libera el buffer, 5) actualiza la tabla del proceso dueño y 6) actualiza `seg->base = nueva_base` en el propio registro global. El **orden importa**: primero se lee todo a RAM local antes de empezar a escribir, para no pisar datos que todavía no se copiaron si el rango viejo y el nuevo se solapan.

#### `gestor_memoria_compactar(gm)` — gestor_memoria.c:566
- **Qué hace:** reordena **todos** los segmentos ocupados al principio de la memoria, en orden de `base` creciente, eliminando cualquier hueco intermedio.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:77`, tras recibir `MENSAJE_DESALOJO_OK` del KS (es decir, solo después de que el KS confirmó que **todas las CPUs fueron desalojadas** — precondición documentada explícitamente en el header: "Solo debe llamarse con las CPUs desalojadas").
- **Para qué:** resolver la fragmentación externa que hace que `buscar_hueco` no encuentre un hueco contiguo aunque haya espacio total suficiente.
- **Cómo funciona paso a paso:**
  1. Loguea `"## Inicio de compactación"` (log obligatorio) **antes** de tomar el lock.
  2. Toma el lock y obtiene los segmentos ordenados por base (mismo helper que usa `buscar_hueco`).
  3. Recorre los segmentos en ese orden manteniendo un cursor `proxima_base_libre` (arranca en 0). Para cada segmento: si su `base` actual **no coincide** con `proxima_base_libre`, está desplazado y se llama `mover_segmento` para reubicarlo ahí. Si ya coincide, no se toca (evita I/O innecesario para segmentos que ya están "bien puestos", por ejemplo el primero de la memoria si ya arranca en 0).
  4. Avanza `proxima_base_libre += seg->tamanio` — así el siguiente segmento se apila justo después, sin huecos.
  5. Libera el array temporal y el lock.
  6. **Fuera** del lock, espera `COMPACTION_DELAY` milisegundos (simula el costo de la operación — el resto de la memoria ya quedó consistente y liberada para otros usos aunque el "delay" siga corriendo, una decisión de que el delay es solo de simulación/timing, no de exclusión real).
  7. Loguea `"## Fin de compactación"`.
  - **Complejidad:** en el peor caso mueve todos los `n` segmentos, cada uno con I/O real hacia los sticks (no es una operación en memoria local, son sockets) — es cara a propósito, para que se note el costo de la fragmentación en las métricas del TP.

---

## SWAP (mediano plazo)

#### `gestor_memoria_registrar_swap(gm, fd, tamanio_total, block_size)` — gestor_memoria.c:602
- **Qué hace:** guarda el fd del módulo SWAP conectado y calcula la cantidad de bloques (`tamanio_total / block_size`), inicializando el bitmap de uso en todo `false`.
- **Dónde se usa:** `kernel_memory/src/conexiones/conexiones.c:134`, cuando el módulo SWAP se conecta a KM y le informa su tamaño total y tamaño de bloque.
- **Cómo funciona:** simple, protegido por `mutex_swap`. Si `block_size == 0` evita división por cero dejando `swap_cant_bloques=0` (SWAP quedaría inutilizable pero sin crashear).

#### `swap_escribir_bloque(gm, nro_bloque, buffer)` / `swap_leer_bloque(gm, nro_bloque, buffer)` *(static)* — gestor_memoria.c:615 / 626
- **Qué hacen:** protocolo puntual de un bloque contra el módulo SWAP — mismo patrón de mensaje que `stick_operar` pero para el fd de swap y con tamaño fijo `swap_block_size` (no variable como en los sticks).
- **Dónde se usan:** dentro del archivo, en `gestor_memoria_suspender_proceso`, `gestor_memoria_dessuspender_proceso` y `swap_io_rw`. **Deben llamarse con `mutex_swap` tomado** (no lockean internamente).
- **Cómo funcionan:** `escribir` manda código+nro_bloque+buffer completo y espera `MENSAJE_OK`; `leer` manda código+nro_bloque y recibe directo el bloque completo con `MSG_WAITALL`. A diferencia de los sticks, acá no hay noción de "conectado" chequeada (se asume que si `fd_swap != -1` el canal sigue vivo — no hay manejo explícito de caída de SWAP a mitad de operación, a diferencia de los sticks que sí tienen `stick_desconectado`/memoria corrupta).

#### `swap_bloques_libres(gm)` *(static)* — gestor_memoria.c:634
- **Qué hace:** cuenta cuántas posiciones del bitmap `swap_bloques_usados` están en `false`.
- **Dónde se usa:** único caller `gestor_memoria_suspender_proceso`, como pre-chequeo antes de empezar a mover datos.
- **Cómo funciona:** O(n) sobre el bitmap. Debe llamarse con `mutex_swap` tomado.

#### `buscar_segmento_swap(gm, pid, id_segmento)` *(static)* — gestor_memoria.c:643
- **Qué hace:** búsqueda lineal en `gm->segmentos_swap` por `(pid, id_segmento)`.
- **Dónde se usa:** único caller `swap_io_rw`.

#### `gestor_memoria_suspender_proceso(gm, pid)` — gestor_memoria.c:651
- **Qué hace:** mueve **todos** los segmentos de un proceso desde memoria principal hacia SWAP, liberando la memoria principal a medida que copia.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:170`, cuando el KS decide suspender un proceso (mediano plazo — típicamente un proceso bloqueado por demasiado tiempo, ver `hilo_timer_suspension` en `planificador.c`).
- **Cómo funciona paso a paso:**
  1. Si no hay SWAP conectado (`fd_swap == -1`), falla de entrada.
  2. Toma un **snapshot** (copia) de los segmentos del proceso bajo `gm->mutex`, liberando el lock inmediatamente después — así el resto de la función no mantiene el lock de memoria tomado durante todo el proceso (que puede ser lento, con I/O de red).
  3. **Pre-chequeo:** bajo `mutex_swap`, calcula cuántos bloques necesita en total (`ceil(tamanio_segmento / block_size)` por cada segmento, sumados) y lo compara contra `swap_bloques_libres`. Si no alcanza, aborta **sin mover nada** — todo o nada, evita dejar el proceso a medio suspender.
  4. Recién ahí, **de a un segmento por vez**: lee el segmento completo de memoria física a un buffer (`gestor_memoria_leer_fisico`), después, bajo `mutex_swap`, recorre el bitmap buscando bloques libres (`for b in 0..cant_bloques: if !usado`) y va copiando tramos de `swap_block_size` (con `memset` de padding con ceros si el último tramo es más chico que un bloque completo), escribiendo cada bloque con `swap_escribir_bloque` y marcándolo usado, guardando su número en `sw->bloques` **en orden**. El segmento swapeado se agrega a `gm->segmentos_swap`.
  5. Recién con los datos ya seguros en SWAP, se libera la memoria principal de ese segmento (bajo `gm->mutex`) y se quita su entrada de la `tabla_segmentos` del proceso (bajo el lock de `gestor_procesos`).
  6. Se repite para el siguiente segmento del snapshot.
  - **Por qué "de a un segmento":** si se cayera algo a mitad de camino, los segmentos ya procesados quedan consistentes (en SWAP, liberados de RAM) y los que faltan siguen en RAM — no hay un estado intermedio "medio segmento en RAM y medio en SWAP" salvo por el padding de ceros del último bloque parcial.

#### `gestor_memoria_dessuspender_proceso(gm, pid)` — gestor_memoria.c:760
- **Qué hace:** restaura desde SWAP todos los segmentos de un proceso suspendido, usando el mismo algoritmo de huecos (Best/Worst Fit) que una creación normal — **sin disparar compactación**.
- **Dónde se usa:** `kernel_memory_atencion_scheduler.c:181`, cuando el KS pide reactivar un proceso suspendido (por ejemplo porque se liberó memoria y hay lugar).
- **Cómo funciona paso a paso:**
  1. Snapshot de los `t_segmento_swap` del proceso bajo `mutex_swap`. Si no hay ninguno, devuelve `false` (nada que restaurar).
  2. Bajo `gm->mutex`, intenta reservar un hueco (`buscar_hueco`) para **cada** segmento, en el orden en que estaban en la lista. Si **todos** entran, se van agregando a `gm->segmentos` y a una lista auxiliar `reservados`. Si en algún punto uno no entra, `entra_todo=false` y se **revierte** todo lo reservado hasta ahí (`list_remove_element` + `free` de cada uno) antes de soltar el lock — de nuevo, todo o nada: la consigna exige que si no entran *todos* los segmentos sin compactar, la operación se rechace entera (así lo documenta el comentario del código: "si alguno no entra, se revierte lo reservado").
  3. Si no entró todo, devuelve `false` sin haber tocado nada persistente (los cambios en memoria ya fueron revertidos).
  4. Si entró todo: para cada par (segmento swapeado, segmento físico reservado), lee sus bloques de SWAP en orden (`swap_leer_bloque` por cada entrada de `sw->bloques`, concatenando en un buffer), libera esos bloques en el bitmap, escribe el buffer completo en la nueva base física (`gestor_memoria_escribir_fisico`), regenera la entrada en la `tabla_segmentos` del proceso (`tabla_proceso_agregar`) y destruye el `t_segmento_swap` ya restaurado.
  5. Loguea la cantidad de segmentos restaurados y devuelve `true`.
  - **Nota de diseño:** a diferencia de `crear_segmento`, acá **no** se contempla el caso "hay espacio total pero no contiguo → pedir compactación" — simplemente falla. Es el KS quien, al recibir `false`, decide si reintenta más tarde o mantiene al proceso suspendido.

#### `swap_io_rw(gm, pid, num_segmento, desplazamiento, tamanio, buffer, escribir)` *(static)* — gestor_memoria.c:852
- **Qué hace:** lee o escribe directamente sobre los bloques de SWAP de un segmento suspendido, sin traerlo de vuelta a memoria — cubre IO sobre un proceso que se suspendió mientras esperaba esa misma IO.
- **Dónde se usa:** único caller `gestor_memoria_io_rw`, como segundo intento tras fallar la traducción normal.
- **Cómo funciona:** busca el segmento swapeado (`buscar_segmento_swap`) y valida que `desplazamiento+tamanio` no exceda su tamaño. Recorre con un cursor por bloques: `idx_bloque = offset / block_size`, `off_local = offset % block_size`, tramo = mínimo entre lo que queda del bloque actual y lo que falta. **Siempre lee el bloque completo primero** (`swap_leer_bloque`), y si es escritura modifica solo la porción `off_local..off_local+tramo` en el buffer local antes de reescribir el bloque entero (`swap_leer_bloque` + `memcpy` parcial + `swap_escribir_bloque` — patrón read-modify-write, necesario porque SWAP solo opera bloques completos, no permite escritura parcial directa). Si es lectura, copia el tramo pedido del bloque hacia el buffer de salida sin reescribir nada.

---

## Topología de sticks para las CPUs

#### `escribir_uint32(p, valor)` *(static)* — gestor_memoria.c:925
- **Qué hace:** escribe 4 bytes en `p` y devuelve el puntero avanzado.
- **Dónde se usa:** `escribir_string_con_longitud` y `gestor_memoria_serializar_topologia`.

#### `escribir_string_con_longitud(p, str)` *(static)* — gestor_memoria.c:932
- **Qué hace:** serializa un string como `uint32 longitud + bytes` (sin `\0` final).
- **Dónde se usa:** único caller `gestor_memoria_serializar_topologia` (para `ip` y `puerto` de cada stick).
- **Por qué este formato:** al no mandar el `\0`, el lado que deserializa (la CPU) necesita saber la longitud de antemano para saber cuántos bytes leer y dónde empieza el próximo campo — de ahí el prefijo.

#### `tamanio_topologia_serializada(gm, cant_out)` *(static)* — gestor_memoria.c:943
- **Qué hace:** calcula de antemano cuántos bytes va a ocupar el buffer serializado completo (para poder hacer un único `malloc` exacto) y de paso cuenta cuántos sticks están **conectados** (los caídos se excluyen de la topología que ve la CPU).
- **Dónde se usa:** único caller `gestor_memoria_serializar_topologia`, como "primera pasada" antes de la "segunda pasada" que efectivamente escribe.
- **Cómo funciona:** suma 4 bytes iniciales (el contador) + por cada stick conectado: 12 bytes fijos (id+tamaño+base) + 4+len(ip) + 4+len(puerto).

#### `gestor_memoria_serializar_topologia(gm, tamanio_out)` — gestor_memoria.c:961
- **Qué hace:** arma el buffer binario con la lista de sticks conectados, listo para mandarlo por socket a una CPU.
- **Dónde se usa:** `kernel_memory_atencion_cpu.c:86`, cuando una CPU pide la topología (típicamente al conectarse o cuando KM le avisa que hay más memoria).
- **Cómo funciona:** patrón "dos pasadas" — primero calcula el tamaño exacto (`tamanio_topologia_serializada`) para reservar el buffer justo, después recorre de nuevo la lista escribiendo secuencialmente con `escribir_uint32`/`escribir_string_con_longitud`, saltando los sticks no conectados igual que en la primera pasada (así ambas pasadas cuentan lo mismo). El caller es responsable de `free()` el buffer devuelto (documentado en el header). Todo bajo `gm->mutex` tomado una sola vez para las dos pasadas (evita que la lista de sticks cambie entre contar y escribir).

---

## Flujo completo: crear segmento con compactación

1. El KS recibe la syscall `SYSCALL_MEM_ALLOC` de una CPU y llama `planificador_crear_segmento_km`, que manda `MENSAJE_CREAR_SEGMENTO` a KM.
2. `kernel_memory_atencion_scheduler.c` recibe el mensaje y llama **`gestor_memoria_crear_segmento`**.
3. Adentro, **`buscar_hueco`** recorre los huecos vía **`segmentos_ordenados`** + **`hueco_es_mejor_que_el_actual`** según **`estrategia_es_best_fit`**. No encuentra hueco contiguo.
4. Se mide **`espacio_libre_sin_lock`**: hay espacio total suficiente → se devuelve `CREAR_SEGMENTO_NECESITA_COMPACTACION`.
5. `kernel_memory_atencion_scheduler.c` responde al KS con `MENSAJE_COMPACTACION` en vez de OK.
6. El KS (en `planificador.c`) desaloja todas las CPUs (`interrumpir_cpus_para_compactar`) y, una vez confirmado, manda `MENSAJE_DESALOJO_OK` a KM.
7. KM llama **`gestor_memoria_compactar`**: obtiene **`segmentos_ordenados`** y para cada uno desplazado llama **`mover_segmento`**, que a su vez usa **`copiar_segmento_entre_sticks`** (lectura + escritura, posiblemente cruzando **`stick_para_direccion`**/`stick_operar` varias veces) y **`actualizar_base_de_segmento_en_proceso`**.
8. Tras el `usleep(COMPACTION_DELAY)` y el log `"## Fin de compactación"`, KM vuelve a intentar la creación original: **`gestor_memoria_crear_segmento`** se llama de nuevo (desde `kernel_memory_atencion_scheduler.c:78`), esta vez **`buscar_hueco`** sí encuentra un hueco contiguo (todo quedó apilado al principio), se crea el `t_segmento_fisico`, se llama **`tabla_proceso_agregar`**, y responde `CREAR_SEGMENTO_OK` al KS.

## Flujo completo: suspensión y restauración vía SWAP

1. En `planificador.c`, `hilo_timer_suspension` detecta que un proceso lleva demasiado tiempo bloqueado y decide suspenderlo: manda a KM el pedido de suspensión (vía `atencion_scheduler`).
2. KM llama **`gestor_memoria_suspender_proceso`**: toma snapshot de los segmentos del proceso, valida bloques libres con **`swap_bloques_libres`**, y por cada segmento hace **`gestor_memoria_leer_fisico`** (que internamente reparte en tramos con **`stick_para_direccion`**/`stick_operar` si el segmento cruza sticks) seguido de una secuencia de **`swap_escribir_bloque`**, y finalmente libera el segmento de `gm->segmentos` y de la tabla del proceso.
3. El proceso queda en estado `SUSP_BLOCK` en el KS, con su memoria completamente liberada — esto dispara `MENSAJE_MAS_MEMORIA`-equivalente indirecto (más huecos disponibles) que `planificador_evento_memoria_liberada` puede aprovechar para admitir otros procesos en espera.
4. Cuando el KS decide restaurar (por ejemplo tras liberarse memoria, o vencer un timeout), pide a KM des-suspender.
5. KM llama **`gestor_memoria_dessuspender_proceso`**: toma snapshot de `segmentos_swap` del proceso, reserva un hueco por cada uno con **`buscar_hueco`** (todo o nada — si alguno no entra, revierte y devuelve `false` sin usar compactación), y si entran todos, por cada uno hace una secuencia de **`swap_leer_bloque`** para reconstruir el buffer, **`gestor_memoria_escribir_fisico`** para volcarlo a la nueva base física, y **`tabla_proceso_agregar`** para regenerar su entrada en la tabla del proceso.
6. Si `dessuspender` falla (no entra todo sin compactar), el proceso queda suspendido y es responsabilidad del KS decidir si reintenta más tarde o fuerza una compactación antes de reintentar.
