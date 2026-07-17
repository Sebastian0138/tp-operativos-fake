#ifndef KERNEL_MEMORY_CONFIG_H
#define KERNEL_MEMORY_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    int segment_max_size;
    char* allocation_strategy;
    int instruction_delay;
    int compaction_delay;
    char* scripts_basepath;
    char* listen_ip;
    char* listen_port;
} t_kernel_memory_config;

t_kernel_memory_config* kernel_memory_config_create(char* path, t_log* logger);
void kernel_memory_config_log(t_kernel_memory_config* config, t_log* logger);
void kernel_memory_config_destroy(t_kernel_memory_config* config);

#endif