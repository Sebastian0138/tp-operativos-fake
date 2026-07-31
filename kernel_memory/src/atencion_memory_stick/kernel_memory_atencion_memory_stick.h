#ifndef KERNEL_MEMORY_ATENCION_MEMORY_STICK_H_
#define KERNEL_MEMORY_ATENCION_MEMORY_STICK_H_

#include <commons/log.h>
#include "../gestor_memoria/gestor_memoria.h"

/**
 * Atiende la conexion de control de un Memory Stick: recibe su INFORMAR_TAMANIO,
 * lo registra en la topologia y queda bloqueado monitoreando el socket hasta que
 * el stick se desconecta. Al volver, el fd puede cerrarse.
 */
void atender_memory_stick(int fd, t_gestor_memoria* gestor_memoria, t_log* logger);

#endif
