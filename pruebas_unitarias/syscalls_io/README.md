# Prueba unitaria — Atención de Syscalls: IO (SLEEP, STDIN, STDOUT)

Verifica los tres tipos de IO en una sola corrida, y que el dato haga el viaje completo:
**lo que escribís por STDIN tiene que salir por STDOUT**.

## Qué parte del enunciado prueba

`Plug & Pray.pdf`, **Kernel Scheduler**, sección *"IO"*:

> "Las IO dentro del entorno de nuestro trabajo práctico pueden ser 3: SLEEP, STDIN y
> STDOUT. En todos los casos, al momento de que llegue la petición de la CPU, el proceso
> pasará del estado EXEC al estado BLOCK y el Kernel Scheduler deberá enviar un nuevo
> Proceso a ejecutar si lo hubiera."

Y las tres subsecciones:

> **Sleep** — "se recibirá de la CPU un tiempo en milisegundos que deberá estar el Proceso
> en la IO de tipo Sleep."
>
> **STDIN** — "se recibirán por parte de la CPU un tamaño total a leer y la dirección
> física donde se deberá guardar el contenido leído. Se deberá solicitar a la IO de tipo
> STDIN que le solicite al usuario ingresar el total a leer y luego, cuando este retorne
> los caracteres leídos, se deberán enviar al Módulo Kernel Memory para que este las
> escriba en donde corresponda."
>
> **STDOUT** — "el Kernel Scheduler recibirá de parte de la CPU la dirección física y el
> tamaño a leer. Una vez que este le retorne todos los bytes, deberá pedirle esta
> información al Kernel Memory para luego, enviársela a la IO de tipo STDOUT."

Trazabilidad código ↔ enunciado en `docs/MAPA_TP.md` §2.9.

---

## Cómo levantarla

```bash
./pruebas_unitarias/syscalls_io/levantar.sh
```

**ES INTERACTIVA.** En la ventana `5 IO STDIN` te va a aparecer:

```
## PID: 1 - Ingrese 10 caracteres:
```

Escribí 10 caracteres y dale Enter. Tienen que salir por la ventana `5 IO STDOUT`.

### Qué se espera ver

Corrida real (con `HOLA-MUNDO` como entrada), uniendo los tres logs:

```
05:32:31:915  KS   ## (1) - Solicitó syscall: MEM_ALLOC

05:32:32:393  KS   ## (1) - Solicitó syscall: STDIN
05:32:32:393  KS   ## (1) Pasa del estado EXEC al estado BLOCK
05:32:32:393  IO   ## PID: 1 - Ingrese 10 caracteres:
05:32:32:648  KS   ## (2) Pasa del estado READY al estado EXEC     <- entra otro proceso
05:32:32:525  KM   ## PID: 1 - Escritura - Dir. Física: 0 - Tamaño: 10

05:32:47:955  KS   ## (1) - Solicitó syscall: STDOUT
05:32:48:046  KM   ## PID: 1 - Lectura - Dir. Física: 0 - Tamaño: 10
05:32:48:046  IO   ## PID: 1 - HOLA-MUNDO                          <- volvió desde memoria

05:32:48:260  KS   ## (1) - Solicitó syscall: SLEEP
05:32:48:260  IO   ## PID: 1 - Haciendo sleep por 2000 milisegundos.
05:32:50:260  IO   ## PID: 1 - Fin de IO                           <- 2000 ms exactos
```

### Lo que hay que mirar

**1. El dato viaja y vuelve.** `HOLA-MUNDO` no se guarda en ninguna variable del Kernel:
entra por el teclado, el KS se lo manda a KM, KM lo escribe en el Memory Stick, y 15
segundos después KM lo lee de ahí y el KS se lo pasa a la IO STDOUT. La cadena completa es

```
teclado -> IO STDIN -> KS -> KM -> Memory Stick -> KM -> KS -> IO STDOUT -> pantalla
```

Los dos logs de KM (`Escritura` y `Lectura`, ambos en Dir. Física 0, tamaño 10) son la
prueba de que pasó por memoria de verdad.

**2. Mientras PID 1 espera el teclado, PID 2 ejecuta.** A las `32:648`, con PID 1
bloqueado en STDIN, entra PID 2. Eso es literalmente lo que pide el enunciado: *"el Kernel
Scheduler deberá enviar un nuevo Proceso a ejecutar si lo hubiera"*. Sin el testigo PID 2
no habría cómo comprobarlo.

**3. El SLEEP dura lo que dice.** `48:260 -> 50:260` = 2000 ms clavados.

**4. Los tres tipos pasan por EXEC → BLOCK.** Ninguna IO es sincrónica: las tres liberan
la CPU. Se ve en los tres `Pasa del estado EXEC al estado BLOCK`.

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (8 ventanas: una IO de cada tipo) |
| `test_padre` | PID 0: crea los dos y se va |
| `test_io_ciclo` | PID 1: MEM_ALLOC → STDIN → STDOUT → SLEEP → MEM_FREE → EXIT |
| `test_io_cpu` | PID 2: CPU-bound, el testigo |
| `km_sys.cfg` / `ks_sys.cfg` | Configs |

## Los pseudocódigos

### `test_io_ciclo` (PID 1)

```
0  MEM_ALLOC 0 32     <- necesita memoria: STDIN escribe ahí y STDOUT lee de ahí
1  SET AX 0           <- dirección lógica 0
2  SET BX 10          <- 10 caracteres
3  STDIN AX BX
4  STDOUT AX BX       <- misma dirección y tamaño: por eso sale lo mismo que entró
5  SLEEP 2000
6  MEM_FREE 0
7  EXIT
```

`STDIN` y `STDOUT` toman **registros**, no inmediatos (`instrucciones.c:271` y `:286` usan
`get_registro_32`): de ahí los dos `SET` previos.

> **Detalle del enunciado vs. la implementación:** el enunciado dice que a la IO se le pasa
> *"la dirección física"*. Acá se pasa la **lógica**, y KM la traduce recién al momento de
> leer o escribir (`kernel_memory_atencion_scheduler.c:110` y `:139`). Es a propósito y
> está mejor: el proceso puede estar minutos en BLOCK, y en el medio puede haber una
> compactación o una suspensión que le muevan la base del segmento. Una dirección física
> calculada al pedir la IO quedaría inválida; la lógica no.

### `test_io_cpu` (PID 2)

```
0  SET AX 40
1  SET BX 1
2  NOOP
3  NOOP
4  SUB AX BX
5  JNZ AX 2
6  EXIT
```

Sin syscalls, para que se note claramente que ocupa la CPU mientras PID 1 está bloqueado.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia `SCRIPTS_BASEPATH=../pruebas_unitarias/syscalls_io`.
- **`ks_sys.cfg`**: sólo cambia

```ini
SUSPENSION_TIMEOUT=15000    # el default es 1000
```

Con el default de 1000 ms, el `SLEEP 2000` **suspende** al proceso a mitad de camino:

```
48:260  (1) EXEC → BLOCK
49:260  (1) BLOCK → SUSP. BLOCK        <- a los 1000 ms
50:260  (1) SUSP. BLOCK → SUSP. READY
```

Es el comportamiento correcto, pero acá es ruido: mete swap y mediano plazo en una prueba
que quiere mostrar IO. La suspensión tiene su propia carpeta
([`../suspension/`](../suspension/)); acá se la saca del medio.

Y **es interactiva**: si tardás en escribir, con el timeout bajo el proceso se te suspende
mientras pensás qué tipear.
