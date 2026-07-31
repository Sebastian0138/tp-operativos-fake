#include "kernel_memory_atencion_memory_stick.h"

#include <utils/protocolo.h>
#include <sys/socket.h>
#include <stdint.h>
#include <stdlib.h>

// Recibe el INFORMAR_TAMANIO de un stick, lo registra y queda monitoreando
// la conexion de control: si el stick se desconecta -> memoria corrupta.
void atender_memory_stick(int fd, t_gestor_memoria* gestor_memoria, t_log* logger) {
    t_codigo_mensaje codigo;
    if (recv(fd, &codigo, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0 || codigo != MENSAJE_INFORMAR_TAMANIO) {
        log_warning(logger, "Memory Stick (fd %d) no informo su tamaño — conexion rechazada", fd);
        return;
    }

    uint32_t tamanio, ip_len, port_len;
    if (recv(fd, &tamanio, sizeof(uint32_t), MSG_WAITALL) <= 0) return;

    if (recv(fd, &ip_len, sizeof(uint32_t), MSG_WAITALL) <= 0) return;
    char* ip = malloc(ip_len + 1);
    if (recv(fd, ip, ip_len, MSG_WAITALL) <= 0) { free(ip); return; }
    ip[ip_len] = '\0';

    if (recv(fd, &port_len, sizeof(uint32_t), MSG_WAITALL) <= 0) { free(ip); return; }
    char* puerto = malloc(port_len + 1);
    if (recv(fd, puerto, port_len, MSG_WAITALL) <= 0) { free(ip); free(puerto); return; }
    puerto[port_len] = '\0';

    int id_stick = gestor_memoria_registrar_stick(gestor_memoria, tamanio, ip, puerto);
    free(ip);
    free(puerto);

    if (id_stick == -1) return;

    // El stick no vuelve a escribir por esta conexion: un recv que retorna
    // significa que el stick se desconecto -> notificar memoria corrupta.
    char descarte;
    while (recv(fd, &descarte, 1, MSG_WAITALL) > 0);
    gestor_memoria_stick_desconectado(gestor_memoria, id_stick);
}
