#ifndef TIENDA_CADENA_H
#define TIENDA_CADENA_H

#include "configuracion.h"

/*
 * Ejecuta la tienda hasta que se solicita salir o hay un error. Devuelve
 * EXIT_SUCCESS al salir de forma ordenada o EXIT_FAILURE si hay un error.
 * Los argumentos tienda y entrada, si se indica, pertenecen a config. Con
 * entrada nula la tienda abre la lista. Si el historial existe, debe ser regular y vacío.
 */
int tienda_cadena_ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                          const ConfigTienda *entrada);

#endif
