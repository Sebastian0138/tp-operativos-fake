#ifndef KERNEL_MEMORY_CONTEXTO_H_
#define KERNEL_MEMORY_CONTEXTO_H_

#include <stdint.h>
#include <commons/collections/list.h>

#include <utils/registros.h>
#include <utils/segmento.h>

/**
 * Contexto de ejecucion de un proceso.
 * Kernel Memory guarda uno por cada proceso activo.
 */
typedef struct {
    uint32_t pid;
    t_registros_cpu registros;
    t_list* tabla_segmentos;    // Lista de t_segmento*
} t_contexto_ejecucion;

#endif /* KERNEL_MEMORY_CONTEXTO_H_ */
