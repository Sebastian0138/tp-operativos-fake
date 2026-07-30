# Pruebas Oficiales

Una carpeta por cada prueba del `1c2026 - Plug & Pray - Pruebas.pdf`
(Documento de Pruebas Finales), con su propio `levantar.sh` para correrla
sola, cada módulo en su ventana de `gnome-terminal`.

| Carpeta | Prueba | Script |
|---|---|---|
| `01_prueba_base` | Prueba Base | `PLANI_PRE_0.prc` y luego `MEMORIA_PRE_0.prc` |
| `02_prueba_pcp` | Planificación Corto Plazo | `PCP.prc` |
| `03_prueba_memoria` | Prueba Memoria | `PLANI_MEM.prc` con `BEST` y luego con `WORST` |
| `04_prueba_pmp` | Planificación Mediano Plazo | `PMP.prc` (pide texto por STDIN) |
| `05_prueba_php` | Herencia de Prioridades | `PHP.prc` |
| `06_prueba_estabilidad_general` | Estabilidad General | sin script fijo, la define el ayudante en el momento |

Las pruebas con dos fases (`01_prueba_base`, `03_prueba_memoria`) bajan y
vuelven a levantar todo el sistema entre fase y fase; el script espera un
ENTER para pasar a la fase 2.

Requiere el proyecto compilado (`./compilar_todo.sh`) y `gnome-terminal`.
Los `.prc` de arranque viven en `../plug-n-pray-pruebas-main`; las configs
de cada prueba, en `../kernel_scheduler/config/ks_*.cfg` y
`../kernel_memory/config/km_*.cfg`.

Para correr las 6 en secuencia sin intervención manual (headless, con logs
guardados), usar `../scripts_pruebas/run_bateria.sh` en su lugar.
