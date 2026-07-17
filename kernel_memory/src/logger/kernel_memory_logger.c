#include "kernel_memory_logger.h"

t_log* kernel_memory_boot_logger_create(void) {
    return log_create(
        "logs/kernel_memory_boot.log",
        "KERNEL_MEMORY_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* kernel_memory_logger_create(t_kernel_memory_config* config) {
    return log_create(
        "logs/kernel_memory.log",
        "KERNEL_MEMORY",
        true,
        log_level_from_string(config->log_level)
    );
}