#include "mutex_kernel.h"
#include <stdlib.h>
#include <string.h>

t_mutex_kernel* mutex_kernel_create(char* nombre) {
    t_mutex_kernel* mutex = malloc(sizeof(t_mutex_kernel));
    mutex->nombre = strdup(nombre);
    mutex->pid_duenio = 0;
    mutex->tomado = false;
    mutex->cola_bloqueados = queue_create();
    return mutex;
}

void mutex_kernel_destroy(t_mutex_kernel* mutex) {
    if (mutex == NULL) return;
    free(mutex->nombre);
    queue_destroy_and_destroy_elements(mutex->cola_bloqueados, free);
    free(mutex);
}
