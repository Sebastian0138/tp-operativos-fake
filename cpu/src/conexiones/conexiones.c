#include "conexiones.h"
#include "cpu/cpu.h"

#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

int conectar_a_kernel_scheduler(const char* ip, const char* puerto, uint32_t id_cpu, t_log* logger) {
    int fd_conexion = crear_conexion(ip, puerto);
    if (fd_conexion == -1) {
        log_error(logger, "No se pudo conectar a Kernel Scheduler en %s:%s", ip, puerto);
        return -1;
    }

    // Handshake: me presento como CPU al KS
    int32_t handshake = HANDSHAKE_CPU;
    send(fd_conexion, &handshake, sizeof(int32_t), 0);

    // Enviar ID de CPU
    send(fd_conexion, &id_cpu, sizeof(uint32_t), 0);

    log_info(logger, "Conectado a Kernel Scheduler en %s:%s", ip, puerto);
    return fd_conexion;
}

int conectar_a_kernel_memory(const char* ip, const char* puerto, uint32_t id_cpu, uint32_t* segment_max_size, t_log* logger) {
    int fd_conexion = crear_conexion(ip, puerto);
    if (fd_conexion == -1) {
        log_error(logger, "No se pudo conectar a Kernel Memory en %s:%s", ip, puerto);
        return -1;
    }

    // Handshake: me presento como CPU al KM, con mi ID
    int32_t handshake = HANDSHAKE_CPU;
    send(fd_conexion, &handshake, sizeof(int32_t), 0);
    int32_t id = (int32_t) id_cpu;
    send(fd_conexion, &id, sizeof(int32_t), 0);

    // Recibir SEGMENT_MAX_SIZE de KM
    recv(fd_conexion, segment_max_size, sizeof(uint32_t), MSG_WAITALL);

    log_info(logger, "Conectado a Kernel Memory en %s:%s (segment_max_size=%u)", ip, puerto, *segment_max_size);
    return fd_conexion;
}

static void enviar_syscall_a_ks(struct t_cpu* cpu, t_codigo_syscall syscall_tipo, t_log* logger) {
    // Toda syscall libera la CPU; el PC se incrementa antes de mandar el
    // contexto para que, al resumir el proceso, continue en la instruccion
    // siguiente y no repita la misma syscall en un loop infinito.
    cpu->registros.pc++;

    t_codigo_mensaje cod = MENSAJE_SYSCALL;

    send(cpu->socket_kernel, &cod, sizeof(t_codigo_mensaje), 0);
    send(cpu->socket_kernel, &syscall_tipo, sizeof(t_codigo_syscall), 0);

    log_info(logger, "## (%u) - Solicitó syscall: %s", cpu->pid_actual, codigo_syscall_to_string(syscall_tipo));
}

void enviar_syscall_sleep(struct t_cpu* cpu, uint32_t tiempo, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_SLEEP, logger);

    send(cpu->socket_kernel, &tiempo, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_exit(struct t_cpu* cpu, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_EXIT, logger);
}

void enviar_syscall_seg_fault(struct t_cpu* cpu, t_log* logger) {
    // No incrementa el PC "para continuar": el proceso finaliza igual, pero se
    // mantiene el mismo esquema de mensaje que las demas syscalls.
    enviar_syscall_a_ks(cpu, SYSCALL_SEG_FAULT, logger);
}

