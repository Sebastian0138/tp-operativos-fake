#include "cpu_config.h"

#include <stdlib.h>
#include <string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return config_has_property(config, "LOG_LEVEL") &&
           config_has_property(config, "IP_KERNEL_SCHEDULER") &&
           config_has_property(config, "PORT_KERNEL_SCHEDULER") &&
           config_has_property(config, "IP_KERNEL_MEMORY") &&
           config_has_property(config, "PORT_KERNEL_MEMORY");
}

t_cpu_config* cpu_config_create(char* path, t_log* logger) {
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

    t_cpu_config* cpu_config = malloc(sizeof(t_cpu_config));

    cpu_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    cpu_config->ip_kernel_scheduler = strdup(config_get_string_value(config, "IP_KERNEL_SCHEDULER"));
    cpu_config->port_kernel_scheduler = strdup(config_get_string_value(config, "PORT_KERNEL_SCHEDULER"));
    cpu_config->ip_kernel_memory = strdup(config_get_string_value(config, "IP_KERNEL_MEMORY"));
    cpu_config->port_kernel_memory = strdup(config_get_string_value(config, "PORT_KERNEL_MEMORY"));

    config_destroy(config);
    return cpu_config;
}

void cpu_config_log(t_cpu_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "IP_KERNEL_SCHEDULER=%s", config->ip_kernel_scheduler);
    log_info(logger, "PORT_KERNEL_SCHEDULER=%s", config->port_kernel_scheduler);
    log_info(logger, "IP_KERNEL_MEMORY=%s", config->ip_kernel_memory);
    log_info(logger, "PORT_KERNEL_MEMORY=%s", config->port_kernel_memory);
}

void cpu_config_destroy(t_cpu_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->ip_kernel_scheduler);
    free(config->port_kernel_scheduler);
    free(config->ip_kernel_memory);
    free(config->port_kernel_memory);
    free(config);
}
