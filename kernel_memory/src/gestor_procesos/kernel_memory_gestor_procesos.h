#ifndef KERNEL_MEMORY_GESTOR_PROCESOS_H_
#define KERNEL_MEMORY_GESTOR_PROCESOS_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <commons/collections/list.h>
#include <commons/log.h>

#include "../contexto/contexto.h"

// Lo que el KM guarda por cada proceso
typedef struct {
    t_contexto_ejecucion* contexto;   // PID + registros + tabla segmentos
    char* path_instrucciones;          // ruta al archivo .txt
} t_proceso_km;

// El gestor en sí — una lista de t_proceso_km* con su mutex
typedef struct {
    t_list*         lista_procesos;
    pthread_mutex_t mutex;
} t_gestor_procesos;

// Funciones
t_gestor_procesos* gestor_procesos_create();
void               gestor_procesos_destroy(t_gestor_procesos* gestor);

void               gestor_agregar_proceso(t_gestor_procesos* gestor, uint32_t pid, char* path);
void               gestor_remover_proceso(t_gestor_procesos* gestor, uint32_t pid);
t_proceso_km*      gestor_buscar_proceso(t_gestor_procesos* gestor, uint32_t pid);
char*              gestor_obtener_instruccion(t_gestor_procesos* gestor, uint32_t pid, uint32_t pc, t_log* logger);

// Busca sin tomar el mutex — para usar dentro de una region ya protegida
// con gestor_lock()/gestor_unlock(), cuando el caller necesita leer el
// contenido de t_proceso_km (contexto, tabla_segmentos) de forma segura.
t_proceso_km*      gestor_buscar_proceso_sin_lock(t_gestor_procesos* gestor, uint32_t pid);
void               gestor_lock(t_gestor_procesos* gestor);
void               gestor_unlock(t_gestor_procesos* gestor);

#endif /* KERNEL_MEMORY_GESTOR_PROCESOS_H_ */