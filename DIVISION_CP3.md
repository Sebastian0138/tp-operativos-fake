# División del trabajo — Checkpoint 3 (Plug & Pray)

Documento para repartir el CP3 entre **4 personas** de forma equitativa.
No es una guía de implementación paso a paso: define **qué le toca a cada uno**, con qué archivos,
qué entrega y con quién se coordina.

---

## 1. Contexto

El repo está congelado en **Checkpoint 2 "Completo"** (rama `checkpoint-2`, commit `2ef9084`).
El CP2 dejó andando: planificación FIFO/RR mononivel, ciclo de instrucción de CPU, IO completo, y
**toda la memoria física mockeada**.

El **Checkpoint 3** (fecha consigna 20/06/2026) pide:

- **Kernel Scheduler**: planificación de corto y largo plazo *completa* + **Herencia de Prioridades**.
- **CPU**: *completo* (operando contra memoria real, no mocks).
- **Kernel Memory**: esquema de memoria principal completo + administración de espacio libre + lectura/escritura sobre **Memory Sticks** reales.
- **Memory Stick**: *completo*.

**Fuera de alcance del CP3** (queda para la entrega final): planificación de **mediano plazo**
(suspensión SUSP_BLOCK/SUSP_READY, `SUSPENSION_TIMEOUT`) y el módulo **SWAP**. Las colas `cola_susp_*`
ya existen en el código pero no se tocan en este checkpoint.

> **Sobre ser 4 y no 5**: la consigna asume 5 integrantes, pero el alcance del CP3 entra cómodo en 4
> porque SWAP y mediano plazo no forman parte de este checkpoint. Si hay un 5º integrante, lo natural
> es que arranque SWAP + suspensión adelantando la entrega final (no está incluido en este reparto).

---

## 2. Estado actual verificado (qué falta para el CP3)

| Módulo | Estado hoy (CP2) | Falta para CP3 |
|---|---|---|
| **Kernel Scheduler** | FIFO/RR mononivel, syscalls, IO, mutex, contexto | Colas Multinivel (CMN) + desalojo entre colas, herencia de prioridades, syscalls **MEM_ALLOC/MEM_FREE** (hoy **sin `case`**), desalojo por compactación |
| **Kernel Memory** | Contexto por PID + fetch de instrucciones reales; **memoria mockeada** | Esquema de segmentación real, tabla de segmentos por PID, huecos + **Best/Worst Fit**, crear/eliminar segmento, espacio libre real, compactación, R/W real contra Sticks |
| **Memory Stick** | Solo handshake (esqueleto) | Param de tamaño en cfg, `malloc` del bloque, informar tamaño a KM, atender LEER/ESCRIBIR con `MEMORY_DELAY` |
| **CPU** | Ciclo completo + MMU + syscalls; R/W a **un** stick | Awareness de **N sticks** + ruteo de dirección física al stick correcto, split de pedidos multi-stick, SEG_FAULT contra tabla real |

Confirmado leyendo el código: `kernel_scheduler` no usa `queues_algorithms` ni `prioridad_actual`;
`kernel_memory` responde lecturas con `calloc` de ceros y descarta escrituras (`free` inmediato),
y el espacio libre sale de `free_space_mock`; `memory_stick/conexiones.c:71` tiene `// TODO: atender al CPU`.

---

## 3. Contrato compartido (definir el **DÍA 1**, entre los 4)

El punto de coordinación central es `utils/src/utils/protocolo.h`. **Nadie mergea cambios a este archivo
sin avisar**; los 4 codean contra el enum congelado. Hoy tiene `t_codigo_mensaje` (14 opcodes) y
`t_codigo_syscall` (10, ya incluye `SYSCALL_MEM_ALLOC/MEM_FREE`). Handshakes hardcodeados por módulo:
CPU=1, IO=2, SCHEDULER=3, MEMORY_STICK=4, SWAP=5. Serialización manual campo a campo (`send`/`recv`),
sin capa de paquetes: mantener el patrón "primero el `t_codigo_mensaje`, después los campos".

