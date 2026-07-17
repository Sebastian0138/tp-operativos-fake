#ifndef UTILS_SEGMENTO_H_
#define UTILS_SEGMENTO_H_

#include <stdint.h>

typedef struct {
    uint32_t id_segmento;
    uint32_t base;
    uint32_t limite;
    uint32_t tamanio;
} t_segmento;

#endif /* UTILS_SEGMENTO_H_ */
