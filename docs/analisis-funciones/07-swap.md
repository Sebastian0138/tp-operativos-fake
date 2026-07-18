# Módulo swap — Análisis detallado

Nodo de almacenamiento en disco para el mediano plazo: persiste en un archivo los
segmentos de memoria de los procesos que Kernel Memory decide suspender
(`SUSP_BLOCK`), y se los devuelve cuando Kernel Scheduler pide desuspenderlos.
El módulo es deliberadamente "tonto": no administra espacio ni sabe de PIDs — solo
lee/escribe bloques de tamaño fijo en un archivo, indexados por número de bloque.
Toda la lógica de qué bloque le corresponde a qué segmento/proceso vive en
`kernel_memory/src/gestor_memoria/gestor_memoria.c` (bitmap de bloques libres,
`swap_escribir_bloque`/`swap_leer_bloque`, línea 615/626).

Son ~265 líneas en 4 archivos + sus headers. No hay hilos: es un único bucle
bloqueante atendiendo a un solo cliente (KM).

---

### `main.c`

Responsabilidad: arranque del proceso — logger de boot, carga de config, logger
definitivo, apertura del archivo de swap, y delega todo el ciclo de vida a
`conectar_y_atender_kernel_memory`.

#### `main(argc, argv)` — main.c:8
- **Qué hace:** Punto de entrada. Crea el boot logger, valida que se haya pasado
  el path de config como único argumento, carga la config (`swap_config_create`),
  crea el logger definitivo (`swap_logger_create`), abre/crea el archivo de swap
  con `fopen(path, "w+b")`, y llama a `conectar_y_atender_kernel_memory` que
  bloquea hasta que KM se desconecta. Al volver, cierra el archivo y libera
  logger/config.
- **Dónde se usa:** Es el entry point del binario `bin/swap`; no lo llama nadie
  del código, lo invoca el sistema operativo al ejecutar `./bin/swap <config>`
  (ver `scripts_modulos/swap.sh`).
- **Para qué:** Orquestar el arranque del nodo de almacenamiento en disco.
- **Cómo funciona:** Modo `"w+b"` en `fopen` **trunca** el archivo si ya existía
  (lo deja en 0 bytes) y lo crea si no existe — el comentario en el código
  ("se asume que inicia libre") confirma que cada arranque de `swap` empieza con
  un archivo de swap vacío, sin importar corridas previas. No pre-asigna el
  tamaño total (`SWAP_FILE_SIZE`) del archivo con `ftruncate`; el archivo crece
  bajo demanda a medida que se escriben bloques con `fseek` más allá del final
  actual (ver `conectar_y_atender_kernel_memory`).

---

### `config/swap_config.c` + `.h`

Responsabilidad: parseo y validación del `.cfg` del módulo con la librería
`commons/config` de la cátedra.

Claves obligatorias (`tiene_todas_las_propiedades`): `LOG_LEVEL`,
`SWAP_FILE_PATH`, `SWAP_FILE_SIZE`, `BLOCK_SIZE`, `IP_KERNEL_MEMORY`,
`PORT_KERNEL_MEMORY`.

#### `tiene_todas_las_propiedades(config)` *(static)* — swap_config.c:6
- **Qué hace:** Devuelve `true` solo si las 6 claves de arriba están presentes en
  el `t_config` crudo.
- **Dónde se usa:** Único caller: `swap_config_create` (swap_config.c:23).
- **Para qué:** Fail-fast: evita seguir arrancando con una config incompleta.
- **Cómo funciona:** Cadena de `config_has_property` unidas con `&&` (short-circuit).

#### `swap_config_create(path, logger)` — swap_config.c:16
- **Qué hace:** Abre el archivo de config (`config_create`), valida claves, y si
  todo está OK, reserva un `t_swap_config` y copia cada valor (los strings con
  `strdup`, los numéricos con `config_get_int_value`).
- **Dónde se usa:** `main.c:26`.
- **Para qué:** Punto único de entrada de configuración; el resto del módulo
  trabaja siempre contra el struct tipado `t_swap_config`, nunca contra el
  `t_config` crudo.
- **Cómo funciona:** Si `config_create` falla o faltan propiedades, loguea el
  error, libera lo que corresponda y devuelve `NULL` (el caller en `main.c`
  aborta el arranque). Si todo sale bien, destruye el `t_config` intermedio
  (`config_destroy`) porque ya copió todo lo que necesita al struct propio.

