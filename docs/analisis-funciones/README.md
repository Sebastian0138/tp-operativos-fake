# Análisis minucioso — Plug & Pray (índice)

Documentación función por función de todo el código fuente del TP (`src/*.c` + `*.h` de
los 7 módulos, ~7400 líneas). Cada función no trivial está documentada con: **qué hace**,
**dónde se usa** (callers reales, verificados con `grep` sobre el repo — no supuestos),
**para qué** (rol en el sistema) y **cómo funciona** (mecanismo interno paso a paso).

Generado leyendo el código fuente actual directamente, sin asumir que documentación previa
del repo (`DOCUMENTACION_FUNCIONES.md`, `DIVISION_CP3.md`) estuviera al día. Varias veces no
lo estaba — ver "Hallazgos" abajo.

## Cómo está dividido

| Archivo | Módulo / alcance | Líneas doc | Líneas código cubierto |
|---|---|---|---|
| [00-utils.md](00-utils.md) | `utils/` — sockets, boot_loader, protocolo, instrucciones, structs compartidos | 243 | 483 |
| [01-cpu.md](01-cpu.md) | `cpu/` completo — ciclo fetch/decode/execute, MMU, multi-stick | 547 | 1154 |
| [02-kernel_scheduler-infra.md](02-kernel_scheduler-infra.md) | `kernel_scheduler/` — main, config, logger, pcb, mutex, conexiones (servidor + dispatch de syscalls) | 407 | 947 |
| [03-kernel_scheduler-planificador.md](03-kernel_scheduler-planificador.md) | `kernel_scheduler/planificador.c` — CMN, desalojo, herencia de prioridades, mutex de usuario, compactación, SWAP, IO async | 937 | 1689 |
| [04-kernel_memory-infra.md](04-kernel_memory-infra.md) | `kernel_memory/` — main, config, logger, gestor_procesos, conexiones, atencion_cpu, atencion_scheduler | 336 | 875 |
| [05-kernel_memory-gestor_memoria.md](05-kernel_memory-gestor_memoria.md) | `kernel_memory/gestor_memoria.c` — segmentación, huecos, Best/Worst Fit, compactación, topología sticks, SWAP | 367 | 1135 |
| [06-memory_stick.md](06-memory_stick.md) | `memory_stick/` completo | 168 | 493 |
| [07-swap.md](07-swap.md) | `swap/` completo | 195 | 265 |
| [08-io.md](08-io.md) | `io/` completo | 163 | 356 |

División alineada con el reparto de 4 personas que charlamos antes: 00-utils es contrato
compartido; 01 + parte de 03 mapea a CPU/scheduler-core; 02+03 es Kernel Scheduler completo;
04+05 es Kernel Memory completo; 06+07+08 es almacenamiento físico + IO.

## Versión archivo por archivo (sin resumir)

Los docs de arriba están organizados por tema. En [por-archivo/](por-archivo/) está la misma
información reorganizada estrictamente por archivo fuente (`.c`/`.h` uno por uno, con TODAS sus
funciones incluidas las triviales, todos los campos de struct, y todos los callers reales), sin
condensar nada — pensada para consulta puntual de un archivo concreto en vez de lectura por tema.

| Archivo | Módulo(s) | Archivos fuente cubiertos |
|---|---|---|
| [por-archivo/00-utils.md](por-archivo/00-utils.md) | utils | 11 |
| [por-archivo/01-cpu.md](por-archivo/01-cpu.md) | cpu | 13 |
| [por-archivo/02-kernel_scheduler.md](por-archivo/02-kernel_scheduler.md) | kernel_scheduler | 14 |
| [por-archivo/03-kernel_memory.md](por-archivo/03-kernel_memory.md) | kernel_memory | 17 |
| [por-archivo/04-memory_stick-swap.md](por-archivo/04-memory_stick-swap.md) | memory_stick + swap | 13 |
| [por-archivo/05-io.md](por-archivo/05-io.md) | io | 7 |

