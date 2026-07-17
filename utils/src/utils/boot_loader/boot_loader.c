#include "boot_loader.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool validate_config_rules(t_config* config, char** required_keys, int num_keys, t_log* boot_logger) {
    bool all_valid = true;
    for (int i = 0; i < num_keys; i++) {
        if (!config_has_property(config, required_keys[i])) {
            log_error(boot_logger, "Falta la clave de configuración requerida: %s", required_keys[i]);
            all_valid = false;
        }
    }
    return all_valid;
}

bool system_boot(const char* module_name, const char* config_path, char** required_keys, int num_keys, t_config** out_config, t_log** out_logger) {
    // 1. Iniciar el boot_logger
    t_log* boot_logger = log_create("boot.log", "BootLoader", true, LOG_LEVEL_INFO);
    if (!boot_logger) {
        printf("ERROR CRITICO: Falló al crear boot_logger.log\n");
        return false;
    }

    log_info(boot_logger, "Iniciando módulo %s...", module_name);
    log_info(boot_logger, "Cargando configuración desde: %s", config_path);

    // 2. Iniciar el config
    t_config* config = config_create((char*)config_path);
    if (!config) {
        log_error(boot_logger, "Falló al abrir el archivo de configuración: %s", config_path);
        log_destroy(boot_logger);
        return false;
    }

    // 3. Validar reglas de config
    if (num_keys > 0) {
        if (!validate_config_rules(config, required_keys, num_keys, boot_logger)) {
            log_error(boot_logger, "La validación de la configuración falló para %s. Abortando inicio.", module_name);
            config_destroy(config);
            log_destroy(boot_logger);
            return false;
        }
        log_info(boot_logger, "Configuración validada exitosamente. Las %d claves requeridas están presentes.", num_keys);
    } else {
        log_info(boot_logger, "No hay claves estrictamente requeridas para %s. Omitiendo validación.", module_name);
    }

    // 4. Iniciar el logger principal del módulo
    char logger_name[256];
    snprintf(logger_name, sizeof(logger_name), "%s.log", module_name);
    
    // Lo creamos con LOG_LEVEL_DEBUG por defecto
    t_log* main_logger = log_create(logger_name, (char*)module_name, true, LOG_LEVEL_DEBUG);
    if (!main_logger) {
        log_error(boot_logger, "Falló al crear el logger principal para %s", module_name);
        config_destroy(config);
        log_destroy(boot_logger);
        return false;
    }

    *out_config = config;
    *out_logger = main_logger;

    log_info(boot_logger, "Inicio exitoso. Transfiriendo al logger principal de %s.", module_name);
    
    // 5. Destruir boot_logger ya que terminó su labor
    log_destroy(boot_logger);

    return true;
}
