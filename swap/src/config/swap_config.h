#ifndef SWAP_CONFIG_H
#define SWAP_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    char* swap_file_path;
    int swap_file_size;
    int block_size;
    char* ip_kernel_memory;
    char* port_kernel_memory;
} t_swap_config;

t_swap_config* swap_config_create(char* path, t_log* logger);
void swap_config_log(t_swap_config* config, t_log* logger);
void swap_config_destroy(t_swap_config* config);

#endif
