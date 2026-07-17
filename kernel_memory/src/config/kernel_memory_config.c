#include "kernel_memory_config.h"

#include <stdlib.h>
#include <string.h>

static bool tiene_todas_las_propiedades(t_config* config) {
    return
        config_has_property(config, "LOG_LEVEL") &&
        config_has_property(config, "SEGMENT_MAX_SIZE") &&
        config_has_property(config, "ALLOCATION_STRATEGY") &&
        config_has_property(config, "INSTRUCTION_DELAY") &&
        config_has_property(config, "COMPACTION_DELAY") &&
        config_has_property(config, "SCRIPTS_BASEPATH") &&
        config_has_property(config, "LISTEN_IP") &&
        config_has_property(config, "LISTEN_PORT");
}

t_kernel_memory_config* kernel_memory_config_create(char* path, t_log* logger) {
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

    t_kernel_memory_config* kernel_memory_config = malloc(sizeof(t_kernel_memory_config));

    kernel_memory_config->log_level = strdup(config_get_string_value(config, "LOG_LEVEL"));
    kernel_memory_config->segment_max_size = config_get_int_value(config, "SEGMENT_MAX_SIZE");
    kernel_memory_config->allocation_strategy = strdup(config_get_string_value(config, "ALLOCATION_STRATEGY"));
    kernel_memory_config->instruction_delay = config_get_int_value(config, "INSTRUCTION_DELAY");
    kernel_memory_config->compaction_delay = config_get_int_value(config, "COMPACTION_DELAY");
    kernel_memory_config->scripts_basepath = strdup(config_get_string_value(config, "SCRIPTS_BASEPATH"));
    kernel_memory_config->listen_ip = strdup(config_get_string_value(config, "LISTEN_IP"));
    kernel_memory_config->listen_port = strdup(config_get_string_value(config, "LISTEN_PORT"));

    config_destroy(config);
    return kernel_memory_config;
}

void kernel_memory_config_log(t_kernel_memory_config* config, t_log* logger) {
    log_info(logger, "Leyendo del archivo de configuración los siguientes parámetros:");
    log_info(logger, "LOG_LEVEL=%s", config->log_level);
    log_info(logger, "SEGMENT_MAX_SIZE=%d", config->segment_max_size);
    log_info(logger, "ALLOCATION_STRATEGY=%s", config->allocation_strategy);
    log_info(logger, "INSTRUCTION_DELAY=%d", config->instruction_delay);
    log_info(logger, "COMPACTION_DELAY=%d", config->compaction_delay);
    log_info(logger, "SCRIPTS_BASEPATH=%s", config->scripts_basepath);
    log_info(logger, "LISTEN_IP=%s", config->listen_ip);
    log_info(logger, "LISTEN_PORT=%s", config->listen_port);
}

void kernel_memory_config_destroy(t_kernel_memory_config* config) {
    if (config == NULL) {
        return;
    }

    free(config->log_level);
    free(config->allocation_strategy);
    free(config->scripts_basepath);
    free(config->listen_ip);
    free(config->listen_port);
    free(config);
}