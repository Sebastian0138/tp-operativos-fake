#ifndef UTILS_ESTADOS_H_
#define UTILS_ESTADOS_H_

/**
 * Estados posibles de un proceso en el sistema.
 * Prefijo ESTADO_ para evitar colision con macros estandar (EXIT).
 */
typedef enum {
    ESTADO_NEW,
    ESTADO_READY,
    ESTADO_EXEC,
    ESTADO_BLOCK,
    ESTADO_SUSP_BLOCK,
    ESTADO_SUSP_READY,
    ESTADO_EXIT
} t_estado_proceso;

/**
 * Devuelve el nombre legible de un estado de proceso.
 */
static inline const char* estado_to_string(t_estado_proceso estado) {
    switch (estado) {
        case ESTADO_NEW:        return "NEW";
        case ESTADO_READY:      return "READY";
        case ESTADO_EXEC:       return "EXEC";
        case ESTADO_BLOCK:      return "BLOCK";
        case ESTADO_SUSP_BLOCK: return "SUSP. BLOCK";
        case ESTADO_SUSP_READY: return "SUSP. READY";
        case ESTADO_EXIT:       return "EXIT";
        default:                return "DESCONOCIDO";
    }
}

#endif /* UTILS_ESTADOS_H_ */
