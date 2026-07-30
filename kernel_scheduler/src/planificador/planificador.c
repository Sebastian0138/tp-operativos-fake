#include "planificador.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <utils/protocolo.h>

// ============================================================
// Helpers de colas READY multinivel
// ============================================================

static bool es_cmn(t_planificador* pl) {
    return strcmp(pl->config->planification_algorithm, "CMN") == 0;
}

// Indice de cola READY que le corresponde al proceso.
// Con FIFO/RR hay una sola cola; con CMN, una por prioridad.
static int cola_de(t_planificador* pl, t_pcb* pcb) {
    if (!es_cmn(pl)) return 0;
    int q = pcb->prioridad_actual;
    if (q < 0) q = 0;
    if (q >= pl->cant_colas) q = pl->cant_colas - 1;
    return q;
}

static bool cola_usa_rr(t_planificador* pl, int cola) {
    return strcmp(pl->alg_por_cola[cola], "RR") == 0;
}

// Desalojo entre colas (solo CMN + QUEUE_PREEMPTION): si llego un proceso mas
// prioritario que alguno en ejecucion, se interrumpe al menos prioritario.
static void chequear_desalojo_por_prioridad(t_planificador* pl, t_pcb* nuevo) {
    if (!es_cmn(pl) || !pl->queue_preemption) return;

    // Buscar la victima: el proceso en EXEC con peor prioridad (numero mas alto),
    // estrictamente menos prioritario que el que llega.
    uint32_t victima_pid = 0;
    int victima_prio = nuevo->prioridad_actual;
    bool hay_victima = false;

    pthread_mutex_lock(&pl->mutex_lista_exec);
    for (int i = 0; i < list_size(pl->lista_exec); i++) {
        t_pcb* p = list_get(pl->lista_exec, i);
        if (p->prioridad_actual > victima_prio) {
            victima_prio = p->prioridad_actual;
            victima_pid = p->pid;
            hay_victima = true;
        }
    }
    pthread_mutex_unlock(&pl->mutex_lista_exec);

    if (!hay_victima) return;

    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (!cpu->libre && cpu->en_rafaga && cpu->pid_ejecutando == victima_pid
            && cpu->motivo_interrupcion == INT_NINGUNA) {
            cpu->motivo_interrupcion = INT_PREEMPCION;
            cpu->pid_preemptor = nuevo->pid;
            cpu->prioridad_preemptor = nuevo->prioridad_actual;
            cpu->prioridad_victima = victima_prio;

            // El log obligatorio del desalojo lo emite el hilo que atiende la
            // respuesta de la CPU: hasta que el proceso no vuelva por la
            // interrupcion, el desalojo todavia no ocurrio (puede volver antes
            // por syscall o EXIT y entonces nunca se lo desaloja).
            log_debug(pl->logger,
                "Interrupción por cola más prioritaria enviada: PID %u (prioridad %d) en CPU %d, preemptor PID %u (prioridad %d)",
                victima_pid, victima_prio, cpu->id_cpu, nuevo->pid, nuevo->prioridad_actual);

            t_codigo_mensaje msg = MENSAJE_INTERRUPCION;
            send(cpu->socket_cpu, &msg, sizeof(t_codigo_mensaje), 0);
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);
}

// Inserta el PCB en su cola READY (al final, o al principio para el desalojo
// por compactacion) SIN avisarle todavia al corto plazo.
static void insertar_en_ready(t_planificador* pl, t_pcb* pcb, bool al_frente) {
    int q = cola_de(pl, pcb);

    pthread_mutex_lock(&pl->mutex_cola_ready);
    if (al_frente) {
        list_add_in_index(pl->colas_ready[q], 0, pcb);
    } else {
        list_add(pl->colas_ready[q], pcb);
    }
    pthread_mutex_unlock(&pl->mutex_cola_ready);
}

// Habilita al corto plazo a tomar el proceso ya encolado y dispara el chequeo
// de desalojo entre colas. Se llama SIEMPRE despues de haber emitido los logs
// de la transicion a READY: el corto plazo loguea "Pasa del estado READY al
// estado EXEC", y si se lo señaliza antes de terminar de loguear, ese log se
// puede intercalar en medio de los de la transicion.
static void habilitar_corto_plazo(t_planificador* pl, t_pcb* pcb, bool al_frente) {
    sem_post(&pl->sem_procesos_ready);

    // Durante el desalojo por compactacion no tiene sentido desalojar de nuevo
    if (!al_frente) {
        chequear_desalojo_por_prioridad(pl, pcb);
    }
}

static void encolar_en_ready(t_planificador* pl, t_pcb* pcb, bool al_frente) {
    insertar_en_ready(pl, pcb, al_frente);
    habilitar_corto_plazo(pl, pcb, al_frente);
}

// Saca de una cola el PCB con ese PID, preservando el orden de los demas.
// El caller debe tener tomado el mutex que protege la cola.
static t_pcb* sacar_de_cola_sin_lock(t_queue* cola, uint32_t pid) {
    t_pcb* encontrado = NULL;
    int size = queue_size(cola);
    for (int i = 0; i < size; i++) {
        t_pcb* p = queue_pop(cola);
        if (p->pid == pid && encontrado == NULL) {
            encontrado = p;
        } else {
            queue_push(cola, p);
        }
    }
    return encontrado;
}

// Saca el proceso mas prioritario de READY (cola de menor indice, posicion 0).
static t_pcb* desencolar_ready(t_planificador* pl) {
    t_pcb* pcb = NULL;
    pthread_mutex_lock(&pl->mutex_cola_ready);
    for (int q = 0; q < pl->cant_colas && pcb == NULL; q++) {
        if (list_size(pl->colas_ready[q]) > 0) {
            pcb = list_remove(pl->colas_ready[q], 0);
        }
    }
    pthread_mutex_unlock(&pl->mutex_cola_ready);
    return pcb;
}

// ============================================================
// Creacion / destruccion
// ============================================================

t_planificador* planificador_create(t_kernel_scheduler_config* config, t_log* logger, int socket_memoria) {
    t_planificador* pl = malloc(sizeof(t_planificador));
    if (pl == NULL) return NULL;

    // Armar las colas READY segun el algoritmo configurado
    if (strcmp(config->planification_algorithm, "CMN") == 0) {
        int cant = 0;
        while (config->queues_algorithms[cant] != NULL) cant++;
        if (cant == 0) {
            log_error(logger, "PLANIFICATION_ALGORITHM=CMN pero QUEUES_ALGORITHMS esta vacio");
            free(pl);
            return NULL;
        }
        pl->cant_colas = cant;
        pl->alg_por_cola = malloc(sizeof(char*) * cant);
        for (int i = 0; i < cant; i++) {
            pl->alg_por_cola[i] = config->queues_algorithms[i];
        }
    } else {
        pl->cant_colas = 1;
        pl->alg_por_cola = malloc(sizeof(char*));
        pl->alg_por_cola[0] = config->planification_algorithm;
    }

    pl->colas_ready = malloc(sizeof(t_list*) * pl->cant_colas);
    for (int i = 0; i < pl->cant_colas; i++) {
        pl->colas_ready[i] = list_create();
    }

    pl->queue_preemption = config->queue_preemption != NULL
                        && strcmp(config->queue_preemption, "TRUE") == 0;

    pl->cola_new = queue_create();
    pl->lista_exec = list_create();
    pl->cola_block = queue_create();
    pl->cola_susp_block = queue_create();
    pl->cola_susp_ready = queue_create();
    pl->lista_exit = list_create();
    pl->cpus_conectadas = list_create();
    pl->ios_conectados = list_create();
    pl->lista_mutex = list_create();

    pthread_mutex_init(&pl->mutex_cola_new, NULL);
    pthread_mutex_init(&pl->mutex_cola_ready, NULL);
    pthread_mutex_init(&pl->mutex_lista_exec, NULL);
    pthread_mutex_init(&pl->mutex_cola_block, NULL);
    pthread_mutex_init(&pl->mutex_lista_exit, NULL);
    pthread_mutex_init(&pl->mutex_cpus, NULL);
    pthread_mutex_init(&pl->mutex_ios, NULL);
    pthread_mutex_init(&pl->mutex_lista_mutex, NULL);
    pthread_mutex_init(&pl->mutex_socket_memoria, NULL);

    sem_init(&pl->sem_procesos_new, 0, 0);
    sem_init(&pl->sem_procesos_ready, 0, 0);
    sem_init(&pl->sem_cpus_libres, 0, 0);
    sem_init(&pl->sem_io_stdin_libres, 0, 0);
    sem_init(&pl->sem_io_stdout_libres, 0, 0);
    sem_init(&pl->sem_io_sleep_libres, 0, 0);

    pl->compactacion_en_curso = false;
    pthread_mutex_init(&pl->mutex_compactacion, NULL);
    pthread_cond_init(&pl->cond_compactacion, NULL);

    pthread_mutex_init(&pl->mutex_cola_susp, NULL);
    pthread_mutex_init(&pl->mutex_fin_io, NULL);
    pl->lista_espera_memoria = list_create();
    pthread_mutex_init(&pl->mutex_espera_memoria, NULL);
    pthread_mutex_init(&pl->mutex_evento_memoria, NULL);

    pl->config = config;
    pl->logger = logger;
    pl->socket_memoria = socket_memoria;
    pl->proximo_pid = 1; // El PID 0 se crea manualmente al inicio
    pthread_mutex_init(&pl->mutex_pid, NULL);

    return pl;
}

