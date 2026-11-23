#ifndef MONITOR_H
#define MONITOR_H

#include "configuracion.h"

/*
 * Muestra lo que declara la tienda por TCP si local es NULL. Una consulta
 * histórica pasa un lector sobre el historial ya validado.
 * Devuelve EXIT_SUCCESS cuando completa los datos solicitados.
 */
int monitor_mostrar(const Configuracion *config, const ConfigTienda *tienda, bool ventas,
                    bool (*local)(void *, const char *, char *), void *contexto);

#endif
