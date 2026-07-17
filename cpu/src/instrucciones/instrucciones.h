#ifndef CPU_INSTRUCCIONES_H_
#define CPU_INSTRUCCIONES_H_

#include <stdint.h>
#include <stdbool.h>
#include <utils/instrucciones.h>
#include <commons/log.h>

char* cpu_fetch(struct t_cpu* cpu, t_log* logger);
int cpu_decode(const char* raw, t_instruccion* out);
int cpu_execute(struct t_cpu* cpu, t_instruccion* inst, t_log* logger);
bool cpu_check_interrupt(struct t_cpu* cpu, t_log* logger);

#endif /* CPU_INSTRUCCIONES_H_ */
