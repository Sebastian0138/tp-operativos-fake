#ifndef MEMORY_STICK_LOGGER_H
#define MEMORY_STICK_LOGGER_H

#include <commons/log.h>
#include "../config/memory_stick_config.h"

t_log* memory_stick_boot_logger_create(void);
t_log* memory_stick_logger_create(t_memory_stick_config* config);

#endif
