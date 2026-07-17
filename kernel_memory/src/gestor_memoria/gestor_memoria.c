#include "gestor_memoria.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <utils/protocolo.h>
#include <utils/segmento.h>
#include <utils/sockets/sockets.h>

// ============================================================
// Creacion / destruccion
// ============================================================

t_gestor_memoria* gestor_memoria_create(t_kernel_memory_config* config, t_gestor_procesos* gestor_procesos, t_log* logger) {
    t_gestor_memoria* gm = malloc(sizeof(t_gestor_memoria));
    if (gm == NULL) return NULL;

    gm->sticks = list_create();
    gm->segmentos = list_create();
    gm->memoria_total = 0;
    pthread_mutex_init(&gm->mutex, NULL);

    gm->fd_notif_ks = -1;
    pthread_mutex_init(&gm->mutex_notif, NULL);

    gm->fd_swap = -1;
    gm->swap_block_size = 0;
    gm->swap_cant_bloques = 0;
    gm->swap_bloques_usados = NULL;
    gm->segmentos_swap = list_create();
    pthread_mutex_init(&gm->mutex_swap, NULL);

    gm->config = config;
    gm->gestor_procesos = gestor_procesos;
    gm->logger = logger;
    return gm;
}

static void stick_destroy(void* elemento) {
    t_stick* stick = (t_stick*) elemento;
    if (stick->fd_datos != -1) close(stick->fd_datos);
    free(stick->ip);
    free(stick->puerto);
    free(stick);
}

static void segmento_swap_destroy(void* elemento) {
    t_segmento_swap* seg = (t_segmento_swap*) elemento;
    list_destroy_and_destroy_elements(seg->bloques, free);
    free(seg);
}

void gestor_memoria_destroy(t_gestor_memoria* gm) {
    if (gm == NULL) return;
    list_destroy_and_destroy_elements(gm->sticks, stick_destroy);
    list_destroy_and_destroy_elements(gm->segmentos, free);
    list_destroy_and_destroy_elements(gm->segmentos_swap, segmento_swap_destroy);
    if (gm->fd_swap != -1) close(gm->fd_swap);
    free(gm->swap_bloques_usados);
    pthread_mutex_destroy(&gm->mutex);
    pthread_mutex_destroy(&gm->mutex_notif);
    pthread_mutex_destroy(&gm->mutex_swap);
    free(gm);
}

// ============================================================
// Notificaciones al Kernel Scheduler
// ============================================================

void gestor_memoria_set_notif_ks(t_gestor_memoria* gm, int fd) {
    pthread_mutex_lock(&gm->mutex_notif);
    gm->fd_notif_ks = fd;
    pthread_mutex_unlock(&gm->mutex_notif);
}

static void notificar_ks(t_gestor_memoria* gm, t_codigo_mensaje codigo, uint32_t* payload) {
    pthread_mutex_lock(&gm->mutex_notif);
    if (gm->fd_notif_ks != -1) {
        send(gm->fd_notif_ks, &codigo, sizeof(t_codigo_mensaje), 0);
        if (payload != NULL) {
            send(gm->fd_notif_ks, payload, sizeof(uint32_t), 0);
        }
    }
    pthread_mutex_unlock(&gm->mutex_notif);
}

// ============================================================
// Sticks
// ============================================================

int gestor_memoria_registrar_stick(t_gestor_memoria* gm, uint32_t tamanio, char* ip, char* puerto) {
    // Abrir la conexion de datos KM -> stick antes de publicarlo
    int fd_datos = crear_conexion(ip, puerto);
    if (fd_datos == -1) {
        log_error(gm->logger, "No se pudo abrir el canal de datos hacia el stick %s:%s", ip, puerto);
        return -1;
    }
    int32_t handshake = HANDSHAKE_KERNEL_MEMORY;
    send(fd_datos, &handshake, sizeof(int32_t), 0);

    t_stick* stick = malloc(sizeof(t_stick));
    stick->tamanio = tamanio;
    stick->ip = strdup(ip);
    stick->puerto = strdup(puerto);
    stick->fd_datos = fd_datos;
    stick->conectado = true;

    pthread_mutex_lock(&gm->mutex);
    stick->id = list_size(gm->sticks);
    stick->base = gm->memoria_total;      // se agrega al final de la lista, ampliando el total
    gm->memoria_total += tamanio;
    list_add(gm->sticks, stick);
    uint32_t nuevo_total = gm->memoria_total;
    pthread_mutex_unlock(&gm->mutex);

    log_info(gm->logger, "## Memory Stick de %u bytes Conectada", tamanio);
    log_info(gm->logger, "Stick %u registrado: base global %u, %s:%s — memoria total: %u bytes",
             stick->id, stick->base, ip, puerto, nuevo_total);

    // Notificar al KS que se dispone de mas memoria
    notificar_ks(gm, MENSAJE_MAS_MEMORIA, &nuevo_total);
    return stick->id;
}

