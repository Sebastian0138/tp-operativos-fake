#include "cpu_logger.h"

t_log* cpu_boot_logger_create(void) {
    return log_create(
        "logs/cpu_boot.log",
        "CPU_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* cpu_logger_create(t_cpu_config* config) {
    return log_create(
        "logs/cpu.log",
        "CPU",
        true,
        log_level_from_string(config->log_level)
    );
}
