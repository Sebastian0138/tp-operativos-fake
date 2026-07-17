#include <stdio.h>
#include <stdlib.h>

#include "config/cpu_config.h"
#include "logger/cpu_logger.h"
#include "conexiones/conexiones.h"
#include "cpu/cpu.h"

int main(int argc, char** argv) {
    t_log* boot_logger = cpu_boot_logger_create();

    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando CPU");

    if (argc != 3) {
        log_error(boot_logger, "Cantidad de argumentos inválida. Uso: ./bin/cpu [Archivo Config] [Identificador]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    uint32_t id_cpu = (uint32_t) atoi(argv[2]);
    log_info(boot_logger, "Identificador de CPU: %u", id_cpu);

    log_info(boot_logger, "Intentando cargar archivo de configuración: %s", argv[1]);

    t_cpu_config* config = cpu_config_create(argv[1], boot_logger);
    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Archivo de configuración cargado correctamente");
    cpu_config_log(config, boot_logger);

    t_log* logger = cpu_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        cpu_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Logger principal creado correctamente");
    log_destroy(boot_logger);

    log_info(logger, "CPU iniciado correctamente");

    // --- CONEXIONES ---
    uint32_t segment_max_size;
    int socket_memoria = conectar_a_kernel_memory(config->ip_kernel_memory, config->port_kernel_memory, id_cpu, &segment_max_size, logger);
    int socket_ks = conectar_a_kernel_scheduler(config->ip_kernel_scheduler, config->port_kernel_scheduler, id_cpu, logger);

    cpu_config_destroy(config);

    // --- CREAR CPU Y EJECUTAR ---
    // Los Memory Sticks se descubren dinamicamente via la topologia de KM.
    struct t_cpu* cpu = cpu_create(socket_ks, socket_memoria, id_cpu, segment_max_size);
    actualizar_topologia_sticks(cpu, logger);
    atender_peticiones_kernel_scheduler(cpu, logger);

    // --- LIMPIEZA ---
    cpu_destroy(cpu);
    log_destroy(logger);

    return EXIT_SUCCESS;
}