void gestor_memoria_stick_desconectado(t_gestor_memoria* gm, int id_stick) {
    pthread_mutex_lock(&gm->mutex);
    for (int i = 0; i < list_size(gm->sticks); i++) {
        t_stick* stick = list_get(gm->sticks, i);
        if ((int) stick->id == id_stick && stick->conectado) {
            stick->conectado = false;
            if (stick->fd_datos != -1) {
                close(stick->fd_datos);
                stick->fd_datos = -1;
            }
            pthread_mutex_unlock(&gm->mutex);
            log_error(gm->logger, "Memory Stick %d desconectado: memoria corrupta", id_stick);
            notificar_ks(gm, MENSAJE_MEMORIA_CORRUPTA, NULL);
            return;
        }
    }
    pthread_mutex_unlock(&gm->mutex);
}

// ============================================================
// Huecos y segmentos
// ============================================================

static int comparar_por_base(const void* a, const void* b) {
    const t_segmento_fisico* sa = *(const t_segmento_fisico**) a;
    const t_segmento_fisico* sb = *(const t_segmento_fisico**) b;
    return (sa->base > sb->base) - (sa->base < sb->base);
}

// Devuelve un array ordenado por base de los segmentos ocupados (el caller lo libera).
// Debe llamarse con gm->mutex tomado.
static t_segmento_fisico** segmentos_ordenados(t_gestor_memoria* gm, int* cant_out) {
    int cant = list_size(gm->segmentos);
    t_segmento_fisico** arr = malloc(sizeof(t_segmento_fisico*) * (cant > 0 ? cant : 1));
    for (int i = 0; i < cant; i++) {
        arr[i] = list_get(gm->segmentos, i);
    }
    qsort(arr, cant, sizeof(t_segmento_fisico*), comparar_por_base);
    *cant_out = cant;
    return arr;
}

// Busca el hueco segun ALLOCATION_STRATEGY (BEST/WORST) donde entre `tamanio`.
// Devuelve true y deja la base del hueco elegido en *base_out.
// Debe llamarse con gm->mutex tomado.
static bool buscar_hueco(t_gestor_memoria* gm, uint32_t tamanio, uint32_t* base_out) {
    bool best = strcmp(gm->config->allocation_strategy, "WORST") != 0; // default BEST

    int cant;
    t_segmento_fisico** arr = segmentos_ordenados(gm, &cant);

    bool encontrado = false;
    uint32_t mejor_base = 0;
    uint32_t mejor_tamanio = 0;

    uint32_t cursor = 0;
    for (int i = 0; i <= cant; i++) {
        uint32_t fin_hueco = (i < cant) ? arr[i]->base : gm->memoria_total;
        if (fin_hueco > cursor) {
            uint32_t tam_hueco = fin_hueco - cursor;
            if (tam_hueco >= tamanio) {
                bool mejora = !encontrado
                    || (best  && tam_hueco < mejor_tamanio)
                    || (!best && tam_hueco > mejor_tamanio);
                if (mejora) {
                    encontrado = true;
                    mejor_base = cursor;
                    mejor_tamanio = tam_hueco;
                }
            }
        }
        if (i < cant) {
            uint32_t fin_seg = arr[i]->base + arr[i]->tamanio;
            if (fin_seg > cursor) cursor = fin_seg;
        }
    }

    free(arr);
    if (encontrado) *base_out = mejor_base;
    return encontrado;
}

// Espacio libre con gm->mutex tomado
static uint32_t espacio_libre_sin_lock(t_gestor_memoria* gm) {
    uint32_t ocupado = 0;
    for (int i = 0; i < list_size(gm->segmentos); i++) {
        t_segmento_fisico* seg = list_get(gm->segmentos, i);
        ocupado += seg->tamanio;
    }
    return gm->memoria_total - ocupado;
}

uint32_t gestor_memoria_espacio_libre(t_gestor_memoria* gm) {
    pthread_mutex_lock(&gm->mutex);
    uint32_t libre = espacio_libre_sin_lock(gm);
    pthread_mutex_unlock(&gm->mutex);
    return libre;
}

// Refleja el alta del segmento en la tabla de segmentos del contexto del proceso
static void tabla_proceso_agregar(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t base, uint32_t tamanio) {
    gestor_lock(gm->gestor_procesos);
    t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gm->gestor_procesos, pid);
    if (proc != NULL) {
        t_segmento* seg = malloc(sizeof(t_segmento));
        seg->id_segmento = id_segmento;
        seg->base = base;
        seg->tamanio = tamanio;
        seg->limite = base + tamanio - 1;
        list_add(proc->contexto->tabla_segmentos, seg);
    }
    gestor_unlock(gm->gestor_procesos);
}

