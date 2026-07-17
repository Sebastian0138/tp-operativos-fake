#ifndef CPU_CONFIG_H
#define CPU_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    char* ip_kernel_scheduler;
    char* port_kernel_scheduler;
    char* ip_kernel_memory;
    char* port_kernel_memory;
} t_cpu_config;

t_cpu_config* cpu_config_create(char* path, t_log* logger);
void cpu_config_log(t_cpu_config* config, t_log* logger);
void cpu_config_destroy(t_cpu_config* config);

#endif
