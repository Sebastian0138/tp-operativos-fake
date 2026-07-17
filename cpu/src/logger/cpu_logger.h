#ifndef CPU_LOGGER_H
#define CPU_LOGGER_H

#include <commons/log.h>
#include "../config/cpu_config.h"

t_log* cpu_boot_logger_create(void);
t_log* cpu_logger_create(t_cpu_config* config);

#endif
