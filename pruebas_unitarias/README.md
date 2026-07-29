# Pruebas unitarias — Plug & Pray

Una carpeta por funcionalidad del enunciado. Cada prueba es **autocontenida**: trae su
propio lanzador, su pseudocódigo y sus configs, y no toca los configs por defecto de los
módulos.

---

## Índice

| Prueba | Qué parte del enunciado cubre | Módulos que ejercita | ¿Interactiva? |
|---|---|---|---|
| [`bsod/`](bsod/) | KM *"Desconexión de un Memory Stick"* + KS *"Planificación de Largo Plazo"* (BSOD) | KM, KS, stick, CPU, IO | **Sí** — hay que matar el stick |
| [`suspension/`](suspension/) | KS *"Planificación de Mediano Plazo"* — BLOCK → SUSP. BLOCK | KS, KM, SWAP, CPU, IO SLEEP | No — cicla sola |
| [`dessuspension_memoria/`](dessuspension_memoria/) | Mediano plazo — des-suspensión **"en caso de que se libere memoria"** | KS, KM, SWAP, CPU, 4× IO SLEEP | No |
| [`dessuspension_stick/`](dessuspension_stick/) | Mediano plazo — des-suspensión **"se agregue un memory stick nuevo"** | + 2º stick en caliente | **Sí** — hay que conectar el stick |
| [`dessuspension_compactacion/`](dessuspension_compactacion/) | Mediano plazo — des-suspensión **"o se compacte la misma"** + desalojo de CPUs por compactación | + compactación de KM, **2 CPUs** | No |
| [`pcp_fifo/`](pcp_fifo/) | KS *"Planificación de Corto Plazo"* — FIFO, sin desalojo | KS, CPU | No |
| [`pcp_rr/`](pcp_rr/) | Corto plazo — RR, desalojo por quantum | KS, CPU | No |
| [`pcp_cmn_sin_desalojo/`](pcp_cmn_sin_desalojo/) | Corto plazo — Colas Multinivel `[FIFO,RR]`, **sin** desalojo entre colas | KS, CPU, IO SLEEP | No |
| [`pcp_cmn_con_desalojo/`](pcp_cmn_con_desalojo/) | Corto plazo — Colas Multinivel `[RR,FIFO]`, **con** desalojo entre colas | KS, CPU, IO SLEEP | No |
| [`syscalls_retorno_cpu/`](syscalls_retorno_cpu/) | KS *"Atención de Syscalls"* — las syscalls de memoria y mutex devuelven el proceso a **la misma CPU** | KS, KM, **2 CPUs** | No |
| [`syscalls_io/`](syscalls_io/) | KS *"IO"* — SLEEP, STDIN y STDOUT; el dato va y vuelve por memoria | KS, KM, stick, IO ×3 | **Sí** — hay que escribir 10 caracteres |
| [`syscalls_mutex/`](syscalls_mutex/) | KS *"Mutex"* — exclusión mutua y orden FIFO de desbloqueo | KS, CPU, IO SLEEP | No |
| [`syscalls_mutex_herencia/`](syscalls_mutex_herencia/) | KS *"Herencia de prioridades"* — inversión de prioridades y su resolución | KS, CPU, IO SLEEP (CMN) | No |
| [`desalojo_compactacion/`](desalojo_compactacion/) | KS *"Desalojo por compactación"* — interrumpir **todas** las CPUs, barrera de despacho, reencolado **al frente** | KS, KM, stick, **3 CPUs** | No |
| [`compactacion_integridad/`](compactacion_integridad/) | KM *"Compactación"* — que los **datos** sobrevivan la mudanza, incluso cruzando la frontera entre sticks | KM, **2 sticks**, CPU, IO STDOUT | No |
| [`mutex_muerte_del_duenio/`](mutex_muerte_del_duenio/) | Caso borde de *"Mutex"* — el dueño finaliza **sin liberar**; el kernel libera por él | KS, CPU, IO SLEEP | No |

Las tres `dessuspension_*` prueban la **misma** cláusula del mediano plazo con un
disparador distinto cada una, y las tres verifican además el criterio de orden: **por
prioridad y, a igual prioridad, por antigüedad de suspensión**.

Las cuatro `pcp_*` cubren la sección de corto plazo entre todas. Las dos de CMN usan el
orden de `QUEUES_ALGORITHMS` **invertido** a propósito, así entre ambas queda ejercitado
FIFO y RR en la cola 0 y en la cola 1. Y forman un par A/B: mismo escenario, mismos
pseudocódigos, y la única diferencia observable sale de `QUEUE_PREEMPTION`.

Trazabilidad completa entre enunciado y código: `docs/MAPA_TP.md`.

---

## Cómo levantar cualquiera de ellas

Antes que nada, compilar:

```bash
./compilar_todo.sh
```

Después, desde la **raíz del repo**:

```bash
./pruebas_unitarias/bsod/levantar.sh
./pruebas_unitarias/suspension/levantar.sh
```

