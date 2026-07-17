#ifndef SWAP_LOGGER_H
#define SWAP_LOGGER_H

#include <commons/log.h>
#include "../config/swap_config.h"

t_log* swap_boot_logger_create(void);
t_log* swap_logger_create(t_swap_config* config);

#endif
