#ifndef UTILS_REGISTROS_H_
#define UTILS_REGISTROS_H_

#include <stdint.h>

/**
 * Registros de la CPU.
 * Compartido entre CPU (los usa) y Kernel Memory (los guarda en contexto).
 */
typedef struct {
    uint32_t pc;        // Program Counter

    uint8_t  ax;        // Registros de proposito general (8 bits)
    uint8_t  bx;
    uint8_t  cx;
    uint8_t  dx;

    uint32_t eax;       // Registros de proposito general (32 bits)
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;

    uint32_t si;        // Source Index
    uint32_t di;        // Destination Index
} t_registros_cpu;

#endif /* UTILS_REGISTROS_H_ */
