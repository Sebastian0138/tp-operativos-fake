#ifndef KERNEL_SCHEDULER_PCB_H_
#define KERNEL_SCHEDULER_PCB_H_

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <commons/collections/list.h>

#include <utils/estados.h>
#include <utils/registros.h>

/**
 * Process Control Block.
 * Estructura principal que el Kernel Scheduler mantiene por cada proceso.
 */
typedef struct {
    uint32_t pid;
    char* path_instrucciones;
    t_estado_proceso estado;

    // Prioridades (para algoritmos con prioridad, no CP2 basico)
    int prioridad_original;
    int prioridad_actual;

    // CPU asignada (-1 si no tiene)
    int cpu_asignada;

    // Info de bloqueo
    char* motivo_bloqueo;           // Descripcion del motivo (ej: "IO_SLEEP", "MUTEX")
    char* recurso_bloqueado;        // Nombre del recurso que lo bloquea (ej: nombre del mutex)

    // Identificador incremental de cada entrada a BLOCK: invalida timers de
    // suspension viejos si el proceso se desbloqueo y volvio a bloquearse.
    uint32_t bloqueo_id;

    // Lista de nombres de mutex tomados (t_list* de char*)
    t_list* lista_mutex_tomados;

    // Registros guardados del CPU al desalojar
    t_registros_cpu registros;

    // Timestamps opcionales (se llenan cuando corresponda)
    struct timespec ts_llegada_ready;
    struct timespec ts_inicio_exec;
    struct timespec ts_inicio_block;
    struct timespec ts_suspension;

} t_pcb;

/**
 * Crea un PCB nuevo con valores iniciales.
 * El PCB arranca en ESTADO_NEW con registros en cero.
 */
t_pcb* pcb_create(uint32_t pid, char* path_instrucciones);

/**
 * Libera toda la memoria asociada al PCB.
 */
void pcb_destroy(t_pcb* pcb);

#endif /* KERNEL_SCHEDULER_PCB_H_ */