**Opcodes nuevos a acordar y congelar entre los 4** (agregar **al final** del enum, sin reordenar):

| Opcode | Sentido | Dueño principal |
|---|---|---|
| `MENSAJE_CREAR_SEGMENTO` | KS → KM (MEM_ALLOC) | P3 define / P1 consume |
| `MENSAJE_ELIMINAR_SEGMENTO` | KS → KM (MEM_FREE) | P3 define / P1 consume |
| `MENSAJE_FINALIZAR_PROCESO` | KS → KM (EXIT) | P3 / P1 |
| `MENSAJE_COMPACTACION` | KM → KS ("desalojá todas las CPUs") | P1 / P3 |
| `MENSAJE_DESALOJO_OK` | KS → KM (desalojo confirmado) | P3 / P1 |
| `MENSAJE_INFORMAR_TAMANIO` | Memory Stick → KM (tamaño + ip:puerto) | P2 |
| `MENSAJE_TOPOLOGIA_STICKS` | KM → CPU (lista de sticks) | P2 define / P4 consume |
| `MENSAJE_MEMORIA_CORRUPTA` | KM → KS (stick desconectado → BSOD) | P2 / P3 |

**Decisión de arquitectura a validar entre todos (afecta a P1, P2 y P4):** según la consigna la CPU se
conecta **directo** a los Memory Sticks y "el KM define una estrategia para que las CPUs conozcan todos
los sticks". Diseño acordado: la CPU recibe de KM la **topología** de sticks (`ip`, `puerto`, `base`
global, `tamanio`) y rutea cada dirección física al stick correcto (partiendo el pedido si cruza el
borde de un stick). KM también actúa como cliente de los sticks para el camino STDIN/STDOUT. El stick
es "tonto": indexa su `malloc` desde 0; el que rutea (CPU o KM) traduce dirección global → (stick, offset local).

---

## 4. Reparto en 4 personas

> El trabajo se dividió buscando **cargas parejas**: Kernel Memory es el módulo más grande del CP3,
> así que se parte en dos personas (P1 = lógica de segmentación, P2 = almacenamiento físico y el puente
> KM↔Sticks). Kernel Scheduler es una persona (P3) y CPU + integración/testing es la cuarta (P4).
> Ver la **estimación de balance** al final de la sección.

### 👤 Persona 1 — Kernel Memory: memoria principal y segmentación
**El corazón del CP3.** Dueña de `kernel_memory/` en la lógica de memoria (no el transporte a sticks).

- **Qué hace**: reemplazar el mock por un esquema de **segmentación pura real**: estructura de memoria
  con lista de segmentos ocupados y **huecos libres** sobre el espacio total; poblar la **tabla de
  segmentos por PID** (hoy siempre vacía).
- **Crear segmento**: validar contra `SEGMENT_MAX_SIZE`, elegir hueco con **Best Fit / Worst Fit**
  según `ALLOCATION_STRATEGY`. Si hay espacio total pero no contiguo → disparar **compactación**.
- **Compactación**: avisar a KS (`MENSAJE_COMPACTACION`), esperar `MENSAJE_DESALOJO_OK`, reordenar
  segmentos al inicio, actualizar **todas** las tablas de segmentos, respetar `COMPACTION_DELAY`,
  loguear `## Inicio/Fin de compactación`.
- **Eliminar segmento** y **finalización de proceso** (liberar todos los segmentos del PID).
- **Espacio libre real**: eliminar `free_space_mock`; calcularlo sobre los huecos.
- **Archivos**: `kernel_memory/src/atencion_scheduler/*` (reemplazar mocks líneas 47-87), **nuevo**
  `kernel_memory/src/gestor_memoria/*` (huecos + Best/Worst + compactación), `gestor_procesos/*`
  (tabla de segmentos), `config/*` (usar `ALLOCATION_STRATEGY`, `COMPACTION_DELAY`, `SEGMENT_MAX_SIZE`; quitar `FREE_SPACE_MOCK`).
