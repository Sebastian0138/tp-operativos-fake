#include "conexiones.h"
#include "../atencion_cpu/kernel_memory_atencion_cpu.h"
#include "../atencion_scheduler/kernel_memory_atencion_scheduler.h"
#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

// Recibe el INFORMAR_TAMANIO de un stick, lo registra y queda monitoreando
// la conexion de control: si el stick se desconecta -> memoria corrupta.
static void atender_memory_stick(int fd) {
    t_codigo_mensaje codigo;
    if (recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0 || codigo != MENSAJE_INFORMAR_TAMANIO) {
        log_warning(_logger, "Memory Stick (fd %d) no informo su tamaño — conexion rechazada", fd);
        return;
    }

    uint32_t tamanio, ip_len, port_len;
    if (recv(fd, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) return;

    if (recv(fd, &ip_len, sizeof(uint32_t), MSG_WAITALL) <= 0) return;
    char* ip = malloc(ip_len + 1);
    if (recv(fd, ip, ip_len, MSG_WAITALL) <= 0) { free(ip); return; }
    ip[ip_len] = '\0';

    if (recv(fd, &port_len, sizeof(uint32_t), MSG_WAITALL) <= 0) { free(ip); return; }
    char* puerto = malloc(port_len + 1);
    if (recv(fd, puerto, port_len, MSG_WAITALL) <= 0) { free(ip); free(puerto); return; }
    puerto[port_len] = '\0';

    int id_stick = gestor_memoria_registrar_stick(_gestor_memoria, tamanio, ip, puerto);
    free(ip);
    free(puerto);

    if (id_stick == -1) return;

    // El stick no vuelve a escribir por esta conexion: un recv que retorna
    // significa que el stick se desconecto -> notificar memoria corrupta.
    char descarte;
    while (recv(fd, &descarte, 1, MSG_WAITALL) > 0);
    gestor_memoria_stick_desconectado(_gestor_memoria, id_stick);
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
        case HANDSHAKE_SCHEDULER_NOTIF: {
            // Canal exclusivo de notificaciones KM -> KS. Este hilo no lee mas:
            // el fd queda registrado para que el gestor de memoria pueda empujar
            // MAS_MEMORIA / MEMORIA_CORRUPTA sin interferir con el canal principal.
            log_info(_logger, "Canal de notificaciones al Kernel Scheduler conectado (fd: %d)", fd_conexion);
            gestor_memoria_set_notif_ks(_gestor_memoria, fd_conexion);
            return NULL; // no cerrar el fd
        }
        case HANDSHAKE_MEMORY_STICK:
            atender_memory_stick(fd_conexion);
            break;
        case HANDSHAKE_SWAP: {
            // El SWAP informa su tamanio total y el tamanio de bloque. El fd
            // queda en manos del gestor de memoria (las operaciones de bloque
            // se hacen bajo demanda con su mutex); este hilo no lo lee mas.
            t_codigo_mensaje codigo;
            if (recv(fd_conexion, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0
                || codigo != MENSAJE_INFORMAR_TAMANIO) {
                log_warning(_logger, "SWAP (fd %d) no informo su tamaño — conexion rechazada", fd_conexion);
                break;
            }
            uint32_t total, block;
            if (recv(fd_conexion, &total, sizeof(uint32_t), MSG_WAITALL) <= 0) break;
            if (recv(fd_conexion, &block, sizeof(uint32_t), MSG_WAITALL) <= 0) break;

            gestor_memoria_registrar_swap(_gestor_memoria, fd_conexion, total, block);
            return NULL; // no cerrar el fd
        }
        default:
            log_warning(_logger, "Handshake desconocido: %d", handshake);
            break;
    }

    close(fd_conexion);
    return NULL;
}
