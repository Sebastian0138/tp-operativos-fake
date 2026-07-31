#include "conexiones.h"
#include "../atencion_cpu/kernel_memory_atencion_cpu.h"
#include "../atencion_scheduler/kernel_memory_atencion_scheduler.h"
#include "../atencion_scheduler_notif/kernel_memory_atencion_scheduler_notif.h"
#include "../atencion_memory_stick/kernel_memory_atencion_memory_stick.h"
#include "../atencion_swap/kernel_memory_atencion_swap.h"
#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>

static t_log*                   _logger = NULL;
static t_gestor_procesos*       _gestor = NULL;
static t_gestor_memoria*        _gestor_memoria = NULL;
static t_kernel_memory_config*  _config = NULL;

void* manejar_cliente(void* arg);

void iniciar_servidor_kernel_memory(const char*             puerto,
                                    t_kernel_memory_config* config,
                                    t_gestor_procesos*      gestor,
                                    t_gestor_memoria*       gestor_memoria,
                                    t_log*                  logger) {
    _logger = logger;
    _gestor = gestor;
    _gestor_memoria = gestor_memoria;
    _config = config;

    int fd_escucha = crear_servidor(puerto);
    if (fd_escucha == -1) {
        log_error(logger, "Fallo al crear el socket servidor (puerto: %s)", puerto);
        return;
    }

    log_info(logger, "Kernel Memory escuchando en puerto %s", puerto);

    while (1) {
        int* fd_conexion_ptr = malloc(sizeof(int));
        *fd_conexion_ptr = accept(fd_escucha, NULL, NULL);

        if (*fd_conexion_ptr == -1) {
            log_error(logger, "Error al aceptar conexion entrante");
            free(fd_conexion_ptr);
            continue;
        }

        pthread_t hilo;
        pthread_create(&hilo, NULL, manejar_cliente, fd_conexion_ptr);
        pthread_detach(hilo);
    }
}

void* manejar_cliente(void* arg) {
    int* fd_ptr = (int*) arg;
    int fd_conexion = *fd_ptr;
    free(fd_ptr);

    int32_t handshake;
    ssize_t bytes = recv(fd_conexion, &handshake, sizeof(int32_t), MSG_WAITALL);

    if (bytes <= 0) {
        close(fd_conexion);
        return NULL;
    }

    switch (handshake) {
        case HANDSHAKE_CPU:
            atender_cpu(fd_conexion, _gestor, _gestor_memoria, _config, _logger);
            break;
        case HANDSHAKE_SCHEDULER:
            atender_scheduler(fd_conexion, _gestor, _gestor_memoria, _config, _logger);
            break;
        case HANDSHAKE_SCHEDULER_NOTIF:
            // Se queda con el fd: es el canal de push KM -> KS, no se cierra.
            if (atender_scheduler_notif(fd_conexion, _gestor_memoria, _logger)) return NULL;
            break;
        case HANDSHAKE_MEMORY_STICK:
            atender_memory_stick(fd_conexion, _gestor_memoria, _logger);
            break;
        case HANDSHAKE_SWAP:
            // Si el handshake salio bien el fd queda en el gestor: no se cierra.
            if (atender_swap(fd_conexion, _gestor_memoria, _logger)) return NULL;
            break;
        default:
            log_warning(_logger, "Handshake desconocido: %d", handshake);
            break;
    }

    close(fd_conexion);
    return NULL;
}
