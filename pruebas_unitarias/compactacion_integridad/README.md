# Prueba unitaria — Integridad de los datos al compactar (2 Memory Sticks)

Verifica que la compactación no sólo consolide los huecos, sino que **el contenido de los
segmentos llegue intacto a su nueva ubicación**, incluso cuando un segmento vive a caballo
de dos Memory Sticks.

## Por qué existe esta prueba

Las demás pruebas de compactación
([`../dessuspension_compactacion/`](../dessuspension_compactacion/) y
[`../desalojo_compactacion/`](../desalojo_compactacion/)) comprueban que **los tamaños
entren** después de compactar: que un segmento de 30 que antes no cabía, ahora quepa.

Eso no dice nada sobre los bytes. **Una compactación que consolide los huecos
perfectamente pero escriba los datos en el offset equivocado pasa las dos en verde.**

Y no es un riesgo teórico: `mover_segmento()` (`gestor_memoria.c:551`) delega en
`copiar_segmento_entre_sticks()` (`:500`), que tiene que **partir la copia** cuando el
segmento cruza la frontera entre dos sticks. Ahí es donde viven los off-by-one. Con un solo
stick esa rama ni siquiera se ejecuta.

> **Valgrind no cubre esto.** La memoria que se compacta no está en el espacio de
> direcciones de Kernel Memory: vive en el `calloc` del proceso `memory_stick`
> (`memory_stick/src/memoria/memoria.c:9`), y KM la toca mandando una dirección y un tamaño
> por socket (`stick_operar()`, `gestor_memoria.c:396`). Si el cálculo de la base nueva está
> mal, KM hace un `send()` impecable de un `uint32_t` bien inicializado — con el número
> equivocado. Valgrind sobre KM no ve nada, y valgrind sobre el stick ve un `memcpy` dentro
> de un buffer válido en un offset válido. Es un error de **aritmética**, no de memoria.

---

## Cómo levantarla

```bash
./pruebas_unitarias/compactacion_integridad/levantar.sh          # sticks de 90 y 110
./pruebas_unitarias/compactacion_integridad/levantar.sh 90 110   # explícito
```

**No hay que tocar nada.** Dura ~40 segundos. Mirá la ventana **5 IO STDOUT**.

### Qué se espera ver

La ventana del IO STDOUT tiene que imprimir **dos veces exactamente lo mismo**:

```
A a B b C c D d E e        <- CONTROL: leído antes de compactar
A a B b C c D d E e        <- leído después de compactar
```

Concatenado, tal como sale del log: `AaBbCcDdEeAaBbCcDdEe`.

Si la segunda tanda difiere de la primera en un solo carácter, **la compactación está
corrompiendo datos**. Si las dos están mal, el problema es anterior (escritura o MMU) y la
compactación no tiene nada que ver: eso es lo que aporta el control.

### La evidencia de que cruzó la frontera

Corrida real, operaciones sobre los sticks **durante** la compactación
(`05:54:37:625` → `05:54:38:267`):

```
37:675  [stick 1]  ## Lectura de 20 bytes
37:725  [stick 1]  ## Escritura de 20 bytes
37:775  [stick 1]  ## Lectura de 10 bytes      ┐ segmento 4, leído de [80,100)
37:866  [stick 2]  ## Lectura de 10 bytes      ┘ PARTIDO entre los dos sticks
37:916  [stick 1]  ## Escritura de 20 bytes
37:966  [stick 2]  ## Lectura de 20 bytes
38:016  [stick 1]  ## Escritura de 20 bytes
38:066  [stick 2]  ## Lectura de 20 bytes
38:116  [stick 1]  ## Escritura de 10 bytes    ┐ segmento 8, aterrizando en [80,100)
38:166  [stick 2]  ## Escritura de 10 bytes    ┘ PARTIDO entre los dos sticks
```

**4 operaciones de 10 bytes** (2 segmentos partidos × 2 sticks cada uno) y **6 de 20** (las
que caen enteras dentro de un stick). La cuenta cierra exacto con el diseño de abajo.

`copiar_segmento_entre_sticks()` quedó ejercitado en las dos direcciones: **leyendo** un
segmento que estaba a caballo de la frontera, y **escribiendo** un segmento que aterriza a
caballo de ella.

---

## El diseño: por qué 90 y 110, y no 100 y 100

Es el corazón de la prueba. Los dos sticks tienen tamaños **distintos y no múltiplos del
tamaño de segmento**, para que la frontera caiga **dentro** de un segmento.

```
stick 1 -> direcciones globales [0,  90)
stick 2 -> direcciones globales [90, 200)
                                 ^
                                 frontera
```

Con 10 segmentos de 20 bytes, las bases son 0, 20, 40, ..., 180:

```
base:   0    20   40   60   80  |100  120  140  160  180
seg:  [ 0][ 1][ 2][ 3][ 4][ 5][ 6][ 7][ 8][ 9]
                          ^^^^
                          el segmento 4 ocupa [80,100) y CRUZA la frontera de 90
```

