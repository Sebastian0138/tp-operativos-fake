#include "kernel_memory_atencion_cpu.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <utils/protocolo.h>
#include <utils/segmento.h>
#include <utils/registros.h>

void atender_cpu(int fd,
                 t_gestor_procesos*      gestor,
                 t_gestor_memoria*       gestor_memoria,
                 t_kernel_memory_config* config,
                 t_log*                  logger) {

    // La CPU se presenta con su ID despues del handshake
    int32_t id_cpu = -1;
    recv(fd, &id_cpu, sizeof(int32_t), MSG_WAITALL);
    log_info(logger, "## CPU %d Conectada", id_cpu);

    uint32_t seg_max = (uint32_t) config->segment_max_size;
    send(fd, &seg_max, sizeof(uint32_t), 0);

    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) break;

        switch (codigo) {

            case MENSAJE_CONTEXTO: {
                uint32_t pid;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);

                gestor_lock(gestor);
                t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gestor, pid);

                if (proc == NULL) {
                    gestor_unlock(gestor);
                    log_warning(logger, "PID %u no encontrado - mandando contexto vacio", pid);
                    t_registros_cpu regs_vacios;
                    memset(&regs_vacios, 0, sizeof(t_registros_cpu));
                    send(fd, &regs_vacios, sizeof(t_registros_cpu), 0);
                    uint32_t cant = 0;
                    send(fd, &cant, sizeof(uint32_t), 0);
                } else {
                    // Se manda con el mutex del gestor tomado: contexto->registros y
                    // tabla_segmentos pueden ser mutados concurrentemente por otro hilo
                    // (ej. atencion_scheduler) mientras se arma la respuesta.
                    send(fd, &proc->contexto->registros, sizeof(t_registros_cpu), 0);
                    uint32_t cant = list_size(proc->contexto->tabla_segmentos);
                    send(fd, &cant, sizeof(uint32_t), 0);
                    for (uint32_t i = 0; i < cant; i++) {
                        t_segmento* seg = list_get(proc->contexto->tabla_segmentos, i);
                        send(fd, seg, sizeof(t_segmento), 0);
                    }
                    gestor_unlock(gestor);
                    log_info(logger, "## PID: %u - Contexto enviado (%u segmentos)", pid, cant);
                }
                break;
            }

            case MENSAJE_PEDIR_INSTRUCCION: {
                uint32_t pid, pc;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &pc,  sizeof(uint32_t), MSG_WAITALL);

                usleep(config->instruction_delay * 1000);

                char* instruccion = gestor_obtener_instruccion(gestor, pid, pc, logger);

                if (instruccion == NULL) {
                    uint32_t len = 0;
                    send(fd, &len, sizeof(uint32_t), 0);
                } else {
                    uint32_t len = strlen(instruccion);
                    send(fd, &len, sizeof(uint32_t), 0);
                    send(fd, instruccion, len, 0);
                    free(instruccion);
                }
                break;
            }

            case MENSAJE_TOPOLOGIA_STICKS: {
                uint32_t tamanio_buffer;
                void* buffer = gestor_memoria_serializar_topologia(gestor_memoria, &tamanio_buffer);
                send(fd, buffer, tamanio_buffer, 0);
                free(buffer);
                break;
            }

            default:
                log_warning(logger, "Mensaje desconocido de CPU: %d", codigo);
                break;
        }
    }

    log_info(logger, "CPU desconectada (fd: %d)", fd);
}