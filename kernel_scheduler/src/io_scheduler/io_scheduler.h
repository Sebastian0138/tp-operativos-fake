#ifndef KERNEL_SCHEDULER_IO_SCHEDULER_H_
#define KERNEL_SCHEDULER_IO_SCHEDULER_H_

#include <stdint.h>

#include <utils/protocolo.h>

/**
 * Tipos de operacion IO soportados.
 */
typedef enum {
    IO_SLEEP,
    IO_STDIN,
    IO_STDOUT
} t_tipo_io;

/**
 * Pedido de IO que el Scheduler encola para el modulo IO.
 */
typedef struct {
    uint32_t pid;
    t_tipo_io tipo;
    uint32_t tiempo_ms;         // Para IO_SLEEP
    uint32_t direccion_logica;  // Para IO_STDIN/IO_STDOUT
    uint32_t tamanio;           // Bytes a leer/escribir
    char* contenido;            // Buffer de datos (puede ser NULL)
} t_pedido_io;

#endif /* KERNEL_SCHEDULER_IO_SCHEDULER_H_ */
