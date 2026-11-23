#include "cierre.h"
#include "red.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/*
 * Pide a las tiendas comprobar una por una las ventas que resultaron de la recuperación.
 * Devuelve true si cada una comprueba todos los identificadores e importes.
 */
static bool verificar(const Configuracion *config, const ConfigTienda *propia,
                      const char *cadena, const CambioLista *lista, int fin, int64_t vence) {
    for (size_t fila = 0; ; ++fila) {
        char respuesta[RED_MAX_LINEA + 1], datos[RED_MAX_LINEA + 1];
        if (!red_pedir(propia, fin, vence, datos, "FICHA %s %zu %" PRIu32 "\n",
                       cadena, fila, lista->version)) {
            return false;
        }
        if (strcmp(datos, "FIN\n") == 0) {
            return true;
        }
        if (strncmp(datos, "FICHA ", 6) != 0) {
            return false;
        }
        datos[strlen(datos) - 1] = '\0';
        for (size_t i = 0; i < lista->cantidad; ++i) {
            const ConfigTienda *par = &config->tiendas[lista->orden[i]];
            if (!red_pedir(par, fin, vence, respuesta, "COTEJA %s %s %" PRIu32 "\n",
                           cadena, datos + 6, lista->version) || strcmp(respuesta, "COINCIDE\n") != 0) {
                return false;
            }
        }
    }
}

/*
 * Pide detener la difusión ordinaria para intercambiar las ventas faltantes
 * y comprobar que todas las tiendas conocen las mismas ventas e importes.
 * Devuelve true si todas guardan su preparación y la primera guarda la decisión.
 */
static bool preparar(const Configuracion *config, const ConfigTienda *propia,
                     const char *cadena, const CambioLista *lista, int fin, int64_t vence) {
    char respuesta[RED_MAX_LINEA + 1];
    for (size_t i = 0; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!red_pedir(par, fin, vence, respuesta, "CONGELAR_CIERRE %s %" PRIu32 "\n",
                       cadena, lista->version) || strcmp(respuesta, "CONGELADA\n") != 0) {
            return false;
        }
    }
    if (!recuperacion_sincronizar(config, cadena, lista, fin, vence) ||
        !verificar(config, propia, cadena, lista, fin, vence)) {
        return false;
    }
    char resumen[RED_MAX_LINEA + 1];
    if (!red_pedir(propia, fin, vence, resumen, "RESUMEN_CIERRE %s %" PRIu32 "\n",
                   cadena, lista->version) || strncmp(resumen, "RESUMEN ", 8) != 0) {
        return false;
    }
    resumen[strlen(resumen) - 1] = '\0';
    // La lista empieza por la tienda que coordina. Su preparación se guarda primero.
    for (size_t i = 0; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!red_pedir(par, fin, vence, respuesta, "PREPARAR_CIERRE %s %" PRIu32 " %s\n",
                       cadena, lista->version, resumen + 8) || strcmp(respuesta, "PREPARADA\n") != 0) {
            return false;
        }
    }
    return red_pedir(propia, fin, vence, respuesta, "DECIDIR_CIERRE %s %" PRIu32 "\n",
                     cadena, lista->version) && strcmp(respuesta, "GUARDADA\n") == 0;
}

/*
 * Ejecuta un paso de cierre sobre la lista activa copiada al emisor.
 * En la distribución devuelve true si todas confirman que guardaron la decisión.
 * Los avisos de salida se intentan después y sus fallos se informan por separado.
 */
bool cierre_ejecutar(const Configuracion *config, const ConfigTienda *propia,
                     const CambioLista *lista, enum TrabajoCierre trabajo, int fin) {
    int64_t ahora = red_reloj_ms();
    if (ahora < 0 || !lista->cantidad) {
        return false;
    }
    const char *cadena = config->cadenas[propia->cadena].id;
    int64_t vence = ahora + RECUPERACION_MS;
    char respuesta[RED_MAX_LINEA + 1];
    if (trabajo == AVISAR_CIERRE) {
        const ConfigTienda *primera = &config->tiendas[lista->orden[0]];
        return red_pedir(primera, fin, vence, respuesta, "AVISO_CIERRE %s %" PRIu32 " %s\n",
                         cadena, lista->version, propia->id) && strcmp(respuesta, "AVISADA\n") == 0;
    }
    if (trabajo == CONCILIAR_CIERRE && !preparar(config, propia, cadena, lista, fin, vence)) {
        return false;
    }
    vence = red_reloj_ms() + RECUPERACION_MS;
    // Las demás guardan la decisión antes de que se envíen los avisos de salida.
    for (size_t i = 1; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!red_pedir(par, fin, vence, respuesta, "DECIDIR_CIERRE %s %" PRIu32 "\n",
                       cadena, lista->version) || strcmp(respuesta, "GUARDADA\n") != 0) {
            return false;
        }
    }
    for (size_t i = 1; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!red_pedir(par, fin, vence, respuesta, "SALIR_CIERRE %s %" PRIu32 "\n",
                       cadena, lista->version) || strcmp(respuesta, "SALIDA\n") != 0) {
            fprintf(stderr, "Salida sin confirmar de %s\n", par->id);
        }
    }
    return true;
}

// Recuperación de un cierre acreditado por un par

/*
 * Lee los miembros y ventas que ofrece el par y los envía al propietario local.
 * Devuelve true cuando el propietario confirma que guardó la evidencia.
 */
static bool copiar_evidencia(const ConfigTienda *propia, const ConfigTienda *par,
                             const char *cadena, const char *cabecera, int fin, int64_t vence) {
    char respuesta[RED_MAX_LINEA + 1], datos[RED_MAX_LINEA + 1];
    if (!red_pedir(propia, fin, vence, respuesta, "EVIDENCIA_INICIO %s %s", cadena, cabecera) ||
        strcmp(respuesta, "CONTINUA\n") != 0) {
        return false;
    }
    const char *consultas[] = {"CIERRE_MIEMBRO", "CIERRE_DATO"};
    const char *cargas[] = {"EVIDENCIA_MIEMBRO", "EVIDENCIA_VENTA"};
    for (size_t tipo = 0; tipo < 2; ++tipo) {
        for (size_t fila = 0; ; ++fila) {
            if (!red_pedir(par, fin, vence, datos, "%s %s %zu\n", consultas[tipo], cadena, fila)) {
                return false;
            }
            if (strcmp(datos, "FIN\n") == 0) {
                break;
            }
            if (!red_pedir(propia, fin, vence, respuesta, "%s %s %s", cargas[tipo], cadena, datos) ||
                strcmp(respuesta, "CONTINUA\n") != 0) {
                return false;
            }
        }
    }
    return red_pedir(propia, fin, vence, respuesta, "EVIDENCIA_FIN %s 0\n", cadena) &&
           strcmp(respuesta, "GUARDADA\n") == 0;
}

bool cierre_buscar(const Configuracion *config, const ConfigTienda *propia, int fin) {
    int64_t ahora = red_reloj_ms();
    if (ahora < 0) {
        return false;
    }
    int64_t vence = ahora + RECUPERACION_MS;
    const char *cadena = config->cadenas[propia->cadena].id;
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        const ConfigTienda *par = &config->tiendas[i];
        char respuesta[RED_MAX_LINEA + 1];
        if (par != propia && par->cadena == propia->cadena &&
            red_pedir(par, fin, vence, respuesta, "PRUEBA_CIERRE %s 0\n", cadena) &&
            strncmp(respuesta, "CERRADA ", 8) == 0 &&
            copiar_evidencia(propia, par, cadena, respuesta + 8, fin, vence)) {
            return true;
        }
    }
    return false;
}
