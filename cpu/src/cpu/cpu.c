#include "cpu/cpu.h"

#include "instrucciones/instrucciones.h"
#include "registros_cpu/registros_cpu.h"
#include "conexiones/conexiones.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <utils/protocolo.h>
#include <commons/log.h>

struct t_cpu* cpu_create(int socket_kernel, int socket_memoria, uint32_t id_cpu, uint32_t segment_max_size) {
    struct t_cpu* cpu = malloc(sizeof(struct t_cpu));
    if (cpu == NULL) return NULL;

    memset(&(cpu->registros), 0, sizeof(t_registros_cpu));
    cpu->socket_kernel = socket_kernel;
    cpu->socket_memoria = socket_memoria;
    cpu->sticks = list_create();
    cpu->id_cpu = id_cpu;
    cpu->segment_max_size = segment_max_size;
    cpu->ejecutando = false;
    cpu->pid_actual = 0;
    cpu->tabla_segmentos = list_create();

    return cpu;
}

static void stick_cpu_destroy(void* elemento) {
    t_stick_cpu* stick = (t_stick_cpu*) elemento;
    if (stick->fd != -1) close(stick->fd);
    free(stick->ip);
    free(stick->puerto);
    free(stick);
}

void cpu_destroy(struct t_cpu* cpu) {
    if (cpu == NULL) return;
    if (cpu->tabla_segmentos != NULL) {
        list_destroy_and_destroy_elements(cpu->tabla_segmentos, free);
    }
    if (cpu->sticks != NULL) {
        list_destroy_and_destroy_elements(cpu->sticks, stick_cpu_destroy);
    }
    free(cpu);
}

void atender_peticiones_kernel_scheduler(struct t_cpu* cpu, t_log* logger) {
    while (1) {
        
        // Espero el codigo del Kernel_scheduler
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(cpu->socket_kernel, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);

        if (bytes <= 0) {
            log_info(logger, "Kernel Scheduler desconectado. Cerrando CPU %u.", cpu->id_cpu);
            break;
        }

        // A partir del codigo recibido resuelvo
        switch (codigo) {
            case MENSAJE_ENVIAR_PID: {

                // Recibo el PID a ejecutar del Kernel_scheduler
                uint32_t pid;

                recv(cpu->socket_kernel, &pid, sizeof(uint32_t), MSG_WAITALL);
                cpu->pid_actual = pid;
                cpu->ejecutando = true;

                // Refrescar la topologia de sticks: pueden haberse conectado
                // nuevos Memory Sticks desde el ultimo despacho.
                actualizar_topologia_sticks(cpu, logger);
                solicitar_contexto_a_km(cpu, logger);

                bool termino = false;

                while (!termino) {

                    // Etapa FETCH
                    char* instruccion_string = cpu_fetch(cpu, logger);

                    if (instruccion_string == NULL) {
                        log_error(logger, "FETCH fallo para PID %u", cpu->pid_actual);
                        break;
                    }
                    log_info(logger, "## PID: %u - FETCH completado - Instrucción: %s", cpu->pid_actual, instruccion_string);

                    // Etapa DECODE
                    t_instruccion inst;

                    if (cpu_decode(instruccion_string, &inst) == -1) {
                        log_error(logger, "DECODE fallo para instrucción: %s", instruccion_string);
                        free(instruccion_string);
                        break;
                    }
                    log_info(logger, "## PID: %u - DECODE completado - Instrucción: %s - Parámetros: %s", cpu->pid_actual, cod_instruccion_to_string(inst.codigo), parametros_to_string(&inst));
                    free(instruccion_string);

                    // Etapa EXECUTE
                    int resultado = cpu_execute(cpu, &inst, logger);
                    log_info(logger, "## PID: %u - EXECUTE completado - Resultado: %d", cpu->pid_actual, resultado);
                    
                    if (resultado == 1) {
                        termino = true; //Salgo del while
                    }
                    else {
                        // Aumento el PC en 1, a no ser que reciba JNZ que dicha inst. lo actualiza.
                        // Esto tiene que pasar SIEMPRE al terminar el ciclo (incluso si hay
                        // interrupcion pendiente), porque la instruccion ya se ejecuto: si no se
                        // incrementa antes de mandar el contexto en cpu_check_interrupt, al
                        // resumir el proceso se vuelve a ejecutar la misma instruccion en un loop
                        // infinito.
                        // No se incrementa si la instruccion ya modifico el PC:
                        // JNZ que salto, o SET sobre el registro PC.
                        bool modifico_pc =
                            (inst.codigo == INST_JNZ && get_registro_32(cpu, inst.parametros[0]) != 0)
                         || (inst.codigo == INST_SET && strcmp(inst.parametros[0], "PC") == 0);
                        if (!modifico_pc) {
                            cpu->registros.pc++;
                        }

                        // Verifico si no hay interrupcion no bloqueante
                        if (cpu_check_interrupt(cpu, logger)) {
                            termino = true; //Salgo del while
                        }
                    }
                    
                }

                // Reseteo la CPU para que pueda recibir otro PID.
                cpu->ejecutando = false;
                cpu->pid_actual = 0;
                break;
            }
            case MENSAJE_INTERRUPCION: {
                log_info(logger, "## Interrupción recibida");
                break;
            }
            default:
                log_warning(logger, "Mensaje desconocido del KS: %d", codigo);
                break;
        }
    }
}