void enviar_syscall_stdin(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tam, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_STDIN, logger);

    send(cpu->socket_kernel, &dir_logica, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &tam, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_stdout(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tam, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_STDOUT, logger);

    send(cpu->socket_kernel, &dir_logica, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &tam, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_contexto_a_ks(struct t_cpu* cpu, t_log* logger) {
    // Kernel Scheduler espera primero el codigo de mensaje (lee MENSAJE_INTERRUPCION
    // en atender_peticiones_cpu) y despues el struct de registros; mandar solo los
    // registros desincroniza el protocolo del socket para siempre.
    t_codigo_mensaje cod = MENSAJE_INTERRUPCION;
    send(cpu->socket_kernel, &cod, sizeof(t_codigo_mensaje), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_mutex_create(struct t_cpu* cpu, char* nombre_mutex, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_MUTEX_CREATE, logger);
    uint32_t len = strlen(nombre_mutex);

    send(cpu->socket_kernel, &len, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, nombre_mutex, len, 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_mutex_lock(struct t_cpu* cpu, char* nombre_mutex, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_MUTEX_LOCK, logger);
    uint32_t len = strlen(nombre_mutex);

    send(cpu->socket_kernel, &len, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, nombre_mutex, len, 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_mutex_unlock(struct t_cpu* cpu, char* nombre_mutex, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_MUTEX_UNLOCK, logger);
    uint32_t len = strlen(nombre_mutex);

    send(cpu->socket_kernel, &len, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, nombre_mutex, len, 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_mem_alloc(struct t_cpu* cpu, uint32_t id_segmento, uint32_t tamanio, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_MEM_ALLOC, logger);

    send(cpu->socket_kernel, &id_segmento, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &tamanio, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_mem_free(struct t_cpu* cpu, uint32_t id_segmento, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_MEM_FREE, logger);

    send(cpu->socket_kernel, &id_segmento, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void enviar_syscall_init_proc(struct t_cpu* cpu, char* archivo, int32_t prioridad, t_log* logger) {
    enviar_syscall_a_ks(cpu, SYSCALL_INIT_PROC, logger);
    uint32_t len = strlen(archivo);

    send(cpu->socket_kernel, &len, sizeof(uint32_t), 0);
    send(cpu->socket_kernel, archivo, len, 0);
    send(cpu->socket_kernel, &prioridad, sizeof(int32_t), 0);
    send(cpu->socket_kernel, &(cpu->registros), sizeof(t_registros_cpu), 0);
}

void solicitar_contexto_a_km(struct t_cpu* cpu, t_log* logger) {
    t_codigo_mensaje cod = MENSAJE_CONTEXTO;
    send(cpu->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(cpu->socket_memoria, &(cpu->pid_actual), sizeof(uint32_t), 0);

    recv(cpu->socket_memoria, &(cpu->registros), sizeof(t_registros_cpu), MSG_WAITALL);

    uint32_t cant_segmentos;
    recv(cpu->socket_memoria, &cant_segmentos, sizeof(uint32_t), MSG_WAITALL);

    list_clean_and_destroy_elements(cpu->tabla_segmentos, free);
    for (uint32_t i = 0; i < cant_segmentos; i++) {
        t_segmento* seg = malloc(sizeof(t_segmento));
        recv(cpu->socket_memoria, seg, sizeof(t_segmento), MSG_WAITALL);
        list_add(cpu->tabla_segmentos, seg);
    }

    log_info(logger, "## PID: %u - Contexto recibido de KM (%u segmentos)", cpu->pid_actual, cant_segmentos);
}

// ============================================================
// Topologia de Memory Sticks y ruteo de direcciones fisicas
// ============================================================

static t_stick_cpu* buscar_stick_por_id(struct t_cpu* cpu, uint32_t id) {
    for (int i = 0; i < list_size(cpu->sticks); i++) {
        t_stick_cpu* stick = list_get(cpu->sticks, i);
        if (stick->id == id) return stick;
    }
    return NULL;
}

void actualizar_topologia_sticks(struct t_cpu* cpu, t_log* logger) {
    t_codigo_mensaje cod = MENSAJE_TOPOLOGIA_STICKS;
    send(cpu->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);

    uint32_t cant;
    if (recv(cpu->socket_memoria, &cant, sizeof(uint32_t), MSG_WAITALL) <= 0) return;

    for (uint32_t i = 0; i < cant; i++) {
        uint32_t id, tamanio, base, ip_len, port_len;
        recv(cpu->socket_memoria, &id,      sizeof(uint32_t), MSG_WAITALL);
        recv(cpu->socket_memoria, &tamanio, sizeof(uint32_t), MSG_WAITALL);
        recv(cpu->socket_memoria, &base,    sizeof(uint32_t), MSG_WAITALL);

        recv(cpu->socket_memoria, &ip_len, sizeof(uint32_t), MSG_WAITALL);
        char* ip = malloc(ip_len + 1);
        recv(cpu->socket_memoria, ip, ip_len, MSG_WAITALL);
        ip[ip_len] = '\0';

        recv(cpu->socket_memoria, &port_len, sizeof(uint32_t), MSG_WAITALL);
        char* puerto = malloc(port_len + 1);
        recv(cpu->socket_memoria, puerto, port_len, MSG_WAITALL);
        puerto[port_len] = '\0';

        if (buscar_stick_por_id(cpu, id) != NULL) {
            // Ya lo conociamos y estamos conectados
            free(ip);
            free(puerto);
            continue;
        }

        int fd = crear_conexion(ip, puerto);
        if (fd == -1) {
            log_error(logger, "No se pudo conectar al Memory Stick %u en %s:%s", id, ip, puerto);
            free(ip);
            free(puerto);
            continue;
        }
        int32_t handshake = HANDSHAKE_CPU;
        send(fd, &handshake, sizeof(int32_t), 0);
        int32_t id_cpu = (int32_t) cpu->id_cpu;
        send(fd, &id_cpu, sizeof(int32_t), 0);

        t_stick_cpu* stick = malloc(sizeof(t_stick_cpu));
        stick->id = id;
        stick->base = base;
        stick->tamanio = tamanio;
        stick->ip = ip;
        stick->puerto = puerto;
        stick->fd = fd;
        list_add(cpu->sticks, stick);

        log_info(logger, "Memory Stick %u agregado a la topologia: base %u, %u bytes (%s:%s)",
                 id, base, tamanio, ip, puerto);
    }
}

static t_stick_cpu* stick_para_direccion(struct t_cpu* cpu, uint32_t dir) {
    for (int i = 0; i < list_size(cpu->sticks); i++) {
        t_stick_cpu* stick = list_get(cpu->sticks, i);
        if (dir >= stick->base && dir < stick->base + stick->tamanio) {
            return stick;
        }
    }
    return NULL;
}

// Operacion contra un unico stick por la conexion directa de la CPU.
static bool stick_operar(t_stick_cpu* stick, bool escribir, uint32_t dir_local, uint32_t tamanio, void* buffer) {
    t_codigo_mensaje cod = escribir ? MENSAJE_ESCRIBIR_MEMORIA : MENSAJE_LEER_MEMORIA;
    send(stick->fd, &cod, sizeof(t_codigo_mensaje), 0);
    send(stick->fd, &dir_local, sizeof(uint32_t), 0);
    send(stick->fd, &tamanio, sizeof(uint32_t), 0);
    if (escribir) {
        send(stick->fd, buffer, tamanio, 0);
    }

    t_codigo_mensaje respuesta;
    if (recv(stick->fd, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0) return false;
    if (respuesta != MENSAJE_OK) return false;

    if (!escribir) {
        if (recv(stick->fd, buffer, tamanio, MSG_WAITALL) <= 0) return false;
    }
    return true;
}

// Divide la operacion entre los sticks involucrados y la consolida como una sola.
static bool operar_fisico(struct t_cpu* cpu, bool escribir, uint32_t dir_fisica, uint32_t tamanio, void* buffer, t_log* logger) {
    uint32_t restante = tamanio;
    uint32_t dir = dir_fisica;
    char* cursor = (char*) buffer;
    bool reintentado = false;

    while (restante > 0) {
        t_stick_cpu* stick = stick_para_direccion(cpu, dir);
        if (stick == NULL && !reintentado) {
            // Puede haberse conectado un stick nuevo: refrescar topologia una vez
            actualizar_topologia_sticks(cpu, logger);
            reintentado = true;
            continue;
        }
        if (stick == NULL) {
            log_error(logger, "PID: %u - Direccion fisica %u fuera de la topologia conocida", cpu->pid_actual, dir);
            return false;
        }

        uint32_t dir_local = dir - stick->base;
        uint32_t disponible = stick->tamanio - dir_local;
        uint32_t tramo = restante < disponible ? restante : disponible;

        if (!stick_operar(stick, escribir, dir_local, tramo, cursor)) {
            log_error(logger, "PID: %u - Fallo %s en Memory Stick %u", cpu->pid_actual,
                      escribir ? "escritura" : "lectura", stick->id);
            return false;
        }

        dir += tramo;
        cursor += tramo;
        restante -= tramo;
    }
    return true;
}

bool leer_memoria_fisica(struct t_cpu* cpu, uint32_t dir_fisica, uint32_t tamanio, void* destino, t_log* logger) {
    return operar_fisico(cpu, false, dir_fisica, tamanio, destino, logger);
}

bool escribir_memoria_fisica(struct t_cpu* cpu, uint32_t dir_fisica, uint32_t tamanio, const void* origen, t_log* logger) {
    return operar_fisico(cpu, true, dir_fisica, tamanio, (void*) origen, logger);
}