static bool coincide_segmento(uint32_t pid, uint32_t id_segmento, void* elemento) {
    t_segmento_fisico* seg = (t_segmento_fisico*) elemento;
    return seg->pid == pid && seg->id_segmento == id_segmento;
}

t_resultado_crear_segmento gestor_memoria_crear_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento, uint32_t tamanio) {
    if (tamanio == 0 || tamanio > (uint32_t) gm->config->segment_max_size) {
        log_error(gm->logger, "PID: %u - Segmento %u de %u bytes invalido (SEGMENT_MAX_SIZE=%d)",
                  pid, id_segmento, tamanio, gm->config->segment_max_size);
        return CREAR_SEGMENTO_SIN_ESPACIO;
    }

    pthread_mutex_lock(&gm->mutex);

    uint32_t base;
    if (!buscar_hueco(gm, tamanio, &base)) {
        uint32_t libre = espacio_libre_sin_lock(gm);
        pthread_mutex_unlock(&gm->mutex);
        if (libre >= tamanio) {
            log_info(gm->logger, "PID: %u - Hay %u bytes libres pero no contiguos para %u: se necesita compactar",
                     pid, libre, tamanio);
            return CREAR_SEGMENTO_NECESITA_COMPACTACION;
        }
        log_error(gm->logger, "PID: %u - Sin espacio para segmento %u de %u bytes (libre: %u)",
                  pid, id_segmento, tamanio, libre);
        return CREAR_SEGMENTO_SIN_ESPACIO;
    }

    t_segmento_fisico* seg = malloc(sizeof(t_segmento_fisico));
    seg->pid = pid;
    seg->id_segmento = id_segmento;
    seg->base = base;
    seg->tamanio = tamanio;
    list_add(gm->segmentos, seg);

    pthread_mutex_unlock(&gm->mutex);

    tabla_proceso_agregar(gm, pid, id_segmento, base, tamanio);

    log_info(gm->logger, "## PID: %u - Segmento Creado %u - Tamaño: %u", pid, id_segmento, tamanio);
    return CREAR_SEGMENTO_OK;
}

bool gestor_memoria_eliminar_segmento(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento) {
    // Quitar de los segmentos ocupados (marca el espacio como libre)
    pthread_mutex_lock(&gm->mutex);
    bool encontrado = false;
    for (int i = 0; i < list_size(gm->segmentos); i++) {
        if (coincide_segmento(pid, id_segmento, list_get(gm->segmentos, i))) {
            t_segmento_fisico* seg = list_remove(gm->segmentos, i);
            free(seg);
            encontrado = true;
            break;
        }
    }
    pthread_mutex_unlock(&gm->mutex);

    if (!encontrado) {
        log_warning(gm->logger, "PID: %u - MEM_FREE de segmento inexistente %u", pid, id_segmento);
        return false;
    }

    // Quitar la entrada de la tabla de segmentos del proceso
    gestor_lock(gm->gestor_procesos);
    t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gm->gestor_procesos, pid);
    if (proc != NULL) {
        for (int i = 0; i < list_size(proc->contexto->tabla_segmentos); i++) {
            t_segmento* seg = list_get(proc->contexto->tabla_segmentos, i);
            if (seg->id_segmento == id_segmento) {
                list_remove(proc->contexto->tabla_segmentos, i);
                free(seg);
                break;
            }
        }
    }
    gestor_unlock(gm->gestor_procesos);

    log_info(gm->logger, "PID: %u - Segmento %u eliminado", pid, id_segmento);
    return true;
}

void gestor_memoria_finalizar_proceso(t_gestor_memoria* gm, uint32_t pid) {
    // Liberar todos los segmentos del PID en memoria principal
    pthread_mutex_lock(&gm->mutex);
    for (int i = list_size(gm->segmentos) - 1; i >= 0; i--) {
        t_segmento_fisico* seg = list_get(gm->segmentos, i);
        if (seg->pid == pid) {
            list_remove(gm->segmentos, i);
            free(seg);
        }
    }
    pthread_mutex_unlock(&gm->mutex);

    // Liberar tambien sus bloques de SWAP si estaba suspendido
    pthread_mutex_lock(&gm->mutex_swap);
    for (int i = list_size(gm->segmentos_swap) - 1; i >= 0; i--) {
        t_segmento_swap* sw = list_get(gm->segmentos_swap, i);
        if (sw->pid == pid) {
            for (int b = 0; b < list_size(sw->bloques); b++) {
                uint32_t* nro = list_get(sw->bloques, b);
                gm->swap_bloques_usados[*nro] = false;
            }
            list_remove(gm->segmentos_swap, i);
            segmento_swap_destroy(sw);
        }
    }
    pthread_mutex_unlock(&gm->mutex_swap);

    // Liberar el contexto y demas estructuras del proceso
    gestor_remover_proceso(gm->gestor_procesos, pid);
    log_info(gm->logger, "PID: %u - Proceso finalizado: segmentos y estructuras liberados", pid);
}

