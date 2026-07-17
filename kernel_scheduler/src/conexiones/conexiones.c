#include "conexiones.h"
#include <utils/sockets/sockets.h>
#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

// Punteros globales para acceso desde hilos de conexion
static t_log* _logger = NULL;
static t_planificador* _planificador = NULL;
static t_kernel_scheduler_config* _config = NULL;

// Declaracion interna
void* manejar_cliente(void* arg);
void atender_peticiones_cpu(int socket_cpu, int32_t id_cpu);

void iniciar_servidor_kernel_scheduler(const char* puerto, t_planificador* planificador, t_kernel_scheduler_config* config, t_log* logger) {
    _logger = logger;
    _planificador = planificador;
    _config = config;

    int fd_escucha = crear_servidor(puerto);
    if (fd_escucha == -1) {
        log_error(logger, "Fallo al crear el socket servidor (puerto: %s)", puerto);
        return;
    }

    log_info(logger, "Kernel Scheduler escuchando en puerto %s", puerto);

    while (1) {
        pthread_t hilo;
        int* fd_conexion_ptr = malloc(sizeof(int));
        *fd_conexion_ptr = accept(fd_escucha, NULL, NULL);

        if (*fd_conexion_ptr == -1) {
            log_error(logger, "Error al aceptar conexion entrante");
            free(fd_conexion_ptr);
            continue;
        }

        pthread_create(&hilo, NULL, manejar_cliente, fd_conexion_ptr);
        pthread_detach(hilo);
    }
}

void* manejar_cliente(void* arg) {
    int* fd_ptr = (int*) arg;
    int fd_conexion = *fd_ptr;
    free(fd_ptr);

    // Recibir handshake: el cliente nos dice si es CPU o IO
    int32_t handshake;
    ssize_t bytes = recv(fd_conexion, &handshake, sizeof(int32_t), MSG_WAITALL);

    if (bytes == 0) {
        log_info(_logger, "Un cliente se desconecto antes del handshake");
        close(fd_conexion);
        return NULL;
    }

    if (bytes == -1) {
        log_error(_logger, "Error al recibir handshake");
        close(fd_conexion);
        return NULL;
    }

    switch (handshake) {
        case HANDSHAKE_CPU: {
            int32_t id_cpu;
            // Recibir ID de CPU
            ssize_t r_bytes = recv(fd_conexion, &id_cpu, sizeof(int32_t), MSG_WAITALL);
            if (r_bytes <= 0) {
                log_error(_logger, "Fallo al recibir ID de CPU. Usando ID secuencial.");
                pthread_mutex_lock(&_planificador->mutex_cpus);
                id_cpu = list_size(_planificador->cpus_conectadas) + 1;
                pthread_mutex_unlock(&_planificador->mutex_cpus);
            }

            planificador_registrar_cpu(_planificador, fd_conexion, id_cpu);
            atender_peticiones_cpu(fd_conexion, id_cpu);
            break;
        }
        case HANDSHAKE_IO: {
            uint32_t nombre_len;
            if (recv(fd_conexion, &nombre_len, sizeof(uint32_t), MSG_WAITALL) <= 0) {
                close(fd_conexion);
                break;
            }
            char* nombre = malloc(nombre_len + 1);
            if (recv(fd_conexion, nombre, nombre_len, MSG_WAITALL) <= 0) {
                free(nombre);
                close(fd_conexion);
                break;
            }
            nombre[nombre_len] = '\0';

            t_tipo_io tipo;
            if (strcmp(nombre, "STDIN") == 0)       tipo = IO_STDIN;
            else if (strcmp(nombre, "STDOUT") == 0) tipo = IO_STDOUT;
            else if (strcmp(nombre, "SLEEP") == 0)  tipo = IO_SLEEP;
            else {
                log_warning(_logger, "Tipo de IO desconocido: %s — conexion rechazada", nombre);
                free(nombre);
                close(fd_conexion);
                break;
            }

            // El fd queda en manos del pool de IO del planificador (planificador_tomar_io_libre
            // lo va a usar bajo demanda desde los hilos que atienden syscalls de CPU); este hilo
            // de handshake no lo lee ni lo cierra.
            planificador_registrar_io(_planificador, fd_conexion, tipo, nombre);
            free(nombre);
            break;
        }
        default:
            log_warning(_logger, "Handshake desconocido: %d — conexion rechazada", handshake);
            close(fd_conexion);
            break;
    }

    return NULL;
}

