#ifndef KERNEL_SCHEDULER_CONFIG_H
#define KERNEL_SCHEDULER_CONFIG_H

#include <commons/config.h>
#include <commons/log.h>
#include <stdbool.h>

typedef struct {
    char* log_level;
    char* planification_algorithm;
    char** queues_algorithms;
    int rr_quantum;
    char* queue_preemption;
    int suspension_timeout;
    char* listen_ip;
    char* listen_port;
    char* ip_memory;
    char* port_memory;
} t_kernel_scheduler_config;

t_kernel_scheduler_config* kernel_scheduler_config_create(char* path, t_log* logger);
void kernel_scheduler_config_log(t_kernel_scheduler_config* config, t_log* logger);
void kernel_scheduler_config_destroy(t_kernel_scheduler_config* config);

#endif
