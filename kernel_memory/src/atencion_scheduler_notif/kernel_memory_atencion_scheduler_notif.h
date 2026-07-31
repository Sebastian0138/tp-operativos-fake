#ifndef KERNEL_MEMORY_ATENCION_SCHEDULER_NOTIF_H_
#define KERNEL_MEMORY_ATENCION_SCHEDULER_NOTIF_H_

#include <stdbool.h>
#include <commons/log.h>
#include "../gestor_memoria/gestor_memoria.h"

/**
 * Registra el canal exclusivo de notificaciones Kernel Memory -> Kernel Scheduler.
 * Por este socket no se lee nunca: queda guardado en el gestor de memoria para
 * empujar MAS_MEMORIA / MEMORIA_CORRUPTA sin interferir con el canal principal.
 *
 * Devuelve true si el fd quedo registrado: el llamador NO debe cerrarlo.
 */
bool atender_scheduler_notif(int fd, t_gestor_memoria* gestor_memoria, t_log* logger);

#endif
