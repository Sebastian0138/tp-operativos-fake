#ifndef MEMORY_STICK_CONEXIONES_H
#define MEMORY_STICK_CONEXIONES_H

#include <commons/log.h>
#include "config/memory_stick_config.h"
#include "memoria/memoria.h"

// Crea el socket de escucha (bind + listen). Debe llamarse ANTES de conectarse
// a Kernel Memory: apenas KM registra el stick abre una conexion de datos hacia
// este servidor, que ya tiene que estar escuchando.
int crear_escucha_memory_stick(t_memory_stick_config* config, t_memoria_stick* memoria, t_log* logger);

// Loop de accept que atiende pedidos de lectura/escritura de CPUs y Kernel Memory (bloqueante)
void atender_conexiones_memory_stick(int fd_escucha);

// Conecta al Kernel Memory como cliente e informa tamanio + ip:puerto de escucha
int conectar_a_kernel_memory(t_memory_stick_config* config, uint32_t tamanio, t_log* logger);

#endif // MEMORY_STICK_CONEXIONES_H
