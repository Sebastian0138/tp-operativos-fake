#include "conexiones.h"
#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

// Compartidos entre hilos de atencion (se setean en iniciar_servidor)
static t_log* _logger = NULL;
static t_memoria_stick* _memoria = NULL;
static int _memory_delay_ms = 0;

// Declaracion interna
void* manejar_cliente(void* arg);

int crear_escucha_memory_stick(t_memory_stick_config* config, t_memoria_stick* memoria, t_log* logger) {
    _logger = logger;
    _memoria = memoria;
    _memory_delay_ms = config->memory_delay;

    int fd_escucha = crear_servidor(config->listen_port);
    if (fd_escucha == -1) {
        log_error(logger, "Fallo al crear el socket servidor (puerto: %s)", config->listen_port);
        return -1;
    }

    log_info(logger, "Memory Stick escuchando en puerto %s", config->listen_port);
    return fd_escucha;
}

void atender_conexiones_memory_stick(int fd_escucha) {
    while (1) {
        pthread_t hilo;
        int* fd_conexion_ptr = malloc(sizeof(int));
        *fd_conexion_ptr = accept(fd_escucha, NULL, NULL);

        if (*fd_conexion_ptr == -1) {
            log_error(_logger, "Error al aceptar conexion entrante");
            free(fd_conexion_ptr);
            continue;
        }

        pthread_create(&hilo, NULL, manejar_cliente, fd_conexion_ptr);
        pthread_detach(hilo);
    }
}

// Loop de atencion de lecturas/escrituras. Lo usan tanto las CPUs como el
// Kernel Memory: ambos hablan el mismo protocolo de datos contra el stick.
static void atender_peticiones(int fd) {
    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) break;

        switch (codigo) {
            case MENSAJE_LEER_MEMORIA: {
                uint32_t dir, tamanio;
                if (recv(fd, &dir,     sizeof(uint32_t), MSG_WAITALL) <= 0) return;
                if (recv(fd, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) return;

                usleep(_memory_delay_ms * 1000);

                char* buffer = malloc(tamanio > 0 ? tamanio : 1);
                bool ok = memoria_stick_leer(_memoria, dir, tamanio, buffer);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                if (ok) {
                    send(fd, buffer, tamanio, 0);
                    log_info(_logger, "## Lectura de %u bytes", tamanio);
                } else {
                    log_error(_logger, "Lectura fuera de rango: dir %u, tamanio %u (bloque de %u bytes)",
                              dir, tamanio, _memoria->tamanio);
                }
                free(buffer);
                break;
            }

            case MENSAJE_ESCRIBIR_MEMORIA: {
                uint32_t dir, tamanio;
                if (recv(fd, &dir,     sizeof(uint32_t), MSG_WAITALL) <= 0) return;
                if (recv(fd, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) return;

                char* buffer = malloc(tamanio > 0 ? tamanio : 1);
                if (tamanio > 0 && recv(fd, buffer, tamanio, MSG_WAITALL) <= 0) {
                    free(buffer);
                    return;
                }

                usleep(_memory_delay_ms * 1000);

                bool ok = memoria_stick_escribir(_memoria, dir, tamanio, buffer);
                free(buffer);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                if (ok) {
                    log_info(_logger, "## Escritura de %u bytes", tamanio);
                } else {
                    log_error(_logger, "Escritura fuera de rango: dir %u, tamanio %u (bloque de %u bytes)",
                              dir, tamanio, _memoria->tamanio);
                }
                break;
            }

            default:
                log_warning(_logger, "Mensaje desconocido: %d", codigo);
                break;
        }
    }
}

void* manejar_cliente(void* arg) {
    int* fd_ptr = (int*) arg;
    int fd_conexion = *fd_ptr;
    free(fd_ptr);

    int32_t handshake;
    ssize_t bytes = recv(fd_conexion, &handshake, sizeof(int32_t), MSG_WAITALL);

    if (bytes <= 0) {
        log_info(_logger, "Un cliente se desconecto antes del handshake");
        close(fd_conexion);
        return NULL;
    }

    switch (handshake) {
        case HANDSHAKE_CPU: {
            int32_t id_cpu;
            if (recv(fd_conexion, &id_cpu, sizeof(int32_t), MSG_WAITALL) <= 0) {
                close(fd_conexion);
                return NULL;
            }
            log_info(_logger, "## CPU %d Conectada", id_cpu);
            atender_peticiones(fd_conexion);
            break;
        }
        case HANDSHAKE_KERNEL_MEMORY:
            log_info(_logger, "Kernel Memory conectado al canal de datos (fd: %d)", fd_conexion);
            atender_peticiones(fd_conexion);
            break;
        default:
            log_warning(_logger, "Handshake desconocido: %d — conexion rechazada", handshake);
            break;
    }

    close(fd_conexion);
    return NULL;
}

int conectar_a_kernel_memory(t_memory_stick_config* config, uint32_t tamanio, t_log* logger) {
    int fd_conexion = crear_conexion(config->ip_kernel_memory, config->port_kernel_memory);
    if (fd_conexion == -1) {
        log_error(logger, "No se pudo conectar a Kernel Memory en %s:%s",
                  config->ip_kernel_memory, config->port_kernel_memory);
        return -1;
    }

    int32_t handshake = HANDSHAKE_MEMORY_STICK;
    send(fd_conexion, &handshake, sizeof(int32_t), 0);

    // Informar tamanio + ip:puerto de escucha, para que KM registre el stick
    // y les pase la topologia a las CPUs.
    t_codigo_mensaje cod = MENSAJE_INFORMAR_TAMANIO;
    send(fd_conexion, &cod, sizeof(t_codigo_mensaje), 0);
    send(fd_conexion, &tamanio, sizeof(uint32_t), 0);

    uint32_t ip_len = strlen(config->listen_ip);
    send(fd_conexion, &ip_len, sizeof(uint32_t), 0);
    send(fd_conexion, config->listen_ip, ip_len, 0);

    uint32_t port_len = strlen(config->listen_port);
    send(fd_conexion, &port_len, sizeof(uint32_t), 0);
    send(fd_conexion, config->listen_port, port_len, 0);

    log_info(logger, "## Conectado a Kernel Memory");
    return fd_conexion;
}
