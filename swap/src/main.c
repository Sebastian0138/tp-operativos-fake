#include <stdio.h>
#include <stdlib.h>

#include "config/swap_config.h"
#include "logger/swap_logger.h"
#include "conexiones/conexiones.h"

int main(int argc, char** argv) {
    t_log* boot_logger = swap_boot_logger_create();

    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando Swap");

    if (argc != 2) {
        log_error(boot_logger, "Cantidad de argumentos inválida. Uso: ./bin/swap [Archivo Config]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Intentando cargar archivo de configuración: %s", argv[1]);

    t_swap_config* config = swap_config_create(argv[1], boot_logger);
    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Archivo de configuración cargado correctamente");
    swap_config_log(config, boot_logger);

    t_log* logger = swap_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        swap_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Logger principal creado correctamente");
    log_destroy(boot_logger);

    log_info(logger, "Swap iniciado correctamente");

    // Crear/abrir el archivo de SWAP. Se asume que inicia libre: no hace
    // falta limpiar su contenido explicitamente.
    FILE* archivo_swap = fopen(config->swap_file_path, "w+b");
    if (archivo_swap == NULL) {
        log_error(logger, "No se pudo crear el archivo de SWAP: %s", config->swap_file_path);
        swap_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    // Conectarse a KM, informar tamanios y atender lecturas/escrituras de bloques
    conectar_y_atender_kernel_memory(config, archivo_swap, logger);

    fclose(archivo_swap);
    log_destroy(logger);
    swap_config_destroy(config);

    return EXIT_SUCCESS;
}