// ============================================================
// Lectura / escritura fisica global (dividida entre sticks)
// ============================================================

// Debe llamarse con gm->mutex tomado. Devuelve el stick que contiene la direccion.
static t_stick* stick_para_direccion(t_gestor_memoria* gm, uint32_t dir) {
    for (int i = 0; i < list_size(gm->sticks); i++) {
        t_stick* stick = list_get(gm->sticks, i);
        if (dir >= stick->base && dir < stick->base + stick->tamanio) {
            return stick;
        }
    }
    return NULL;
}

// Ejecuta la operacion contra un unico stick por su canal de datos.
static bool stick_operar(t_gestor_memoria* gm, t_stick* stick, bool escribir,
                         uint32_t dir_local, uint32_t tamanio, void* buffer) {
    if (!stick->conectado || stick->fd_datos == -1) return false;

    t_codigo_mensaje cod = escribir ? MENSAJE_ESCRIBIR_MEMORIA : MENSAJE_LEER_MEMORIA;
    send(stick->fd_datos, &cod, sizeof(t_codigo_mensaje), 0);
    send(stick->fd_datos, &dir_local, sizeof(uint32_t), 0);
    send(stick->fd_datos, &tamanio, sizeof(uint32_t), 0);
    if (escribir) {
        send(stick->fd_datos, buffer, tamanio, 0);
    }

    t_codigo_mensaje respuesta;
    if (recv(stick->fd_datos, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) <= 0) return false;
    if (respuesta != MENSAJE_OK) return false;

    if (!escribir) {
        if (recv(stick->fd_datos, buffer, tamanio, MSG_WAITALL) <= 0) return false;
    }
    return true;
}

// Divide la operacion global entre los sticks que abarca y la consolida.
static bool operar_fisico(t_gestor_memoria* gm, bool escribir, uint32_t dir_fisica, uint32_t tamanio, void* buffer) {
    pthread_mutex_lock(&gm->mutex);

    uint32_t restante = tamanio;
    uint32_t dir = dir_fisica;
    char* cursor = (char*) buffer;
    bool ok = true;

    while (restante > 0 && ok) {
        t_stick* stick = stick_para_direccion(gm, dir);
        if (stick == NULL) {
            log_error(gm->logger, "Direccion fisica %u fuera de la memoria total (%u bytes)", dir, gm->memoria_total);
            ok = false;
            break;
        }

        uint32_t dir_local = dir - stick->base;
        uint32_t disponible_en_stick = stick->tamanio - dir_local;
        uint32_t tramo = restante < disponible_en_stick ? restante : disponible_en_stick;

        ok = stick_operar(gm, stick, escribir, dir_local, tramo, cursor);

        dir += tramo;
        cursor += tramo;
        restante -= tramo;
    }

    pthread_mutex_unlock(&gm->mutex);
    return ok;
}

bool gestor_memoria_leer_fisico(t_gestor_memoria* gm, uint32_t dir_fisica, uint32_t tamanio, void* destino) {
    return operar_fisico(gm, false, dir_fisica, tamanio, destino);
}

bool gestor_memoria_escribir_fisico(t_gestor_memoria* gm, uint32_t dir_fisica, uint32_t tamanio, const void* origen) {
    return operar_fisico(gm, true, dir_fisica, tamanio, (void*) origen);
}

// ============================================================
// Traduccion logica -> fisica (para el camino KS/IO)
// ============================================================

bool gestor_memoria_traducir(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio, uint32_t* dir_fisica_out) {
    uint32_t seg_max = (uint32_t) gm->config->segment_max_size;
    uint32_t num_segmento = dir_logica / seg_max;
    uint32_t desplazamiento = dir_logica % seg_max;

    pthread_mutex_lock(&gm->mutex);
    t_segmento_fisico* encontrado = NULL;
    for (int i = 0; i < list_size(gm->segmentos); i++) {
        t_segmento_fisico* seg = list_get(gm->segmentos, i);
        if (seg->pid == pid && seg->id_segmento == num_segmento) {
            encontrado = seg;
            break;
        }
    }

    if (encontrado == NULL || desplazamiento + tamanio > encontrado->tamanio) {
        // Sin log de error: el caller puede resolverlo contra SWAP (proceso suspendido)
        pthread_mutex_unlock(&gm->mutex);
        return false;
    }

    *dir_fisica_out = encontrado->base + desplazamiento;
    pthread_mutex_unlock(&gm->mutex);
    return true;
}

// ============================================================
// Compactacion
// ============================================================