- **Logs obligatorios**: "Creación de Segmento", "Escritura/lectura en espacio de usuario", "Inicio/Fin de compactación".
- **Coordina con**: P2 (le pide leer/escribir bytes a los sticks), P3 (protocolo de compactación y crear/eliminar segmento).

### 👤 Persona 2 — Almacenamiento físico: Memory Stick + camino KM↔Sticks
Slice vertical: el `memory_stick/` real y la parte de KM que le habla.

- **Memory Stick** (`memory_stick/`): agregar `MEMORY_SIZE` al `.cfg` y al struct; `malloc(MEMORY_SIZE)`
  del bloque; tras handshake **informar el tamaño + ip:puerto** a KM (`MENSAJE_INFORMAR_TAMANIO`);
  implementar el loop de atención (hoy `conexiones.c:71` es `// TODO`) para `MENSAJE_LEER_MEMORIA` /
  `MENSAJE_ESCRIBIR_MEMORIA` sobre el bloque, respetando `MEMORY_DELAY`. Logs: "Escritura/Lectura de N bytes".
- **Kernel Memory ↔ Sticks**: en `kernel_memory/src/conexiones/conexiones.c:74-76` dejar de marcar el
  stick como "pendiente"; **registrarlo** (guardar ip:puerto + tamaño + base global, ampliar total,
  avisar a KS que hay más memoria). Implementar el **R/W físico real** que hoy es mock: traducir la
  operación de P1 al/los stick(s), **dividir pedidos que abarcan más de un stick** y **consolidar** el
  resultado como una sola operación. Definir y publicar `MENSAJE_TOPOLOGIA_STICKS` para P4.
- **Archivos**: `memory_stick/src/{conexiones,config}/*`, `memory_stick/config/memory_stick.cfg`;
  `kernel_memory/src/conexiones/*` (registro de sticks) + el R/W físico que consume P1.
- **Coordina con**: P1 (le expone "leé/escribí N bytes desde dirección física X"), P4 (la CPU también
  lee/escribe a sticks: acordar el **mismo formato** de mensaje en el socket del stick).

### 👤 Persona 3 — Kernel Scheduler: planificación completa + herencia + memoria
Dueña de `kernel_scheduler/`.

- **Colas Multinivel (CMN)**: reemplazar la única `cola_ready` por **una cola por nivel de prioridad**
  (0..N-1 según `QUEUES_ALGORITHMS`), cada una con su algoritmo (FIFO/RR).
- **Desalojo entre colas** (`QUEUE_PREEMPTION=TRUE`): al llegar un Proceso más prioritario, desalojar al
  que ejecuta si es menos prioritario. Log: "Desalojo por Cola más prioritaria".
- **Herencia de prioridades**: usar `prioridad_original`/`prioridad_actual` del PCB (hoy solo se setean).
  Al bloquearse un proceso de mayor prioridad por un mutex tomado por uno menor, el dueño **hereda** la
  prioridad mayor; al liberar, vuelve a la original. Log: "Cambio de prioridad de Proceso".
  Archivos: `mutex_kernel/*`, `planificador.c` (crear/lock/unlock mutex).
- **Syscalls de memoria** (hoy **sin handler**): agregar `case SYSCALL_MEM_ALLOC` y `SYSCALL_MEM_FREE`
  → mandar `MENSAJE_CREAR/ELIMINAR_SEGMENTO` a KM y **devolver el proceso a la misma CPU** (lo exige la consigna).
- **Desalojo por compactación**: atender `MENSAJE_COMPACTACION` → interrumpir **todas** las CPUs, no
  despachar hasta el fin, colocar los desalojados **al principio** de READY, responder `MENSAJE_DESALOJO_OK`.
- **BSOD**: ante `MENSAJE_MEMORIA_CORRUPTA` finalizar todos los procesos y el KS.
- **Coordina con**: P1/P2 (segmentos y compactación), P4 (interrupciones a CPUs).

### 👤 Persona 4 — CPU completo + integración, deploy y testing
Dueña de `cpu/`, scripts y la validación integral.

