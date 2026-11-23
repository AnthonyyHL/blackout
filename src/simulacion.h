#ifndef SIMULACION_H
#define SIMULACION_H

#include <stdbool.h>

/*
 * Devuelve EXIT_SUCCESS si acredita los cierres. Una petición de detención
 * por SIGINT (Ctrl+C) o SIGTERM devuelve 130 si termina sin errores.
 * En los demás casos devuelve EXIT_FAILURE.
 */
int simulacion_ejecutar(const char *ruta, bool reanudar);

#endif