void gestor_memoria_compactar(t_gestor_memoria* gm) {
    log_info(gm->logger, "## Inicio de compactación");

    pthread_mutex_lock(&gm->mutex);

    int cant;
    t_segmento_fisico** arr = segmentos_ordenados(gm, &cant);

    uint32_t cursor = 0;
    for (int i = 0; i < cant; i++) {
        t_segmento_fisico* seg = arr[i];
        if (seg->base != cursor) {
            // Mover los datos: se lee el segmento completo y se escribe en su
            // nueva base (puede cambiar parcial o totalmente de stick).
            char* buffer = malloc(seg->tamanio);

            // Se opera con los sticks directamente (sin re-tomar gm->mutex):
            // replicamos la logica de operar_fisico de forma inline.
            uint32_t restante = seg->tamanio;
            uint32_t dir = seg->base;
            char* p = buffer;
            while (restante > 0) {
                t_stick* stick = stick_para_direccion(gm, dir);
                if (stick == NULL) break;
                uint32_t dir_local = dir - stick->base;
                uint32_t tramo = restante < stick->tamanio - dir_local ? restante : stick->tamanio - dir_local;
                stick_operar(gm, stick, false, dir_local, tramo, p);
                dir += tramo; p += tramo; restante -= tramo;
            }

            restante = seg->tamanio;
            dir = cursor;
            p = buffer;
            while (restante > 0) {
                t_stick* stick = stick_para_direccion(gm, dir);
                if (stick == NULL) break;
                uint32_t dir_local = dir - stick->base;
                uint32_t tramo = restante < stick->tamanio - dir_local ? restante : stick->tamanio - dir_local;
                stick_operar(gm, stick, true, dir_local, tramo, p);
                dir += tramo; p += tramo; restante -= tramo;
            }

            free(buffer);

            // Actualizar la tabla de segmentos del proceso duenio
            gestor_lock(gm->gestor_procesos);
            t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gm->gestor_procesos, seg->pid);
            if (proc != NULL) {
                for (int j = 0; j < list_size(proc->contexto->tabla_segmentos); j++) {
                    t_segmento* entrada = list_get(proc->contexto->tabla_segmentos, j);
                    if (entrada->id_segmento == seg->id_segmento) {
                        entrada->base = cursor;
                        entrada->limite = cursor + seg->tamanio - 1;
                        break;
                    }
                }
            }
            gestor_unlock(gm->gestor_procesos);

            seg->base = cursor;
        }
        cursor += seg->tamanio;
    }

    free(arr);
    pthread_mutex_unlock(&gm->mutex);

    // Tiempo de espera configurado antes de dar por finalizada la compactacion
    usleep(gm->config->compaction_delay * 1000);

    log_info(gm->logger, "## Fin de compactación");
}

// ============================================================
// SWAP / mediano plazo
// ============================================================

void gestor_memoria_registrar_swap(t_gestor_memoria* gm, int fd, uint32_t tamanio_total, uint32_t block_size) {
    pthread_mutex_lock(&gm->mutex_swap);
    gm->fd_swap = fd;
    gm->swap_block_size = block_size;
    gm->swap_cant_bloques = block_size > 0 ? tamanio_total / block_size : 0;
    gm->swap_bloques_usados = calloc(gm->swap_cant_bloques, sizeof(bool));
    pthread_mutex_unlock(&gm->mutex_swap);

    log_info(gm->logger, "## SWAP conectado: %u bytes en %u bloques de %u",
             tamanio_total, gm->swap_cant_bloques, block_size);
}

// Operaciones de bloque contra el modulo SWAP. Deben llamarse con mutex_swap tomado.
static bool swap_escribir_bloque(t_gestor_memoria* gm, uint32_t nro_bloque, void* buffer) {
    t_codigo_mensaje cod = MENSAJE_ESCRIBIR_MEMORIA;
    send(gm->fd_swap, &cod, sizeof(t_codigo_mensaje), 0);
    send(gm->fd_swap, &nro_bloque, sizeof(uint32_t), 0);
    send(gm->fd_swap, buffer, gm->swap_block_size, 0);

    t_codigo_mensaje respuesta;
    return recv(gm->fd_swap, &respuesta, sizeof(t_codigo_mensaje), MSG_WAITALL) > 0
        && respuesta == MENSAJE_OK;
}

static bool swap_leer_bloque(t_gestor_memoria* gm, uint32_t nro_bloque, void* buffer) {
    t_codigo_mensaje cod = MENSAJE_LEER_MEMORIA;
    send(gm->fd_swap, &cod, sizeof(t_codigo_mensaje), 0);
    send(gm->fd_swap, &nro_bloque, sizeof(uint32_t), 0);
    return recv(gm->fd_swap, buffer, gm->swap_block_size, MSG_WAITALL) > 0;
}

// Cuenta bloques libres. Debe llamarse con mutex_swap tomado.
static uint32_t swap_bloques_libres(t_gestor_memoria* gm) {
    uint32_t libres = 0;
    for (uint32_t i = 0; i < gm->swap_cant_bloques; i++) {
        if (!gm->swap_bloques_usados[i]) libres++;
    }
    return libres;
}

