#ifndef KERNEL_MEMORY_MEMORIA_INSTRUCCIONES_H_
#define KERNEL_MEMORY_MEMORIA_INSTRUCCIONES_H_

#include <stdint.h>

/**
 * Mock de memoria de instrucciones para CP2.
 * Devuelve la linea de instruccion correspondiente al PC del proceso.
 *
 * @param pid  PID del proceso
 * @param pc   Program Counter (numero de linea, base 0)
 * @return String con la instruccion (memoria del caller), o NULL si no hay mas
 */
char* memoria_instrucciones_fetch(uint32_t pid, uint32_t pc);

#endif /* KERNEL_MEMORY_MEMORIA_INSTRUCCIONES_H_ */