Todos los lanzadores aceptan el tamaño del Memory Stick como primer argumento
(default `256`):

```bash
./pruebas_unitarias/suspension/levantar.sh 512
```

Los scripts se autolocalizan con `dirname "${BASH_SOURCE[0]}"`, así que funcionan desde
cualquier directorio. También podés correrlos parado adentro de la carpeta de la prueba.

### Qué hace un lanzador

Siempre lo mismo, en este orden:

1. Mata cualquier módulo vivo de una corrida anterior (clientes primero, servidores después)
2. Borra los `logs/*.log` de los 6 módulos, para que la corrida arranque limpia
3. Abre una ventana de `gnome-terminal` por instancia, respetando las dependencias:

```
1 KERNEL MEMORY      4 KERNEL SCHEDULER      6 CPU 1
2 MEMORY STICK       5.x IO (las que haga falta)
3 SWAP
```

La regla es una sola: **nadie se conecta a un socket que todavía no escucha.** Servidores
primero, clientes después, con 1-2 segundos entre medio.

Cada ventana sobrevive a la muerte de su módulo y muestra `--- X TERMINADO ---`, así se
puede leer la salida sin que se cierre en la cara. Es indispensable en la prueba de BSOD,
donde la mitad de los módulos mueren a propósito.

**La cantidad de instancias varía según la prueba** — no todas levantan lo mismo:

| Prueba | IO SLEEP | IO STDIN/STDOUT | CPUs | Ventanas |
|---|---|---|---|---|
| `bsod/` | 1 | 1 + 1 | 1 | 8 |
| `suspension/` | 1 | 1 + 1 | 1 | 8 |
| `dessuspension_memoria/` | **4** | no | 1 | 9 |
| `dessuspension_stick/` | **4** | no | 1 | 9 (+1 al agregar el stick) |
| `dessuspension_compactacion/` | **4** | no | **2** | 10 |
| `pcp_fifo/` · `pcp_rr/` | 2 | no | 1 | 7 |
| `pcp_cmn_sin_desalojo/` · `pcp_cmn_con_desalojo/` | 2 | no | 1 | 7 |
| `syscalls_retorno_cpu/` | 1 | no | **2** | 7 |
| `syscalls_io/` | 1 | **1 + 1** | 1 | 8 |
| `syscalls_mutex/` · `syscalls_mutex_herencia/` | 2 | no | 1 | 7 |
| `desalojo_compactacion/` | 1 | no | **3** | 8 |
| `compactacion_integridad/` | no | 1 STDOUT | 1 | 8 (**2 sticks**) |
| `mutex_muerte_del_duenio/` | 2 | no | 1 | 7 |

Los motivos de esos números están en el README de cada prueba, y no son caprichosos:
levantar de menos hace que las pruebas **pasen en verde sin probar nada**. Ver los puntos
4, 5 y 6 de acá abajo.

### Bajar todo

`Ctrl+C` en cada ventana, o `./stop_all.sh` desde la raíz. El lanzador de la prueba
siguiente también limpia por su cuenta, así que no hace falta bajar nada entre pruebas.

---

## Requisitos

- Proyecto compilado (`./compilar_todo.sh`)
- so-commons-library y `libreadline-dev` instalados (ver `COMO_LEVANTAR.md` §8)
- `gnome-terminal`

---

## Cómo agregar una prueba nueva

Copiar la estructura de `suspension/`, que es la más simple:

```
pruebas_unitarias/<nombre>/
├── levantar.sh        # ejecutable; copiar y ajustar los nombres de config y script
├── README.md          # qué parte del enunciado prueba, qué se espera ver, qué cambia cada config
├── test_<nombre>      # pseudocódigo del proceso inicial
├── km_<nombre>.cfg    # sólo si hace falta cambiar algo del Kernel Memory
└── ks_<nombre>.cfg    # sólo si hace falta cambiar algo del Kernel Scheduler
```

Ocho cosas que se olvidan siempre:

**1. `SCRIPTS_BASEPATH` es relativo a la carpeta del módulo, no a la raíz.** El Kernel
Memory arma el path del pseudocódigo como `SCRIPTS_BASEPATH + "/" + nombre`
(`kernel_memory_atencion_scheduler.c:35-37`) y corre parado en `kernel_memory/`. Por eso
todos los `km_*.cfg` de acá llevan:

```ini
SCRIPTS_BASEPATH=../pruebas_unitarias/<nombre>
```

Sin el `../`, KM busca en `kernel_memory/scripts/`, no encuentra el archivo, y el proceso
muere en el primer fetch.

**2. Los configs se pasan con path relativo al módulo.** En el lanzador:
`./bin/kernel_memory ../pruebas_unitarias/<nombre>/km_<nombre>.cfg`.

**3. Si la prueba necesita que reacciones a tiempo, dale ventana infinita.** Un
`SLEEP 30000` seco te da 30 segundos para leer, pensar y tipear un comando, y no alcanzan.
Usá un loop:

```
SET CX 1
SLEEP 3000
JNZ CX <linea_del_sleep>
```

