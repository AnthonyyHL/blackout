#include "recuperacion.h"
#include "red.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool leer_positivo(const char *texto, int64_t *valor) {
    errno = 0;
    char *fin;
    intmax_t numero = strtoimax(texto, &fin, 10);
    if (errno == ERANGE || *fin || numero <= 0 || numero > INT64_MAX) {
        return false;
    }
    *valor = (int64_t)numero;
    return true;
}

static bool pedir(const ConfigTienda *par, int fin, int64_t vence, char *respuesta,
                  const char *formato, ...) {
    char linea[RED_MAX_LINEA + 1], error[128];
    va_list argumentos;
    va_start(argumentos, formato);
    int largo = vsnprintf(linea, sizeof linea, formato, argumentos);
    va_end(argumentos);
    return largo > 0 && largo <= RED_MAX_LINEA && !red_fin_solicitado(fin) &&
           red_intercambiar_hasta(par, linea, respuesta, error, sizeof error, vence);
}

static size_t lugar(const CambioLista *cambio, size_t tienda) {
    for (size_t i = 0; i < cambio->cantidad; ++i) {
        if (cambio->orden[i] == tienda) {
            return i;
        }
    }
    return cambio->cantidad;
}

// Inventarios de las tiendas

/*
 * Consulta si una tienda ya aplicó la venta. La versión identifica la lista
 * que se está comprobando. Devuelve 1 si confirma el importe, 0 si falta
 * y -1 si falla la consulta o se recibe una respuesta distinta de las esperadas.
 */
static int tiene_venta(const ConfigTienda *par, const char *cadena, const char *origen,
                       int64_t secuencia, int64_t importe, uint32_t version, int fin, int64_t vence) {
    char respuesta[RED_MAX_LINEA + 1], esperada[64];
    if (!pedir(par, fin, vence, respuesta, "TIENE %s %s %" PRId64 " %" PRIu32 "\n",
               cadena, origen, secuencia, version)) {
        return -1;
    }
    if (strcmp(respuesta, "FALTA\n") == 0) {
        return 0;
    }
    snprintf(esperada, sizeof esperada, "CONOCE %" PRId64 "\n", importe);
    return strcmp(respuesta, esperada) == 0 ? 1 : -1;
}

/*
 * Recorre la lista para una venta conocida por algún par.
 * Devuelve true si cada miembro ya la tenía o confirma que acaba de aplicarla.
 * Si el origen sigue presente, primero verifica que el importe coincide en su historial.
 */
static bool reproducir(const Configuracion *config, const char *cadena, const CambioLista *cambio,
                       size_t origen, int64_t secuencia, int64_t importe, int fin, int64_t vence) {
    size_t previa = lugar(cambio, origen); // cantidad señala un origen ausente de la lista.
    const char *id_origen = config->tiendas[origen].id;
    if (previa < cambio->cantidad &&
        tiene_venta(&config->tiendas[origen], cadena, id_origen, secuencia, importe,
                    cambio->version, fin, vence) != 1) {
        return false;
    }
    for (size_t i = 0; i < cambio->cantidad; ++i) {
        if (cambio->orden[i] == origen) {
            continue;
        }
        const ConfigTienda *par = &config->tiendas[cambio->orden[i]];
        int tiene = tiene_venta(par, cadena, id_origen, secuencia, importe,
                              cambio->version, fin, vence);
        if (tiene < 0) {
            return false;
        }
        // Un registro con el mismo importe permite avanzar sin volver a aplicar la venta.
        if (tiene == 1) {
            previa = i;
            continue;
        }
        char respuesta[RED_MAX_LINEA + 1], esperada[RED_MAX_LINEA + 1];
        if (cambio->version == 1) {
            snprintf(esperada, sizeof esperada, "APLICADA %s %s %" PRId64 " %" PRId64 " %zu\n",
                     cadena, id_origen, secuencia, importe, i);
        } else {
            snprintf(esperada, sizeof esperada,
                     "HECHA %s %s %" PRId64 " %" PRId64 " %zu %" PRIu32 "\n",
                     cadena, id_origen, secuencia, importe, i, cambio->version);
        }
        if (!pedir(par, fin, vence, respuesta,
                   "RECUP %s %s %" PRId64 " %" PRId64 " %zu %" PRIu32 "\n",
                   cadena, id_origen, secuencia, importe, previa, cambio->version) ||
            strcmp(respuesta, esperada) != 0) {
            return false;
        }
        previa = i;
    }
    return true;
}

