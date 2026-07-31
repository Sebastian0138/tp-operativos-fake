#include "kernel_memory_atencion_swap.h"

#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>

// El SWAP informa su tamanio total y el tamanio de bloque. El fd queda en manos
// del gestor de memoria (las operaciones de bloque se hacen bajo demanda con su
// mutex); nadie vuelve a leer de este socket desde el hilo de atencion.
bool atender_swap(int fd, t_gestor_memoria* gestor_memoria, t_log* logger) {
    t_codigo_mensaje codigo;
    if (recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0
        || codigo != MENSAJE_INFORMAR_TAMANIO) {
        log_warning(logger, "SWAP (fd %d) no informo su tamaño — conexion rechazada", fd);
        return false;
    }

    uint32_t total, block;
    if (recv(fd, &total, sizeof(uint32_t), MSG_WAITALL) <= 0) return false;
    if (recv(fd, &block, sizeof(uint32_t), MSG_WAITALL) <= 0) return false;

    gestor_memoria_registrar_swap(gestor_memoria, fd, total, block);
    return true;
}
