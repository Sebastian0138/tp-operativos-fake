#ifndef MEMORY_STICK_MEMORIA_H_
#define MEMORY_STICK_MEMORIA_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/**
 * Bloque de memoria fisica que representa este Memory Stick.
 * Las direcciones que recibe son locales al stick (0 .. tamanio-1);
 * la traduccion de direccion global a (stick, offset local) la hace
 * quien rutea (CPU o Kernel Memory).
 */
typedef struct {
    void* bloque;
    uint32_t tamanio;
    pthread_mutex_t mutex;
} t_memoria_stick;

t_memoria_stick* memoria_stick_create(uint32_t tamanio);
void memoria_stick_destroy(t_memoria_stick* memoria);

// Devuelven false si el rango [dir, dir+tamanio) se sale del bloque.
bool memoria_stick_leer(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, void* destino);
bool memoria_stick_escribir(t_memoria_stick* memoria, uint32_t dir, uint32_t tamanio, const void* origen);

#endif /* MEMORY_STICK_MEMORIA_H_ */
