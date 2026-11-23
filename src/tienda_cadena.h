#ifndef TIENDA_CADENA_H
#define TIENDA_CADENA_H

#include "configuracion.h"

/*
 * Ejecuta la tienda hasta que se solicita salir o hay un error. Devuelve
 * EXIT_SUCCESS al salir de forma ordenada o EXIT_FAILURE si hay un error.
 * Los argumentos tienda y entrada, si se indica, pertenecen a config. Con
 * entrada nula, una tienda nueva abre la lista. Un historial existente se
 * valida y recupera con los pares antes de habilitar la admisión.
 */
int tienda_cadena_ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                          const ConfigTienda *entrada);
int tienda_cadena_consultar_cierre(const Configuracion *config, const ConfigTienda *tienda);
int tienda_independiente_ejecutar(const Configuracion *config, const ConfigTienda *tienda);
int tienda_consultar_historial(const Configuracion *config, const ConfigTienda *tienda, bool ventas);

typedef enum { ARCHIVO_NUEVO, ARCHIVO_ABIERTO, ARCHIVO_CERRADO, ARCHIVO_BLOQUEADO } EstadoArchivoTienda;

EstadoArchivoTienda tienda_inspeccionar_historial(const Configuracion *config, const ConfigTienda *tienda,
                                                  const char *clave, int64_t importe, bool *registrada,
                                                  uint64_t *excluidas);

#endif
