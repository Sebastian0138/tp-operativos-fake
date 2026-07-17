#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "config/kernel_scheduler_config.h"
#include "logger/kernel_scheduler_logger.h"
#include "conexiones/conexiones.h"
#include "planificador/planificador.h"

int main(int argc, char** argv) {
    t_log* boot_logger = kernel_scheduler_boot_logger_create();

    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando Kernel Scheduler");

    if (argc != 3) {
        log_error(boot_logger, "Cantidad de argumentos inválida. Uso: ./bin/kernel_scheduler [Archivo Config] [Proceso Inicial]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Proceso inicial: %s", argv[2]);

    log_info(boot_logger, "Intentando cargar archivo de configuración: %s", argv[1]);

    t_kernel_scheduler_config* config = kernel_scheduler_config_create(argv[1], boot_logger);
    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Archivo de configuración cargado correctamente");
    kernel_scheduler_config_log(config, boot_logger);

    t_log* logger = kernel_scheduler_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        kernel_scheduler_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Logger principal creado correctamente");
    log_destroy(boot_logger);

    log_info(logger, "Kernel Scheduler iniciado correctamente");

    // --- INTEGRACION DE CONEXIONES Y PLANIFICACIÓN ---
    // Conectar a Kernel Memory (como cliente)
    int socket_memoria = conectar_a_kernel_memory(config->ip_memory, config->port_memory, logger);

    // Inicializar Planificador central
    t_planificador* planificador = planificador_create(config, logger, socket_memoria);
    if (planificador == NULL) {
        log_error(logger, "No se pudo crear el planificador");
        if (socket_memoria != -1) close(socket_memoria);
        log_destroy(logger);
        kernel_scheduler_config_destroy(config);
        return EXIT_FAILURE;
    }

    // Canal de notificaciones KM -> KS (mas memoria / memoria corrupta)
    conectar_canal_notificaciones_km(config->ip_memory, config->port_memory, planificador, logger);

    // Crear Hilos de Planificación
    pthread_t thread_largo, thread_corto;
    pthread_create(&thread_largo, NULL, hilo_planificador_largo_plazo, planificador);
    pthread_create(&thread_corto, NULL, hilo_planificador_corto_plazo, planificador);
    pthread_detach(thread_largo);
    pthread_detach(thread_corto);

    // Encolar Proceso Inicial (PID 0)
    t_pcb* pcb_0 = pcb_create(0, argv[2]);
    planificador_encolar_new(planificador, pcb_0);

    // Levantar el servidor del Kernel Scheduler para recibir a CPU e IO (es bloqueante)
    iniciar_servidor_kernel_scheduler(config->listen_port, planificador, config, logger);

    // Limpieza
    planificador_destroy(planificador);
    if (socket_memoria != -1) {
        close(socket_memoria);
    }
    log_destroy(logger);
    kernel_scheduler_config_destroy(config);

    return EXIT_SUCCESS;
}
