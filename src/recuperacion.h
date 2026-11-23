#ifndef RECUPERACION_H
#define RECUPERACION_H

#include "configuracion.h"

enum { RECUPERACION_MS = 5000 };

typedef struct {
    uint32_t version, base;
    char tipo;
    size_t objetivo, cantidad;
    unsigned char orden[CONFIG_MAX_TIENDAS];
    uint64_t bajas; // Un bit por posición de la vista base en las bajas del ensayo.
} CambioLista;

// El emisor coordina una sola ronda. Los propietarios validan y registran cada paso.
bool recuperacion_acordar(const Configuracion *config, const ConfigTienda *propia,
                          const CambioLista *cambio, int fin);

// Busca una lista vigente o coordina el arranque de los miembros de la lista guardada.
bool recuperacion_buscar(const Configuracion *config, const ConfigTienda *propia,
                         const CambioLista *guardada, int fin);

bool recuperacion_sincronizar(const Configuracion *config, const char *cadena,
                             const CambioLista *lista, int fin, int64_t vence);

#endif
