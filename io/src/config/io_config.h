#ifndef IO_CONFIG_H
#define IO_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    char* ip_kernel_scheduler;
    char* port_kernel_scheduler;
} t_io_config;

t_io_config* io_config_create(char* path, t_log* logger);
void io_config_log(t_io_config* config, t_log* logger);
void io_config_destroy(t_io_config* config);

#endif
