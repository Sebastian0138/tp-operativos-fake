#include "kernel_scheduler_config.h"

#include <stdlib.h>
#include <string.h>
#include <commons/string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return
        config_has_property(config, "LOG_LEVEL") &&
        config_has_property(config, "PLANIFICATION_ALGORITHM") &&
        config_has_property(config, "QUEUES_ALGORITHMS") &&
        config_has_property(config, "RR_QUANTUM") &&
        config_has_property(config, "QUEUE_PREEMPTION") &&
        config_has_property(config, "SUSPENSION_TIMEOUT") &&
        config_has_property(config, "LISTEN_IP") &&
        config_has_property(config, "LISTEN_PORT") &&
        config_has_property(config, "IP_MEMORY") &&
        config_has_property(config, "PORT_MEMORY");
}

t_kernel_scheduler_config* kernel_scheduler_config_create(char* path, t_log* logger) {
    t_config* config = config_create(path);
    if (config == NULL) {
        log_error(logger, "No se pudo abrir el archivo de configuración: %s", path);
        return NULL;
    }

    if (!tiene_todas_las_propiedades(config)) {
        log_error(logger, "El archivo de configuración no contiene todas las propiedades obligatorias");
        config_destroy(config);
        return NULL;
    }

    t_kernel_scheduler_config* ks_config = malloc(sizeof(t_kernel_scheduler_config));

    ks_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    ks_config->planification_algorithm = strdup(config_get_string_value(config, "PLANIFICATION_ALGORITHM"));
    ks_config->queues_algorithms = config_get_array_value(config, "QUEUES_ALGORITHMS");
    ks_config->rr_quantum = config_get_int_value(config, "RR_QUANTUM");
    ks_config->queue_preemption = strdup(config_get_string_value(config, "QUEUE_PREEMPTION"));
    ks_config->suspension_timeout = config_get_int_value(config, "SUSPENSION_TIMEOUT");
    ks_config->listen_ip = strdup(config_get_string_value(config, "LISTEN_IP"));
    ks_config->listen_port = strdup(config_get_string_value(config, "LISTEN_PORT"));
    ks_config->ip_memory = strdup(config_get_string_value(config, "IP_MEMORY"));
    ks_config->port_memory = strdup(config_get_string_value(config, "PORT_MEMORY"));

    config_destroy(config);
    return ks_config;
}

void kernel_scheduler_config_log(t_kernel_scheduler_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "PLANIFICATION_ALGORITHM=%s", config->planification_algorithm);
    
    int i = 0;
    while (config->queues_algorithms[i] != NULL) {
        log_info(logger, "QUEUES_ALGORITHMS[%d]=%s", i, config->queues_algorithms[i]);
        i++;
    }
    
    log_info(logger, "RR_QUANTUM=%d", config->rr_quantum);
    log_info(logger, "QUEUE_PREEMPTION=%s", config->queue_preemption);
    log_info(logger, "SUSPENSION_TIMEOUT=%d", config->suspension_timeout);
    log_info(logger, "LISTEN_IP=%s", config->listen_ip);
    log_info(logger, "LISTEN_PORT=%s", config->listen_port);
    log_info(logger, "IP_MEMORY=%s", config->ip_memory);
    log_info(logger, "PORT_MEMORY=%s", config->port_memory);
}

void kernel_scheduler_config_destroy(t_kernel_scheduler_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->planification_algorithm);
    string_array_destroy(config->queues_algorithms);
    free(config->queue_preemption);
    free(config->listen_ip);
    free(config->listen_port);
    free(config->ip_memory);
    free(config->port_memory);
    free(config);
}
