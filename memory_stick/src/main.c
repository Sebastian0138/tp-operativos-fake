#include <stdio.h>
#include <stdlib.h>

#include "config/memory_stick_config.h"
#include "logger/memory_stick_logger.h"
#include "conexiones/conexiones.h"
#include "memoria/memoria.h"

int main(int argc, char** argv) {
    t_log* boot_logger = memory_stick_boot_logger_create();

    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando Memory Stick");

    if (argc != 3) {
        log_error(boot_logger, "Cantidad de argumentos inválida. Uso: ./bin/memory_stick [Archivo Config] [Tamaño]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    uint32_t tamanio = (uint32_t) strtoul(argv[2], NULL, 10);
    if (tamanio == 0) {
        log_error(boot_logger, "Tamaño inválido: %s", argv[2]);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Intentando cargar archivo de configuración: %s", argv[1]);

    t_memory_stick_config* config = memory_stick_config_create(argv[1], boot_logger);
    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Archivo de configuración cargado correctamente");
    memory_stick_config_log(config, boot_logger);

    t_log* logger = memory_stick_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        memory_stick_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Logger principal creado correctamente");
    log_destroy(boot_logger);

    log_info(logger, "Memory Stick iniciado correctamente (%u bytes)", tamanio);

    // Reservar el bloque de memoria que representa este stick
    t_memoria_stick* memoria = memoria_stick_create(tamanio);
    if (memoria == NULL) {
        log_error(logger, "No se pudo reservar el bloque de %u bytes", tamanio);
        memory_stick_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    // Primero bind+listen: KM abre una conexion de datos hacia este servidor
    // apenas recibe el INFORMAR_TAMANIO, asi que ya tiene que estar escuchando.
    int fd_escucha = crear_escucha_memory_stick(config, memoria, logger);
    if (fd_escucha == -1) {
        memoria_stick_destroy(memoria);
        memory_stick_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    // Conectarse a Kernel Memory e informar tamanio + ip:puerto de escucha
    int socket_memoria = conectar_a_kernel_memory(config, tamanio, logger);
    if (socket_memoria == -1) {
        memoria_stick_destroy(memoria);
        memory_stick_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    }

    // Atender lecturas/escrituras de CPUs y KM (bloqueante)
    atender_conexiones_memory_stick(fd_escucha);

    memoria_stick_destroy(memoria);
    memory_stick_config_destroy(config);
    log_destroy(logger);

    return EXIT_SUCCESS;
}
