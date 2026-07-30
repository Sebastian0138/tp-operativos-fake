# Prueba Estabilidad General

Documento de Pruebas Finales, pag. 10.

## Procedimiento

1. Iniciar los módulos indicados por el ayudante evaluador con las configuraciones indicadas.
2. Esperar las indicaciones del ayudante evaluador.

## Resultados Esperados

No se observan esperas activas ni memory leaks.

## Por qué no hay `levantar.sh`

Esta prueba no tiene script ni configuración fija: los módulos, configs y
duración los define el ayudante en el momento de la evaluación. No hay nada
que automatizar de antemano.

Para levantar módulos sueltos con la configuración que pida el ayudante, usar
el launcher interactivo genérico del repo:

```bash
./iniciar_todo.sh
```

Te va a preguntar cantidad de Memory Sticks, tamaño, cantidad de CPUs y qué
tipos de IO levantar, y abre cada módulo en su propia ventana con los configs
por defecto de `*/config/*.cfg` (editables antes de correr si el ayudante pide
otros valores).
