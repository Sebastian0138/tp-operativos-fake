#include "pcb.h"
#include <stdlib.h>
#include <string.h>

t_pcb* pcb_create(uint32_t pid, char* path_instrucciones) {
    t_pcb* pcb = malloc(sizeof(t_pcb));
    if (pcb == NULL) {
        return NULL;
    }
    pcb->pid = pid;
    pcb->path_instrucciones = strdup(path_instrucciones);
    pcb->estado = ESTADO_NEW;
    pcb->prioridad_original = 0;
    pcb->prioridad_actual = 0;
    pcb->cpu_asignada = -1;
    pcb->motivo_bloqueo = NULL;
    pcb->recurso_bloqueado = NULL;
    pcb->bloqueo_id = 0;
    pcb->lista_mutex_tomados = list_create();

    memset(&(pcb->registros), 0, sizeof(t_registros_cpu));

    memset(&(pcb->ts_llegada_ready), 0, sizeof(struct timespec));
    memset(&(pcb->ts_inicio_exec), 0, sizeof(struct timespec));
    memset(&(pcb->ts_inicio_block), 0, sizeof(struct timespec));
    memset(&(pcb->ts_suspension), 0, sizeof(struct timespec));

    return pcb;
}

void pcb_destroy(t_pcb* pcb) {
    if (pcb == NULL) {
        return;
    }
    free(pcb->path_instrucciones);
    if (pcb->motivo_bloqueo != NULL) {
        free(pcb->motivo_bloqueo);
    }
    if (pcb->recurso_bloqueado != NULL) {
        free(pcb->recurso_bloqueado);
    }
    if (pcb->lista_mutex_tomados != NULL) {
        list_destroy_and_destroy_elements(pcb->lista_mutex_tomados, free);
    }
    free(pcb);
}
