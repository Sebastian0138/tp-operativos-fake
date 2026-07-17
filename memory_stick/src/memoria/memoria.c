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

// Devuelve true si el rango [dir, dir + tamanio) cae entero dentro del bloque
// de memoria (que va de 0 a memoria->tamanio).
//
// Ojo: NO escribimos "dir + tamanio <= memoria->tamanio" porque dir y tamanio
// son uint32 y esa suma podria desbordar (dar la vuelta a un numero chico) y
// pasar el chequeo siendo invalida. Por eso lo verificamos en dos partes que
// nunca desbordan:
//   1) que el inicio no se pase del final del bloque, y
//   2) que lo que queda de bloque desde `dir` alcance para `tamanio` bytes.
static bool rango_valido(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio) {
    bool inicio_dentro_del_bloque = dir <= memoria->tamanio;
    uint32_t espacio_disponible_desde_dir = memoria->tamanio - dir;
    bool entran_los_bytes_pedidos = tamanio <= espacio_disponible_desde_dir;

    return inicio_dentro_del_bloque && entran_los_bytes_pedidos;
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