// Libera la CPU y avisa al corto plazo que hay una disponible.
static void liberar_cpu(t_cpu_conectada* cpu) {
    pthread_mutex_lock(&_planificador->mutex_cpus);
    cpu->libre = true;
    cpu->pid_ejecutando = 0;
    cpu->en_rafaga = false;
    pthread_mutex_unlock(&_planificador->mutex_cpus);
    sem_post(&_planificador->sem_cpus_libres);
}

// Finaliza un proceso: transicion a EXIT, liberacion de los mutex que tuviera
// tomados (despertando a los que esperaban) y liberacion en Kernel Memory.
static void finalizar_proceso(t_pcb* pcb, const char* motivo) {
    planificador_transicionar_exec_a_exit(_planificador, pcb, motivo);

    // Un proceso que finaliza no puede dejar mutex tomados para siempre:
    // se liberan en orden y se despierta al siguiente de cada cola.
    while (list_size(pcb->lista_mutex_tomados) > 0) {
        char* nombre = list_get(pcb->lista_mutex_tomados, 0);
        char* copia = strdup(nombre);
        planificador_unlock_mutex(_planificador, pcb, copia);
        free(copia);
    }

    planificador_finalizar_proceso_km(_planificador, pcb->pid);

    // Sus segmentos quedaron libres: des-suspender / reintentar allocs pendientes
    planificador_evento_memoria_liberada(_planificador);
}