Con dos sticks de **100 y 100**, la frontera caería en 100, que es múltiplo de 20: **ningún
segmento cruzaría** y la rama que se quiere probar no se ejecutaría nunca. La prueba pasaría
en verde sin probar nada.

### Qué pasa al compactar

Se liberan los impares (bases 20, 60, 100, 140, 180) y sobreviven los pares:

| Segmento | Base antes | Base después | Al moverse |
|---|---|---|---|
| 0 | 0 | 0 | no se mueve |
| 2 | 40 | 20 | entero, dentro del stick 1 |
| 4 | **80** | 40 | **lectura partida** (10 + 10) |
| 6 | 120 | 60 | entero: lee del stick 2, escribe en el stick 1 |
| 8 | 160 | **80** | **escritura partida** (10 + 10) |

De ahí salen las 4 operaciones de 10 bytes y las 6 de 20.

### El disparo de la compactación

```
libre total: 100 bytes (5 huecos de 20)      hueco más grande: 20

MEM_ALLOC 10 30  ->  30 > 20   (no entra en ningún hueco)
                     30 <= 100 (sí entra en el total)
                     -> CREAR_SEGMENTO_NECESITA_COMPACTACION
```

(`gestor_memoria_crear_segmento()`, `gestor_memoria.c:282-292`.)

---

## Archivos

| Archivo | Qué es |
|---|---|
| `levantar.sh` | Lanzador (8 ventanas, **2 Memory Sticks**) |
| `test_padre` | Pseudocódigo del PID 0, generado con las cuentas de arriba |
| `km_sys.cfg` / `ks_sys.cfg` | Configs |

## El pseudocódigo — `test_padre` (107 instrucciones)

Cinco fases:

```
1)  MEM_ALLOC 0..9 de 20 bytes           -> llena los 200 bytes
2)  marca los segmentos 0,2,4,6,8:
        SET DI <seg*256 + 0>             -> letra mayúscula en el offset 0
        SET AX <letra>
        MOV_OUT AX
        SET DI <seg*256 + 19>            -> minúscula en el offset 19
        SET AX <letra>
        MOV_OUT AX
3)  CONTROL: los lee todos con STDOUT    -> imprime "A a B b C c D d E e"
4)  MEM_FREE 1,3,5,7,9                   -> fragmenta
    MEM_ALLOC 10 30                      -> DISPARA LA COMPACTACIÓN
5)  los vuelve a leer con STDOUT         -> tiene que imprimir lo MISMO
```

Tres decisiones que importan:

**Las marcas van en el offset 0 y en el 19** (el primero y el último byte del segmento de
20). Una copia con off-by-one, un tamaño mal calculado o un desplazamiento corrido
arruinaría al menos uno de los dos extremos. Marcar sólo el byte 0 dejaría pasar un
`tamanio - 1`.

**La fase 3 es un control, no relleno.** Si la primera tanda ya sale mal, el problema está
en la escritura o en la MMU y la compactación es inocente. Sin control, un fallo sería
ambiguo.

**Las direcciones se calculan como `segmento * SEGMENT_MAX_SIZE + offset`**, que es lo que
hace la MMU al revés (`mmu_traducir()`, `instrucciones.c:25-26`):

```c
num_segmento   = dir / seg_max;
desplazamiento = dir % seg_max;
```

Con `SEGMENT_MAX_SIZE=256`, el segmento 4 offset 19 es la dirección lógica `4*256+19 = 1043`.

> **Detalle de registros:** las direcciones van en `DI` y `EAX` porque `AX`..`DX` son de
> **8 bits** (`registros.h:13-16`) y no les entra un valor mayor a 255. `MOV_OUT AX` escribe
> 1 byte porque `tamanio_registro("AX")` devuelve 1 (`registros_cpu.c:36`); si usaras `EAX`
> escribiría 4 y se pisarían las marcas.

---

## Las configs

- **`km_sys.cfg`**: sólo cambia
  `SCRIPTS_BASEPATH=../pruebas_unitarias/compactacion_integridad`. El resto default:
  `BEST` fit, `SEGMENT_MAX_SIZE=256`, `COMPACTION_DELAY=100`.
- **`ks_sys.cfg`**: sólo cambia `SUSPENSION_TIMEOUT=15000` (el default es 1000). Son 20
  `STDOUT` y cada uno bloquea al proceso; con el timeout bajo se suspendería a mitad del
  volcado y el log se llenaría de swap que no tiene nada que ver.

**El orden de arranque de los sticks NO es indistinto.** El primero en registrarse se queda
con las direcciones bajas (`gestor_memoria_registrar_stick()`, `gestor_memoria.c:93`, va
ampliando el total). Por eso el lanzador levanta el de 90 **antes** que el de 110. Si los
invertís, la frontera queda en 110 y ningún segmento la cruza: la prueba sigue pasando pero
deja de probar lo que dice probar.