/*
 * Pide a cada propietario sus ventas y las comprueba en las demás tiendas.
 * Devuelve true si todas confirman cada venta. Cuando reaparece el mismo
 * identificador en otro inventario, se comprueba de nuevo su importe.
 */
bool recuperacion_sincronizar(const Configuracion *config, const char *cadena,
                             const CambioLista *cambio, int fin, int64_t vence) {
    for (size_t i = 0; i < cambio->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[cambio->orden[i]];
        for (size_t fila = 0; ; ++fila) {
            char respuesta[RED_MAX_LINEA + 1], id[CONFIG_MAX_ID + 1];
            char secuencia_texto[20], importe_texto[20];
            int64_t secuencia, importe;
            char fin_linea;
            if (!pedir(par, fin, vence, respuesta, "FICHA %s %zu %" PRIu32 "\n",
                       cadena, fila, cambio->version)) {
                return false;
            }
            if (strcmp(respuesta, "FIN\n") == 0) {
                break;
            }
            if (sscanf(respuesta, "FICHA %32s %19[0-9] %19[0-9]%c",
                       id, secuencia_texto, importe_texto, &fin_linea) != 4 || fin_linea != '\n' ||
                !leer_positivo(secuencia_texto, &secuencia) ||
                !leer_positivo(importe_texto, &importe)) {
                return false;
            }
            const ConfigTienda *origen = configuracion_buscar(config, id);
            if (!origen || origen->cadena != par->cadena ||
                !reproducir(config, cadena, cambio, (size_t)(origen - config->tiendas),
                             secuencia, importe, fin, vence)) {
                return false;
            }
        }
    }
    return true;
}

// Acuerdo y búsqueda de una lista de participantes

/*
 * Pide comprobar el orden de la lista propuesta y, si hace falta, recuperar ventas.
 * Devuelve true cuando todas las tiendas confirman que instalaron la lista.
 * Si falla antes de instalar, intenta cancelar la propuesta en las tiendas alcanzables.
 */
bool recuperacion_acordar(const Configuracion *config, const ConfigTienda *propia,
                          const CambioLista *cambio, int fin) {
    const char *cadena = config->cadenas[propia->cadena].id;
    char objetivo[CONFIG_MAX_ID + 1];
    if (cambio->tipo == 'M') {
        snprintf(objetivo, sizeof objetivo, "%" PRIu64, cambio->bajas);
    } else {
        snprintf(objetivo, sizeof objetivo, "%s", config->tiendas[cambio->objetivo].id);
    }
    int64_t ahora = red_reloj_ms();
    if (ahora < 0 || cambio->cantidad == 0) {
        return false;
    }
    int64_t vence = ahora + RECUPERACION_MS;
    char respuesta[RED_MAX_LINEA + 1];
    bool listo = true;
    for (size_t i = 0; i < cambio->cantidad && listo; ++i) {
        const ConfigTienda *par = &config->tiendas[cambio->orden[i]];
        listo = pedir(par, fin, vence, respuesta, "PREPARAR %s %" PRIu32 " %c %s %zu %" PRIu32 "\n",
                      cadena, cambio->version, cambio->tipo, objetivo, cambio->cantidad, cambio->base) &&
                strcmp(respuesta, "PREPARADA\n") == 0;
        for (size_t j = 0; j < cambio->cantidad && listo; ++j) {
            listo = pedir(par, fin, vence, respuesta, "MIEMBRO %s %" PRIu32 " %zu %s\n",
                          cadena, cambio->version, j, config->tiendas[cambio->orden[j]].id) &&
                    strcmp(respuesta, "VERIFICADO\n") == 0;
        }
    }
    // Una baja cambia el recorrido. El retorno o reinicio también debe recuperar ventas.
    if (listo && cambio->tipo != 'B' && cambio->tipo != 'M') {
        listo = recuperacion_sincronizar(config, cadena, cambio, fin, vence);
    }
    if (!listo || red_reloj_ms() >= vence) {
        // Registrar la cancelación evita que un PREPARAR tardío vuelva a abrir la ronda.
        for (size_t i = 0; i < cambio->cantidad; ++i) {
            pedir(&config->tiendas[cambio->orden[i]], fin, vence + RECUPERACION_MS, respuesta,
                  "CANCELAR %s %" PRIu32 "\n", cadena, cambio->version);
        }
        return false;
    }
    // Si alguna tienda ya pudo guardar la lista, se conserva el resultado parcial.
    bool instalada = true;
    for (size_t i = 0; i < cambio->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[cambio->orden[i]];
        if (!pedir(par, fin, vence + RECUPERACION_MS, respuesta,
                   "INSTALAR %s %" PRIu32 "\n", cadena, cambio->version) ||
            strcmp(respuesta, "INSTALADA\n") != 0) {
            instalada = false;
        }
    }
    return instalada;
}

