#ifndef CONEXIONES_H
#define CONEXIONES_H

#include <commons/log.h>

#include "planificador/planificador.h"
#include "config/kernel_scheduler_config.h"

// Levanta el servidor que escucha conexiones de CPU e IO
void iniciar_servidor_kernel_scheduler(const char* puerto, t_planificador* planificador, t_kernel_scheduler_config* config, t_log* logger);

// Conecta al Kernel Memory como cliente (canal principal de peticiones)
int conectar_a_kernel_memory(const char* ip, const char* puerto, t_log* logger);

// Abre la segunda conexion con Kernel Memory, dedicada a notificaciones
// KM -> KS (mas memoria disponible, memoria corrupta -> BSOD), y lanza el
// hilo que las atiende.
int conectar_canal_notificaciones_km(const char* ip, const char* puerto, t_planificador* planificador, t_log* logger);

#endif /* CONEXIONES_H */
