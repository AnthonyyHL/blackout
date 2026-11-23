#ifndef TIENDA_CADENA_H
#define TIENDA_CADENA_H

#include "configuracion.h"
#include "cierre.h"

/*
 * Ejecuta la tienda hasta que se solicita salir o hay un error. Devuelve
 * EXIT_SUCCESS al salir de forma ordenada o EXIT_FAILURE si hay un error.
 * Los argumentos tienda y entrada, si se indica, pertenecen a config. Con
 * entrada nula, una tienda nueva abre la lista de participantes. Con un
 * historial previo, las compras esperan a que se recupere la lista con los pares.
 */
int tienda_cadena_ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                          const ConfigTienda *entrada);
// Sirve por TCP la evidencia del historial sin admitir compras nuevas.
int tienda_cadena_consultar_cierre(const Configuracion *config, const ConfigTienda *tienda);
int tienda_independiente_ejecutar(const Configuracion *config, const ConfigTienda *tienda);
// Consulta el historial en modo de solo lectura desde el mismo proceso.
int tienda_consultar_historial(const Configuracion *config, const ConfigTienda *tienda, bool ventas);

typedef enum { ARCHIVO_NUEVO, ARCHIVO_ABIERTO, ARCHIVO_CERRADO, ARCHIVO_BLOQUEADO } EstadoArchivoTienda;

// Los resultados opcionales identifican una compra registrada y las tiendas excluidas del cierre.
EstadoArchivoTienda tienda_inspeccionar_historial(const Configuracion *config, const ConfigTienda *tienda,
                                                  const char *clave, int64_t importe, bool *registrada,
                                                  uint64_t *excluidas);

typedef struct {
    bool disponible, cerrada, cierre_pendiente, recuperacion_pendiente;
    size_t ventas_propias, ventas_sin_confirmacion;
    int64_t ingreso_propio, total_conocido;
    EvidenciaCierre certificado;
} ResumenHistorial;

// Lee con los mismos validadores de recuperación. El llamador libera certificado.items.
ResumenHistorial tienda_resumir_historial(const Configuracion *config, const ConfigTienda *tienda);
bool tienda_certificados_coinciden(const EvidenciaCierre *a, const EvidenciaCierre *b);

#endif
