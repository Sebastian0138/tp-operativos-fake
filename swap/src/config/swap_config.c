#include "swap_config.h"

#include <stdlib.h>
#include <string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return
        config_has_property(config, "LOG_LEVEL") &&
        config_has_property(config, "SWAP_FILE_PATH") &&
        config_has_property(config, "SWAP_FILE_SIZE") &&
        config_has_property(config, "BLOCK_SIZE") &&
        config_has_property(config, "IP_KERNEL_MEMORY") &&
        config_has_property(config, "PORT_KERNEL_MEMORY");
}

t_swap_config* swap_config_create(char* path, t_log* logger) {
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

    t_swap_config* swap_config = malloc(sizeof(t_swap_config));

    swap_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    swap_config->swap_file_path = strdup(config_get_string_value(config, "SWAP_FILE_PATH"));
    swap_config->swap_file_size = config_get_int_value(config, "SWAP_FILE_SIZE");
    swap_config->block_size = config_get_int_value(config, "BLOCK_SIZE");
    swap_config->ip_kernel_memory = strdup(config_get_string_value(config, "IP_KERNEL_MEMORY"));
    swap_config->port_kernel_memory = strdup(config_get_string_value(config, "PORT_KERNEL_MEMORY"));

    config_destroy(config);
    return swap_config;
}

void swap_config_log(t_swap_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "SWAP_FILE_PATH=%s", config->swap_file_path);
    log_info(logger, "SWAP_FILE_SIZE=%d", config->swap_file_size);
    log_info(logger, "BLOCK_SIZE=%d", config->block_size);
    log_info(logger, "IP_KERNEL_MEMORY=%s", config->ip_kernel_memory);
    log_info(logger, "PORT_KERNEL_MEMORY=%s", config->port_kernel_memory);
}

void swap_config_destroy(t_swap_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->swap_file_path);
    free(config->ip_kernel_memory);
    free(config->port_kernel_memory);
    free(config);
}
