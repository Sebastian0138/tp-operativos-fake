#ifndef UTILS_PROTOCOLO_H_
#define UTILS_PROTOCOLO_H_

/**
 * Codigos de handshake: primer int32 que envia un cliente al conectarse
 * a cualquier servidor del sistema, para identificarse.
 */
#define HANDSHAKE_CPU             1
#define HANDSHAKE_IO              2
#define HANDSHAKE_SCHEDULER       3
#define HANDSHAKE_MEMORY_STICK    4
#define HANDSHAKE_SWAP            5
#define HANDSHAKE_SCHEDULER_NOTIF 6  // Segunda conexion KS->KM, canal exclusivo de notificaciones KM->KS
#define HANDSHAKE_KERNEL_MEMORY   7  // KM como cliente de un Memory Stick (canal de datos)

/**
 * Codigos de mensaje intercambiados entre modulos via sockets.
 * Todos los modulos incluyen este header para hablar el mismo "idioma".
 */
typedef enum {
    MENSAJE_HANDSHAKE,
    MENSAJE_ENVIAR_PID,
    MENSAJE_PEDIR_INSTRUCCION,
    MENSAJE_DEVOLVER_INSTRUCCION,
    MENSAJE_SYSCALL,
    MENSAJE_FIN_IO,
    MENSAJE_INTERRUPCION,
    MENSAJE_CONTEXTO,
    MENSAJE_OK,
    MENSAJE_ERROR,
    MENSAJE_LEER_MEMORIA,
    MENSAJE_ESCRIBIR_MEMORIA,
    MENSAJE_ESPACIO_LIBRE,
    MENSAJE_ACTUALIZAR_CONTEXTO,
    // --- CP3: opcodes nuevos (agregados al final, sin reordenar) ---
    MENSAJE_CREAR_SEGMENTO,      // KS -> KM  {pid, id_segmento, tamanio} -> OK | ERROR | COMPACTACION
    MENSAJE_ELIMINAR_SEGMENTO,   // KS -> KM  {pid, id_segmento} -> OK | ERROR
    MENSAJE_FINALIZAR_PROCESO,   // KS -> KM  {pid} -> OK
    MENSAJE_COMPACTACION,        // KM -> KS  (respuesta a CREAR_SEGMENTO: hace falta compactar)
    MENSAJE_DESALOJO_OK,         // KS -> KM  (todas las CPUs desalojadas, puede compactar)
    MENSAJE_INFORMAR_TAMANIO,    // Stick -> KM  {tamanio, ip, puerto}
    MENSAJE_TOPOLOGIA_STICKS,    // CPU -> KM (pedido) / KM -> CPU {cant, [id, tamanio, base, ip, puerto]...}
    MENSAJE_MEMORIA_CORRUPTA,    // KM -> KS (canal de notificaciones): stick desconectado -> BSOD
    MENSAJE_MAS_MEMORIA,         // KM -> KS (canal de notificaciones): {nuevo_total} se amplio la memoria
    // --- Entrega final: mediano plazo + SWAP ---
    MENSAJE_SUSPENDER_PROCESO,   // KS -> KM  {pid}: mover todos sus segmentos a SWAP y liberar memoria
    MENSAJE_DESSUSPENDER_PROCESO // KS -> KM  {pid}: restaurar segmentos desde SWAP (OK | ERROR si no entra sin compactar)
} t_codigo_mensaje;

/**
 * Codigos de syscall que el CPU envia al Kernel Scheduler
 * cuando una instruccion requiere atencion del kernel.
 */
typedef enum {
    SYSCALL_SLEEP,
    SYSCALL_STDIN,
    SYSCALL_STDOUT,
    SYSCALL_MUTEX_CREATE,
    SYSCALL_MUTEX_LOCK,
    SYSCALL_MUTEX_UNLOCK,
    SYSCALL_INIT_PROC,
    SYSCALL_EXIT,
    SYSCALL_MEM_ALLOC,
    SYSCALL_MEM_FREE,
    // --- CP3 ---
    SYSCALL_SEG_FAULT   // La MMU detecto un acceso invalido: el proceso debe finalizar con motivo SEG_FAULT
} t_codigo_syscall;

/**
 * Devuelve el nombre legible de un codigo de mensaje.
 * Util para logging.
 */
const char* codigo_mensaje_to_string(t_codigo_mensaje codigo);

/**
 * Devuelve el nombre legible de un codigo de syscall.
 * Util para logging.
 */
const char* codigo_syscall_to_string(t_codigo_syscall codigo);

#endif /* UTILS_PROTOCOLO_H_ */
