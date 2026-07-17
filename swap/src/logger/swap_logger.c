#include "swap_logger.h"

t_log* swap_boot_logger_create(void) {
    return log_create(
        "logs/swap_boot.log",
        "SWAP_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* swap_logger_create(t_swap_config* config) {
    return log_create(
        "logs/swap.log",
        "SWAP",
        true,
        log_level_from_string(config->log_level)
    );
}
