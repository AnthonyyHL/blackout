#ifndef TIENDA_LOCAL_H
#define TIENDA_LOCAL_H

#include "configuracion.h"
#include <stdbool.h>
#include <stddef.h>
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
    TIENDA_META_INCOMPATIBLE,
    TIENDA_SIN_MEMORIA,
    TIENDA_CONFLICTO
} ResultadoTiendaLocal;

typedef struct {
    int64_t importe;
    char clave[CONFIG_MAX_ID + 1];
} VentaLocal;

typedef struct {
    int fd_historial; // -1 indica que la tienda no tiene un historial abierto.
    int64_t meta;
    // En una tienda independiente, la meta se compara con el ingreso propio.
    int64_t ingreso_propio;
    bool cerrada_por_meta;
    bool historial_por_verificar; // Una escritura fallida bloquea los registros nuevos.
    // La posición en el archivo identifica la venta en compras y consultas.
    VentaLocal *ventas;
    size_t cantidad, capacidad;
} TiendaLocal;

#define TIENDA_LOCAL_INICIAL { .fd_historial = -1 }

// La instancia operativa necesita acceso exclusivo a un historial de archivo regular.
ResultadoTiendaLocal tienda_local_abrir(TiendaLocal *tienda, const char *ruta,
                                        int64_t meta);
// El modo histórico abre un archivo existente para lectura.
ResultadoTiendaLocal tienda_local_abrir_consulta(TiendaLocal *tienda, const char *ruta,
                                                 int64_t meta, bool historico);
ResultadoTiendaLocal tienda_local_registrar(TiendaLocal *tienda, int64_t importe);
// Un reintento con la misma clave e importe recupera el ID original.
ResultadoTiendaLocal tienda_local_comprar(TiendaLocal *tienda, const char *clave, int64_t importe, size_t *id);
ResultadoTiendaLocal tienda_local_cerrar(TiendaLocal *tienda);

#endif