void atender_peticiones_cpu(int socket_cpu, int32_t id_cpu) {
    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(socket_cpu, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) {
            planificador_remover_cpu(_planificador, socket_cpu);
            close(socket_cpu);
            break;
        }

        // Obtener la CPU y el PCB ejecutando
        t_cpu_conectada* cpu = planificador_obtener_cpu_por_id(_planificador, id_cpu);
        if (cpu == NULL) {
            close(socket_cpu);
            break;
        }

        // La CPU devolvio el proceso (syscall o interrupcion): dejo de ejecutar
        // instrucciones. Capturamos y limpiamos el motivo de interrupcion.
        pthread_mutex_lock(&_planificador->mutex_cpus);
        uint32_t pid_ejec = cpu->pid_ejecutando;
        t_motivo_interrupcion motivo_int = cpu->motivo_interrupcion;
        uint32_t pid_preemptor = cpu->pid_preemptor;
        int prioridad_preemptor = cpu->prioridad_preemptor;
        cpu->en_rafaga = false;
        cpu->motivo_interrupcion = INT_NINGUNA;
        pthread_mutex_unlock(&_planificador->mutex_cpus);

        // Buscar el PCB en ejecución en lista_exec
        t_pcb* pcb_ejecutando = NULL;
        pthread_mutex_lock(&_planificador->mutex_lista_exec);
        for (int i = 0; i < list_size(_planificador->lista_exec); i++) {
            t_pcb* p = list_get(_planificador->lista_exec, i);
            if (p->pid == pid_ejec) {
                pcb_ejecutando = p;
                break;
            }
        }
        pthread_mutex_unlock(&_planificador->mutex_lista_exec);

        switch (codigo) {
            case MENSAJE_SYSCALL: {
                t_codigo_syscall syscall_tipo;
                recv(socket_cpu, &syscall_tipo, sizeof(t_codigo_syscall), MSG_WAITALL);

                if (pcb_ejecutando != NULL) {
                    log_info(_logger, "## (%u) - Solicitó syscall: %s", pid_ejec, codigo_syscall_to_string(syscall_tipo));
                }

                if (syscall_tipo == SYSCALL_EXIT) {
                    if (pcb_ejecutando != NULL) {
                        finalizar_proceso(pcb_ejecutando, "SUCCESS");
                    }
                    liberar_cpu(cpu);
                }
                else if (syscall_tipo == SYSCALL_SEG_FAULT) {
                    if (pcb_ejecutando != NULL) {
                        finalizar_proceso(pcb_ejecutando, "SEG_FAULT");
                    }
                    liberar_cpu(cpu);
                }
                else if (syscall_tipo == SYSCALL_SLEEP) {
                    uint32_t tiempo_ms;
                    recv(socket_cpu, &tiempo_ms, sizeof(uint32_t), MSG_WAITALL);

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        planificador_transicionar_exec_a_block(_planificador, pcb_ejecutando, "SLEEP");
                        planificador_ejecutar_io_async(_planificador, pcb_ejecutando, IO_SLEEP, tiempo_ms, 0);
                    }
                    liberar_cpu(cpu);
                }
                else if (syscall_tipo == SYSCALL_STDIN || syscall_tipo == SYSCALL_STDOUT) {
                    uint32_t dir_logica, tam;
                    recv(socket_cpu, &dir_logica, sizeof(uint32_t), MSG_WAITALL);
                    recv(socket_cpu, &tam, sizeof(uint32_t), MSG_WAITALL);

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        const char* motivo = (syscall_tipo == SYSCALL_STDIN) ? "STDIN" : "STDOUT";
                        planificador_transicionar_exec_a_block(_planificador, pcb_ejecutando, motivo);
                        planificador_ejecutar_io_async(_planificador, pcb_ejecutando,
                            syscall_tipo == SYSCALL_STDIN ? IO_STDIN : IO_STDOUT, dir_logica, tam);
                    }
                    liberar_cpu(cpu);
                }
                else if (syscall_tipo == SYSCALL_MUTEX_CREATE) {
                    uint32_t len;
                    recv(socket_cpu, &len, sizeof(uint32_t), MSG_WAITALL);
                    char* nombre = malloc(len + 1);
                    recv(socket_cpu, nombre, len, MSG_WAITALL);
                    nombre[len] = '\0';

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        planificador_crear_mutex(_planificador, nombre);
                        // La creacion de mutex devuelve el proceso a la misma CPU
                        planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                    }
                    free(nombre);
                }
                else if (syscall_tipo == SYSCALL_MUTEX_LOCK) {
                    uint32_t len;
                    recv(socket_cpu, &len, sizeof(uint32_t), MSG_WAITALL);
                    char* nombre = malloc(len + 1);
                    recv(socket_cpu, nombre, len, MSG_WAITALL);
                    nombre[len] = '\0';

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        bool adquirido = planificador_lock_mutex(_planificador, pcb_ejecutando, nombre);
                        if (adquirido) {
                            log_info(_logger, "## (%u) Toma el Mutex %s", pcb_ejecutando->pid, nombre);
                            list_add(pcb_ejecutando->lista_mutex_tomados, strdup(nombre));
                            // Si lo tomo, sigue ejecutando en la misma CPU
                            planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                        } else {
                            if (pcb_ejecutando->recurso_bloqueado) free(pcb_ejecutando->recurso_bloqueado);
                            pcb_ejecutando->recurso_bloqueado = strdup(nombre);
                            planificador_transicionar_exec_a_block(_planificador, pcb_ejecutando, "MUTEX");
                            liberar_cpu(cpu);
                        }
                    }
                    free(nombre);
                }
                else if (syscall_tipo == SYSCALL_MUTEX_UNLOCK) {
                    uint32_t len;
                    recv(socket_cpu, &len, sizeof(uint32_t), MSG_WAITALL);
                    char* nombre = malloc(len + 1);
                    recv(socket_cpu, nombre, len, MSG_WAITALL);
                    nombre[len] = '\0';

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        log_info(_logger, "## (%u) Libera el Mutex %s", pcb_ejecutando->pid, nombre);
                        planificador_unlock_mutex(_planificador, pcb_ejecutando, nombre);
                        // La liberacion de mutex devuelve el proceso a la misma CPU
                        planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                    }
                    free(nombre);
                }
                else if (syscall_tipo == SYSCALL_MEM_ALLOC) {
                    uint32_t id_segmento, tamanio;
                    recv(socket_cpu, &id_segmento, sizeof(uint32_t), MSG_WAITALL);
                    recv(socket_cpu, &tamanio, sizeof(uint32_t), MSG_WAITALL);

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        bool hubo_compactacion = false;
                        bool ok = planificador_crear_segmento_km(_planificador, pcb_ejecutando->pid,
                                                                 id_segmento, tamanio, &hubo_compactacion);
                        if (ok) {
                            // La syscall de memoria devuelve el proceso a la misma CPU
                            planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                            if (hubo_compactacion) {
                                // La compactacion es un evento de reordenamiento de memoria:
                                // puede haber des-suspensiones pendientes que ahora entren
                                planificador_evento_memoria_liberada(_planificador);
                            }
                        } else {
                            // Sin espacio ni compactando: el proceso espera a que se libere
                            // memoria (suspensiones, MEM_FREE o fin de otros procesos).
                            planificador_transicionar_exec_a_block(_planificador, pcb_ejecutando, "MEMORIA");
                            planificador_bloquear_por_memoria(_planificador, pcb_ejecutando, id_segmento, tamanio);
                            liberar_cpu(cpu);
                        }
                    }
                }
                else if (syscall_tipo == SYSCALL_MEM_FREE) {
                    uint32_t id_segmento;
                    recv(socket_cpu, &id_segmento, sizeof(uint32_t), MSG_WAITALL);

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;

                        if (planificador_eliminar_segmento_km(_planificador, pcb_ejecutando->pid, id_segmento)) {
                            planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                            // Se libero memoria: des-suspender / reintentar allocs pendientes
                            planificador_evento_memoria_liberada(_planificador);
                        } else {
                            log_warning(_logger, "## (%u) MEM_FREE del segmento %u fallo", pcb_ejecutando->pid, id_segmento);
                            planificador_despachar_directo(_planificador, pcb_ejecutando, cpu);
                        }
                    }
                }
                else if (syscall_tipo == SYSCALL_INIT_PROC) {
                    // Leer path y prioridad
                    uint32_t path_len;
                    recv(socket_cpu, &path_len, sizeof(uint32_t), MSG_WAITALL);
                    char* path = malloc(path_len + 1);
                    recv(socket_cpu, path, path_len, MSG_WAITALL);
                    path[path_len] = '\0';

                    int32_t prioridad;
                    recv(socket_cpu, &prioridad, sizeof(int32_t), MSG_WAITALL);

                    if (pcb_ejecutando != NULL) {
                        t_registros_cpu regs;
                        recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                        pcb_ejecutando->registros = regs;
                    }

                    // Con Colas Multinivel no se planifican procesos con prioridad
                    // fuera del rango [0, cant_colas): se rechaza la creacion.
                    bool prioridad_valida = true;
                    if (strcmp(_config->planification_algorithm, "CMN") == 0) {
                        prioridad_valida = prioridad >= 0 && prioridad < _planificador->cant_colas;
                    }

                    if (prioridad_valida) {
                        pthread_mutex_lock(&_planificador->mutex_pid);
                        uint32_t nuevo_pid = _planificador->proximo_pid++;
                        pthread_mutex_unlock(&_planificador->mutex_pid);

                        t_pcb* nuevo_pcb = pcb_create(nuevo_pid, path);
                        nuevo_pcb->prioridad_original = prioridad;
                        nuevo_pcb->prioridad_actual = prioridad;

                        planificador_encolar_new(_planificador, nuevo_pcb);
                    } else {
                        log_error(_logger, "INIT_PROC con prioridad %d fuera de rango [0, %d): no se planifica",
                                  prioridad, _planificador->cant_colas);
                    }

                    free(path);

                    // El proceso llamante vuelve a READY y se libera la CPU.
                    if (pcb_ejecutando != NULL) {
                        planificador_transicionar_exec_a_ready(_planificador, pcb_ejecutando);
                    }
                    liberar_cpu(cpu);
                }
                break;
            }
            case MENSAJE_INTERRUPCION: {
                // La CPU devuelve el proceso desalojado con su contexto
                if (pcb_ejecutando != NULL) {
                    t_registros_cpu regs;
                    recv(socket_cpu, &regs, sizeof(t_registros_cpu), MSG_WAITALL);
                    pcb_ejecutando->registros = regs;

                    switch (motivo_int) {
                        case INT_COMPACTACION:
                            // Excepcionalmente, va al PRINCIPIO de su cola de READY
                            log_info(_logger, "## (%u) - Desalojado por compactación", pid_ejec);
                            planificador_transicionar_exec_a_ready_frente(_planificador, pcb_ejecutando);
                            break;
                        case INT_PREEMPCION:
                            // El log obligatorio ya se emitio al enviar la interrupcion
                            (void) pid_preemptor;
                            (void) prioridad_preemptor;
                            planificador_transicionar_exec_a_ready(_planificador, pcb_ejecutando);
                            break;
                        case INT_QUANTUM:
                        default:
                            log_info(_logger, "## (%u) - Desalojado por fin de quantum", pid_ejec);
                            planificador_transicionar_exec_a_ready(_planificador, pcb_ejecutando);
                            break;
                    }
                }
                liberar_cpu(cpu);
                break;
            }
            default:
                break;
        }
    }
}

