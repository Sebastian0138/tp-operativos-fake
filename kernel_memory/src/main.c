#include <stdio.h>
#include <stdlib.h>
#include "config/kernel_memory_config.h"
#include "logger/kernel_memory_logger.h"
#include "conexiones/conexiones.h"
#include "gestor_procesos/kernel_memory_gestor_procesos.h"
#include "gestor_memoria/gestor_memoria.h"

int main(int argc, char** argv) {
    t_log* boot_logger = kernel_memory_boot_logger_create();
    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando Kernel Memory");

    if (argc != 2) {
        log_error(boot_logger, "Uso: ./bin/kernel_memory [Archivo Config]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    t_kernel_memory_config* config = kernel_memory_config_create(argv[1], boot_logger);
    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    kernel_memory_config_log(config, boot_logger);

    t_log* logger = kernel_memory_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        kernel_memory_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_destroy(boot_logger);
    log_info(logger, "Kernel Memory iniciado correctamente");

    t_gestor_procesos* gestor = gestor_procesos_create();
    if (gestor == NULL) {
        log_error(logger, "No se pudo crear el gestor de procesos");
        kernel_memory_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    t_gestor_memoria* gestor_memoria = gestor_memoria_create(config, gestor, logger);
    if (gestor_memoria == NULL) {
        log_error(logger, "No se pudo crear el gestor de memoria");
        gestor_procesos_destroy(gestor);
        kernel_memory_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    iniciar_servidor_kernel_memory(config->listen_port, config, gestor, gestor_memoria, logger);

    gestor_memoria_destroy(gestor_memoria);
    gestor_procesos_destroy(gestor);
    kernel_memory_config_destroy(config);
    log_destroy(logger);

    return EXIT_SUCCESS;
}