#### `swap_config_log(config, logger)` — swap_config.c:42
- **Qué hace:** Loguea cada campo de la config ya cargada (`LOG_LEVEL=...`,
  `SWAP_FILE_PATH=...`, etc.).
- **Dónde se usa:** `main.c:33`, inmediatamente después de cargar la config, con
  el boot logger.
- **Para qué:** Dejar constancia en el log de boot de qué parámetros efectivos
  está usando el proceso (útil para debug de despliegue).
- **Cómo funciona:** Serie de `log_info` con formato `CLAVE=valor`, uno por campo.

#### `swap_config_destroy(config)` — swap_config.c:52
- **Qué hace:** Libera los 4 strings (`log_level`, `swap_file_path`,
  `ip_kernel_memory`, `port_kernel_memory`) y el struct.
- **Dónde se usa:** `main.c:38` (si falla crear el logger), `main.c:53` (si falla
  abrir el archivo de swap) y `main.c:63` (al final del `main`, cierre normal).
- **Para qué:** Evitar leaks de memoria en cualquier camino de salida.
- **Cómo funciona:** Null-check al principio (`if (config == NULL) return;`) para
  poder llamarla de forma segura incluso si `swap_config_create` falló antes.

---

### `logger/swap_logger.c` + `.h`

Responsabilidad: crear los dos loggers estándar del módulo, usando la librería
`commons/log`.

#### `swap_boot_logger_create(void)` — swap_logger.c:3
- **Qué hace:** Crea un logger fijo a `logs/swap_boot.log`, tag `SWAP_BOOT`,
  también a consola (`true`), nivel `LOG_LEVEL_INFO` hardcodeado.
- **Dónde se usa:** `main.c:9`, antes de tener la config cargada.
- **Para qué:** Loguear los pasos de arranque (carga de config, etc.) *antes* de
  que exista la config que define el nivel de log real — por eso su nivel está
  fijo en INFO y no depende de `LOG_LEVEL`.
- **Cómo funciona:** Wrapper directo sobre `log_create` de la librería de la
  cátedra.

#### `swap_logger_create(config)` — swap_logger.c:12
- **Qué hace:** Crea el logger definitivo a `logs/swap.log`, tag `SWAP`, a
  consola, con el nivel que indique `config->log_level` (convertido con
  `log_level_from_string`).
- **Dónde se usa:** `main.c:35`.
- **Para qué:** Es el logger que se usa durante toda la vida útil del proceso
  (incluido dentro de `conectar_y_atender_kernel_memory`), ya con el nivel que
  pidió el usuario en el `.cfg`.
- **Cómo funciona:** Igual que el de boot pero el nivel sale de config en vez de
  estar hardcodeado.

---

### `conexiones/conexiones.c` + `.h`

Responsabilidad: todo el protocolo de red del módulo. Es el único archivo con
lógica de negocio real (85% del módulo entero está acá si se cuenta `main.c`).

#### `conectar_y_atender_kernel_memory(config, archivo_swap, logger)` — conexiones.c:10
- **Qué hace:** (1) Se conecta a KM (`crear_conexion`) con la IP/puerto de config.
  (2) Manda el handshake `HANDSHAKE_SWAP` (valor `5`, `utils/protocolo.h:12`).
  (3) Manda `MENSAJE_INFORMAR_TAMANIO` seguido de `swap_file_size` y `block_size`
  (dos `uint32_t`). (4) Entra en un bucle infinito `recv`-bloqueante atendiendo
  `MENSAJE_ESCRIBIR_MEMORIA` y `MENSAJE_LEER_MEMORIA` hasta que KM cierra la
  conexión (`recv` devuelve `<= 0`).
- **Dónde se usa:** Único caller: `main.c:59`. Del lado remoto, quien la "dispara"
  es **Kernel Memory**: el `case HANDSHAKE_SWAP` en
  `kernel_memory/src/conexiones/conexiones.c:118-136` recibe el handshake y el
  `MENSAJE_INFORMAR_TAMANIO`, y llama a `gestor_memoria_registrar_swap` (que
  guarda el fd y arma el bitmap de bloques libres en KM). Después, cada vez que
  `gestor_memoria_suspender_proceso`/`gestor_memoria_dessuspender_proceso`
  necesitan persistir o restaurar un segmento, usan las funciones `static`
  `swap_escribir_bloque` / `swap_leer_bloque` de
  `kernel_memory/src/gestor_memoria/gestor_memoria.c:615` y `:626`, que arman
  exactamente los mensajes `MENSAJE_ESCRIBIR_MEMORIA`/`MENSAJE_LEER_MEMORIA` que
  este bucle atiende. Ese emparejamiento request/response ocurre en
  `gestor_memoria.c:710` (escritura durante suspensión), `:821` y `:877/:880`
  (lectura/escritura durante restauración, caso de re-suspensión parcial).
