#ifndef MEMORY_STICK_CONFIG_H
#define MEMORY_STICK_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    int memory_delay;
    char* listen_ip;
    char* listen_port;
    char* ip_kernel_memory;
    char* port_kernel_memory;
} t_memory_stick_config;

t_memory_stick_config* memory_stick_config_create(char* path, t_log* logger);
void memory_stick_config_log(t_memory_stick_config* config, t_log* logger);
void memory_stick_config_destroy(t_memory_stick_config* config);

#endif