static void io_conectada_destroy(void* elemento) {
    t_io_conectada* io = (t_io_conectada*) elemento;
    close(io->fd);
    free(io->nombre);
    free(io);
}

void planificador_destroy(t_planificador* pl) {
    if (pl == NULL) return;

    queue_destroy_and_destroy_elements(pl->cola_new, (void*)pcb_destroy);
    for (int i = 0; i < pl->cant_colas; i++) {
        list_destroy_and_destroy_elements(pl->colas_ready[i], (void*)pcb_destroy);
    }
    free(pl->colas_ready);
    free(pl->alg_por_cola);
    list_destroy_and_destroy_elements(pl->lista_exec, (void*)pcb_destroy);
    queue_destroy_and_destroy_elements(pl->cola_block, (void*)pcb_destroy);
    queue_destroy_and_destroy_elements(pl->cola_susp_block, (void*)pcb_destroy);
    queue_destroy_and_destroy_elements(pl->cola_susp_ready, (void*)pcb_destroy);
    list_destroy_and_destroy_elements(pl->lista_exit, (void*)pcb_destroy);
    list_destroy_and_destroy_elements(pl->cpus_conectadas, free);
    list_destroy_and_destroy_elements(pl->ios_conectados, io_conectada_destroy);
    list_destroy_and_destroy_elements(pl->lista_mutex, (void*)mutex_kernel_destroy);

    pthread_mutex_destroy(&pl->mutex_cola_new);
    pthread_mutex_destroy(&pl->mutex_cola_ready);
    pthread_mutex_destroy(&pl->mutex_lista_exec);
    pthread_mutex_destroy(&pl->mutex_cola_block);
    pthread_mutex_destroy(&pl->mutex_lista_exit);
    pthread_mutex_destroy(&pl->mutex_cpus);
    pthread_mutex_destroy(&pl->mutex_ios);
    pthread_mutex_destroy(&pl->mutex_lista_mutex);
    pthread_mutex_destroy(&pl->mutex_socket_memoria);

    sem_destroy(&pl->sem_procesos_new);
    sem_destroy(&pl->sem_procesos_ready);
    sem_destroy(&pl->sem_cpus_libres);
    sem_destroy(&pl->sem_io_stdin_libres);
    sem_destroy(&pl->sem_io_stdout_libres);
    sem_destroy(&pl->sem_io_sleep_libres);

    pthread_mutex_destroy(&pl->mutex_compactacion);
    pthread_cond_destroy(&pl->cond_compactacion);

    pthread_mutex_destroy(&pl->mutex_cola_susp);
    pthread_mutex_destroy(&pl->mutex_fin_io);
    list_destroy_and_destroy_elements(pl->lista_espera_memoria, free);
    pthread_mutex_destroy(&pl->mutex_espera_memoria);
    pthread_mutex_destroy(&pl->mutex_evento_memoria);

    pthread_mutex_destroy(&pl->mutex_pid);

    free(pl);
}

// ============================================================
// Hilo de Largo Plazo (NEW -> READY)
// ============================================================

void* hilo_planificador_largo_plazo(void* arg) {
    t_planificador* pl = (t_planificador*) arg;
    log_info(pl->logger, "Hilo de Largo Plazo iniciado.");

    while (1) {
        sem_wait(&pl->sem_procesos_new);

        pthread_mutex_lock(&pl->mutex_cola_new);
        t_pcb* pcb = queue_pop(pl->cola_new);
        pthread_mutex_unlock(&pl->mutex_cola_new);

        if (pcb != NULL) {
            // Registrar el proceso en Kernel Memory (crea su contexto).
            // Los procesos inician sin memoria asignada, asi que el pasaje
            // NEW -> READY no tiene otra limitacion.
            if (pl->socket_memoria != -1) {
                pthread_mutex_lock(&pl->mutex_socket_memoria);

                t_codigo_mensaje cod = MENSAJE_ENVIAR_PID;
                send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
                send(pl->socket_memoria, &(pcb->pid), sizeof(uint32_t), 0);

                uint32_t path_len = strlen(pcb->path_instrucciones);
                send(pl->socket_memoria, &path_len, sizeof(uint32_t), 0);
                send(pl->socket_memoria, pcb->path_instrucciones, path_len, 0);

                t_codigo_mensaje respuesta;
                recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);

                pthread_mutex_unlock(&pl->mutex_socket_memoria);
            }

            planificador_transicionar_new_a_ready(pl, pcb);
        }
    }
    return NULL;
}

// ============================================================
// Despacho y quantum
// ============================================================

typedef struct {
    t_planificador* pl;
    uint32_t pid;
    int id_cpu;
    uint32_t rafaga;    // invalida el timer si la CPU ya arranco otra rafaga
} t_timer_args;

void* hilo_timer_quantum(void* arg) {
    t_timer_args* args = (t_timer_args*) arg;
    t_planificador* pl = args->pl;
    uint32_t target_pid = args->pid;
    int target_cpu = args->id_cpu;
    uint32_t target_rafaga = args->rafaga;
    int quantum_ms = pl->config->rr_quantum;

    free(args);

    usleep(quantum_ms * 1000);

    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (cpu->id_cpu == target_cpu && !cpu->libre && cpu->en_rafaga
            && cpu->pid_ejecutando == target_pid && cpu->rafaga == target_rafaga
            && cpu->motivo_interrupcion == INT_NINGUNA) {

            log_info(pl->logger, "Quantum vencido para PID %u en CPU %d. Enviando interrupción...",
                     target_pid, target_cpu);
            cpu->motivo_interrupcion = INT_QUANTUM;
            // El send se hace bajo mutex_cpus para que no se intercale con otro
            // hilo escribiendo al mismo socket_cpu.
            t_codigo_mensaje msg = MENSAJE_INTERRUPCION;
            send(cpu->socket_cpu, &msg, sizeof(t_codigo_mensaje), 0);
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);

    return NULL;
}

