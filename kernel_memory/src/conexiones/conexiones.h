#ifndef KERNEL_MEMORY_CONEXIONES_H
#define KERNEL_MEMORY_CONEXIONES_H

#include <commons/log.h>
#include "../gestor_procesos/kernel_memory_gestor_procesos.h"
#include "../gestor_memoria/gestor_memoria.h"
#include "../config/kernel_memory_config.h"

void iniciar_servidor_kernel_memory(const char*             puerto,
                                    t_kernel_memory_config* config,
                                    t_gestor_procesos*      gestor,
                                    t_gestor_memoria*       gestor_memoria,
                                    t_log*                  logger);

#endif
