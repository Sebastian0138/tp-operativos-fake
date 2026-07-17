#ifndef KERNEL_SCHEDULER_PLANIFICADOR_H_
#define KERNEL_SCHEDULER_PLANIFICADOR_H_

#include <stdint.h>
#include <stdbool.h>
#include <commons/collections/list.h>
#include <commons/collections/queue.h>

#include "pcb/pcb.h"
#include "../io_scheduler/io_scheduler.h"
#include "../mutex_kernel/mutex_kernel.h"

#include <pthread.h>
#include <semaphore.h>
#include <commons/log.h>
#include "config/kernel_scheduler_config.h"

/**
 * Motivo de la ultima interrupcion enviada a una CPU. Permite distinguir,
 * al recibir la devolucion del proceso, si fue por quantum, por desalojo
 * de cola mas prioritaria o por compactacion.
 */
typedef enum {
    INT_NINGUNA,
    INT_QUANTUM,
    INT_PREEMPCION,
    INT_COMPACTACION
} t_motivo_interrupcion;

/**
 * Representa una CPU conectada al Kernel Scheduler.
 */
typedef struct {
    int id_cpu;
    int socket_cpu;
    bool libre;
    uint32_t pid_ejecutando;    // PID del proceso en EXEC, 0 si libre

    // true desde que se despacha un PID hasta que la CPU lo devuelve
    // (syscall o interrupcion). Una CPU con el proceso "estacionado" en una
    // syscall no esta ejecutando instrucciones (relevante para compactacion).
    bool en_rafaga;

    // Identificador incremental de rafaga: invalida timers de quantum viejos
    // cuando el mismo PID vuelve a la misma CPU (retorno directo de syscalls).
    uint32_t rafaga;

    t_motivo_interrupcion motivo_interrupcion;
    // Datos para el log de desalojo por cola mas prioritaria
    uint32_t pid_preemptor;
    int prioridad_preemptor;
} t_cpu_conectada;

/**
 * Representa un dispositivo IO conectado (STDIN, STDOUT o SLEEP).
 * Cada dispositivo atiende una peticion por vez.
 */
typedef struct {
    int fd;
    t_tipo_io tipo;
    char* nombre;
    bool libre;
    bool desconectado;   // se marco para remover pero seguia en uso al desconectarse
} t_io_conectada;

/**
 * Planificador central con las colas de estados.
 *
 * READY es un arreglo de colas (t_list* de t_pcb*): con FIFO/RR hay una sola
 * cola (indice 0); con CMN hay una por nivel de prioridad, cada una con su
 * algoritmo (FIFO o RR) segun QUEUES_ALGORITHMS.
 */
typedef struct {
    t_queue* cola_new;
    t_list** colas_ready;       // arreglo de cant_colas listas
    int      cant_colas;
    char**   alg_por_cola;      // algoritmo de cada cola ("FIFO"/"RR"), apunta al config
    bool     queue_preemption;  // desalojo entre colas (solo CMN)

    t_list*  lista_exec;        // Lista porque puede haber multiples CPUs
    t_queue* cola_block;
    t_queue* cola_susp_block;
    t_queue* cola_susp_ready;
    t_list*  lista_exit;        // Lista para registrar procesos finalizados

    // Lista de CPUs conectadas (t_cpu_conectada*)
    t_list*  cpus_conectadas;

    // Lista de IOs conectadas (t_io_conectada*) y de Mutex creados (t_mutex_kernel*)
    t_list*  ios_conectados;
    t_list*  lista_mutex;

    // Sincronización de colas y CPUs
    pthread_mutex_t mutex_cola_new;
    pthread_mutex_t mutex_cola_ready;   // protege todas las colas de READY
    pthread_mutex_t mutex_lista_exec;
    pthread_mutex_t mutex_cola_block;
    pthread_mutex_t mutex_lista_exit;
    pthread_mutex_t mutex_cpus;
    pthread_mutex_t mutex_ios;
    pthread_mutex_t mutex_lista_mutex;
    pthread_mutex_t mutex_socket_memoria;

    sem_t sem_procesos_new;
    sem_t sem_procesos_ready;
    sem_t sem_cpus_libres;
    sem_t sem_io_stdin_libres;
    sem_t sem_io_stdout_libres;
    sem_t sem_io_sleep_libres;

    // Desalojo por compactacion: mientras esta activo no se despacha nada.
    bool compactacion_en_curso;
    pthread_mutex_t mutex_compactacion;
    pthread_cond_t cond_compactacion;

    // Mediano plazo
    pthread_mutex_t mutex_cola_susp;        // protege cola_susp_block y cola_susp_ready
    t_list* lista_espera_memoria;           // t_espera_memoria*: MEM_ALLOC sin espacio, esperando
    pthread_mutex_t mutex_espera_memoria;
    pthread_mutex_t mutex_evento_memoria;   // serializa des-suspensiones y reintentos de alloc

    // Datos globales compartidos
    t_kernel_scheduler_config* config;
    t_log* logger;
    int socket_memoria;
    uint32_t proximo_pid;
    pthread_mutex_t mutex_pid;
} t_planificador;

/**
 * Crea el planificador con todas las colas vacias.
 */
t_planificador* planificador_create(t_kernel_scheduler_config* config, t_log* logger, int socket_memoria);

/**
 * Destruye el planificador y todas sus colas.
 */
void planificador_destroy(t_planificador* planificador);