// Envio efectivo de un PID a una CPU. La secuencia de locks evita tanto el
// despacho durante una compactacion como la intercalacion de escrituras en el
// socket de la CPU:
//   1) actualizar contexto en KM (sin locks propios tomados)
//   2) mutex_compactacion: esperar que no haya compactacion en curso
//   3) mutex_cpus: marcar la CPU ocupada y enviar el PID
static void despachar(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu) {
    planificador_actualizar_contexto_en_km(pl, pcb->pid, pcb->registros);

    pthread_mutex_lock(&pl->mutex_compactacion);
    while (pl->compactacion_en_curso) {
        pthread_cond_wait(&pl->cond_compactacion, &pl->mutex_compactacion);
    }

    pthread_mutex_lock(&pl->mutex_cpus);
    cpu->libre = false;
    cpu->pid_ejecutando = pcb->pid;
    cpu->en_rafaga = true;
    cpu->rafaga++;
    cpu->motivo_interrupcion = INT_NINGUNA;
    uint32_t rafaga = cpu->rafaga;
    int id_cpu = cpu->id_cpu;

    t_codigo_mensaje cod = MENSAJE_ENVIAR_PID;
    send(cpu->socket_cpu, &cod, sizeof(t_codigo_mensaje), 0);
    send(cpu->socket_cpu, &(pcb->pid), sizeof(uint32_t), 0);
    pthread_mutex_unlock(&pl->mutex_cpus);
    pthread_mutex_unlock(&pl->mutex_compactacion);

    pcb->cpu_asignada = id_cpu;

    // Quantum: aplica el algoritmo de la cola a la que pertenece el proceso
    if (cola_usa_rr(pl, cola_de(pl, pcb))) {
        t_timer_args* t_args = malloc(sizeof(t_timer_args));
        t_args->pl = pl;
        t_args->pid = pcb->pid;
        t_args->id_cpu = id_cpu;
        t_args->rafaga = rafaga;

        // Detached: el hilo libera sus recursos al terminar, nadie lo joinea.
        // Si no se pudo crear hay que liberar los args a mano (si no, se pierden)
        // y el proceso queda sin timer: se avisa en vez de fallar en silencio.
        pthread_t t_hilo;
        if (pthread_create(&t_hilo, NULL, hilo_timer_quantum, t_args) != 0) {
            log_error(pl->logger, "No se pudo crear el hilo de quantum para PID %u en CPU %d",
                      pcb->pid, id_cpu);
            free(t_args);
        } else {
            pthread_detach(t_hilo);
        }
    }
}

void planificador_despachar_directo(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu) {
    despachar(pl, pcb, cpu);
}

// Hilo de Corto Plazo (READY -> EXEC)
void* hilo_planificador_corto_plazo(void* arg) {
    t_planificador* pl = (t_planificador*) arg;
    log_info(pl->logger, "Hilo de Corto Plazo iniciado.");

    while (1) {
        sem_wait(&pl->sem_procesos_ready);
        sem_wait(&pl->sem_cpus_libres);

        t_pcb* pcb = desencolar_ready(pl);
        if (pcb == NULL) {
            sem_post(&pl->sem_cpus_libres);
            continue;
        }

        pthread_mutex_lock(&pl->mutex_cpus);
        t_cpu_conectada* cpu_libre = NULL;
        for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
            t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
            if (cpu->libre) {
                cpu_libre = cpu;
                break;
            }
        }
        pthread_mutex_unlock(&pl->mutex_cpus);

        if (cpu_libre == NULL) {
            // sem_cpus_libres deberia garantizar que hay una; por seguridad reencolar
            encolar_en_ready(pl, pcb, true);
            sem_post(&pl->sem_cpus_libres);
            continue;
        }

        // Transición a EXEC
        t_estado_proceso anterior = pcb->estado;
        pcb->estado = ESTADO_EXEC;
        clock_gettime(CLOCK_MONOTONIC, &(pcb->ts_inicio_exec));

        pthread_mutex_lock(&pl->mutex_lista_exec);
        list_add(pl->lista_exec, pcb);
        pthread_mutex_unlock(&pl->mutex_lista_exec);

        log_info(pl->logger, "## (%u) Pasa del estado %s al estado EXEC", pcb->pid, estado_to_string(anterior));

        despachar(pl, pcb, cpu_libre);
    }
    return NULL;
}

// ============================================================
// Métodos de transiciones seguras
// ============================================================

void planificador_encolar_new(t_planificador* pl, t_pcb* pcb) {
    pthread_mutex_lock(&pl->mutex_cola_new);
    queue_push(pl->cola_new, pcb);
    pthread_mutex_unlock(&pl->mutex_cola_new);

    log_info(pl->logger, "## (%u) Se crea el proceso - Estado: NEW", pcb->pid);
    sem_post(&pl->sem_procesos_new);
}

void planificador_transicionar_new_a_ready(t_planificador* pl, t_pcb* pcb) {
    t_estado_proceso anterior = pcb->estado;
    pcb->estado = ESTADO_READY;
    clock_gettime(CLOCK_MONOTONIC, &(pcb->ts_llegada_ready));

    log_info(pl->logger, "## (%u) Pasa del estado %s al estado READY", pcb->pid, estado_to_string(anterior));
    encolar_en_ready(pl, pcb, false);
}

// Helper para sacar PCB de lista_exec
static t_pcb* remover_de_exec(t_planificador* pl, uint32_t pid) {
    pthread_mutex_lock(&pl->mutex_lista_exec);
    t_pcb* encontrado = NULL;
    for (int i = 0; i < list_size(pl->lista_exec); i++) {
        t_pcb* p = list_get(pl->lista_exec, i);
        if (p->pid == pid) {
            encontrado = list_remove(pl->lista_exec, i);
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_lista_exec);
    return encontrado;
}

void planificador_transicionar_exec_a_exit(t_planificador* pl, t_pcb* pcb, const char* motivo) {
    t_pcb* real_pcb = remover_de_exec(pl, pcb->pid);
    if (real_pcb == NULL) real_pcb = pcb; // Fallback

    t_estado_proceso anterior = real_pcb->estado;
    real_pcb->estado = ESTADO_EXIT;

    pthread_mutex_lock(&pl->mutex_lista_exit);
    list_add(pl->lista_exit, real_pcb);
    pthread_mutex_unlock(&pl->mutex_lista_exit);

    log_info(pl->logger, "## (%u) Pasa del estado %s al estado EXIT", real_pcb->pid, estado_to_string(anterior));
    log_info(pl->logger, "## (%u) finalizó su ejecución con motivo de %s", real_pcb->pid, motivo);
}

static void exec_a_ready(t_planificador* pl, t_pcb* pcb, bool al_frente) {
    t_pcb* real_pcb = remover_de_exec(pl, pcb->pid);
    if (real_pcb == NULL) real_pcb = pcb;

    t_estado_proceso anterior = real_pcb->estado;
    real_pcb->estado = ESTADO_READY;
    real_pcb->cpu_asignada = -1;
    clock_gettime(CLOCK_MONOTONIC, &(real_pcb->ts_llegada_ready));

    log_info(pl->logger, "## (%u) Pasa del estado %s al estado READY", real_pcb->pid, estado_to_string(anterior));
    encolar_en_ready(pl, real_pcb, al_frente);
}

void planificador_transicionar_exec_a_ready(t_planificador* pl, t_pcb* pcb) {
    exec_a_ready(pl, pcb, false);
}

void planificador_transicionar_exec_a_ready_frente(t_planificador* pl, t_pcb* pcb) {
    exec_a_ready(pl, pcb, true);
}

// ============================================================
// Mediano plazo: suspension por timeout de BLOCK
// ============================================================

typedef struct {
    t_planificador* pl;
    uint32_t pid;
    uint32_t bloqueo_id;    // invalida el timer si el proceso se desbloqueo en el medio
} t_timer_suspension_args;

static void* hilo_timer_suspension(void* arg) {
    t_timer_suspension_args* args = (t_timer_suspension_args*) arg;
    t_planificador* pl = args->pl;
    uint32_t pid = args->pid;
    uint32_t bloqueo_id = args->bloqueo_id;
    free(args);

    usleep(pl->config->suspension_timeout * 1000);

    // Si el proceso sigue en BLOCK con el MISMO bloqueo, se suspende.
    // La transicion se hace atomica (ambas colas bajo lock, orden block->susp):
    // si no, un fin de IO concurrente podria no encontrar el PCB en ninguna cola.
    pthread_mutex_lock(&pl->mutex_cola_block);
    t_pcb* encontrado = NULL;
    int size = queue_size(pl->cola_block);
    for (int i = 0; i < size; i++) {
        t_pcb* p = queue_pop(pl->cola_block);
        if (p->pid == pid && p->bloqueo_id == bloqueo_id) {
            encontrado = p;
        } else {
            queue_push(pl->cola_block, p);
        }
    }

    if (encontrado == NULL) {
        pthread_mutex_unlock(&pl->mutex_cola_block);
        return NULL;
    }

    t_estado_proceso anterior = encontrado->estado;
    encontrado->estado = ESTADO_SUSP_BLOCK;
    clock_gettime(CLOCK_MONOTONIC, &(encontrado->ts_suspension));

    pthread_mutex_lock(&pl->mutex_cola_susp);
    queue_push(pl->cola_susp_block, encontrado);
    pthread_mutex_unlock(&pl->mutex_cola_susp);
    pthread_mutex_unlock(&pl->mutex_cola_block);

    log_info(pl->logger, "## (%u) Pasa del estado %s al estado %s",
             encontrado->pid, estado_to_string(anterior), estado_to_string(ESTADO_SUSP_BLOCK));

    // Mover sus segmentos a SWAP (libera memoria principal)
    if (pl->socket_memoria != -1) {
        pthread_mutex_lock(&pl->mutex_socket_memoria);
        t_codigo_mensaje cod = MENSAJE_SUSPENDER_PROCESO;
        send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
        send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
        t_codigo_mensaje respuesta;
        recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);
        pthread_mutex_unlock(&pl->mutex_socket_memoria);

        if (respuesta != MENSAJE_OK) {
            log_warning(pl->logger, "## (%u) Kernel Memory no pudo mover el proceso a SWAP", pid);
        }
    }

    // La suspension libero memoria: intentar des-suspender / reintentar allocs
    planificador_evento_memoria_liberada(pl);
    return NULL;
}

