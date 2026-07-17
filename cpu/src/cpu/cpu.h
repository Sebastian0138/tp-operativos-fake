#ifndef CPU_CPU_H_
#define CPU_CPU_H_

#include <stdint.h>
#include <stdbool.h>
#include <commons/log.h>
#include <commons/collections/list.h>
#include <utils/registros.h>
#include <utils/segmento.h>

/**
 * Un Memory Stick conocido por la CPU (segun la topologia que informa KM).
 * Su rango de direcciones fisicas globales es [base, base + tamanio).
 */
typedef struct {
    uint32_t id;
    uint32_t base;
    uint32_t tamanio;
    char* ip;
    char* puerto;
    int fd;             // conexion directa CPU -> stick
} t_stick_cpu;

struct t_cpu {
    t_registros_cpu registros;
    int socket_kernel;
    int socket_memoria;
    t_list* sticks;     // t_stick_cpu* ordenados por base (topologia de KM)
    uint32_t id_cpu;
    uint32_t segment_max_size;
    bool ejecutando;
    uint32_t pid_actual;
    t_list* tabla_segmentos;
};

struct t_cpu* cpu_create(int socket_kernel, int socket_memoria, uint32_t id_cpu, uint32_t segment_max_size);
void cpu_destroy(struct t_cpu* cpu);
void atender_peticiones_kernel_scheduler(struct t_cpu* cpu, t_log* logger);

#endif /* CPU_CPU_H_ */
