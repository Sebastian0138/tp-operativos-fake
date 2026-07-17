#include "memory_stick_logger.h"

t_log* memory_stick_boot_logger_create(void) {
    return log_create(
        "logs/memory_stick_boot.log",
        "MEMORY_STICK_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* memory_stick_logger_create(t_memory_stick_config* config) {
    return log_create(
        "logs/memory_stick.log",
        "MEMORY_STICK",
        true,
        log_level_from_string(config->log_level)
    );
}
