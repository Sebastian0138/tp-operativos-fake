#include "io_config.h"

#include <stdlib.h>
#include <string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return config_has_property(config, "LOG_LEVEL") &&
           config_has_property(config, "IP_KERNEL_SCHEDULER") &&
           config_has_property(config, "PORT_KERNEL_SCHEDULER");
}

t_io_config* io_config_create(char* path, t_log* logger) {
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

    t_io_config* io_config = malloc(sizeof(t_io_config));

    io_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    io_config->ip_kernel_scheduler = strdup(config_get_string_value(config, "IP_KERNEL_SCHEDULER"));
    io_config->port_kernel_scheduler = strdup(config_get_string_value(config, "PORT_KERNEL_SCHEDULER"));

    config_destroy(config);
    return io_config;
}

void io_config_log(t_io_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "IP_KERNEL_SCHEDULER=%s", config->ip_kernel_scheduler);
    log_info(logger, "PORT_KERNEL_SCHEDULER=%s", config->port_kernel_scheduler);
}

void io_config_destroy(t_io_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->ip_kernel_scheduler);
    free(config->port_kernel_scheduler);
    free(config);
}
