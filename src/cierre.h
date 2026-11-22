#ifndef CIERRE_H
#define CIERRE_H

#include "recuperacion.h"

enum TrabajoCierre { SIN_CIERRE, AVISAR_CIERRE, CONCILIAR_CIERRE, DISTRIBUIR_CIERRE };

typedef struct {
    size_t origen;
    int64_t secuencia, importe;
} ItemCierre;

typedef struct {
    uint32_t version;
    int64_t total;
    size_t cantidad, participantes, excluidas, miembros_leidos, items_leidos;
    unsigned char miembros[CONFIG_MAX_TIENDAS];
    ItemCierre *items; // Copia del conjunto certificado para una tienda excluida.
} EvidenciaCierre;

/* El emisor intercambia copias. El propietario decide qué guardar. */
bool cierre_ejecutar(const Configuracion *config, const ConfigTienda *propia,
                     const CambioLista *lista, enum TrabajoCierre trabajo, int fin);
bool cierre_buscar(const Configuracion *config, const ConfigTienda *propia, int fin);

#endif
