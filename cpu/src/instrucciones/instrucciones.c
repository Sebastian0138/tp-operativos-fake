#include "cpu/cpu.h"
#include "instrucciones/instrucciones.h"

#include "registros_cpu/registros_cpu.h"
#include "conexiones/conexiones.h"

#include <utils/protocolo.h>
#include <utils/instrucciones.h>
#include <commons/string.h>
#include <commons/log.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <sys/socket.h>

// ============================================================
// MMU - TRADUCCION DE DIRECCIONES
// ============================================================

static uint32_t mmu_traducir(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tamanio_lectura, t_log* logger) {
    
    uint32_t num_segmento = dir_logica / cpu->segment_max_size;
    uint32_t desplazamiento = dir_logica % cpu->segment_max_size;
    t_segmento* seg = NULL;
    
    //recorre la tabla de segemntos del proceso
    for (int i = 0; i < list_size(cpu->tabla_segmentos); i++) {
        t_segmento* s = list_get(cpu->tabla_segmentos, i);
    
        if (s->id_segmento == num_segmento) {
            seg = s;
            break;
        }
    }

    // No encontro el segmento. El proceso intentó acceder a un segmento que no tiene asignado   
    if (seg == NULL) {

        log_error(logger, "## SEG_FAULT - Segmento %u no existe", num_segmento);
        return UINT32_MAX;
    }

    // Verifica que la lectura/escritura no se salga del segmento
    if (desplazamiento + tamanio_lectura > seg->tamanio) {
        
        log_error(logger, "## SEG_FAULT - Desplazamiento %u + tamanio %u excede segmento %u (tamanio %u)", desplazamiento, tamanio_lectura, num_segmento, seg->tamanio);
        return UINT32_MAX;
    }

    return seg->base + desplazamiento;
}

// ============================================================
// CICLO DE INSTRUCCION
// ============================================================