// Busca un segmento swapeado por (pid, id). Debe llamarse con mutex_swap tomado.
static t_segmento_swap* buscar_segmento_swap(t_gestor_memoria* gm, uint32_t pid, uint32_t id_segmento) {
    for (int i = 0; i < list_size(gm->segmentos_swap); i++) {
        t_segmento_swap* seg = list_get(gm->segmentos_swap, i);
        if (seg->pid == pid && seg->id_segmento == id_segmento) return seg;
    }
    return NULL;
}

bool gestor_memoria_suspender_proceso(t_gestor_memoria* gm, uint32_t pid) {
    if (gm->fd_swap == -1) {
        log_error(gm->logger, "PID: %u - No hay SWAP conectado: no se puede suspender", pid);
        return false;
    }

    // Snapshot de los segmentos del proceso (id, base, tamanio)
    pthread_mutex_lock(&gm->mutex);
    t_list* snapshot = list_create();
    uint32_t bytes_totales = 0;
    for (int i = 0; i < list_size(gm->segmentos); i++) {
        t_segmento_fisico* seg = list_get(gm->segmentos, i);
        if (seg->pid == pid) {
            t_segmento_fisico* copia = malloc(sizeof(t_segmento_fisico));
            *copia = *seg;
            list_add(snapshot, copia);
            bytes_totales += seg->tamanio;
        }
    }
    pthread_mutex_unlock(&gm->mutex);

    // Pre-chequeo de bloques: los bloques se asignan por segmento (sin compartir)
    pthread_mutex_lock(&gm->mutex_swap);
    uint32_t bloques_necesarios = 0;
    for (int i = 0; i < list_size(snapshot); i++) {
        t_segmento_fisico* seg = list_get(snapshot, i);
        bloques_necesarios += (seg->tamanio + gm->swap_block_size - 1) / gm->swap_block_size;
    }
    if (bloques_necesarios > swap_bloques_libres(gm)) {
        pthread_mutex_unlock(&gm->mutex_swap);
        list_destroy_and_destroy_elements(snapshot, free);
        log_error(gm->logger, "PID: %u - SWAP sin bloques suficientes (%u necesarios)", pid, bloques_necesarios);
        return false;
    }
    pthread_mutex_unlock(&gm->mutex_swap);

    // Mover de a UN segmento: copiar a swap y recien ahi liberar su memoria
    for (int i = 0; i < list_size(snapshot); i++) {
        t_segmento_fisico* seg = list_get(snapshot, i);

        char* buffer = malloc(seg->tamanio);
        gestor_memoria_leer_fisico(gm, seg->base, seg->tamanio, buffer);

        pthread_mutex_lock(&gm->mutex_swap);
        t_segmento_swap* sw = malloc(sizeof(t_segmento_swap));
        sw->pid = pid;
        sw->id_segmento = seg->id_segmento;
        sw->tamanio = seg->tamanio;
        sw->bloques = list_create();

        uint32_t restante = seg->tamanio;
        char* cursor = buffer;
        char* bloque_buffer = malloc(gm->swap_block_size);
        for (uint32_t b = 0; b < gm->swap_cant_bloques && restante > 0; b++) {
            if (gm->swap_bloques_usados[b]) continue;

            uint32_t tramo = restante < gm->swap_block_size ? restante : gm->swap_block_size;
            memcpy(bloque_buffer, cursor, tramo);
            memset(bloque_buffer + tramo, 0, gm->swap_block_size - tramo);
            swap_escribir_bloque(gm, b, bloque_buffer);

            gm->swap_bloques_usados[b] = true;
            uint32_t* nro = malloc(sizeof(uint32_t));
            *nro = b;
            list_add(sw->bloques, nro);

            cursor += tramo;
            restante -= tramo;
        }
        free(bloque_buffer);
        list_add(gm->segmentos_swap, sw);
        pthread_mutex_unlock(&gm->mutex_swap);
        free(buffer);

        // Segmento ya copiado: liberar su memoria principal y su entrada
        // en la tabla de segmentos del proceso.
        pthread_mutex_lock(&gm->mutex);
        for (int j = 0; j < list_size(gm->segmentos); j++) {
            t_segmento_fisico* s = list_get(gm->segmentos, j);
            if (s->pid == pid && s->id_segmento == seg->id_segmento) {
                list_remove(gm->segmentos, j);
                free(s);
                break;
            }
        }
        pthread_mutex_unlock(&gm->mutex);

        gestor_lock(gm->gestor_procesos);
        t_proceso_km* proc = gestor_buscar_proceso_sin_lock(gm->gestor_procesos, pid);
        if (proc != NULL) {
            for (int j = 0; j < list_size(proc->contexto->tabla_segmentos); j++) {
                t_segmento* entrada = list_get(proc->contexto->tabla_segmentos, j);
                if (entrada->id_segmento == seg->id_segmento) {
                    list_remove(proc->contexto->tabla_segmentos, j);
                    free(entrada);
                    break;
                }
            }
        }
        gestor_unlock(gm->gestor_procesos);
    }

    int cant = list_size(snapshot);
    list_destroy_and_destroy_elements(snapshot, free);
    log_info(gm->logger, "PID: %u - Proceso suspendido: %d segmentos (%u bytes) movidos a SWAP",
             pid, cant, bytes_totales);
    return true;
}

