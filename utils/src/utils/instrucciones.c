#include "instrucciones.h"

#include <string.h>
#include <stdio.h>

const char* cod_instruccion_to_string(t_codigo_instruccion codigo) {
    switch (codigo) {
        case INST_NOOP:          return "NOOP";
        case INST_SET:           return "SET";
        case INST_MOV_IN:        return "MOV_IN";
        case INST_MOV_OUT:       return "MOV_OUT";
        case INST_SUM:           return "SUM";
        case INST_SUB:           return "SUB";
        case INST_JNZ:           return "JNZ";
        case INST_COPY_MEM:      return "COPY_MEM";
        case INST_SLEEP:         return "SLEEP";
        case INST_MUTEX_CREATE:  return "MUTEX_CREATE";
        case INST_MUTEX_LOCK:    return "MUTEX_LOCK";
        case INST_MUTEX_UNLOCK:  return "MUTEX_UNLOCK";
        case INST_MEM_ALLOC:     return "MEM_ALLOC";
        case INST_MEM_FREE:      return "MEM_FREE";
        case INST_IO_STDIN:      return "IO_STDIN";
        case INST_IO_STDOUT:     return "IO_STDOUT";
        case INST_INIT_PROC:     return "INIT_PROC";
        case INST_EXIT:          return "EXIT";
        default:                 return "UNKNOWN";
    }
}

char* parametros_to_string(t_instruccion* inst) {
    if (inst->cantidad_parametros == 0) return "";
    static char buffer[256];
    buffer[0] = '\0';
    for (int i = 0; i < inst->cantidad_parametros; i++) {
        if (i > 0) strcat(buffer, " ");
        strcat(buffer, inst->parametros[i]);
    }
    return buffer;
}
