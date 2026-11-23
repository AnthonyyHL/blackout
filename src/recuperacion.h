#ifndef RECUPERACION_H
#define RECUPERACION_H

#include "configuracion.h"

enum { RECUPERACION_MS = 5000 };

/*
 * Una vista es la lista ordenada de tiendas que participan en la cadena.
 * La versión distingue esa lista de las anteriores.
 */
typedef struct {
    uint32_t version, base; // Versión propuesta y versión de partida.
    char tipo; // Distingue baja individual, baja múltiple, retorno y reinicio.
    size_t objetivo, cantidad;
    unsigned char orden[CONFIG_MAX_TIENDAS]; // Índices de tiendas en la configuración.
    uint64_t bajas; // Un bit por posición retirada de la lista de partida.
} CambioLista;

// El emisor pide el cambio. Cada propietario comprueba y guarda su parte del acuerdo.
bool recuperacion_acordar(const Configuracion *config, const ConfigTienda *propia,
                          const CambioLista *cambio, int fin);

// Busca una lista vigente o coordina el arranque de los miembros de la lista guardada.
bool recuperacion_buscar(const Configuracion *config, const ConfigTienda *propia,
                         const CambioLista *guardada, int fin);

bool recuperacion_sincronizar(const Configuracion *config, const char *cadena,
                             const CambioLista *lista, int fin, int64_t vence);

#endif
