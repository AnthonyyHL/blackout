#include "cierre.h"
#include "red.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

/*
 * Envía un dato del inventario ya conciliado a todos los propietarios.
 * Cada uno verifica su propio registro y cuenta cada ID una sola vez.
 */
static bool verificar(const Configuracion *config, const ConfigTienda *propia,
                      const char *cadena, const CambioLista *lista, int fin, int64_t vence) {
    for (size_t fila = 0; ; ++fila) {
        char respuesta[RED_MAX_LINEA + 1], datos[RED_MAX_LINEA + 1];
        if (!pedir(propia, fin, vence, datos, "FICHA %s %zu %" PRIu32 "\n",
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
            if (!pedir(par, fin, vence, respuesta, "COTEJA %s %s %" PRIu32 "\n",
                       cadena, datos + 6, lista->version) || strcmp(respuesta, "COINCIDE\n") != 0) {
                return false;
            }
        }
    }
}

static bool preparar(const Configuracion *config, const ConfigTienda *propia,
                     const char *cadena, const CambioLista *lista, int fin, int64_t vence) {
    char respuesta[RED_MAX_LINEA + 1];
    for (size_t i = 0; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!pedir(par, fin, vence, respuesta, "CONGELAR_CIERRE %s %" PRIu32 "\n",
                   cadena, lista->version) || strcmp(respuesta, "CONGELADA\n") != 0) {
            return false;
        }
    }
    if (!recuperacion_sincronizar(config, cadena, lista, fin, vence) ||
        !verificar(config, propia, cadena, lista, fin, vence)) {
        return false;
    }
    char resumen[RED_MAX_LINEA + 1];
    if (!pedir(propia, fin, vence, resumen, "RESUMEN_CIERRE %s %" PRIu32 "\n",
               cadena, lista->version) || strncmp(resumen, "RESUMEN ", 8) != 0) {
        return false;
    }
    resumen[strlen(resumen) - 1] = '\0';
    // El coordinador registra su preparación antes de pedir la de los demás.
    for (size_t i = 0; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!pedir(par, fin, vence, respuesta, "PREPARAR_CIERRE %s %" PRIu32 " %s\n",
                   cadena, lista->version, resumen + 8) || strcmp(respuesta, "PREPARADA\n") != 0) {
            return false;
        }
    }
    return pedir(propia, fin, vence, respuesta, "DECIDIR_CIERRE %s %" PRIu32 "\n",
                 cadena, lista->version) && strcmp(respuesta, "GUARDADA\n") == 0;
}

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
        return pedir(primera, fin, vence, respuesta, "AVISO_CIERRE %s %" PRIu32 " %s\n",
                     cadena, lista->version, propia->id) && strcmp(respuesta, "AVISADA\n") == 0;
    }
    if (trabajo == CONCILIAR_CIERRE && !preparar(config, propia, cadena, lista, fin, vence)) {
        return false;
    }
    vence = red_reloj_ms() + RECUPERACION_MS;
    for (size_t i = 1; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!pedir(par, fin, vence, respuesta, "DECIDIR_CIERRE %s %" PRIu32 "\n",
                   cadena, lista->version) || strcmp(respuesta, "GUARDADA\n") != 0) {
            return false;
        }
    }
    for (size_t i = 1; i < lista->cantidad; ++i) {
        const ConfigTienda *par = &config->tiendas[lista->orden[i]];
        if (!pedir(par, fin, vence, respuesta, "SALIR_CIERRE %s %" PRIu32 "\n",
                   cadena, lista->version) || strcmp(respuesta, "SALIDA\n") != 0) {
            fprintf(stderr, "Salida sin confirmar de %s\n", par->id);
        }
    }
    return true;
}

static bool copiar_evidencia(const ConfigTienda *propia, const ConfigTienda *par,
                             const char *cadena, const char *cabecera, int fin, int64_t vence) {
    char respuesta[RED_MAX_LINEA + 1], datos[RED_MAX_LINEA + 1];
    if (!pedir(propia, fin, vence, respuesta, "EVIDENCIA_INICIO %s %s", cadena, cabecera) ||
        strcmp(respuesta, "CONTINUA\n") != 0) {
        return false;
    }
    const char *consultas[] = {"CIERRE_MIEMBRO", "CIERRE_DATO"};
    const char *cargas[] = {"EVIDENCIA_MIEMBRO", "EVIDENCIA_VENTA"};
    for (size_t tipo = 0; tipo < 2; ++tipo) {
        for (size_t fila = 0; ; ++fila) {
            if (!pedir(par, fin, vence, datos, "%s %s %zu\n", consultas[tipo], cadena, fila)) {
                return false;
            }
            if (strcmp(datos, "FIN\n") == 0) {
                break;
            }
            if (!pedir(propia, fin, vence, respuesta, "%s %s %s", cargas[tipo], cadena, datos) ||
                strcmp(respuesta, "CONTINUA\n") != 0) {
                return false;
            }
        }
    }
    return pedir(propia, fin, vence, respuesta, "EVIDENCIA_FIN %s 0\n", cadena) &&
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
            pedir(par, fin, vence, respuesta, "PRUEBA_CIERRE %s 0\n", cadena) &&
            strncmp(respuesta, "CERRADA ", 8) == 0 &&
            copiar_evidencia(propia, par, cadena, respuesta + 8, fin, vence)) {
            return true;
        }
    }
    return false;
}