/*
 * Busca la primera tienda de la lista de mayor versión anunciada, si está activa.
 * Al pedirle retorno o reanudación, true solo indica que recibió una respuesta.
 * Si no hay tienda activa, la primera de la lista guardada intenta acordar
 * el reinicio cuando responden los miembros de esa versión.
 */
bool recuperacion_buscar(const Configuracion *config, const ConfigTienda *propia,
                         const CambioLista *guardada, int fin) {
    size_t yo = (size_t)(propia - config->tiendas);
    const char *cadena = config->cadenas[propia->cadena].id;
    int64_t ahora = red_reloj_ms();
    if (ahora < 0 || guardada->cantidad == 0 ||
        guardada->version == UINT32_MAX || guardada->base == UINT32_MAX) {
        return false;
    }
    int64_t vence = ahora + RECUPERACION_MS;
    uint32_t mayor = guardada->version;
    const ConfigTienda *entrada = NULL;
    bool incluida = true;
    size_t presentes = 1;
    char respuesta[RED_MAX_LINEA + 1];
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        const ConfigTienda *par = &config->tiendas[i];
        if (i == yo || par->cadena != propia->cadena) {
            continue;
        }
        int64_t numero_version;
        char version_texto[11], dentro[2], activa[2];
        char primera[CONFIG_MAX_ID + 1], lf;
        if (!pedir(par, fin, vence, respuesta, "VIGENTE %s %s\n", cadena, propia->id) ||
            sscanf(respuesta, "VIGENTE %10[0-9] %32s %1[01] %1[01]%c",
                   version_texto, primera, dentro, activa, &lf) != 5 || lf != '\n' ||
            !leer_positivo(version_texto, &numero_version) || numero_version > UINT32_MAX) {
            continue;
        }
        uint32_t version = (uint32_t)numero_version;
        if (version == guardada->version && lugar(guardada, i) < guardada->cantidad) {
            ++presentes;
        }
        if (version >= mayor) {
            // Una primera tienda de una versión anterior deja de ser candidata.
            if (version > mayor) {
                entrada = NULL;
                mayor = version;
            }
            const ConfigTienda *primera_tienda = configuracion_buscar(config, primera);
            if (*activa == '1' && primera_tienda == par) {
                entrada = par;
                incluida = *dentro == '1';
            }
        }
    }
    if (entrada) {
        return pedir(entrada, fin, vence, respuesta, "%s %s %s\n",
                    incluida ? "REANUDAR" : "RETORNO", cadena, propia->id);
    }
    if (mayor == guardada->version && guardada->orden[0] == yo &&
        presentes == guardada->cantidad) {
        CambioLista reanudar = *guardada;
        reanudar.version = (guardada->base > guardada->version ? guardada->base : guardada->version) + 1;
        reanudar.base = guardada->version;
        reanudar.tipo = 'I';
        reanudar.objetivo = yo;
        return recuperacion_acordar(config, propia, &reanudar, fin);
    }
    return false;
}