void planificador_transicionar_exec_a_block(t_planificador* pl, t_pcb* pcb, const char* motivo_bloqueo) {
    t_pcb* real_pcb = remover_de_exec(pl, pcb->pid);
    if (real_pcb == NULL) real_pcb = pcb;

    t_estado_proceso anterior = real_pcb->estado;
    real_pcb->estado = ESTADO_BLOCK;
    real_pcb->cpu_asignada = -1;
    real_pcb->bloqueo_id++;
    if (real_pcb->motivo_bloqueo) free(real_pcb->motivo_bloqueo);
    real_pcb->motivo_bloqueo = strdup(motivo_bloqueo);
    clock_gettime(CLOCK_MONOTONIC, &(real_pcb->ts_inicio_block));

    pthread_mutex_lock(&pl->mutex_cola_block);
    queue_push(pl->cola_block, real_pcb);
    pthread_mutex_unlock(&pl->mutex_cola_block);

    log_info(pl->logger, "## (%u) Pasa del estado %s al estado BLOCK", real_pcb->pid, estado_to_string(anterior));

    // Timer de suspension: si sigue en BLOCK al vencer SUSPENSION_TIMEOUT,
    // pasa a SUSP. BLOCK. Los bloqueados esperando memoria (MEM_ALLOC sin
    // espacio) no se suspenden: su pedido pendiente lo resuelve el gestor
    // de memoria y swapearlos generaria un estado inconsistente.
    if (strcmp(motivo_bloqueo, "MEMORIA") != 0) {
        t_timer_suspension_args* args = malloc(sizeof(t_timer_suspension_args));
        args->pl = pl;
        args->pid = real_pcb->pid;
        args->bloqueo_id = real_pcb->bloqueo_id;

        pthread_t hilo;
        pthread_create(&hilo, NULL, hilo_timer_suspension, args);
        pthread_detach(hilo);
    }
}

// Pasa el PCB de BLOCK a READY sin encolarlo ni loguear: devuelve el PCB real
// (o NULL si no estaba en cola_block) y deja en *anterior el estado previo, para
// que el caller decida cuando loguear y cuando señalizar al corto plazo.
static t_pcb* block_a_ready_sin_encolar(t_planificador* pl, uint32_t pid, t_estado_proceso* anterior) {
    pthread_mutex_lock(&pl->mutex_cola_block);
    t_pcb* encontrado = sacar_de_cola_sin_lock(pl->cola_block, pid);
    pthread_mutex_unlock(&pl->mutex_cola_block);

    if (encontrado == NULL) return NULL;

    *anterior = encontrado->estado;
    encontrado->estado = ESTADO_READY;
    encontrado->bloqueo_id++;   // invalida el timer de suspension pendiente
    if (encontrado->motivo_bloqueo) {
        free(encontrado->motivo_bloqueo);
        encontrado->motivo_bloqueo = NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &(encontrado->ts_llegada_ready));

    return encontrado;
}

t_pcb* planificador_transicionar_block_a_ready(t_planificador* pl, t_pcb* pcb) {
    t_estado_proceso anterior;
    t_pcb* encontrado = block_a_ready_sin_encolar(pl, pcb->pid, &anterior);

    if (encontrado != NULL) {
        log_info(pl->logger, "## (%u) Pasa del estado %s al estado READY", encontrado->pid, estado_to_string(anterior));
        encolar_en_ready(pl, encontrado, false);
    }

    return encontrado;
}

// Pasa el PCB de SUSP. BLOCK a SUSP. READY (la memoria sigue en SWAP) sin
// loguear. Devuelve NULL si no estaba suspendido.
static t_pcb* susp_block_a_susp_ready_sin_loguear(t_planificador* pl, uint32_t pid, t_estado_proceso* anterior) {
    pthread_mutex_lock(&pl->mutex_cola_susp);
    t_pcb* encontrado = sacar_de_cola_sin_lock(pl->cola_susp_block, pid);
    if (encontrado != NULL) {
        *anterior = encontrado->estado;
        encontrado->estado = ESTADO_SUSP_READY;
        if (encontrado->motivo_bloqueo) {
            free(encontrado->motivo_bloqueo);
            encontrado->motivo_bloqueo = NULL;
        }
        queue_push(pl->cola_susp_ready, encontrado);
    }
    pthread_mutex_unlock(&pl->mutex_cola_susp);
    return encontrado;
}

t_pcb* planificador_desbloquear(t_planificador* pl, uint32_t pid, bool* estaba_suspendido) {
    *estaba_suspendido = false;

    // Caso normal: estaba en BLOCK -> READY
    t_pcb buscado = {0};
    buscado.pid = pid;
    t_pcb* despertado = planificador_transicionar_block_a_ready(pl, &buscado);
    if (despertado != NULL) return despertado;

    // Estaba suspendido: SUSP. BLOCK -> SUSP. READY (la memoria sigue en SWAP)
    t_estado_proceso anterior;
    t_pcb* encontrado = susp_block_a_susp_ready_sin_loguear(pl, pid, &anterior);
    if (encontrado != NULL) {
        log_info(pl->logger, "## (%u) Pasa del estado %s al estado %s",
                 encontrado->pid, estado_to_string(anterior), estado_to_string(ESTADO_SUSP_READY));
        *estaba_suspendido = true;
    }

    return encontrado;
}

// Desbloqueo por fin de IO. La transicion y los DOS logs obligatorios que le
// corresponden ("Pasa del estado BLOCK al estado READY" y "finalizó IO y pasa a
// READY") se emiten bajo el mismo lock, y al corto plazo se lo señaliza recien
// despues de haber logueado. Sin eso, el "Pasa del estado READY al estado EXEC"
// del corto plazo se intercala entre los dos y queda un proceso en EXEC antes de
// que termine su IO; y dos fines de IO simultaneos mezclan sus pares de logs.
static t_pcb* desbloquear_por_fin_io(t_planificador* pl, uint32_t pid, bool* estaba_suspendido) {
    *estaba_suspendido = false;

    pthread_mutex_lock(&pl->mutex_fin_io);

    t_estado_proceso anterior;
    t_pcb* pcb = block_a_ready_sin_encolar(pl, pid, &anterior);

    if (pcb != NULL) {
        log_info(pl->logger, "## (%u) Pasa del estado %s al estado READY", pid, estado_to_string(anterior));
        log_info(pl->logger, "## (%u) finalizó IO y pasa a READY", pid);
        // Recien ahora el proceso queda visible para el corto plazo.
        insertar_en_ready(pl, pcb, false);
        pthread_mutex_unlock(&pl->mutex_fin_io);

        habilitar_corto_plazo(pl, pcb, false);
        return pcb;
    }

    // Se suspendio mientras esperaba la IO: SUSP. BLOCK -> SUSP. READY.
    // No entra a READY, asi que no hay nada que señalizarle al corto plazo.
    pcb = susp_block_a_susp_ready_sin_loguear(pl, pid, &anterior);
    if (pcb != NULL) {
        log_info(pl->logger, "## (%u) Pasa del estado %s al estado %s",
                 pid, estado_to_string(anterior), estado_to_string(ESTADO_SUSP_READY));
        log_info(pl->logger, "## (%u) finalizó IO y pasa a SUSP. READY", pid);
        *estaba_suspendido = true;
    }

    pthread_mutex_unlock(&pl->mutex_fin_io);
    return pcb;
}

