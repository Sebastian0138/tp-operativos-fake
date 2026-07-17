#ifndef IO_LOGGER_H
#define IO_LOGGER_H

#include <commons/log.h>
#include "../config/io_config.h"

t_log* io_boot_logger_create(void);
t_log* io_logger_create(t_io_config* config);

#endif