- **CPU multi-stick**: hoy la CPU se conecta a **un** `socket_memory_stick`. Con N sticks y direcciones
  físicas globales, recibir de KM la **topología** (`MENSAJE_TOPOLOGIA_STICKS`), conectarse a cada stick
  y **rutear** cada dirección física al stick correcto, partiendo lecturas/escrituras que cruzan el borde
  (MOV_IN/MOV_OUT/COPY_MEM).
- **SEG_FAULT real**: verificar la MMU ahora que la tabla de segmentos llega **poblada** desde KM.
- **Config y scripts**: agregar `MEMORY_SIZE` a los cfg de sticks; **corregir `start_all.sh`** para
  lanzar IO **con el tipo** (`bin/io config/io.cfg STDIN|STDOUT|SLEEP` — hoy `start_all.sh:130` lo lanza
  sin tipo y aborta) y para levantar **múltiples sticks/CPUs/IO**; alinear el orden de arranque
  (KM → sticks → KS → CPUs → IO). Verificar puertos (KS 8000, KM 8002, sticks 8003+).
- **Testing integral**: correr las pruebas de la cátedra en `plug-n-pray-pruebas-main/`
  (`PCP*.prc`, `PMP*.prc`, `PLANI*.prc`, `MEMORIA_PRE_*.prc`, `PHP*.prc`) y cruzar logs contra los
  "logs mínimos y obligatorios" de la consigna. Contrastar con `RESULTADOS.md`.
- **Coordina con**: todos (es quien integra y detecta desajustes de protocolo).

### Estimación de balance (para chequear que es equitativo)

| Persona | Foco | Peso relativo | Riesgo |
|---|---|---|---|
| P1 | KM segmentación / huecos / Best-Worst / compactación | ~Alto | Alto (lógica nueva) |
| P2 | Memory Stick + KM↔Sticks + multi-stick | ~Alto | Medio-Alto (concurrencia/red) |
| P3 | KS: CMN + herencia + syscalls memoria + compactación | ~Alto | Alto (concurrencia) |
| P4 | CPU multi-stick + integración + testing + scripts | ~Alto | Medio (mucha integración) |

Cargas parejas. Si algo se desbalancea en la práctica: la compactación (hoy en P1) puede pasar a P2
si P1 queda muy cargada, y el testing (P4) es el comodín que puede ayudar a quien vaya más lento.

---

## 5. Orden de trabajo y dependencias

1. **Día 1 (los 4 juntos)**: congelar `protocolo.h` con los opcodes nuevos y el formato de cada mensaje,
   y cerrar la decisión de arquitectura CPU↔Sticks de la sección 3.
2. **Primero lo físico**: P2 completa Memory Stick (malloc + R/W) y el registro de sticks en KM →
   desbloquea a P1 (que necesita escribir/leer bytes reales) y a P4 (CPU R/W).
3. **En paralelo**: P1 (segmentación/huecos/compactación) y P3 (CMN + herencia + syscalls de memoria).
4. **Integración continua**: P4 arma el entorno multi-módulo y corre pruebas a medida que P1/P2/P3
   entregan, reportando desajustes.

## 6. Cómo se valida (criterio de aprobado)

- **Compilar**: `./start_all.sh` compila todo en orden con `set -e`.
- **Levantar**: KM → Memory Stick(s) → KS (con proceso inicial) → CPU(s) → IO(s por tipo).
- **Pruebas de la cátedra** (`plug-n-pray-pruebas-main/`): MEMORIA (Best/Worst Fit, compactación),
  PLANI/PCP (FIFO/RR/CMN, desalojo), PHP (herencia de prioridades). PMP (mediano plazo) **NO** entra en CP3.
- **Logs mínimos y obligatorios**: cada módulo debe emitirlos con el **formato exacto** de la consigna,
  guardados en archivo (sin los `< >`). Un log faltante o mal formado = TP no evaluable.
- **Chequeos puntuales**: MEM_ALLOC que dispara compactación (log Inicio/Fin), SEG_FAULT
  (desplazamiento + tamaño > tamaño de segmento), herencia de prioridades (log "Cambio de prioridad"),
  lectura/escritura que abarca 2 sticks consolidada como una sola operación.
