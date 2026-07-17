#include "memory_stick_config.h"

#include <stdlib.h>
#include <string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return
        config_has_property(config, "LOG_LEVEL") &&
        config_has_property(config, "MEMORY_DELAY") &&
        config_has_property(config, "LISTEN_IP") &&
        config_has_property(config, "LISTEN_PORT") &&
        config_has_property(config, "IP_KERNEL_MEMORY") &&
        config_has_property(config, "PORT_KERNEL_MEMORY");
}

t_memory_stick_config* memory_stick_config_create(char* path, t_log* logger) {
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

    t_memory_stick_config* memory_stick_config = malloc(sizeof(t_memory_stick_config));

    memory_stick_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    memory_stick_config->memory_delay = config_get_int_value(config, "MEMORY_DELAY");
    memory_stick_config->listen_ip = strdup(config_get_string_value(config, "LISTEN_IP"));
    memory_stick_config->listen_port = strdup(config_get_string_value(config, "LISTEN_PORT"));
    memory_stick_config->ip_kernel_memory = strdup(config_get_string_value(config, "IP_KERNEL_MEMORY"));
    memory_stick_config->port_kernel_memory = strdup(config_get_string_value(config, "PORT_KERNEL_MEMORY"));

    config_destroy(config);
    return memory_stick_config;
}

void memory_stick_config_log(t_memory_stick_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "MEMORY_DELAY=%d", config->memory_delay);
    log_info(logger, "LISTEN_IP=%s", config->listen_ip);
    log_info(logger, "LISTEN_PORT=%s", config->listen_port);
    log_info(logger, "IP_KERNEL_MEMORY=%s", config->ip_kernel_memory);
    log_info(logger, "PORT_KERNEL_MEMORY=%s", config->port_kernel_memory);
}

void memory_stick_config_destroy(t_memory_stick_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->listen_ip);
    free(config->listen_port);
    free(config->ip_kernel_memory);
    free(config->port_kernel_memory);
    free(config);
}
