#ifndef UTILS_INSTRUCCIONES_H_
#define UTILS_INSTRUCCIONES_H_

#define INSTRUCCION_MAX_PARAMS 3
#define INSTRUCCION_PARAM_LEN  64

/**
 * Codigos de instruccion que el CPU decodifica.
 * Se extiende a medida que se sumen instrucciones en futuros checkpoints.
 */
typedef enum {
    INST_NOOP,
    INST_SET,
    INST_MOV_IN,
    INST_MOV_OUT,
    INST_SUM,
    INST_SUB,
    INST_JNZ,
    INST_COPY_MEM,
    INST_SLEEP,
    INST_WAIT,
    INST_SIGNAL,
    INST_MUTEX_CREATE,
    INST_MUTEX_LOCK,
    INST_MUTEX_UNLOCK,
    INST_MEM_ALLOC,
    INST_MEM_FREE,
    INST_IO_STDIN,
    INST_IO_STDOUT,
    INST_INIT_PROC,
    INST_EXIT
} t_codigo_instruccion;

/**
 * Instruccion parseada lista para ejecutar.
 * El CPU la arma en la etapa Decode.
 */
typedef struct {
    t_codigo_instruccion codigo;
    char parametros[INSTRUCCION_MAX_PARAMS][INSTRUCCION_PARAM_LEN];
    int cantidad_parametros;
} t_instruccion;

/**
 * Devuelve el nombre legible de un codigo de instruccion.
 */
const char* cod_instruccion_to_string(t_codigo_instruccion codigo);

/**
 * Devuelve los parametros de una instruccion como string separado por espacios.
 */
char* parametros_to_string(t_instruccion* inst);

#endif /* UTILS_INSTRUCCIONES_H_ */
