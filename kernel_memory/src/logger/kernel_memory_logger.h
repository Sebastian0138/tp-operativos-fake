#ifndef KERNEL_MEMORY_LOGGER_H
#define KERNEL_MEMORY_LOGGER_H

#include <commons/log.h>
#include "../config/kernel_memory_config.h"

t_log* kernel_memory_boot_logger_create(void);
t_log* kernel_memory_logger_create(t_kernel_memory_config* config);

#endif