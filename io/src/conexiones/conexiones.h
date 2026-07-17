#ifndef IO_CONEXIONES_H
#define IO_CONEXIONES_H

#include <commons/log.h>

int conectar_a_kernel_scheduler(const char* ip, const char* puerto, const char* nombre_interfaz, t_log* logger);

#endif // IO_CONEXIONES_H