`CX` vale 1 y nunca cambia, así que el salto se toma siempre. El proceso cicla
`BLOCK → READY → EXEC → BLOCK` indefinidamente y podés observar con calma.

**4. Levantá al menos tantas IO de un tipo como procesos puedan usarla a la vez.**
`planificador_tomar_io_libre()` (`planificador.c:1175`) hace `sem_wait` sobre un semáforo
**por tipo de IO**, y cada dispositivo conectado hace un `sem_post`. Si hay menos
dispositivos que procesos, los que sobran hacen cola — **estando ya en BLOCK**, porque el
`exec → block` ocurre antes (`conexiones.c:226-227`) y el timer de suspensión ya arrancó.

O sea:

```
tiempo en BLOCK  =  espera por dispositivo  +  duración real de la IO
```

Si un proceso tiene que quedarse en BLOCK **menos** que `SUSPENSION_TIMEOUT`, no alcanza
con que su `SLEEP` sea corto: hay que garantizarle además que va a encontrar dispositivo
libre. Si no, el margen que creés tener no existe y el proceso se suspende igual.

**5. La cantidad de CPUs es parte del diseño, y no siempre va para el mismo lado.**

| Si querés ver... | Necesitás | Porque |
|---|---|---|
| Desalojo por quantum o por cola más prioritaria | **1 CPU** | Sin contención nadie compite: cada proceso agarra una CPU libre y no hay nada que decidir |
| Desalojo de CPUs por compactación | **2 CPUs** | El que dispara la compactación está estacionado en su syscall y no está `en_rafaga`; hace falta otro proceso ejecutando en otra CPU |
| Que se desalojen **todas** las CPUs, y no una | **3 CPUs + 4 procesos** | Con dos CPUs sólo hay una en ráfaga: "todas" es indistinguible de "la única". Y hace falta un 4º proceso en READY para probar la barrera de despacho y el reencolado al frente |
| Retorno de una syscall a la misma CPU | **2 CPUs + 3 procesos** | Hace falta un tercer proceso esperando en READY: si el retorno directo estuviera roto, se colaría. Con menos, "soltó la CPU" y "no la soltó" dan el mismo log |

**6. Si la prueba involucra desalojo de cualquier tipo, los procesos tienen que ser
CPU-bound.** `chequear_desalojo_por_prioridad()` (`planificador.c:57`) e
`interrumpir_cpus_para_compactar()` (`:975`) exigen `!libre && en_rafaga`. Un proceso
estacionado en una syscall **no** está en ráfaga y no se lo puede desalojar. Usá un loop de
instrucciones puras, sin syscalls:

```
SET AX 25
SET BX 1
NOOP
NOOP
SUB AX BX
JNZ AX 2
EXIT
```

(`SUB` opera registro contra registro, no acepta inmediato: de ahí el `SET BX 1`.)

**7. Que "entre" no quiere decir que esté bien.** Verificar tamaños, estados y
transiciones no dice nada sobre los **datos**. Una operación que mueva memoria puede
consolidar los huecos a la perfección y escribir los bytes en el offset equivocado: todas
las pruebas de estado pasan en verde. Si la funcionalidad toca datos del usuario, escribí
un patrón reconocible antes, leelo después, y **dejá un control** (una lectura previa) para
saber si un fallo es de la operación o es anterior. Ver
[`compactacion_integridad/`](compactacion_integridad/).

   Y ojo: **valgrind no cubre esto.** La memoria del usuario vive en el proceso
   `memory_stick`, no en el de Kernel Memory: KM le manda una dirección y un tamaño por
   socket. Una dirección mal calculada es un `send()` impecable de un `uint32_t` válido.
   Valgrind audita accesos a memoria, no aritmética.

**8. Antes de creerle a una corrida, contá las instancias.** Un lanzamiento anterior que
quedó a medio ejecutar sigue abriendo ventanas **después** del `pkill` del siguiente, y
terminás con dos CPUs registradas bajo el mismo ID o IOs de más. Los síntomas son
espectaculares y engañosos: syscalls atribuidas al PID equivocado, procesos que nunca
finalizan, dos despachos simultáneos con una sola CPU. Parece un bug de concurrencia del
Kernel y es basura de la corrida anterior.

```bash
grep -c "CPU .* Conectada" kernel_scheduler/logs/kernel_scheduler.log
grep -c "IO .* Conectada"  kernel_scheduler/logs/kernel_scheduler.log
pgrep -af "bin/cpu"
```

Si los números no son los que esperabas, bajá todo, esperá unos segundos y volvé a
levantar. Ver la nota de metodología en
[`mutex_muerte_del_duenio/README.md`](mutex_muerte_del_duenio/README.md).

Y una regla de fondo: **no toques los configs por defecto de los módulos.** Copiá el
config, cambiá lo mínimo, y documentá en el README qué cambiaste y por qué. Si una prueba
necesita 4 parámetros distintos del default, probablemente esté probando 4 cosas a la vez.