// ============================================================
// Espera de memoria (MEM_ALLOC sin espacio) y des-suspensiones
// ============================================================

typedef struct {
    t_pcb* pcb;
    uint32_t id_segmento;
    uint32_t tamanio;
} t_espera_memoria;

void planificador_bloquear_por_memoria(t_planificador* pl, t_pcb* pcb, uint32_t id_segmento, uint32_t tamanio) {
    t_espera_memoria* espera = malloc(sizeof(t_espera_memoria));
    espera->pcb = pcb;
    espera->id_segmento = id_segmento;
    espera->tamanio = tamanio;

    pthread_mutex_lock(&pl->mutex_espera_memoria);
    list_add(pl->lista_espera_memoria, espera);
    pthread_mutex_unlock(&pl->mutex_espera_memoria);

    log_info(pl->logger, "## (%u) Bloqueado esperando memoria para el segmento %u (%u bytes)",
             pcb->pid, id_segmento, tamanio);
}

// Candidato a des-suspension, para ordenar por prioridad y antiguedad
typedef struct {
    uint32_t pid;
    int prioridad;
    struct timespec ts_suspension;
} t_candidato_dessusp;

static int comparar_candidatos(const void* a, const void* b) {
    const t_candidato_dessusp* ca = (const t_candidato_dessusp*) a;
    const t_candidato_dessusp* cb = (const t_candidato_dessusp*) b;
    if (ca->prioridad != cb->prioridad) return ca->prioridad - cb->prioridad;
    // Misma prioridad: primero el que lleva mas tiempo suspendido
    if (ca->ts_suspension.tv_sec != cb->ts_suspension.tv_sec)
        return ca->ts_suspension.tv_sec < cb->ts_suspension.tv_sec ? -1 : 1;
    return ca->ts_suspension.tv_nsec < cb->ts_suspension.tv_nsec ? -1 : 1;
}

static bool dessuspender_en_km(t_planificador* pl, uint32_t pid) {
    if (pl->socket_memoria == -1) return false;

    pthread_mutex_lock(&pl->mutex_socket_memoria);
    t_codigo_mensaje cod = MENSAJE_DESSUSPENDER_PROCESO;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    t_codigo_mensaje respuesta;
    ssize_t bytes = recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);
    pthread_mutex_unlock(&pl->mutex_socket_memoria);

    return bytes > 0 && respuesta == MENSAJE_OK;
}

void planificador_evento_memoria_liberada(t_planificador* pl) {
    // Serializa el recorrido: si dos hilos liberan memoria a la vez, el
    // segundo espera y vuelve a evaluar sobre el estado ya actualizado.
    pthread_mutex_lock(&pl->mutex_evento_memoria);

    // 1) Des-suspender por orden de prioridad (y mayor tiempo suspendido),
    //    solo los que puedan re-crear TODOS sus segmentos sin compactar.
    pthread_mutex_lock(&pl->mutex_cola_susp);
    int cant = queue_size(pl->cola_susp_ready);
    t_candidato_dessusp* candidatos = malloc(sizeof(t_candidato_dessusp) * (cant > 0 ? cant : 1));
    for (int i = 0; i < cant; i++) {
        t_pcb* p = list_get(pl->cola_susp_ready->elements, i);
        candidatos[i].pid = p->pid;
        candidatos[i].prioridad = p->prioridad_actual;
        candidatos[i].ts_suspension = p->ts_suspension;
    }
    pthread_mutex_unlock(&pl->mutex_cola_susp);

    qsort(candidatos, cant, sizeof(t_candidato_dessusp), comparar_candidatos);

    for (int i = 0; i < cant; i++) {
        if (!dessuspender_en_km(pl, candidatos[i].pid)) continue;

        // KM restauro sus segmentos: SUSP. READY -> READY
        pthread_mutex_lock(&pl->mutex_cola_susp);
        t_pcb* pcb = NULL;
        int size = queue_size(pl->cola_susp_ready);
        for (int j = 0; j < size; j++) {
            t_pcb* p = queue_pop(pl->cola_susp_ready);
            if (p->pid == candidatos[i].pid) {
                pcb = p;
            } else {
                queue_push(pl->cola_susp_ready, p);
            }
        }
        pthread_mutex_unlock(&pl->mutex_cola_susp);

        if (pcb != NULL) {
            t_estado_proceso anterior = pcb->estado;
            pcb->estado = ESTADO_READY;
            clock_gettime(CLOCK_MONOTONIC, &(pcb->ts_llegada_ready));
            log_info(pl->logger, "## (%u) Pasa del estado %s al estado READY",
                     pcb->pid, estado_to_string(anterior));
            encolar_en_ready(pl, pcb, false);
        }
    }
    free(candidatos);

    // 2) Reintentar los MEM_ALLOC que quedaron esperando memoria (en orden
    //    de llegada; los que siguen sin entrar permanecen esperando).
    pthread_mutex_lock(&pl->mutex_espera_memoria);
    t_list* snapshot = list_duplicate(pl->lista_espera_memoria);
    pthread_mutex_unlock(&pl->mutex_espera_memoria);

    for (int i = 0; i < list_size(snapshot); i++) {
        t_espera_memoria* espera = list_get(snapshot, i);

        if (planificador_crear_segmento_km(pl, espera->pcb->pid, espera->id_segmento, espera->tamanio, NULL)) {
            pthread_mutex_lock(&pl->mutex_espera_memoria);
            list_remove_element(pl->lista_espera_memoria, espera);
            pthread_mutex_unlock(&pl->mutex_espera_memoria);

            log_info(pl->logger, "## (%u) Se satisfizo el MEM_ALLOC pendiente del segmento %u",
                     espera->pcb->pid, espera->id_segmento);

            bool suspendido;
            planificador_desbloquear(pl, espera->pcb->pid, &suspendido);
            free(espera);
        }
    }
    list_destroy(snapshot);

    pthread_mutex_unlock(&pl->mutex_evento_memoria);
}

// ============================================================
// Registrar y remover CPUs
// ============================================================

void planificador_registrar_cpu(t_planificador* pl, int socket_cpu, int id_cpu) {
    t_cpu_conectada* cpu = malloc(sizeof(t_cpu_conectada));
    cpu->id_cpu = id_cpu;
    cpu->socket_cpu = socket_cpu;
    cpu->libre = true;
    cpu->pid_ejecutando = 0;
    cpu->en_rafaga = false;
    cpu->rafaga = 0;
    cpu->motivo_interrupcion = INT_NINGUNA;
    cpu->pid_preemptor = 0;
    cpu->prioridad_preemptor = 0;
    cpu->prioridad_victima = 0;

    pthread_mutex_lock(&pl->mutex_cpus);
    list_add(pl->cpus_conectadas, cpu);
    pthread_mutex_unlock(&pl->mutex_cpus);

    log_info(pl->logger, "## CPU %d Conectada", id_cpu);
    sem_post(&pl->sem_cpus_libres);
}

