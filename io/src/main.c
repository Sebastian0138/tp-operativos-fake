#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/types.h> 

#include "config/io_config.h"
#include "logger/io_logger.h"
#include "conexiones/conexiones.h"
#include "io/io_tools.h"

int main(int argc, char** argv) {
    t_log* boot_logger = io_boot_logger_create();

    if (boot_logger == NULL) {
        fprintf(stderr, "No se pudo crear el boot logger\n");
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Iniciando IO");

    if (argc != 3) {
        log_error(boot_logger, "Cantidad de argumentos inválida. Uso: ./bin/io [Archivo Config] [Tipo]");
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Intentando cargar archivo de configuración: %s", argv[1]);

    t_io_config* config = io_config_create(argv[1], boot_logger);

    if (config == NULL) {
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    t_tipo_dispositivo_io tipo_dispositivo;
    if (strcmp(argv[2], "STDIN") == 0) {
        tipo_dispositivo = STDIN;
    } else if (strcmp(argv[2], "STDOUT") == 0) {
        tipo_dispositivo = STDOUT;
    } else if (strcmp(argv[2], "SLEEP") == 0) {
        tipo_dispositivo = SLEEP;
    } else {
        log_error(boot_logger, "Tipo de dispositivo inválido: %s. Debe ser STDIN, STDOUT o SLEEP.", argv[2]);
        io_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Archivo de configuración cargado correctamente");
    io_config_log(config, boot_logger);

    t_log* logger = io_logger_create(config);
    if (logger == NULL) {
        log_error(boot_logger, "No se pudo crear el logger principal");
        io_config_destroy(config);
        log_destroy(boot_logger);
        return EXIT_FAILURE;
    }

    log_info(boot_logger, "Logger principal creado correctamente");
    log_destroy(boot_logger);

    log_info(logger, "IO iniciado correctamente");

    int socket_ks = conectar_a_kernel_scheduler(config->ip_kernel_scheduler, config->port_kernel_scheduler, argv[2], logger);

    t_io io = {
        .tipo_dispositivo = tipo_dispositivo,
        .socket_kernel = socket_ks
    };
    
    // pid_t mi_pid = getpid(); // No se usa el PID real de Linux


    switch (io.tipo_dispositivo){

    case STDIN: {
        uint32_t pid;
        uint32_t tamanio;
        while (recv(io.socket_kernel, &pid, sizeof(uint32_t), MSG_WAITALL) > 0) {
            if (recv(io.socket_kernel, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) break;
            
            log_info(logger, "## PID: %u - Inicio de IO", pid);
            log_info(logger, "## PID: %u - Ingrese %u caracteres:", pid, tamanio);
            
            char* buffer = malloc(tamanio);
            if (buffer == NULL) {
                log_error(logger, "No se pudo asignar memoria para el buffer STDIN");
                break;
            }
            
            uint32_t bytes_leidos = 0;
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {
                if (bytes_leidos < tamanio) {
                    buffer[bytes_leidos] = (char)c;
                }
                bytes_leidos++;
            }
            
            while (bytes_leidos < tamanio) {
                buffer[bytes_leidos] = '\0';
                bytes_leidos++;
            }
            
            send(io.socket_kernel, buffer, tamanio, 0);
            free(buffer);
            
            log_info(logger, "## PID: %u - Fin de IO", pid);
        }
        break;
    }

    case STDOUT: {
        uint32_t pid;
        uint32_t tamanio;
        while (recv(io.socket_kernel, &pid, sizeof(uint32_t), MSG_WAITALL) > 0) {
            if (recv(io.socket_kernel, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) break;
            
            log_info(logger, "## PID: %u - Inicio de IO", pid);
            log_info(logger, "Recibida solicitud STDOUT de %u bytes", tamanio);
            
            char* buffer = malloc(tamanio + 1);
            if (buffer == NULL) {
                log_error(logger, "No se pudo asignar memoria para el buffer STDOUT");
                break;
            }
            
            ssize_t recibidos = recv(io.socket_kernel, buffer, tamanio, MSG_WAITALL);
            if (recibidos <= 0) {
                log_error(logger, "Error al recibir datos para STDOUT");
                free(buffer);
                break;
            }
            
            buffer[tamanio] = '\0';
            
            // Imprimir por pantalla
            printf("%s", buffer);
            fflush(stdout);
            
            // Imprimir en el archivo de log
            log_info(logger, "## PID: %u - %s", pid, buffer);
            
            free(buffer);
            
            log_info(logger, "## PID: %u - Fin de IO", pid);
            
            // Informar al Kernel que la IO finalizó correctamente
            t_codigo_mensaje fin = MENSAJE_OK;
            send(io.socket_kernel, &fin, sizeof(t_codigo_mensaje), 0);
        }
        break;
    }

    case SLEEP: {
        uint32_t pid;
        uint32_t tiempo_ms;
        while (recv(io.socket_kernel, &pid, sizeof(uint32_t), MSG_WAITALL) > 0) {
            if (recv(io.socket_kernel, &tiempo_ms, sizeof(uint32_t), MSG_WAITALL) <= 0) break;
            
            log_info(logger, "## PID: %u - Inicio de IO", pid);
            log_info(logger, "## PID: %u - Haciendo sleep por %u milisegundos.", pid, tiempo_ms);
            
            // usleep recibe microsegundos, así que multiplicamos por 1000
            usleep(tiempo_ms * 1000);
            
            log_info(logger, "## PID: %u - Fin de IO", pid);
            
            // Informar al Kernel que la IO finalizó correctamente
            t_codigo_mensaje fin = MENSAJE_OK;
            send(io.socket_kernel, &fin, sizeof(t_codigo_mensaje), 0);
        }
        break;
    }

    default:
        log_error(logger, "Error de tipo de IO, de alguna manera no coincide con ningún tipo conocido.");
        io_config_destroy(config);
        log_destroy(logger);
        return EXIT_FAILURE;
    break;
    }

    log_destroy(logger);
    io_config_destroy(config);

    return EXIT_SUCCESS;
}
