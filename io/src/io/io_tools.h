#ifndef IO_IO_H_
#define IO_IO_H_

#include <stdint.h>
#include <commons/log.h>

#include <utils/protocolo.h>

/**
 * Tipos de dispositivo IO que este modulo puede manejar.
 */
typedef enum {
    SLEEP,
    STDIN,
    STDOUT
} t_tipo_dispositivo_io;

/**
 * Estado del modulo IO.
 * Contiene la conexion al Kernel Scheduler y el logger.
 */
typedef struct {
    t_tipo_dispositivo_io tipo_dispositivo;
    int socket_kernel;
    t_log* logger;
} t_io;

#endif /* IO_IO_H_ */
