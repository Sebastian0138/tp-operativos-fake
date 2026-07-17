#ifndef KERNEL_MEMORY_ATENCION_CPU_H_
#define KERNEL_MEMORY_ATENCION_CPU_H_

#include <commons/log.h>
#include "../gestor_procesos/kernel_memory_gestor_procesos.h"
#include "../gestor_memoria/gestor_memoria.h"
#include "../config/kernel_memory_config.h"

void atender_cpu(int fd,
                 t_gestor_procesos*      gestor,
                 t_gestor_memoria*       gestor_memoria,
                 t_kernel_memory_config* config,
                 t_log*                  logger);

#endif
