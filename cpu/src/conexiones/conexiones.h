#ifndef CPU_CONEXIONES_H
#define CPU_CONEXIONES_H

#include <commons/log.h>
#include <stdint.h>
#include <stdbool.h>

struct t_cpu;

int conectar_a_kernel_scheduler(const char* ip, const char* puerto, uint32_t id_cpu, t_log* logger);
int conectar_a_kernel_memory(const char* ip, const char* puerto, uint32_t id_cpu, uint32_t* segment_max_size, t_log* logger);

void enviar_syscall_sleep(struct t_cpu* cpu, uint32_t tiempo, t_log* logger);
void enviar_syscall_exit(struct t_cpu* cpu, t_log* logger);
// El proceso hizo un acceso invalido a memoria: el KS lo finaliza con motivo SEG_FAULT
void enviar_syscall_seg_fault(struct t_cpu* cpu, t_log* logger);
void enviar_syscall_stdin(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tam, t_log* logger);
void enviar_syscall_stdout(struct t_cpu* cpu, uint32_t dir_logica, uint32_t tam, t_log* logger);
void enviar_contexto_a_ks(struct t_cpu* cpu, t_log* logger);

void enviar_syscall_mutex_create(struct t_cpu* cpu, char* nombre_mutex, t_log* logger);
void enviar_syscall_mutex_lock(struct t_cpu* cpu, char* nombre_mutex, t_log* logger);
void enviar_syscall_mutex_unlock(struct t_cpu* cpu, char* nombre_mutex, t_log* logger);
void enviar_syscall_mem_alloc(struct t_cpu* cpu, uint32_t id_segmento, uint32_t tamanio, t_log* logger);
void enviar_syscall_mem_free(struct t_cpu* cpu, uint32_t id_segmento, t_log* logger);
void enviar_syscall_init_proc(struct t_cpu* cpu, char* archivo, int32_t prioridad, t_log* logger);

void solicitar_contexto_a_km(struct t_cpu* cpu, t_log* logger);

// Pide a KM la topologia de sticks y se conecta a los que todavia no conocia.
void actualizar_topologia_sticks(struct t_cpu* cpu, t_log* logger);

// Lectura/escritura fisica global: rutea cada direccion al stick correcto,
// dividiendo el pedido si cruza el borde de un stick, y consolida el resultado.
bool leer_memoria_fisica(struct t_cpu* cpu, uint32_t dir_fisica, uint32_t tamanio, void* destino, t_log* logger);
bool escribir_memoria_fisica(struct t_cpu* cpu, uint32_t dir_fisica, uint32_t tamanio, const void* origen, t_log* logger);

#endif // CPU_CONEXIONES_H
