#include "kernel_scheduler_logger.h"

t_log* kernel_scheduler_boot_logger_create(void) {
    return log_create(
        "logs/kernel_scheduler_boot.log",
        "KERNEL_SCHEDULER_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* kernel_scheduler_logger_create(t_kernel_scheduler_config* config) {
    return log_create(
        "logs/kernel_scheduler.log",
        "KERNEL_SCHEDULER",
        true,
        log_level_from_string(config->log_level)
    );
}
