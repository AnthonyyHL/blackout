#ifndef TIENDA_LOCAL_H
#define TIENDA_LOCAL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TIENDA_OK,
    TIENDA_PARAMETRO_INVALIDO,
    TIENDA_IMPORTE_INVALIDO,
    TIENDA_DESBORDAMIENTO,
    TIENDA_CERRADA_POR_META,
    TIENDA_ERROR_APERTURA,
    TIENDA_ERROR_LECTURA,
    TIENDA_ERROR_ESCRITURA,
    TIENDA_ERROR_CIERRE,
    TIENDA_HISTORIAL_INVALIDO,
    TIENDA_HISTORIAL_POR_VERIFICAR,
    TIENDA_META_INCOMPATIBLE
} ResultadoTiendaLocal;

typedef struct {
    int fd_historial;
    int64_t meta;
    // En una tienda independiente, la meta se compara con el ingreso propio.
    int64_t ingreso_propio;
    bool cerrada_por_meta;
    bool historial_por_verificar;
} TiendaLocal;

#define TIENDA_LOCAL_INICIAL { .fd_historial = -1 }

// El historial es un archivo regular de uso exclusivo de la instancia.
ResultadoTiendaLocal tienda_local_abrir(TiendaLocal *tienda, const char *ruta,
                                        int64_t meta);
ResultadoTiendaLocal tienda_local_registrar(TiendaLocal *tienda, int64_t importe);
ResultadoTiendaLocal tienda_local_cerrar(TiendaLocal *tienda);

#endif