void planificador_remover_cpu(t_planificador* pl, int socket_cpu) {
    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (cpu->socket_cpu == socket_cpu) {
            list_remove(pl->cpus_conectadas, i);
            log_info(pl->logger, "## CPU %d Desconectada", cpu->id_cpu);
            if (!cpu->libre) {
                // Si tenía un proceso ejecutando, recuperarlo y reencolarlo
                t_pcb* pcb = remover_de_exec(pl, cpu->pid_ejecutando);
                if (pcb != NULL) {
                    planificador_transicionar_exec_a_ready(pl, pcb);
                }
            } else {
                // Si estaba libre, decrementar el semáforo de cpus libres
                sem_trywait(&pl->sem_cpus_libres);
            }
            free(cpu);
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);
}

t_cpu_conectada* planificador_obtener_cpu_por_socket(t_planificador* pl, int socket_cpu) {
    t_cpu_conectada* encontrado = NULL;
    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (cpu->socket_cpu == socket_cpu) {
            encontrado = cpu;
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);
    return encontrado;
}

t_cpu_conectada* planificador_obtener_cpu_por_id(t_planificador* pl, int id_cpu) {
    t_cpu_conectada* encontrado = NULL;
    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (cpu->id_cpu == id_cpu) {
            encontrado = cpu;
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);
    return encontrado;
}

// ============================================================
// Sincronizacion de contexto con Kernel Memory
// ============================================================

void planificador_actualizar_contexto_en_km(t_planificador* pl, uint32_t pid, t_registros_cpu registros) {
    if (pl->socket_memoria == -1) return;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_ACTUALIZAR_CONTEXTO;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &registros, sizeof(t_registros_cpu), 0);

    t_codigo_mensaje respuesta;
    recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
}

static char* planificador_leer_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio) {
    if (pl->socket_memoria == -1) return NULL;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_LEER_MEMORIA;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &dir_logica, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &tamanio, sizeof(uint32_t), 0);

    t_codigo_mensaje respuesta;
    if (recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0 || respuesta != MENSAJE_OK) {
        pthread_mutex_unlock(&pl->mutex_socket_memoria);
        return NULL;
    }

    // malloc(0) puede devolver NULL segun la libc, lo cual el caller interpretaria
    // como fallo de lectura; se reserva al menos 1 byte para evitar esa ambiguedad.
    char* buffer = malloc(tamanio > 0 ? tamanio : 1);
    if (tamanio > 0) {
        recv(pl->socket_memoria, buffer, tamanio, MSG_WAITALL);
    }

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
    return buffer;
}

static bool planificador_escribir_memoria_en_km(t_planificador* pl, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, void* contenido) {
    if (pl->socket_memoria == -1) return false;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_ESCRIBIR_MEMORIA;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &dir_logica, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &tamanio, sizeof(uint32_t), 0);
    send(pl->socket_memoria, contenido, tamanio, 0);

    t_codigo_mensaje respuesta;
    ssize_t bytes = recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
    return bytes > 0 && respuesta == MENSAJE_OK;
}

// ============================================================
// Syscalls de memoria (CREAR/ELIMINAR SEGMENTO, FINALIZAR PROCESO)
// ============================================================

static void pausar_planificacion(t_planificador* pl) {
    pthread_mutex_lock(&pl->mutex_compactacion);
    pl->compactacion_en_curso = true;
    pthread_mutex_unlock(&pl->mutex_compactacion);
}

static void reanudar_planificacion(t_planificador* pl) {
    pthread_mutex_lock(&pl->mutex_compactacion);
    pl->compactacion_en_curso = false;
    pthread_cond_broadcast(&pl->cond_compactacion);
    pthread_mutex_unlock(&pl->mutex_compactacion);
}

// Interrumpe todas las CPUs que estan ejecutando instrucciones y devuelve
// cuantas siguen en rafaga. Idempotente: no reinterrumpe las ya avisadas.
static int interrumpir_cpus_para_compactar(t_planificador* pl) {
    int en_rafaga = 0;
    pthread_mutex_lock(&pl->mutex_cpus);
    for (int i = 0; i < list_size(pl->cpus_conectadas); i++) {
        t_cpu_conectada* cpu = list_get(pl->cpus_conectadas, i);
        if (!cpu->libre && cpu->en_rafaga) {
            en_rafaga++;
            if (cpu->motivo_interrupcion != INT_COMPACTACION) {
                cpu->motivo_interrupcion = INT_COMPACTACION;
                t_codigo_mensaje msg = MENSAJE_INTERRUPCION;
                send(cpu->socket_cpu, &msg, sizeof(t_codigo_mensaje), 0);
            }
        }
    }
    pthread_mutex_unlock(&pl->mutex_cpus);
    return en_rafaga;
}

bool planificador_crear_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento, uint32_t tamanio, bool* hubo_compactacion) {
    if (hubo_compactacion != NULL) *hubo_compactacion = false;
    if (pl->socket_memoria == -1) return false;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_CREAR_SEGMENTO;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &id_segmento, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &tamanio, sizeof(uint32_t), 0);

    t_codigo_mensaje respuesta;
    if (recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0) {
        pthread_mutex_unlock(&pl->mutex_socket_memoria);
        return false;
    }

    if (respuesta == MENSAJE_COMPACTACION) {
        if (hubo_compactacion != NULL) *hubo_compactacion = true;

        // Los logs de inicio/fin encierran toda la ventana en que la
        // planificacion esta pausada por la compactacion: entre los dos no se
        // despacha ningun proceso.
        log_info(pl->logger, "## Inicio de compactación");
        log_debug(pl->logger, "Kernel Memory pide compactar: desalojando todas las CPUs...");

        // 1) No despachar nada mas hasta que termine la compactacion
        pausar_planificacion(pl);

        // 2) Interrumpir a todas las CPUs en rafaga y esperar que devuelvan
        //    sus procesos. Las CPUs cuyo proceso esta "estacionado" en una
        //    syscall no ejecutan instrucciones y no hace falta esperarlas.
        while (interrumpir_cpus_para_compactar(pl) > 0) {
            usleep(5 * 1000);
        }

        // 3) Confirmar el desalojo y esperar el resultado final
        t_codigo_mensaje ok_desalojo = MENSAJE_DESALOJO_OK;
        send(pl->socket_memoria, &ok_desalojo, sizeof(t_codigo_mensaje), 0);

        if (recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0) {
            respuesta = MENSAJE_ERROR;
        }

        // Kernel Memory ya compacto y reintento el pedido: se cierra la ventana
        // antes de reanudar, asi ningun despacho se cuela entre inicio y fin.
        log_info(pl->logger, "## Fin de compactación");

        // 4) Replanificar normalmente
        reanudar_planificacion(pl);
    }

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
    return respuesta == MENSAJE_OK;
}

bool planificador_eliminar_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento) {
    if (pl->socket_memoria == -1) return false;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_ELIMINAR_SEGMENTO;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);
    send(pl->socket_memoria, &id_segmento, sizeof(uint32_t), 0);

    t_codigo_mensaje respuesta;
    ssize_t bytes = recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
    return bytes > 0 && respuesta == MENSAJE_OK;
}

void planificador_finalizar_proceso_km(t_planificador* pl, uint32_t pid) {
    if (pl->socket_memoria == -1) return;

    pthread_mutex_lock(&pl->mutex_socket_memoria);

    t_codigo_mensaje cod = MENSAJE_FINALIZAR_PROCESO;
    send(pl->socket_memoria, &cod, sizeof(t_codigo_mensaje), 0);
    send(pl->socket_memoria, &pid, sizeof(uint32_t), 0);

    t_codigo_mensaje respuesta;
    recv(pl->socket_memoria, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);

    pthread_mutex_unlock(&pl->mutex_socket_memoria);
}

// ============================================================
// Blue Screen of Death
// ============================================================

static void bsod_finalizar_lista(t_planificador* pl, t_list* lista) {
    for (int i = 0; i < list_size(lista); i++) {
        t_pcb* pcb = list_get(lista, i);
        log_info(pl->logger, "## (%u) finalizó su ejecución con motivo de BSOD", pcb->pid);
    }
}

static void bsod_finalizar_cola(t_planificador* pl, t_queue* cola) {
    bsod_finalizar_lista(pl, cola->elements);
}

