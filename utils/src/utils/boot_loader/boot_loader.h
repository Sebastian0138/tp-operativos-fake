#ifndef BOOT_LOADER_H
#define BOOT_LOADER_H

#include <commons/log.h>
#include <commons/config.h>
#include <stdbool.h>

/**
 * @brief Valida que un config contenga todas las claves requeridas.
 * 
 * @param config El config a verificar.
 * @param required_keys Un arreglo de strings con los nombres de las claves.
 * @param num_keys La cantidad de claves en el arreglo.
 * @param boot_logger Logger donde se informarán las claves faltantes.
 * @return true si todas las claves están presentes, false si falta alguna.
 */
bool validate_config_rules(t_config* config, char** required_keys, int num_keys, t_log* boot_logger);

/**
 * @brief Inicializa el módulo, el logger y el config.
 * 
 * @param module_name Nombre del módulo (se usará para los archivos de log).
 * @param config_path Ruta del archivo de configuración.
 * @param required_keys Arreglo de claves requeridas.
 * @param num_keys Cantidad de claves requeridas.
 * @param out_config Puntero donde se almacenará el config inicializado.
 * @param out_logger Puntero donde se almacenará el logger principal inicializado.
 * @return true si la inicialización fue exitosa, false en caso de error.
 */
bool system_boot(const char* module_name, const char* config_path, char** required_keys, int num_keys, t_config** out_config, t_log** out_logger);

#endif /* BOOT_LOADER_H */
