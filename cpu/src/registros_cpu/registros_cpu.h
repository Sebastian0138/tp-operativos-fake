#ifndef CPU_REGISTROS_CPU_H_
#define CPU_REGISTROS_CPU_H_

#include <stdint.h>
#include <utils/registros.h>

uint8_t get_registro(struct t_cpu* cpu, char* nombre);
void set_registro(struct t_cpu* cpu, char* nombre, uint8_t valor);
uint32_t get_registro_32(struct t_cpu* cpu, char* nombre);
void set_registro_32(struct t_cpu* cpu, char* nombre, uint32_t valor);

// Tamanio en bytes del registro: 1 para AX..DX, 4 para el resto (E**, SI, DI, PC)
uint32_t tamanio_registro(char* nombre);

#endif /* CPU_REGISTROS_CPU_H_ */