bool gestor_memoria_dessuspender_proceso(t_gestor_memoria* gm, uint32_t pid) {
    // Snapshot de los segmentos swapeados del proceso
    pthread_mutex_lock(&gm->mutex_swap);
    t_list* swapeados = list_create();
    for (int i = 0; i < list_size(gm->segmentos_swap); i++) {
        t_segmento_swap* seg = list_get(gm->segmentos_swap, i);
        if (seg->pid == pid) list_add(swapeados, seg);
    }
    pthread_mutex_unlock(&gm->mutex_swap);

    if (list_size(swapeados) == 0) {
        list_destroy(swapeados);
        return false;
    }

    // Reservar TODOS los huecos con el algoritmo configurado, sin compactar.
    // Si alguno no entra, se revierte lo reservado y se responde que no se puede.
    pthread_mutex_lock(&gm->mutex);
    t_list* reservados = list_create();  // t_segmento_fisico* ya insertados en gm->segmentos
    bool entra_todo = true;
    for (int i = 0; i < list_size(swapeados) && entra_todo; i++) {
        t_segmento_swap* sw = list_get(swapeados, i);
        uint32_t base;
        if (buscar_hueco(gm, sw->tamanio, &base)) {
            t_segmento_fisico* nuevo = malloc(sizeof(t_segmento_fisico));
            nuevo->pid = pid;
            nuevo->id_segmento = sw->id_segmento;
            nuevo->base = base;
            nuevo->tamanio = sw->tamanio;
            list_add(gm->segmentos, nuevo);
            list_add(reservados, nuevo);
        } else {
            entra_todo = false;
        }
    }

    if (!entra_todo) {
        for (int i = 0; i < list_size(reservados); i++) {
            list_remove_element(gm->segmentos, list_get(reservados, i));
            free(list_get(reservados, i));
        }
        pthread_mutex_unlock(&gm->mutex);
        list_destroy(reservados);
        list_destroy(swapeados);
        return false;
    }
    pthread_mutex_unlock(&gm->mutex);

    // Restaurar los datos de cada segmento desde sus bloques y regenerar la
    // tabla de segmentos del proceso.
    for (int i = 0; i < list_size(swapeados); i++) {
        t_segmento_swap* sw = list_get(swapeados, i);
        t_segmento_fisico* destino = list_get(reservados, i);

        char* buffer = malloc(sw->tamanio);

        pthread_mutex_lock(&gm->mutex_swap);
        char* bloque_buffer = malloc(gm->swap_block_size);
        uint32_t copiado = 0;
        for (int b = 0; b < list_size(sw->bloques) && copiado < sw->tamanio; b++) {
            uint32_t* nro = list_get(sw->bloques, b);
            swap_leer_bloque(gm, *nro, bloque_buffer);
            uint32_t tramo = sw->tamanio - copiado < gm->swap_block_size
                           ? sw->tamanio - copiado : gm->swap_block_size;
            memcpy(buffer + copiado, bloque_buffer, tramo);
            copiado += tramo;
            gm->swap_bloques_usados[*nro] = false;   // liberar el bloque
        }
        free(bloque_buffer);
        list_remove_element(gm->segmentos_swap, sw);
        pthread_mutex_unlock(&gm->mutex_swap);

        gestor_memoria_escribir_fisico(gm, destino->base, sw->tamanio, buffer);
        free(buffer);

        tabla_proceso_agregar(gm, pid, sw->id_segmento, destino->base, sw->tamanio);
        segmento_swap_destroy(sw);
    }

    int cant = list_size(reservados);
    list_destroy(reservados);
    list_destroy(swapeados);

    log_info(gm->logger, "PID: %u - Proceso des-suspendido: %d segmentos restaurados desde SWAP", pid, cant);
    return true;
}

// ============================================================
// Camino de IO (lectura/escritura por direccion logica)
// ============================================================

