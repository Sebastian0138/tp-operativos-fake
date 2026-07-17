#include "io_logger.h"

t_log* io_boot_logger_create(void) {
    return log_create(
        "logs/io_boot.log",
        "IO_BOOT",
        true,
        LOG_LEVEL_INFO
    );
}

t_log* io_logger_create(t_io_config* config) {
    return log_create(
        "logs/io.log",
        "IO",
        true,
        log_level_from_string(config->log_level)
    );
}