char* cpu_fetch(struct t_cpu* cpu, t_log* logger) {
    log_info(logger, "## PID: %u - FETCH - Program Counter: %u", cpu->pid_actual, cpu->registros.pc);

    t_codigo_mensaje cod = MENSAJE_PEDIR_INSTRUCCION;
    send(cpu->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(cpu->socket_memoria, &(cpu->pid_actual), sizeof(uint32_t), 0);
    send(cpu->socket_memoria, &(cpu->registros.pc), sizeof(uint32_t), 0);

    uint32_t len;
    ssize_t bytes = recv(cpu->socket_memoria, &len, sizeof(uint32_t), MSG_WAITALL);
    if (bytes <= 0 || len == 0) {
        return NULL;
    }

    char* instruccion = malloc(len + 1);
    recv(cpu->socket_memoria, instruccion, len, MSG_WAITALL);
    instruccion[len] = '\0';

    return instruccion;
}

int cpu_decode(const char* instrucciones, t_instruccion* instruccion_out) {
    
    // Separo el string por " " (espacios) y devuelvo un array de strings en instruccion_string 
    char** instruccion_string = string_split((char*)instrucciones, " ");

    // Si la instruccion es NULL o la primera del array es NULL, devuelvo error.
    if (instruccion_string == NULL || instruccion_string[0] == NULL) {
        if (instruccion_string) string_array_destroy(instruccion_string);
        return -1;
    }

    char* inst_code = instruccion_string[0];

    // Leo la primera instruccion y la asocio a su codigo correspondiente. Si no encuentro asociacion devuelvo error.
    if (strcmp(inst_code, "NOOP") == 0)               instruccion_out->codigo = INST_NOOP;
    else if (strcmp(inst_code, "SET") == 0)           instruccion_out->codigo = INST_SET;
    else if (strcmp(inst_code, "MOV_IN") == 0)        instruccion_out->codigo = INST_MOV_IN;
    else if (strcmp(inst_code, "MOV_OUT") == 0)       instruccion_out->codigo = INST_MOV_OUT;
    else if (strcmp(inst_code, "SUM") == 0)           instruccion_out->codigo = INST_SUM;
    else if (strcmp(inst_code, "SUB") == 0)           instruccion_out->codigo = INST_SUB;
    else if (strcmp(inst_code, "JNZ") == 0)           instruccion_out->codigo = INST_JNZ;
    else if (strcmp(inst_code, "COPY_MEM") == 0)      instruccion_out->codigo = INST_COPY_MEM;
    else if (strcmp(inst_code, "SLEEP") == 0)         instruccion_out->codigo = INST_SLEEP;
    else if (strcmp(inst_code, "MUTEX_CREATE") == 0)  instruccion_out->codigo = INST_MUTEX_CREATE;
    else if (strcmp(inst_code, "MUTEX_LOCK") == 0)    instruccion_out->codigo = INST_MUTEX_LOCK;
    else if (strcmp(inst_code, "MUTEX_UNLOCK") == 0)  instruccion_out->codigo = INST_MUTEX_UNLOCK;
    else if (strcmp(inst_code, "MEM_ALLOC") == 0)     instruccion_out->codigo = INST_MEM_ALLOC;
    else if (strcmp(inst_code, "MEM_FREE") == 0)      instruccion_out->codigo = INST_MEM_FREE;
    else if (strcmp(inst_code, "STDIN") == 0)         instruccion_out->codigo = INST_IO_STDIN;
    else if (strcmp(inst_code, "STDOUT") == 0)        instruccion_out->codigo = INST_IO_STDOUT;
    else if (strcmp(inst_code, "INIT_PROC") == 0)     instruccion_out->codigo = INST_INIT_PROC;
    else if (strcmp(inst_code, "EXIT") == 0)          instruccion_out->codigo = INST_EXIT;
    else {
        string_array_destroy(instruccion_string);
        return -1;
    }

    instruccion_out->cantidad_parametros = 0;

    // Recorro instruccion_string y asigno los valores a instruccion_out como parametros.
    // instruccion_string esta NULL-terminado con exactamente (cantidad_tokens + 1)
    // elementos: hay que cortar apenas se encuentra el NULL, no seguir iterando hasta
    // INSTRUCCION_MAX_PARAMS, porque eso lee memoria mas alla del arreglo devuelto por
    // string_split para instrucciones con menos parametros que INSTRUCCION_MAX_PARAMS.
    for (int i = 0; i < INSTRUCCION_MAX_PARAMS; i++) {

        if (instruccion_string[i + 1] == NULL) {
            break;
        }

        strncpy(instruccion_out->parametros[i], instruccion_string[i + 1], INSTRUCCION_PARAM_LEN - 1);
        instruccion_out->parametros[i][INSTRUCCION_PARAM_LEN - 1] = '\0';
        instruccion_out->cantidad_parametros++;
    }
    string_array_destroy(instruccion_string);
    return 0;
}

int cpu_execute(struct t_cpu* cpu, t_instruccion* inst, t_log* logger) {

    log_info(logger, "## PID: %u - Ejecutando: %s - %s", cpu->pid_actual, cod_instruccion_to_string(inst->codigo), parametros_to_string(inst));

    //Reviso el codigo de instruccion y a partir de ahi resuelvo
    switch (inst->codigo) {
        case INST_NOOP:
            //No hago nada, ver si hay que simular tiempo de CPU para esto desp.
            break;

        case INST_SET: {
            // set_registro_32 resuelve cualquier registro (8 o 32 bits, PC, SI, DI)
            uint32_t valor = (uint32_t) strtoul(inst->parametros[1], NULL, 10);
            set_registro_32(cpu, inst->parametros[0], valor);
            break;
        }

        case INST_SUM: {
            uint8_t val_orig = get_registro(cpu, inst->parametros[1]);
            uint8_t val_dest = get_registro(cpu, inst->parametros[0]);
            set_registro(cpu, inst->parametros[0], val_dest + val_orig);
            break;
        }

        case INST_SUB: {
            uint8_t val_orig = get_registro(cpu, inst->parametros[1]);
            uint8_t val_dest = get_registro(cpu, inst->parametros[0]);
            set_registro(cpu, inst->parametros[0], val_dest - val_orig);
            break;
        }

        case INST_JNZ: {
            uint32_t val = get_registro_32(cpu, inst->parametros[0]);
            if (val != 0) {
                cpu->registros.pc = (uint32_t) atoi(inst->parametros[1]);
                return 0;
            }
            break;
        }

        case INST_MOV_IN: {
            // Se leen tantos bytes como el tamanio del Registro Datos (1 u 4)
            uint32_t tamanio = tamanio_registro(inst->parametros[0]);
            uint32_t dir_logica = cpu->registros.si;
            uint32_t dir_fisica = mmu_traducir(cpu, dir_logica, tamanio, logger);

            if (dir_fisica == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }

            uint32_t valor_leido = 0;
            if (!leer_memoria_fisica(cpu, dir_fisica, tamanio, &valor_leido, logger)) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }
            log_info(logger, "PID: %u - Acción: LEER - Dirección Física: %u - Valor: %u",
                     cpu->pid_actual, dir_fisica, valor_leido);
            set_registro_32(cpu, inst->parametros[0], valor_leido);
            break;
        }

        case INST_MOV_OUT: {
            uint32_t tamanio = tamanio_registro(inst->parametros[0]);
            uint32_t dir_logica = cpu->registros.di;
            uint32_t valor = get_registro_32(cpu, inst->parametros[0]);
            uint32_t dir_fisica = mmu_traducir(cpu, dir_logica, tamanio, logger);

            if (dir_fisica == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }

            if (!escribir_memoria_fisica(cpu, dir_fisica, tamanio, &valor, logger)) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }
            log_info(logger, "PID: %u - Acción: ESCRIBIR - Dirección Física: %u - Valor: %u",
                     cpu->pid_actual, dir_fisica, valor);
            break;
        }

        case INST_COPY_MEM: {
            uint32_t tamanio = get_registro_32(cpu, inst->parametros[0]);
            uint32_t dir_origen = cpu->registros.si;
            uint32_t dir_destino = cpu->registros.di;

            uint32_t dir_fisica_origen = mmu_traducir(cpu, dir_origen, tamanio, logger);

            if (dir_fisica_origen == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }

            uint32_t dir_fisica_destino = mmu_traducir(cpu, dir_destino, tamanio, logger);

            if (dir_fisica_destino == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }

            char* buffer = malloc(tamanio > 0 ? tamanio : 1);
            bool ok = leer_memoria_fisica(cpu, dir_fisica_origen, tamanio, buffer, logger);
            if (ok) {
                log_info(logger, "PID: %u - Acción: LEER - Dirección Física: %u - Valor: %u bytes",
                         cpu->pid_actual, dir_fisica_origen, tamanio);
                ok = escribir_memoria_fisica(cpu, dir_fisica_destino, tamanio, buffer, logger);
                if (ok) {
                    log_info(logger, "PID: %u - Acción: ESCRIBIR - Dirección Física: %u - Valor: %u bytes",
                             cpu->pid_actual, dir_fisica_destino, tamanio);
                }
            }
            free(buffer);

            if (!ok) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }
            break;
        }

        case INST_SLEEP: {
            uint32_t tiempo = (uint32_t) atoi(inst->parametros[0]);
            enviar_syscall_sleep(cpu, tiempo, logger);
            return 1;
        }

        case INST_EXIT: {
            enviar_syscall_exit(cpu, logger);
            return 1;
        }

        case INST_IO_STDIN: {
            uint32_t dir = get_registro_32(cpu, inst->parametros[0]);
            uint32_t tam = get_registro_32(cpu, inst->parametros[1]);

            // La MMU valida el acceso ahora (SEG_FAULT si se sale del segmento);
            // al KS se le manda la direccion LOGICA y Kernel Memory la vuelve a
            // traducir al momento de escribir, por si hubo compactacion en el medio.
            if (mmu_traducir(cpu, dir, tam, logger) == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }
            enviar_syscall_stdin(cpu, dir, tam, logger);
            return 1;
        }

        case INST_IO_STDOUT: {
            uint32_t dir = get_registro_32(cpu, inst->parametros[0]);
            uint32_t tam = get_registro_32(cpu, inst->parametros[1]);

            if (mmu_traducir(cpu, dir, tam, logger) == UINT32_MAX) {
                enviar_syscall_seg_fault(cpu, logger);
                return 1;
            }
            enviar_syscall_stdout(cpu, dir, tam, logger);
            return 1;
        }

        case INST_MUTEX_CREATE: {
            enviar_syscall_mutex_create(cpu, inst->parametros[0], logger);
            return 1;
        }
        case INST_MUTEX_LOCK: {
            enviar_syscall_mutex_lock(cpu, inst->parametros[0], logger);
            return 1;
        }
        case INST_MUTEX_UNLOCK: {
            enviar_syscall_mutex_unlock(cpu, inst->parametros[0], logger);
            return 1;
        }
        case INST_MEM_ALLOC: {
            uint32_t id_seg = (uint32_t) atoi(inst->parametros[0]);
            uint32_t tam = (uint32_t) atoi(inst->parametros[1]);
            enviar_syscall_mem_alloc(cpu, id_seg, tam, logger);
            return 1;
        }
        case INST_MEM_FREE: {
            uint32_t id_seg = (uint32_t) atoi(inst->parametros[0]);
            enviar_syscall_mem_free(cpu, id_seg, logger);
            return 1;
        }
        case INST_INIT_PROC: {
            char* archivo = inst->parametros[0];
            int32_t prioridad = atoi(inst->parametros[1]);
            enviar_syscall_init_proc(cpu, archivo, prioridad, logger);
            return 1;
        }

        default:
            log_warning(logger, "Instrucción desconocida: %d", inst->codigo);
            break;
    }

    return 0;
}

//Devuelve true si hay interrupcion, en caso contrario false
bool cpu_check_interrupt(struct t_cpu* cpu, t_log* logger) {
    t_codigo_mensaje codigo;
    ssize_t bytes = recv(cpu->socket_kernel, &codigo, sizeof(t_codigo_mensaje), MSG_DONTWAIT);

    if (bytes > 0 && codigo == MENSAJE_INTERRUPCION) {
        log_info(logger, "## Interrupción recibida");
        enviar_contexto_a_ks(cpu, logger);
        return true;
    }
    return false;
}