int conectar_a_kernel_memory(const char* ip, const char* puerto, t_log* logger) {
    int fd_conexion = crear_conexion(ip, puerto);
    if (fd_conexion != -1) {
        log_info(logger, "## Conectado a Kernel Memory");
        int32_t handshake = HANDSHAKE_SCHEDULER;
        send(fd_conexion, &handshake, sizeof(int32_t), 0);
    } else {
        log_error(logger, "No se pudo conectar a Kernel Memory en %s:%s", ip, puerto);
    }
    return fd_conexion;
}

// Hilo que escucha el canal de notificaciones KM -> KS (mas memoria / BSOD)
static void* hilo_notificaciones_km(void* arg) {
    int fd = (int)(intptr_t) arg;

    while (1) {
        t_codigo_mensaje codigo;
        ssize_t bytes = recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL);
        if (bytes <= 0) {
            log_warning(_logger, "Canal de notificaciones de Kernel Memory cerrado");
            break;
        }

        switch (codigo) {
            case MENSAJE_MAS_MEMORIA: {
                uint32_t nuevo_total;
                recv(fd, &nuevo_total, sizeof(uint32_t), MSG_WAITALL);
                log_info(_logger, "Kernel Memory informa mas memoria disponible: %u bytes totales", nuevo_total);
                // Con mas memoria pueden entrar procesos suspendidos o allocs en espera
                planificador_evento_memoria_liberada(_planificador);
                break;
            }
            case MENSAJE_MEMORIA_CORRUPTA:
                planificador_bsod(_planificador); // no retorna
                break;
            default:
                log_warning(_logger, "Notificacion desconocida de Kernel Memory: %d", codigo);
                break;
        }
    }

    close(fd);
    return NULL;
}

int conectar_canal_notificaciones_km(const char* ip, const char* puerto, t_planificador* planificador, t_log* logger) {
    _logger = logger;
    _planificador = planificador;

    int fd_conexion = crear_conexion(ip, puerto);
    if (fd_conexion == -1) {
        log_error(logger, "No se pudo abrir el canal de notificaciones con Kernel Memory en %s:%s", ip, puerto);
        return -1;
    }

    int32_t handshake = HANDSHAKE_SCHEDULER_NOTIF;
    send(fd_conexion, &handshake, sizeof(int32_t), 0);

    pthread_t hilo;
    pthread_create(&hilo, NULL, hilo_notificaciones_km, (void*)(intptr_t) fd_conexion);
    pthread_detach(hilo);

    log_info(logger, "Canal de notificaciones con Kernel Memory establecido");
    return fd_conexion;
}
