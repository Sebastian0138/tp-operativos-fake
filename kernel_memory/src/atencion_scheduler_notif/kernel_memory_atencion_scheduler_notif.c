#include "kernel_memory_atencion_scheduler_notif.h"

// Canal exclusivo de notificaciones KM -> KS. Este hilo no lee mas: el fd queda
// registrado para que el gestor de memoria pueda empujar MAS_MEMORIA /
// MEMORIA_CORRUPTA sin interferir con el canal principal.
bool atender_scheduler_notif(int fd, t_gestor_memoria* gestor_memoria, t_log* logger) {
    log_info(logger, "Canal de notificaciones al Kernel Scheduler conectado (fd: %d)", fd);
    gestor_memoria_set_notif_ks(gestor_memoria, fd);
    return true;
}
