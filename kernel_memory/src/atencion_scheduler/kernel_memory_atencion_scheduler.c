#include "kernel_memory_atencion_scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <utils/protocolo.h>
#include <utils/registros.h>

void atender_scheduler(int fd,
                       t_gestor_procesos*      gestor,
                       t_gestor_memoria*       gestor_memoria,
                       t_kernel_memory_config* config,
                       t_log*                  logger) {

    log_info(logger, "## Kernel Scheduler Conectado - FD del socket: %d", fd);

    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) break;

        switch (codigo) {

            case MENSAJE_ENVIAR_PID: {
                uint32_t pid;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);

                uint32_t path_len;
                recv(fd, &path_len, sizeof(uint32_t), MSG_WAITALL);

                char* path = malloc(path_len + 1);
                recv(fd, path, path_len, MSG_WAITALL);
                path[path_len] = '\0';

                char path_completo[512];
                snprintf(path_completo, sizeof(path_completo),
                         "%s/%s", config->scripts_basepath, path);
                free(path);

                gestor_agregar_proceso(gestor, pid, path_completo);
                log_info(logger, "## PID: %u - Proceso Creado", pid);

                t_codigo_mensaje ok = MENSAJE_OK;
                send(fd, &ok, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_ESPACIO_LIBRE: {
                uint32_t espacio_libre = gestor_memoria_espacio_libre(gestor_memoria);
                send(fd, &espacio_libre, sizeof(uint32_t), 0);
                break;
            }

            case MENSAJE_CREAR_SEGMENTO: {
                uint32_t pid, id_segmento, tamanio;
                recv(fd, &pid,         sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &id_segmento, sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &tamanio,     sizeof(uint32_t), MSG_WAITALL);

                t_resultado_crear_segmento resultado =
                    gestor_memoria_crear_segmento(gestor_memoria, pid, id_segmento, tamanio);

                if (resultado == CREAR_SEGMENTO_NECESITA_COMPACTACION) {
                    // Pedirle al KS que desaloje todas las CPUs; el protocolo es
                    // sincronico sobre esta misma conexion: COMPACTACION ->
                    // (KS desaloja) -> DESALOJO_OK -> compactar -> reintentar.
                    t_codigo_mensaje pedido = MENSAJE_COMPACTACION;
                    send(fd, &pedido, sizeof(t_codigo_mensaje), 0);

                    t_codigo_mensaje confirmacion;
                    if (recv(fd, &confirmacion, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0
                        || confirmacion != MENSAJE_DESALOJO_OK) {
                        log_error(logger, "No llego DESALOJO_OK del Kernel Scheduler");
                        return;
                    }

                    gestor_memoria_compactar(gestor_memoria);
                    resultado = gestor_memoria_crear_segmento(gestor_memoria, pid, id_segmento, tamanio);
                }

                t_codigo_mensaje respuesta =
                    (resultado == CREAR_SEGMENTO_OK) ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_ELIMINAR_SEGMENTO: {
                uint32_t pid, id_segmento;
                recv(fd, &pid,         sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &id_segmento, sizeof(uint32_t), MSG_WAITALL);

                bool ok = gestor_memoria_eliminar_segmento(gestor_memoria, pid, id_segmento);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_FINALIZAR_PROCESO: {
                uint32_t pid;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);

                gestor_memoria_finalizar_proceso(gestor_memoria, pid);

                t_codigo_mensaje ok = MENSAJE_OK;
                send(fd, &ok, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_LEER_MEMORIA: {
                // Camino IO STDOUT: el KS pide una direccion LOGICA del proceso;
                // la traduccion se hace aca para que siga siendo valida aunque
                // haya habido compactacion o suspension mientras estaba en BLOCK.
                uint32_t pid, dir_logica, tamanio;
                recv(fd, &pid,        sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &dir_logica, sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &tamanio,    sizeof(uint32_t), MSG_WAITALL);

                char* buffer = malloc(tamanio > 0 ? tamanio : 1);
                uint32_t dir_fisica;
                bool ok = gestor_memoria_io_rw(gestor_memoria, pid, dir_logica, tamanio, buffer, false, &dir_fisica);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                if (ok) {
                    send(fd, buffer, tamanio, 0);
                    if (dir_fisica != UINT32_MAX) {
                        log_info(logger, "## PID: %u - Lectura - Dir. Física: %u - Tamaño: %u",
                                 pid, dir_fisica, tamanio);
                    }
                } else {
                    log_error(logger, "PID: %u - Lectura invalida: dir logica %u, tamanio %u",
                              pid, dir_logica, tamanio);
                }
                free(buffer);
                break;
            }

            case MENSAJE_ESCRIBIR_MEMORIA: {
                // Camino IO STDIN: idem lectura, direccion LOGICA traducida aca.
                uint32_t pid, dir_logica, tamanio;
                recv(fd, &pid,        sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &dir_logica, sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &tamanio,    sizeof(uint32_t), MSG_WAITALL);

                char* contenido = malloc(tamanio > 0 ? tamanio : 1);
                recv(fd, contenido, tamanio, MSG_WAITALL);

                uint32_t dir_fisica;
                bool ok = gestor_memoria_io_rw(gestor_memoria, pid, dir_logica, tamanio, contenido, true, &dir_fisica);
                free(contenido);

                if (ok && dir_fisica != UINT32_MAX) {
                    log_info(logger, "## PID: %u - Escritura - Dir. Física: %u - Tamaño: %u",
                             pid, dir_fisica, tamanio);
                } else if (!ok) {
                    log_error(logger, "PID: %u - Escritura invalida: dir logica %u, tamanio %u",
                              pid, dir_logica, tamanio);
                }

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_SUSPENDER_PROCESO: {
                uint32_t pid;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);

                bool ok = gestor_memoria_suspender_proceso(gestor_memoria, pid);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_DESSUSPENDER_PROCESO: {
                uint32_t pid;
                recv(fd, &pid, sizeof(uint32_t), MSG_WAITALL);

                bool ok = gestor_memoria_dessuspender_proceso(gestor_memoria, pid);

                t_codigo_mensaje respuesta = ok ? MENSAJE_OK : MENSAJE_ERROR;
                send(fd, &respuesta, sizeof(t_codigo_mensaje), 0);
                break;
            }

            case MENSAJE_ACTUALIZAR_CONTEXTO: {
                uint32_t pid;
                t_registros_cpu regs;
                recv(fd, &pid,  sizeof(uint32_t), MSG_WAITALL);
                recv(fd, &regs, sizeof(t_registros_cpu), MSG_WAITALL);

                gestor_lock(gestor);
                t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gestor, pid);
                if (proc != NULL) {
                    proc->contexto->registros = regs;
                }
                gestor_unlock(gestor);

                t_codigo_mensaje ok = MENSAJE_OK;
                send(fd, &ok, sizeof(t_codigo_mensaje), 0);
                break;
            }

            default:
                log_warning(logger, "Mensaje desconocido del KS: %d", codigo);
                break;
        }
    }

    log_info(logger, "Kernel Scheduler desconectado");
}