## Hallazgos durante el análisis (relevantes para el equipo)

Estos son datos verificados con `grep`/lectura de código, no en la documentación previa:

1. **`DIVISION_CP3.md` está obsoleto.** Describe segmentación, compactación, syscalls de
   memoria y multi-stick como pendientes ("sin `case`", "memoria mockeada", TODOs). Ya está
   todo implementado — no hay `free_space_mock`, no hay TODOs en `memory_stick/conexiones.c`,
   `SYSCALL_MEM_ALLOC`/`MEM_FREE` tienen handler completo. Más aún: **SWAP y suspensión de
   mediano plazo** (que esa doc daba como "fuera de alcance del CP3, para la entrega final")
   también están implementados (`gestor_memoria_suspender_proceso`,
   `hilo_timer_suspension`, colas `cola_susp_*` activas). El código está en un estado mucho
   más avanzado de lo que ese doc asume.
2. **`codigo_mensaje_to_string()`** (`utils/protocolo.c`) está definida pero **nadie la
   llama en todo el repo** — código muerto. Ver [00-utils.md](00-utils.md).
3. **`memoria_instrucciones.h`** (`kernel_memory/src/memoria_instrucciones/`) declara
   `memoria_instrucciones_fetch` pero **no tiene `.c` ni se implementa/llama en ningún
   lado** — código muerto, la responsabilidad real la cumple
   `gestor_obtener_instruccion` en `gestor_procesos.c`. Ver
   [04-kernel_memory-infra.md](04-kernel_memory-infra.md).
4. **`memory_stick`: tanto CPU como Kernel Memory se conectan directo al stick** como
   clientes independientes del mismo canal de datos físicos (confirmado por los handshakes
   `HANDSHAKE_CPU` y `HANDSHAKE_KERNEL_MEMORY` en `manejar_cliente`) — CPU para el ciclo de
   instrucción, KM para SWAP/STDIN-STDOUT/compactación. Ver [06-memory_stick.md](06-memory_stick.md).
5. **`boot_loader.c` (`system_boot`) es código muerto en su totalidad.** Esto corrige una
   afirmación de este mismo documento en su primera versión, que decía que era "la rutina de
   arranque común a los 6 `main.c`" — verificado con `grep -rln "boot_loader.h"` que **ningún**
   `main.c` lo incluye. Cada módulo reimplementa a mano su propia secuencia de arranque. Ver
   [por-archivo/00-utils.md](por-archivo/00-utils.md).
6. **El enunciado oficial del TP 2026 ("Plug & Pray", `faq.utnso.com.ar/enunciado`) se cruzó
   contra el código** (algoritmos de scheduler, fórmula de MMU, `ALLOCATION_STRATEGY`,
   `SUSPENSION_TIMEOUT`, set de instrucciones, registros, multi-stick): de ~20 puntos
   verificados, 19 cumplen literalmente (mismos nombres de config, mismas fórmulas, mismas
   condiciones). La única divergencia es cosmética: el enum de instrucciones tiene `WAIT`/
   `SIGNAL` de más, no pedidos por este enunciado y sin implementar.

## Diagramas de arquitectura y flujos completos

Para diagramas Mermaid de la arquitectura general y las secuencias entre módulos
(ejecución de un proceso, máquina de estados del planificador, compactación,
suspensión/SWAP, notificaciones asíncronas), ver `../../DOCUMENTACION_FUNCIONES.md` en la
raíz del repo — esos diagramas siguen siendo correctos a alto nivel. Este análisis los
complementa yendo función por función; no los duplica.

Dentro de [03-kernel_scheduler-planificador.md](03-kernel_scheduler-planificador.md) y
[05-kernel_memory-gestor_memoria.md](05-kernel_memory-gestor_memoria.md) hay además
narrativas en prosa paso a paso de los flujos de herencia de prioridades, compactación y
suspensión/restauración vía SWAP, citando cada función en el orden real de ejecución.
