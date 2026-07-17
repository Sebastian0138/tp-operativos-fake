#ifndef KERNEL_SCHEDULER_MUTEX_KERNEL_H_
#define KERNEL_SCHEDULER_MUTEX_KERNEL_H_

#include <stdint.h>
#include <stdbool.h>
#include <commons/collections/queue.h>

/**
 * Mutex del kernel (sin herencia de prioridades).
 * Administra exclusion mutua sobre un recurso nombrado.
 */
typedef struct {
    char* nombre;
    uint32_t pid_duenio;        // PID del proceso que tiene el lock, 0 si libre
    bool tomado;
    t_queue* cola_bloqueados;   // Cola de PIDs (uint32_t*) esperando el mutex
} t_mutex_kernel;

/**
 * Crea un mutex con el nombre dado, en estado libre.
 */
t_mutex_kernel* mutex_kernel_create(char* nombre);

/**
 * Destruye el mutex y su cola de bloqueados.
 */
void mutex_kernel_destroy(t_mutex_kernel* mutex);

#endif /* KERNEL_SCHEDULER_MUTEX_KERNEL_H_ */
