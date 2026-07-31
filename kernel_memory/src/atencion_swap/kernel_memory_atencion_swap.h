#ifndef KERNEL_MEMORY_ATENCION_SWAP_H_
#define KERNEL_MEMORY_ATENCION_SWAP_H_

#include <stdbool.h>
#include <commons/log.h>
#include "../gestor_memoria/gestor_memoria.h"

/**
 * Atiende el handshake del SWAP: recibe su tamanio total y su tamanio de bloque
 * y le entrega el fd al gestor de memoria.
 *
 * Devuelve true si el fd quedo en manos del gestor: en ese caso el llamador NO
 * debe cerrarlo. Devuelve false si el handshake fallo y hay que cerrarlo.
 */
bool atender_swap(int fd, t_gestor_memoria* gestor_memoria, t_log* logger);

#endif
