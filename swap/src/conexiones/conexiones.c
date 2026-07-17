#include "conexiones.h"
#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

void conectar_y_atender_kernel_memory(t_swap_config* config, FILE* archivo_swap, t_log* logger) {
    int fd = crear_conexion(config->ip_kernel_memory, config->port_kernel_memory);
    if (fd == -1) {
        log_error(logger, "No se pudo conectar a Kernel Memory en %s:%s",
                  config->ip_kernel_memory, config->port_kernel_memory);
        return;
    }

    int32_t handshake = HANDSHAKE_SWAP;
    send(fd, &handshake, sizeof(int32_t), 0);
    log_info(logger, "## Conectado a Kernel Memory");

    // Informar tamanio total del swap y tamanio de bloque
    t_codigo_mensaje cod = MENSAJE_INFORMAR_TAMANIO;
    uint32_t total = (uint32_t) config->swap_file_size;
    uint32_t block = (uint32_t) config->block_size;
    send(fd, &cod, sizeof(t_codigo_mensaje), 0);
    send(fd, &total, sizeof(uint32_t), 0);
    send(fd, &block, sizeof(uint32_t), 0);

    // La administracion del espacio es tarea de Kernel Memory: aca solo se
    // leen/escriben bloques completos del archivo.
    char* buffer = malloc(block);

    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) break;

        if (codigo == MENSAJE_ESCRIBIR_MEMORIA) {
            uint32_t nro_bloque;
            if (recv(fd, &nro_bloque, sizeof(uint32_t), MSG_WAITALL) <= 0) break;
            if (recv(fd, buffer, block, MSG_WAITALL) <= 0) break;

            fseek(archivo_swap, (long) nro_bloque * block, SEEK_SET);
            fwrite(buffer, 1, block, archivo_swap);
            fflush(archivo_swap);

            log_info(logger, "## Escritura del bloque: %u", nro_bloque);

            t_codigo_mensaje ok = MENSAJE_OK;
            send(fd, &ok, sizeof(t_codigo_mensaje), 0);
        }
        else if (codigo == MENSAJE_LEER_MEMORIA) {
            uint32_t nro_bloque;
            if (recv(fd, &nro_bloque, sizeof(uint32_t), MSG_WAITALL) <= 0) break;

            fseek(archivo_swap, (long) nro_bloque * block, SEEK_SET);
            size_t leidos = fread(buffer, 1, block, archivo_swap);
            // El archivo inicia "libre": si el bloque nunca se escribio,
            // se completa con ceros.
            for (size_t i = leidos; i < block; i++) buffer[i] = 0;

            log_info(logger, "## Lectura del bloque: %u", nro_bloque);

            send(fd, buffer, block, 0);
        }
        else {
            log_warning(logger, "Mensaje desconocido de Kernel Memory: %d", codigo);
        }
    }

    free(buffer);
    close(fd);
    log_info(logger, "Kernel Memory desconectado, finalizando SWAP");
}
