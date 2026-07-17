#ifndef KERNEL_SCHEDULER_LOGGER_H
#define KERNEL_SCHEDULER_LOGGER_H

#include <commons/log.h>
#include "../config/kernel_scheduler_config.h"

t_log* kernel_scheduler_boot_logger_create(void);
t_log* kernel_scheduler_logger_create(t_kernel_scheduler_config* config);

#endif
