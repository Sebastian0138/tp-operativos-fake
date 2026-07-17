#include <utils/protocolo.h>

const char* codigo_mensaje_to_string(t_codigo_mensaje codigo) {
    switch (codigo) {
        case MENSAJE_HANDSHAKE:            return "HANDSHAKE";
        case MENSAJE_ENVIAR_PID:           return "ENVIAR_PID";
        case MENSAJE_PEDIR_INSTRUCCION:    return "PEDIR_INSTRUCCION";
        case MENSAJE_DEVOLVER_INSTRUCCION: return "DEVOLVER_INSTRUCCION";
        case MENSAJE_SYSCALL:              return "SYSCALL";
        case MENSAJE_FIN_IO:               return "FIN_IO";
        case MENSAJE_INTERRUPCION:         return "INTERRUPCION";
        case MENSAJE_CONTEXTO:             return "CONTEXTO";
        case MENSAJE_OK:                   return "OK";
        case MENSAJE_ERROR:                return "ERROR";
        case MENSAJE_LEER_MEMORIA:         return "LEER_MEMORIA";
        case MENSAJE_ESCRIBIR_MEMORIA:     return "ESCRIBIR_MEMORIA";
        case MENSAJE_ESPACIO_LIBRE:        return "ESPACIO_LIBRE";
        case MENSAJE_ACTUALIZAR_CONTEXTO:  return "ACTUALIZAR_CONTEXTO";
        case MENSAJE_CREAR_SEGMENTO:       return "CREAR_SEGMENTO";
        case MENSAJE_ELIMINAR_SEGMENTO:    return "ELIMINAR_SEGMENTO";
        case MENSAJE_FINALIZAR_PROCESO:    return "FINALIZAR_PROCESO";
        case MENSAJE_COMPACTACION:         return "COMPACTACION";
        case MENSAJE_DESALOJO_OK:          return "DESALOJO_OK";
        case MENSAJE_INFORMAR_TAMANIO:     return "INFORMAR_TAMANIO";
        case MENSAJE_TOPOLOGIA_STICKS:     return "TOPOLOGIA_STICKS";
        case MENSAJE_MEMORIA_CORRUPTA:     return "MEMORIA_CORRUPTA";
        case MENSAJE_MAS_MEMORIA:          return "MAS_MEMORIA";
        case MENSAJE_SUSPENDER_PROCESO:    return "SUSPENDER_PROCESO";
        case MENSAJE_DESSUSPENDER_PROCESO: return "DESSUSPENDER_PROCESO";
        default:                           return "DESCONOCIDO";
    }
}

const char* codigo_syscall_to_string(t_codigo_syscall codigo) {
    switch (codigo) {
        case SYSCALL_SLEEP:        return "SLEEP";
        case SYSCALL_STDIN:        return "STDIN";
        case SYSCALL_STDOUT:       return "STDOUT";
        case SYSCALL_MUTEX_CREATE: return "MUTEX_CREATE";
        case SYSCALL_MUTEX_LOCK:   return "MUTEX_LOCK";
        case SYSCALL_MUTEX_UNLOCK: return "MUTEX_UNLOCK";
        case SYSCALL_INIT_PROC:    return "INIT_PROC";
        case SYSCALL_EXIT:         return "EXIT";
        case SYSCALL_MEM_ALLOC:    return "MEM_ALLOC";
        case SYSCALL_MEM_FREE:     return "MEM_FREE";
        case SYSCALL_SEG_FAULT:    return "SEG_FAULT";
        default:                   return "DESCONOCIDO";
    }
}