// Lee o escribe sobre un segmento swapeado, operando bloque a bloque.
static bool swap_io_rw(t_gestor_memoria* gm, uint32_t pid, uint32_t num_segmento, uint32_t desplazamiento,
                       uint32_t tamanio, void* buffer, bool escribir) {
    pthread_mutex_lock(&gm->mutex_swap);

    t_segmento_swap* sw = buscar_segmento_swap(gm, pid, num_segmento);
    if (sw == NULL || desplazamiento + tamanio > sw->tamanio) {
        pthread_mutex_unlock(&gm->mutex_swap);
        return false;
    }

    char* bloque_buffer = malloc(gm->swap_block_size);
    char* cursor = (char*) buffer;
    uint32_t restante = tamanio;
    uint32_t offset = desplazamiento;

    while (restante > 0) {
        int idx_bloque = offset / gm->swap_block_size;
        uint32_t off_local = offset % gm->swap_block_size;
        uint32_t tramo = gm->swap_block_size - off_local;
        if (tramo > restante) tramo = restante;

        uint32_t* nro = list_get(sw->bloques, idx_bloque);

        // Siempre se opera de a bloques completos (asi lo exige el modulo SWAP):
        // para escrituras parciales se lee, modifica y reescribe el bloque.
        swap_leer_bloque(gm, *nro, bloque_buffer);
        if (escribir) {
            memcpy(bloque_buffer + off_local, cursor, tramo);
            swap_escribir_bloque(gm, *nro, bloque_buffer);
        } else {
            memcpy(cursor, bloque_buffer + off_local, tramo);
        }

        cursor += tramo;
        offset += tramo;
        restante -= tramo;
    }

    free(bloque_buffer);
    pthread_mutex_unlock(&gm->mutex_swap);
    return true;
}

bool gestor_memoria_io_rw(t_gestor_memoria* gm, uint32_t pid, uint32_t dir_logica, uint32_t tamanio,
                          void* buffer, bool escribir, uint32_t* dir_fisica_out) {
    uint32_t dir_fisica;
    if (gestor_memoria_traducir(gm, pid, dir_logica, tamanio, &dir_fisica)) {
        *dir_fisica_out = dir_fisica;
        return escribir
            ? gestor_memoria_escribir_fisico(gm, dir_fisica, tamanio, buffer)
            : gestor_memoria_leer_fisico(gm, dir_fisica, tamanio, buffer);
    }

    // El segmento puede estar en SWAP (proceso suspendido durante la IO):
    // se opera directamente sobre los bloques para no perder la operacion.
    uint32_t seg_max = (uint32_t) gm->config->segment_max_size;
    uint32_t num_segmento = dir_logica / seg_max;
    uint32_t desplazamiento = dir_logica % seg_max;

    if (swap_io_rw(gm, pid, num_segmento, desplazamiento, tamanio, buffer, escribir)) {
        log_info(gm->logger, "PID: %u - %s sobre segmento %u en SWAP (offset %u, %u bytes)",
                 pid, escribir ? "Escritura" : "Lectura", num_segmento, desplazamiento, tamanio);
        *dir_fisica_out = UINT32_MAX;
        return true;
    }
    return false;
}

// ============================================================
// Topologia de sticks para las CPUs
// ============================================================

void* gestor_memoria_serializar_topologia(t_gestor_memoria* gm, uint32_t* tamanio_out) {
    pthread_mutex_lock(&gm->mutex);

    // Calcular tamanio del buffer
    uint32_t total = sizeof(uint32_t); // cant
    uint32_t cant = 0;
    for (int i = 0; i < list_size(gm->sticks); i++) {
        t_stick* stick = list_get(gm->sticks, i);
        if (!stick->conectado) continue;
        cant++;
        total += 3 * sizeof(uint32_t);                      // id, tamanio, base
        total += sizeof(uint32_t) + strlen(stick->ip);      // ip
        total += sizeof(uint32_t) + strlen(stick->puerto);  // puerto
    }

    char* buffer = malloc(total);
    char* p = buffer;

    memcpy(p, &cant, sizeof(uint32_t)); p += sizeof(uint32_t);
    for (int i = 0; i < list_size(gm->sticks); i++) {
        t_stick* stick = list_get(gm->sticks, i);
        if (!stick->conectado) continue;

        memcpy(p, &stick->id, sizeof(uint32_t)); p += sizeof(uint32_t);
        memcpy(p, &stick->tamanio, sizeof(uint32_t)); p += sizeof(uint32_t);
        memcpy(p, &stick->base, sizeof(uint32_t)); p += sizeof(uint32_t);

        uint32_t ip_len = strlen(stick->ip);
        memcpy(p, &ip_len, sizeof(uint32_t)); p += sizeof(uint32_t);
        memcpy(p, stick->ip, ip_len); p += ip_len;

        uint32_t port_len = strlen(stick->puerto);
        memcpy(p, &port_len, sizeof(uint32_t)); p += sizeof(uint32_t);
        memcpy(p, stick->puerto, port_len); p += port_len;
    }

    pthread_mutex_unlock(&gm->mutex);

    *tamanio_out = total;
    return buffer;
}