// Hilos de planificación
void* hilo_planificador_largo_plazo(void* arg);
void* hilo_planificador_corto_plazo(void* arg);

// Métodos de transiciones seguras
void planificador_encolar_new(t_planificador* pl, t_pcb* pcb);
void planificador_transicionar_new_a_ready(t_planificador* pl, t_pcb* pcb);
void planificador_transicionar_exec_a_exit(t_planificador* pl, t_pcb* pcb, const char* motivo);
void planificador_transicionar_exec_a_ready(t_planificador* pl, t_pcb* pcb);
// Variante para desalojo por compactacion: el proceso vuelve AL PRINCIPIO de su cola.
void planificador_transicionar_exec_a_ready_frente(t_planificador* pl, t_pcb* pcb);
void planificador_transicionar_exec_a_block(t_planificador* pl, t_pcb* pcb, const char* motivo_bloqueo);
// Devuelve el PCB real que se movio a READY (o NULL si no estaba en cola_block).
t_pcb* planificador_transicionar_block_a_ready(t_planificador* pl, t_pcb* pcb);

// Desbloquea un proceso por PID: si estaba en BLOCK pasa a READY; si estaba
// suspendido (SUSP. BLOCK) pasa a SUSP. READY. Deja en *estaba_suspendido
// donde estaba. Devuelve NULL si no se encontro en ninguna de las dos colas.
t_pcb* planificador_desbloquear(t_planificador* pl, uint32_t pid, bool* estaba_suspendido);

// Registra un proceso bloqueado esperando memoria para un MEM_ALLOC que no
// pudo satisfacerse. Se reintenta en cada evento de liberacion de memoria.
void planificador_bloquear_por_memoria(t_planificador* pl, t_pcb* pcb, uint32_t id_segmento, uint32_t tamanio);

// Evento "se libero memoria" (MEM_FREE, fin de proceso, suspension, compactacion,
// stick nuevo): recorre los suspendidos por prioridad (y antiguedad) intentando
// des-suspenderlos, y reintenta los MEM_ALLOC en espera.
void planificador_evento_memoria_liberada(t_planificador* pl);

// Funciones para CPUs
void planificador_registrar_cpu(t_planificador* pl, int socket_cpu, int id_cpu);
void planificador_remover_cpu(t_planificador* pl, int socket_cpu);
t_cpu_conectada* planificador_obtener_cpu_por_socket(t_planificador* pl, int socket_cpu);
t_cpu_conectada* planificador_obtener_cpu_por_id(t_planificador* pl, int id_cpu);

// Retorno directo de un proceso a la CPU que hizo la syscall (mutex y memoria):
// actualiza el contexto en KM, marca la CPU en rafaga y reenvia el PID.
// Respeta la pausa por compactacion.
void planificador_despachar_directo(t_planificador* pl, t_pcb* pcb, t_cpu_conectada* cpu);

// Funciones para dispositivos IO
void planificador_registrar_io(t_planificador* pl, int fd, t_tipo_io tipo, char* nombre);
void planificador_remover_io(t_planificador* pl, int fd);
// Bloquea (sem_wait) hasta que haya un dispositivo libre del tipo pedido y lo marca ocupado.
t_io_conectada* planificador_tomar_io_libre(t_planificador* pl, t_tipo_io tipo);
void planificador_liberar_io(t_planificador* pl, t_io_conectada* io);

// Ejecuta de forma asincrona (en un hilo aparte) un pedido de IO para un proceso
// bloqueado: SLEEP, o STDIN/STDOUT interactuando con Kernel Memory para el
// contenido. Al terminar, transiciona el proceso de BLOCK a READY.
void planificador_ejecutar_io_async(t_planificador* pl, t_pcb* pcb, t_tipo_io tipo, uint32_t param, uint32_t tamanio);

// Envia a Kernel Memory los registros actualizados de un proceso (para que la
// proxima vez que se despache a una CPU, el contexto no este desactualizado).
void planificador_actualizar_contexto_en_km(t_planificador* pl, uint32_t pid, t_registros_cpu registros);

// Syscalls de memoria contra Kernel Memory (protocolo sincronico; crear puede
// disparar el desalojo por compactacion). Devuelven true si la operacion salio bien.
// Si hubo_compactacion no es NULL, indica si el pedido disparo una compactacion.
bool planificador_crear_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento, uint32_t tamanio, bool* hubo_compactacion);
bool planificador_eliminar_segmento_km(t_planificador* pl, uint32_t pid, uint32_t id_segmento);
// Avisa a KM que libere todos los segmentos y estructuras del proceso.
void planificador_finalizar_proceso_km(t_planificador* pl, uint32_t pid);

// Blue Screen of Death: finaliza todos los procesos y el Kernel Scheduler.
void planificador_bsod(t_planificador* pl);

// Funciones para Mutex
void planificador_crear_mutex(t_planificador* pl, char* nombre);
// true si el mutex quedo tomado por el pcb; false si quedo encolado esperando.
// Aplica herencia de prioridades si corresponde.
bool planificador_lock_mutex(t_planificador* pl, t_pcb* pcb, char* nombre);
// Libera el mutex; restaura la prioridad original del que libera si la tenia heredada.
void planificador_unlock_mutex(t_planificador* pl, t_pcb* pcb_liberador, char* nombre);

#endif /* KERNEL_SCHEDULER_PLANIFICADOR_H_ */
