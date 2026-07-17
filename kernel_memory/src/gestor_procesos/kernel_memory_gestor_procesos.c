#include "kernel_memory_gestor_procesos.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void destruir_proceso_km(void* elemento) {
    t_proceso_km* proc = (t_proceso_km*) elemento;
    free(proc->path_instrucciones);
    list_destroy_and_destroy_elements(proc->contexto->tabla_segmentos, free);
    free(proc->contexto);
    free(proc);
}

t_gestor_procesos* gestor_procesos_create() {
    t_gestor_procesos* gestor = malloc(sizeof(t_gestor_procesos));
    if (gestor == NULL) return NULL;
    gestor->lista_procesos = list_create();
    pthread_mutex_init(&gestor->mutex, NULL);
    return gestor;
}

void gestor_procesos_destroy(t_gestor_procesos* gestor) {
    if (gestor == NULL) return;
    list_destroy_and_destroy_elements(gestor->lista_procesos, destruir_proceso_km);
    pthread_mutex_destroy(&gestor->mutex);
    free(gestor);
}

void gestor_agregar_proceso(t_gestor_procesos* gestor, uint32_t pid, char* path) {
    t_contexto_ejecucion* contexto = malloc(sizeof(t_contexto_ejecucion));
    contexto->pid = pid;
    memset(&contexto->registros, 0, sizeof(t_registros_cpu));
    contexto->tabla_segmentos = list_create();

    t_proceso_km* proc = malloc(sizeof(t_proceso_km));
    proc->contexto           = contexto;
    proc->path_instrucciones = strdup(path);

    pthread_mutex_lock(&gestor->mutex);
    list_add(gestor->lista_procesos, proc);
    pthread_mutex_unlock(&gestor->mutex);
}

void gestor_remover_proceso(t_gestor_procesos* gestor, uint32_t pid) {
    pthread_mutex_lock(&gestor->mutex);
    for (int i = 0; i < list_size(gestor->lista_procesos); i++) {
        t_proceso_km* proc = list_get(gestor->lista_procesos, i);
        if (proc->contexto->pid == pid) {
            list_remove(gestor->lista_procesos, i);
            destruir_proceso_km(proc);
            break;
        }
    }
    pthread_mutex_unlock(&gestor->mutex);
}

t_proceso_km* gestor_buscar_proceso_sin_lock(t_gestor_procesos* gestor, uint32_t pid) {
    t_proceso_km* encontrado = NULL;
    for (int i = 0; i < list_size(gestor->lista_procesos); i++) {
        t_proceso_km* proc = list_get(gestor->lista_procesos, i);
        if (proc->contexto->pid == pid) {
            encontrado = proc;
            break;
        }
    }
    return encontrado;
}

t_proceso_km* gestor_buscar_proceso(t_gestor_procesos* gestor, uint32_t pid) {
    pthread_mutex_lock(&gestor->mutex);
    t_proceso_km* encontrado = gestor_buscar_proceso_sin_lock(gestor, pid);
    pthread_mutex_unlock(&gestor->mutex);
    return encontrado;
}

void gestor_lock(t_gestor_procesos* gestor) {
    pthread_mutex_lock(&gestor->mutex);
}

void gestor_unlock(t_gestor_procesos* gestor) {
    pthread_mutex_unlock(&gestor->mutex);
}

char* gestor_obtener_instruccion(t_gestor_procesos* gestor, uint32_t pid, uint32_t pc, t_log* logger) {
    t_proceso_km* proc = gestor_buscar_proceso(gestor, pid);
    if (proc == NULL) {
        log_error(logger, "PID %u no encontrado en el gestor", pid);
        return NULL;
    }

    FILE* archivo = fopen(proc->path_instrucciones, "r");
    if (archivo == NULL) {
        log_error(logger, "No se pudo abrir el archivo: %s", proc->path_instrucciones);
        return NULL;
    }

    char linea[256];
    uint32_t linea_actual = 0;
    char* instruccion = NULL;

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        if (linea_actual == pc) {
            linea[strcspn(linea, "\n")] = '\0';
            instruccion = strdup(linea);
            break;
        }
        linea_actual++;
    }

    fclose(archivo);

    if (instruccion != NULL) {
        log_info(logger, "## PID: %u - Obtener instrucción: %u - Instrucción: %s", pid, pc, instruccion);
    }

    return instruccion;
}