- **Para qué:** Es el "servidor de bloques" del mediano plazo: le da a KM un
  disco virtual simple (leer/escribir bloque N) sin que KM tenga que saber nada
  de manejo de archivos.
- **Cómo funciona (paso a paso):**
  1. **Conexión y handshake**: abre el socket cliente y manda el opcode fijo
     `HANDSHAKE_SWAP` como primer `int32_t` — así es como KM distingue esta
     conexión de la de un CPU, un stick o el propio KS (`switch(handshake)` en
     `kernel_memory/conexiones.c`).
  2. **Anuncio de tamaño**: manda `MENSAJE_INFORMAR_TAMANIO` + `total` (tamaño
     total del swap) + `block` (tamaño de bloque), ambos como `uint32_t` — KM
     usa esto para armar su bitmap de bloques libres (`swap_file_size / block_size`
     bloques).
  3. **Buffer único**: reserva `buffer = malloc(block)` una sola vez fuera del
     loop y lo reutiliza en cada operación (evita malloc/free por mensaje).
  4. **Escritura** (`MENSAJE_ESCRIBIR_MEMORIA`): lee `nro_bloque` (`uint32_t`) y
     luego exactamente `block` bytes de payload con `recv(..., MSG_WAITALL)`
     (bloquea hasta juntar todo el buffer, sin importar en cuántos paquetes TCP
     llegue). Hace `fseek(archivo, nro_bloque * block, SEEK_SET)` y
     `fwrite(buffer, 1, block, archivo)` + `fflush` (fuerza el volcado a disco
     antes de responder, no se queda en el buffer de stdio). Loguea
     `"## Escritura del bloque: %u"` y responde `MENSAJE_OK`.
  5. **Lectura** (`MENSAJE_LEER_MEMORIA`): lee `nro_bloque`, hace el mismo
     `fseek` y `fread(buffer, 1, block, archivo)`. Si el bloque nunca fue escrito
     antes (archivo más corto que `nro_bloque * block + block`), `fread` devuelve
     menos bytes de los pedidos (`leidos < block`); el código completa el resto
     del buffer con ceros a mano (`for (i = leidos; i < block; i++) buffer[i] = 0`)
     — así un bloque "virgen" siempre se lee como memoria en cero, igual que un
     segmento recién creado. Loguea `"## Lectura del bloque: %u"` y devuelve el
     buffer completo (`block` bytes) por el socket.
  6. **Mensaje desconocido**: cualquier otro código se loguea como warning y se
     ignora (no rompe la conexión, pero tampoco responde nada — si KM esperaba
     una respuesta quedaría colgado; en la práctica KM solo manda los dos
     códigos de arriba).
  7. **Fin**: cuando `recv` del código de mensaje devuelve `<= 0` (KM cerró la
     conexión, p. ej. al finalizar KM o perder el link), sale del `while(1)`,
     libera el buffer, cierra el fd y loguea `"Kernel Memory desconectado,
     finalizando SWAP"` — el proceso `swap` entero termina ahí (vuelve a
     `main`, que cierra el archivo y sale). **No hay reconexión ni reintento**:
     si KM se cae, el proceso swap muere con él.
- **Nota de diseño (no es un bug, es el contrato):** el swap no valida que
  `nro_bloque` esté dentro de `SWAP_FILE_SIZE`— confía ciegamente en KM. Un
  `nro_bloque` fuera de rango simplemente hace crecer el archivo en disco (POSIX
  permite `fseek` más allá de EOF; el hueco se rellena con ceros al escribir).

---

### Headers (`swap_config.h`, `swap_logger.h`, `conexiones.h`)

Solo declaran el struct `t_swap_config` (ver arriba) y las firmas ya cubiertas.
No tienen lógica propia — están documentados junto a su `.c` correspondiente.
