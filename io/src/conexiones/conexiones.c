#include "conexiones.h"
#include <utils/sockets/sockets.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>

// Codigo de handshake — nos presentamos ante Kernel Scheduler
#define HANDSHAKE_IO 2

int conectar_a_kernel_scheduler(const char* ip, const char* puerto, const char* nombre_interfaz, t_log* logger) {
    int fd_conexion = crear_conexion(ip, puerto);
    if (fd_conexion == -1) {
        log_error(logger, "No se pudo conectar a Kernel Scheduler en %s:%s", ip, puerto);
        return -1;
    }

    // Handshake: me presento como IO
    int32_t handshake = HANDSHAKE_IO;
    send(fd_conexion, &handshake, sizeof(int32_t), 0);

    // Enviar la longitud del nombre de la interfaz y el nombre
    uint32_t nombre_len = strlen(nombre_interfaz);
    send(fd_conexion, &nombre_len, sizeof(uint32_t), 0);
    send(fd_conexion, nombre_interfaz, nombre_len, 0);

    log_info(logger, "## Conectado a Kernel Scheduler");
    return fd_conexion;
}

