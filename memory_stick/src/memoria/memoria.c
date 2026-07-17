#include "memoria.h"
#include <stdlib.h>
#include <string.h>

t_memoria_stick* memoria_stick_create(uint32_t tamanio) {
    t_memoria_stick* memoria = malloc(sizeof(t_memoria_stick));
    if (memoria == NULL) return NULL;

    memoria->bloque = calloc(1, tamanio);
    if (memoria->bloque == NULL) {
        free(memoria);
        return NULL;
    }

    memoria->tamanio = tamanio;
    pthread_mutex_init(&memoria->mutex, NULL);
    return memoria;
}

void memoria_stick_destroy(t_memoria_stick* memoria) {
    if (memoria == NULL) return;
    free(memoria->bloque);
    pthread_mutex_destroy(&memoria->mutex);
    free(memoria);
}

static bool rango_valido(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio) {
    // Comparacion sin overflow: dir + tamanio podria dar vuelta en uint32.
    return dir <= memoria->tamanio && tamanio <= memoria->tamanio - dir;
}

bool memoria_stick_leer(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, void* destino) {
    if (!rango_valido(memoria, dir, tamanio)) return false;

    pthread_mutex_lock(&memoria->mutex);
    memcpy(destino, (char*)memoria->bloque + dir, tamanio);
    pthread_mutex_unlock(&memoria->mutex);
    return true;
}

bool memoria_stick_escribir(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, const void* origen) {
    if (!rango_valido(memoria, dir, tamanio)) return false;

    pthread_mutex_lock(&memoria->mutex);
    memcpy((char*)memoria->bloque + dir, origen, tamanio);
    pthread_mutex_unlock(&memoria->mutex);
    return true;
}