void planificador_bsod(t_planificador* pl) {
    log_error(pl->logger, "Se detectó memoria corrupta: iniciando Blue Screen of Death (BSOD)");

    pausar_planificacion(pl);

    pthread_mutex_lock(&pl->mutex_cola_new);
    bsod_finalizar_cola(pl, pl->cola_new);
    pthread_mutex_unlock(&pl->mutex_cola_new);

    pthread_mutex_lock(&pl->mutex_cola_ready);
    for (int q = 0; q < pl->cant_colas; q++) {
        bsod_finalizar_lista(pl, pl->colas_ready[q]);
    }
    pthread_mutex_unlock(&pl->mutex_cola_ready);

    pthread_mutex_lock(&pl->mutex_lista_exec);
    bsod_finalizar_lista(pl, pl->lista_exec);
    pthread_mutex_unlock(&pl->mutex_lista_exec);

    pthread_mutex_lock(&pl->mutex_cola_block);
    bsod_finalizar_cola(pl, pl->cola_block);
    pthread_mutex_unlock(&pl->mutex_cola_block);

    pthread_mutex_lock(&pl->mutex_cola_susp);
    bsod_finalizar_cola(pl, pl->cola_susp_block);
    bsod_finalizar_cola(pl, pl->cola_susp_ready);
    pthread_mutex_unlock(&pl->mutex_cola_susp);

    log_error(pl->logger, "Finalizando el Kernel Scheduler con motivo de Blue Screen of Death (BSOD)");
    log_destroy(pl->logger);
    exit(EXIT_FAILURE);
}

// ============================================================
// Dispositivos IO (STDIN / STDOUT / SLEEP)
// ============================================================

static sem_t* sem_de_tipo_io(t_planificador* pl, t_tipo_io tipo) {
    switch (tipo) {
        case IO_STDIN:  return &pl->sem_io_stdin_libres;
        case IO_STDOUT: return &pl->sem_io_stdout_libres;
        default:        return &pl->sem_io_sleep_libres;
    }
}

static const char* nombre_tipo_io(t_tipo_io tipo) {
    switch (tipo) {
        case IO_STDIN:  return "STDIN";
        case IO_STDOUT: return "STDOUT";
        default:        return "SLEEP";
    }
}

void planificador_registrar_io(t_planificador* pl, int fd, t_tipo_io tipo, char* nombre) {
    t_io_conectada* io = malloc(sizeof(t_io_conectada));
    io->fd = fd;
    io->tipo = tipo;
    io->nombre = strdup(nombre);
    io->libre = true;
    io->desconectado = false;

    pthread_mutex_lock(&pl->mutex_ios);
    list_add(pl->ios_conectados, io);
    pthread_mutex_unlock(&pl->mutex_ios);

    log_info(pl->logger, "## IO %s Conectada", nombre);
    sem_post(sem_de_tipo_io(pl, tipo));
}

