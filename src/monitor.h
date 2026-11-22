#ifndef MONITOR_H
#define MONITOR_H

#include "configuracion.h"

/* Una consulta local ya tiene un historial validado. Con NULL se usa TCP. */
int monitor_mostrar(const Configuracion *config, const ConfigTienda *tienda, bool ventas,
                    bool (*local)(void *, const char *, char *), void *contexto);

#endif
