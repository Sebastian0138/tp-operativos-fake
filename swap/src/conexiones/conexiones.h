#ifndef SWAP_CONEXIONES_H
#define SWAP_CONEXIONES_H

#include <stdio.h>
#include <commons/log.h>
#include "config/swap_config.h"

// Conecta a Kernel Memory, informa tamanio total y tamanio de bloque, y
// atiende las operaciones de lectura/escritura de bloques (bloqueante).
void conectar_y_atender_kernel_memory(t_swap_config* config, FILE* archivo_swap, t_log* logger);

#endif // SWAP_CONEXIONES_H