void planificador_remover_io(t_planificador* pl, int fd) {
    pthread_mutex_lock(&pl->mutex_ios);
    for (int i = 0; i < list_size(pl->ios_conectados); i++) {
        t_io_conectada* io = list_get(pl->ios_conectados, i);
        if (io->fd == fd) {
            log_info(pl->logger, "## IO %s Desconectada", io->nombre);
            if (io->libre) {
                // Nadie la esta usando: se puede remover y liberar ya mismo.
                list_remove(pl->ios_conectados, i);
                sem_trywait(sem_de_tipo_io(pl, io->tipo));
                io_conectada_destroy(io);
            } else {
                // Esta en uso por un hilo de planificador_ejecutar_io_async: se marca
                // para que ese hilo la remueva/libere al terminar, y no se toca el
                // semaforo (sigue "ocupada" hasta que el usuario actual termine).
                io->desconectado = true;
            }
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_ios);
}

t_io_conectada* planificador_tomar_io_libre(t_planificador* pl, t_tipo_io tipo) {
    sem_wait(sem_de_tipo_io(pl, tipo));

    pthread_mutex_lock(&pl->mutex_ios);
    t_io_conectada* encontrado = NULL;
    for (int i = 0; i < list_size(pl->ios_conectados); i++) {
        t_io_conectada* io = list_get(pl->ios_conectados, i);
        if (io->tipo == tipo && io->libre && !io->desconectado) {
            io->libre = false;
            encontrado = io;
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_ios);
    return encontrado;
}

void planificador_liberar_io(t_planificador* pl, t_io_conectada* io) {
    pthread_mutex_lock(&pl->mutex_ios);
    if (io->desconectado) {
        // Se desconecto mientras estaba en uso: ahora si se remueve del todo.
        list_remove_element(pl->ios_conectados, io);
        pthread_mutex_unlock(&pl->mutex_ios);
        io_conectada_destroy(io);
        return;
    }
    io->libre = true;
    pthread_mutex_unlock(&pl->mutex_ios);
    sem_post(sem_de_tipo_io(pl, io->tipo));
}

typedef struct {
    t_planificador* pl;
    t_pcb* pcb;
    t_tipo_io tipo;
    uint32_t param;     // tiempo_ms (SLEEP) o direccion logica (STDIN/STDOUT)
    uint32_t tamanio;   // bytes a leer/escribir (STDIN/STDOUT), 0 para SLEEP
} t_io_async_args;

static bool io_enviar_pid_y_param(int fd, uint32_t pid, uint32_t param) {
    if (send(fd, &pid, sizeof(uint32_t), 0) <= 0) return false;
    if (send(fd, &param, sizeof(uint32_t), 0) <= 0) return false;
    return true;
}

static bool io_esperar_ok(int fd) {
    t_codigo_mensaje respuesta;
    ssize_t bytes = recv(fd, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL);
    return bytes > 0 && respuesta == MENSAJE_OK;
}

static void* hilo_ejecutar_io(void* arg) {
    t_io_async_args* a = (t_io_async_args*) arg;
    t_planificador* pl = a->pl;
    t_pcb* pcb = a->pcb;

    t_io_conectada* io = planificador_tomar_io_libre(pl, a->tipo);

    bool ok;
    switch (a->tipo) {
        case IO_STDOUT: {
            // La direccion es LOGICA: la traduce Kernel Memory al momento de leer,
            // asi sigue siendo valida aunque haya habido compactacion en el medio.
            char* contenido = planificador_leer_memoria_en_km(pl, pcb->pid, a->param, a->tamanio);
            ok = contenido != NULL
                 && io_enviar_pid_y_param(io->fd, pcb->pid, a->tamanio)
                 && send(io->fd, contenido, a->tamanio, 0) > 0
                 && io_esperar_ok(io->fd);
            if (contenido) free(contenido);
            break;
        }
        case IO_STDIN: {
            char* buffer = malloc(a->tamanio);
            ok = io_enviar_pid_y_param(io->fd, pcb->pid, a->tamanio)
                 && recv(io->fd, buffer, a->tamanio, MSG_WAITALL) > 0;
            if (ok) {
                ok = planificador_escribir_memoria_en_km(pl, pcb->pid, a->param, a->tamanio, buffer);
            }
            free(buffer);
            break;
        }
        default: // IO_SLEEP
            ok = io_enviar_pid_y_param(io->fd, pcb->pid, a->param) && io_esperar_ok(io->fd);
            break;
    }

    if (!ok) {
        log_error(pl->logger, "## (%u) - Error en IO %s, se remueve el dispositivo", pcb->pid, nombre_tipo_io(a->tipo));
        planificador_remover_io(pl, io->fd);
    } else {
        planificador_liberar_io(pl, io);
    }

    // El proceso pudo haberse suspendido mientras esperaba la IO: en ese caso
    // pasa a SUSP. READY (su memoria sigue en SWAP) en vez de READY.
    // Los dos logs del fin de IO los emite desbloquear_por_fin_io, que los
    // mantiene juntos y recien despues habilita al corto plazo.
    bool estaba_suspendido = false;
    desbloquear_por_fin_io(pl, pcb->pid, &estaba_suspendido);

    if (estaba_suspendido) {
        // Quedo elegible para des-suspenderse si ya hay lugar en memoria
        planificador_evento_memoria_liberada(pl);
    }

    free(a);
    return NULL;
}

void planificador_ejecutar_io_async(t_planificador* pl, t_pcb* pcb, t_tipo_io tipo, uint32_t param, uint32_t tamanio) {
    t_io_async_args* args = malloc(sizeof(t_io_async_args));
    args->pl = pl;
    args->pcb = pcb;
    args->tipo = tipo;
    args->param = param;
    args->tamanio = tamanio;

    pthread_t hilo;
    pthread_create(&hilo, NULL, hilo_ejecutar_io, args);
    pthread_detach(hilo);
}

// ============================================================
// Mutex del Kernel (con herencia de prioridades)
// ============================================================

static t_mutex_kernel* buscar_mutex_sin_lock(t_planificador* pl, char* nombre) {
    for (int i = 0; i < list_size(pl->lista_mutex); i++) {
        t_mutex_kernel* m = list_get(pl->lista_mutex, i);
        if (strcmp(m->nombre, nombre) == 0) return m;
    }
    return NULL;
}

// Busca un PCB por PID en EXEC, READY y BLOCK (para herencia de prioridades).
static t_pcb* buscar_pcb_por_pid(t_planificador* pl, uint32_t pid) {
    pthread_mutex_lock(&pl->mutex_lista_exec);
    for (int i = 0; i < list_size(pl->lista_exec); i++) {
        t_pcb* p = list_get(pl->lista_exec, i);
        if (p->pid == pid) {
            pthread_mutex_unlock(&pl->mutex_lista_exec);
            return p;
        }
    }
    pthread_mutex_unlock(&pl->mutex_lista_exec);

    pthread_mutex_lock(&pl->mutex_cola_ready);
    for (int q = 0; q < pl->cant_colas; q++) {
        for (int i = 0; i < list_size(pl->colas_ready[q]); i++) {
            t_pcb* p = list_get(pl->colas_ready[q], i);
            if (p->pid == pid) {
                pthread_mutex_unlock(&pl->mutex_cola_ready);
                return p;
            }
        }
    }
    pthread_mutex_unlock(&pl->mutex_cola_ready);

    pthread_mutex_lock(&pl->mutex_cola_block);
    t_pcb* encontrado = NULL;
    for (int i = 0; i < list_size(pl->cola_block->elements); i++) {
        t_pcb* p = list_get(pl->cola_block->elements, i);
        if (p->pid == pid) {
            encontrado = p;
            break;
        }
    }
    pthread_mutex_unlock(&pl->mutex_cola_block);
    return encontrado;
}

// Cambia la prioridad efectiva de un proceso. Si esta en READY, lo mueve a la
// cola del nuevo nivel (al final). Log obligatorio de cambio de prioridad.
static void cambiar_prioridad(t_planificador* pl, t_pcb* pcb, int nueva) {
    int anterior = pcb->prioridad_actual;
    if (anterior == nueva) return;

    pthread_mutex_lock(&pl->mutex_cola_ready);
    if (pcb->estado == ESTADO_READY && es_cmn(pl)) {
        // Sacarlo de su cola actual y meterlo en la nueva
        for (int q = 0; q < pl->cant_colas; q++) {
            if (list_remove_element(pl->colas_ready[q], pcb)) {
                break;
            }
        }
        pcb->prioridad_actual = nueva;
        int q = nueva;
        if (q < 0) q = 0;
        if (q >= pl->cant_colas) q = pl->cant_colas - 1;
        list_add(pl->colas_ready[q], pcb);
    } else {
        pcb->prioridad_actual = nueva;
    }
    pthread_mutex_unlock(&pl->mutex_cola_ready);

    log_info(pl->logger, "## %u Cambio de prioridad: %d - %d", pcb->pid, anterior, nueva);
}

void planificador_crear_mutex(t_planificador* pl, char* nombre) {
    pthread_mutex_lock(&pl->mutex_lista_mutex);
    if (buscar_mutex_sin_lock(pl, nombre) == NULL) {
        list_add(pl->lista_mutex, mutex_kernel_create(nombre));
        log_info(pl->logger, "Mutex '%s' creado", nombre);
    } else {
        log_warning(pl->logger, "Mutex '%s' ya existia, se ignora la creacion", nombre);
    }
    pthread_mutex_unlock(&pl->mutex_lista_mutex);
}

bool planificador_lock_mutex(t_planificador* pl, t_pcb* pcb, char* nombre) {
    pthread_mutex_lock(&pl->mutex_lista_mutex);

    t_mutex_kernel* m = buscar_mutex_sin_lock(pl, nombre);
    if (m == NULL) {
        // No deberia pasar (los scripts no tienen errores semanticos), pero por
        // robustez lo creamos on-demand en vez de colgar el proceso.
        m = mutex_kernel_create(nombre);
        list_add(pl->lista_mutex, m);
    }

    bool adquirido;
    uint32_t pid_duenio = 0;
    if (!m->tomado) {
        m->tomado = true;
        m->pid_duenio = pcb->pid;
        adquirido = true;
    } else {
        uint32_t* pid_ptr = malloc(sizeof(uint32_t));
        *pid_ptr = pcb->pid;
        queue_push(m->cola_bloqueados, pid_ptr);
        pid_duenio = m->pid_duenio;
        adquirido = false;
    }

    pthread_mutex_unlock(&pl->mutex_lista_mutex);

    // Herencia de prioridades: si el que se bloquea es mas prioritario que el
    // duenio actual, el duenio hereda temporalmente esa prioridad.
    if (!adquirido) {
        t_pcb* duenio = buscar_pcb_por_pid(pl, pid_duenio);
        if (duenio != NULL && pcb->prioridad_actual < duenio->prioridad_actual) {
            cambiar_prioridad(pl, duenio, pcb->prioridad_actual);
        }
    }

    return adquirido;
}

void planificador_unlock_mutex(t_planificador* pl, t_pcb* pcb_liberador, char* nombre) {
    // Quitar el mutex de la lista de tomados del liberador
    for (int i = 0; i < list_size(pcb_liberador->lista_mutex_tomados); i++) {
        char* tomado = list_get(pcb_liberador->lista_mutex_tomados, i);
        if (strcmp(tomado, nombre) == 0) {
            list_remove(pcb_liberador->lista_mutex_tomados, i);
            free(tomado);
            break;
        }
    }

    // Al liberar, el proceso vuelve a su prioridad original si la tenia heredada
    if (pcb_liberador->prioridad_actual != pcb_liberador->prioridad_original) {
        cambiar_prioridad(pl, pcb_liberador, pcb_liberador->prioridad_original);
    }

    pthread_mutex_lock(&pl->mutex_lista_mutex);

    t_mutex_kernel* m = buscar_mutex_sin_lock(pl, nombre);
    if (m == NULL) {
        pthread_mutex_unlock(&pl->mutex_lista_mutex);
        return;
    }

    uint32_t* siguiente_pid = queue_size(m->cola_bloqueados) > 0 ? queue_pop(m->cola_bloqueados) : NULL;
    if (siguiente_pid != NULL) {
        m->pid_duenio = *siguiente_pid;
        m->tomado = true;
    } else {
        m->pid_duenio = 0;
        m->tomado = false;
    }

    pthread_mutex_unlock(&pl->mutex_lista_mutex);

    if (siguiente_pid != NULL) {
        uint32_t pid_despertar = *siguiente_pid;
        free(siguiente_pid);

        // El que esperaba el mutex pudo haberse suspendido mientras tanto:
        // en ese caso pasa a SUSP. READY con el mutex ya tomado.
        bool estaba_suspendido = false;
        t_pcb* despertado = planificador_desbloquear(pl, pid_despertar, &estaba_suspendido);
        if (despertado != NULL) {
            list_add(despertado->lista_mutex_tomados, strdup(nombre));
            log_info(pl->logger, "## (%u) Toma el Mutex %s", despertado->pid, nombre);
        }
        if (estaba_suspendido) {
            planificador_evento_memoria_liberada(pl);
        }
    }
}
