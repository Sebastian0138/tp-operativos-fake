#ifndef KERNEL_MEMORY_GESTOR_MEMORIA_H_
#define KERNEL_MEMORY_GESTOR_MEMORIA_H_

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <commons/collections/list.h>
#include <commons/log.h>

#include "../config/kernel_memory_config.h"
#include "../gestor_procesos/kernel_memory_gestor_procesos.h"

/**
 * Un Memory Stick registrado. Su rango global es [base, base + tamanio).
 * fd_datos es la conexion KM -> stick para lecturas/escrituras
 * (camino STDIN/STDOUT y compactacion).
 */
typedef struct {
    uint32_t id;
    uint32_t tamanio;
    uint32_t base;
    char* ip;
    char* puerto;
    int fd_datos;
    bool conectado;
} t_stick;

/**
 * Un segmento ocupado en la memoria global (esquema de segmentacion pura).
 * Espeja la entrada correspondiente en la tabla de segmentos del proceso.
 */
typedef struct {
    uint32_t pid;
    uint32_t id_segmento;
    uint32_t base;
    uint32_t tamanio;
} t_segmento_fisico;

/**
 * Un segmento que fue movido a SWAP (proceso suspendido).
 * Los bloques se asignan por segmento: un bloque nunca mezcla dos segmentos.
 */
typedef struct {
    uint32_t pid;
    uint32_t id_segmento;
    uint32_t tamanio;
    t_list* bloques;    // uint32_t* numeros de bloque, en orden
} t_segmento_swap;

/**
 * Resultado de un intento de creacion de segmento.
 */
typedef enum {
    CREAR_SEGMENTO_OK,
    CREAR_SEGMENTO_SIN_ESPACIO,          // no hay espacio total suficiente (o supera SEGMENT_MAX_SIZE)
    CREAR_SEGMENTO_NECESITA_COMPACTACION // hay espacio total pero no contiguo
} t_resultado_crear_segmento;

/**
 * Administrador de la memoria de usuario: sticks conectados, segmentos
 * ocupados y huecos derivados. Toda operacion es thread-safe.
 */
typedef struct {
    t_list* sticks;      // t_stick*
    t_list* segmentos;   // t_segmento_fisico*
    uint32_t memoria_total;
    pthread_mutex_t mutex;

    // Canal de notificaciones KM -> KS (segunda conexion del scheduler); -1 si no esta
    int fd_notif_ks;
    pthread_mutex_t mutex_notif;

    // SWAP conectado (-1 si no hay). Los bloques los administra este modulo.
    int fd_swap;
    uint32_t swap_block_size;
    uint32_t swap_cant_bloques;
    bool* swap_bloques_usados;      // bitmap
    t_list* segmentos_swap;         // t_segmento_swap* (segmentos de procesos suspendidos)
    pthread_mutex_t mutex_swap;

    t_kernel_memory_config* config;
    t_gestor_procesos* gestor_procesos;
    t_log* logger;
} t_gestor_memoria;

t_gestor_memoria* gestor_memoria_create(t_kernel_memory_config* config, t_gestor_procesos* gestor_procesos, t_log* logger);
void gestor_memoria_destroy(t_gestor_memoria* gm);

// Canal de notificaciones hacia el Kernel Scheduler
void gestor_memoria_set_notif_ks(t_gestor_memoria* gm, int fd);

// Registro de sticks. registrar_stick abre la conexion de datos KM->stick,
// amplia la memoria total y notifica al KS que hay mas memoria.
// Devuelve el id asignado o -1 si fallo.
int gestor_memoria_registrar_stick(t_gestor_memoria* gm, uint32_t tamanio, char* ip, char* puerto);
// Marca el stick como caido y notifica MEMORIA_CORRUPTA al KS.
void gestor_memoria_stick_desconectado(t_gestor_memoria* gm, int id_stick);

// Espacio libre real (total - ocupado)
uint32_t gestor_memoria_espacio_libre(t_gestor_memoria* gm);

// Segmentos
t_resultado_crear_segmento gestor_memoria_crear_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t tamanio);
bool gestor_memoria_eliminar_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento);
void gestor_memoria_finalizar_proceso(t_gestor_memoria* gm, uint32_t pid);

// Compactacion: reordena todos los segmentos al principio de la memoria,
// moviendo los datos entre sticks y actualizando todas las tablas de segmentos.
// Solo debe llamarse con las CPUs desalojadas (protocolo DESALOJO_OK).
void gestor_memoria_compactar(t_gestor_memoria* gm);

// Lectura/escritura fisica global: divide el pedido entre los sticks
// involucrados y consolida el resultado como una unica operacion.
bool gestor_memoria_leer_fisico(t_gestor_memoria* gm, uint32_t dir_fisica, uint32_t tamanio, void* destino);
bool gestor_memoria_escribir_fisico(t_gestor_memoria* gm, uint32_t dir_fisica, uint32_t tamanio, const void* origen);

// Traduccion de direccion logica de un proceso a fisica global.
// Devuelve false si el segmento no existe o el acceso se sale del mismo.
bool gestor_memoria_traducir(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, uint32_t* dir_fisica_out);

// Lectura/escritura del camino de IO (KS -> KM) por direccion LOGICA.
// Resuelve tanto segmentos en memoria como segmentos swapeados (el proceso
// pudo haberse suspendido mientras esperaba la IO).
// Deja en *dir_fisica_out la direccion fisica usada (o UINT32_MAX si fue swap).
bool gestor_memoria_io_rw(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, void* buffer, bool escribir, uint32_t* dir_fisica_out);

// --- SWAP / mediano plazo ---

// Registra el modulo SWAP conectado (fd bidireccional, tamanio total y de bloque).
void gestor_memoria_registrar_swap(t_gestor_memoria* gm, int fd, uint32_t tamanio_total, uint32_t block_size);

// Mueve todos los segmentos del proceso a bloques de SWAP (de a un segmento,
// liberando la memoria a medida que se copia). Devuelve false si no hay SWAP
// o no alcanzan los bloques libres.
bool gestor_memoria_suspender_proceso(t_gestor_memoria* gm, uint32_t pid);

// Restaura todos los segmentos del proceso desde SWAP usando el algoritmo de
// huecos. Devuelve false si no entran sin disparar una compactacion (en ese
// caso no modifica nada).
bool gestor_memoria_dessuspender_proceso(t_gestor_memoria* gm, uint32_t pid);

// Topologia para las CPUs: serializa la lista de sticks conectados.
// El caller debe liberar el buffer con free(). Formato:
// cant(u32) + por stick: id(u32) tamanio(u32) base(u32) ip_len(u32) ip port_len(u32) port
void* gestor_memoria_serializar_topologia(t_gestor_memoria* gm, uint32_t* tamanio_out);

#endif /* KERNEL_MEMORY_GESTOR_MEMORIA_H_ */
