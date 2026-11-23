#include "configuracion.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *inicio;
    const char *actual;
    const char *campo; // Nombre que acompaña al diagnóstico de lectura.
    char *error;
    size_t capacidad_error;
} LectorJson;

static bool fallar(LectorJson *json, const char *motivo) {
    snprintf(json->error, json->capacidad_error, "byte %zu, %s: %s",
             (size_t)(json->actual - json->inicio) + 1, json->campo, motivo);
    return false;
}

static void saltar_espacios(LectorJson *json) {
    while (*json->actual && strchr(" \t\r\n", *json->actual)) {
        ++json->actual;
    }
}

static bool tomar(LectorJson *json, char signo) {
    saltar_espacios(json);
    if (*json->actual != signo) {
        return false;
    }
    ++json->actual;
    return true;
}

static bool leer_texto(LectorJson *json, char *texto, size_t capacidad) {
    if (!tomar(json, '"')) {
        return fallar(json, "se esperaba una cadena JSON");
    }
    size_t usados = 0;
    while (*json->actual != '"') {
        unsigned char byte = (unsigned char)*json->actual;
        if (byte < 32 || byte > 126) {
            return fallar(json, "cadena sin cerrar o carácter fuera de ASCII imprimible");
        }
        ++json->actual;
        if (byte == '\\') {
            byte = (unsigned char)*json->actual;
            if (byte != '"' && byte != '\\' && byte != '/') {
                return fallar(json, "escape no admitido");
            }
            ++json->actual;
        }
        if (usados == capacidad - 1) {
            return fallar(json, "cadena demasiado larga");
        }
        texto[usados++] = (char)byte;
    }
    ++json->actual;
    texto[usados] = '\0';
    return true;
}

static bool digito(char byte) {
    return byte >= '0' && byte <= '9';
}

// Primero comprueba la sintaxis JSON. La conversión posterior verifica el rango.
static bool leer_numero(LectorJson *json, bool real, const char **inicio) {
    saltar_espacios(json);
    *inicio = json->actual;
    if (*json->actual == '-') {
        ++json->actual;
    }
    if (!digito(*json->actual)) {
        return fallar(json, "se esperaba un número");
    }
    if (*json->actual == '0') {
        ++json->actual;
        if (digito(*json->actual)) {
            return fallar(json, "cero inicial no admitido");
        }
    } else {
        while (digito(*json->actual)) {
            ++json->actual;
        }
    }

    if (*json->actual == '.' || *json->actual == 'e' || *json->actual == 'E') {
        if (!real) {
            return fallar(json, "se esperaba un entero decimal");
        }
        if (*json->actual == '.') {
            ++json->actual;
            if (!digito(*json->actual)) {
                return fallar(json, "fracción sin dígitos");
            }
            while (digito(*json->actual)) {
                ++json->actual;
            }
        }
        if (*json->actual == 'e' || *json->actual == 'E') {
            ++json->actual;
            if (*json->actual == '+' || *json->actual == '-') {
                ++json->actual;
            }
            if (!digito(*json->actual)) {
                return fallar(json, "exponente sin dígitos");
            }
            while (digito(*json->actual)) {
                ++json->actual;
            }
        }
    }
    return true;
}

static bool leer_entero(LectorJson *json, int64_t minimo, int64_t maximo, int64_t *valor) {
    const char *inicio;
    if (!leer_numero(json, false, &inicio)) {
        return false;
    }
    errno = 0;
    intmax_t entero = strtoimax(inicio, NULL, 10);
    if (errno == ERANGE || entero < minimo || entero > maximo) {
        return fallar(json, "entero fuera de rango");
    }
    *valor = (int64_t)entero;
    return true;
}

static bool leer_real(LectorJson *json, double *valor) {
    const char *inicio;
    if (!leer_numero(json, true, &inicio)) {
        return false;
    }
    errno = 0;
    *valor = strtod(inicio, NULL);
    return (errno != ERANGE && isfinite(*valor)) ||
           fallar(json, "número no representable");
}

static bool literal(LectorJson *json, const char *valor) {
    saltar_espacios(json);
    size_t longitud = strlen(valor);
    if (strncmp(json->actual, valor, longitud) != 0) {
        return false;
    }
    json->actual += longitud;
    return true;
}

static bool leer_id(LectorJson *json, char id[CONFIG_MAX_ID + 1]) {
    if (!leer_texto(json, id, CONFIG_MAX_ID + 1)) {
        return false;
    }
    const char *permitidos = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "0123456789_-";
    return (*id && strspn(id, permitidos) == strlen(id)) ||
           fallar(json, "ID vacío o con caracteres no admitidos");
}

static const char clave_limite[] = "limite_gubernamental_centavos";

// Una marca por clave permite detectar duplicados sin imponer un orden de lectura.
static int leer_campo(LectorJson *json, const char *const *campos, size_t cantidad,
                      unsigned *vistos) {
    char clave[40];
    json->campo = "clave";
    if (!leer_texto(json, clave, sizeof clave)) {
        return -1;
    }
    for (size_t i = 0; i < cantidad; ++i) {
        if (strcmp(clave, campos[i]) == 0) {
            json->campo = campos[i];
            unsigned marca = 1u << i;
            if ((*vistos & marca) != 0 || !tomar(json, ':')) {
                fallar(json, (*vistos & marca) ? "clave duplicada" : "falta ':'");
                return -1;
            }
            *vistos |= marca;
            return (int)i;
        }
    }
    if (strcmp(clave, "meta") == 0) {
        snprintf(json->error, json->capacidad_error,
                 "Quite las claves 'meta' y declare un solo '%s' en la raíz.", clave_limite);
    } else {
        snprintf(json->error, json->capacidad_error, "clave desconocida: %s", clave);
    }
    return -1;
}

static bool cerrar_objeto(LectorJson *json, const char *const *campos, size_t cantidad,
                          unsigned vistos) {
    if (!tomar(json, '}')) {
        return fallar(json, "se esperaba ',' o '}'");
    }
    for (size_t i = 0; i < cantidad; ++i) {
        if ((vistos & (1u << i)) == 0) {
            json->campo = campos[i];
            return fallar(json, "campo requerido");
        }
    }
    return true;
}

#define CANTIDAD(campos) (sizeof (campos) / sizeof (campos)[0])

// Objetos del escenario

static bool leer_compras(LectorJson *json, ConfigCompras *compras) {
    const char *const campos[] = {"importe_min_centavos", "importe_max_centavos", "intervalo_ms",
                                  "max_solicitudes_en_vuelo", "max_compras_en_espera"};
    const int64_t maximos[] = {INT64_MAX, INT64_MAX, INT_MAX,
                               CONFIG_MAX_SOLICITUDES_EN_VUELO, CONFIG_MAX_COMPRAS_EN_ESPERA};
    int64_t valores[] = {0, 0, 0, 2, 2}; // Las dos capacidades son opcionales.
    unsigned vistos = 0;
    if (!tomar(json, '{')) {
        return fallar(json, "se esperaba un objeto de compras");
    }
    do {
        int campo = leer_campo(json, campos, CANTIDAD(campos), &vistos);
        if (campo < 0 || !leer_entero(json, 1, maximos[campo], &valores[campo])) {
            return false;
        }
    } while (tomar(json, ','));
    if (!cerrar_objeto(json, campos, 3, vistos)) {
        return false;
    }
    if (valores[0] > valores[1]) {
        json->campo = campos[0];
        return fallar(json, "supera importe_max_centavos");
    }
    *compras = (ConfigCompras){valores[0], valores[1], (int)valores[2], (int)valores[3], (int)valores[4]};
    return true;
}

static bool leer_tienda(LectorJson *json, ConfigTienda *tienda, int cadena) {
    const char *const campos[] = {"id", "historial", "direccion", "puerto", "compras"};
    bool suelta = cadena == CONFIG_SIN_CADENA;
    unsigned vistos = 0;
    tienda->cadena = cadena;

    if (!tomar(json, '{')) {
        return fallar(json, "se esperaba un objeto de tienda");
    }
    do {
        int campo = leer_campo(json, campos, CANTIDAD(campos), &vistos);
        int64_t puerto;
        struct in_addr direccion;
        switch (campo) {
        case 0:
            if (!leer_id(json, tienda->id)) {
                return false;
            }
            break;
        case 1:
            if (!leer_texto(json, tienda->historial, sizeof tienda->historial)) {
                return false;
            }
            if (!tienda->historial[0]) {
                return fallar(json, "ruta vacía");
            }
            break;
        case 2:
            if (!leer_texto(json, tienda->direccion, sizeof tienda->direccion)) {
                return false;
            }
            if (inet_pton(AF_INET, tienda->direccion, &direccion) != 1 ||
                direccion.s_addr == htonl(INADDR_ANY)) {
                return fallar(json, "se esperaba una dirección IPv4 concreta");
            }
            break;
        case 3:
            if (!leer_entero(json, 1, UINT16_MAX, &puerto)) {
                return false;
            }
            tienda->puerto = (uint16_t)puerto;
            break;
        case 4:
            if (!leer_compras(json, &tienda->compras)) {
                return false;
            }
            break;
        default:
            return false;
        }
    } while (tomar(json, ','));
    if (suelta && !tienda->direccion[0] && !tienda->puerto) {
        // Una independiente puede omitir ambos campos. Si declara uno, necesita el otro.
        vistos |= (1u << 2) | (1u << 3);
    }
    // Fuera de la simulación se puede consultar o atender una tienda sin programar compras.
    return cerrar_objeto(json, campos, CANTIDAD(campos) - 1, vistos);
}

static bool leer_tiendas(LectorJson *json, Configuracion *config, int cadena) {
    if (!tomar(json, '[')) {
        return fallar(json, "se esperaba una lista de tiendas");
    }
    if (tomar(json, ']')) {
        return true;
    }
    do {
        if (config->cantidad_tiendas == CONFIG_MAX_TIENDAS) {
            return fallar(json, "se excede el límite de tiendas");
        }
        if (!leer_tienda(json, &config->tiendas[config->cantidad_tiendas], cadena)) {
            return false;
        }
        ++config->cantidad_tiendas;
    } while (tomar(json, ','));
    return tomar(json, ']') || fallar(json, "se esperaba ',' o ']'");
}

static bool leer_cadena(LectorJson *json, Configuracion *config) {
    const char *const campos[] = {"id", "tiendas", "detalle_tiendas"};
    unsigned vistos = 0;
    int64_t cantidad = 0;
    size_t inicio = config->cantidad_tiendas;
    ConfigCadena *cadena = &config->cadenas[config->cantidad_cadenas];

    if (!tomar(json, '{')) {
        return fallar(json, "se esperaba un objeto de cadena");
    }
    do {
        switch (leer_campo(json, campos, CANTIDAD(campos), &vistos)) {
        case 0:
            if (!leer_id(json, cadena->id)) {
                return false;
            }
            break;
        case 1:
            if (!leer_entero(json, 1, CONFIG_MAX_TIENDAS, &cantidad)) {
                return false;
            }
            break;
        case 2:
            if (!leer_tiendas(json, config, (int)config->cantidad_cadenas)) {
                return false;
            }
            break;
        default:
            return false;
        }
    } while (tomar(json, ','));
    if (!cerrar_objeto(json, campos, CANTIDAD(campos), vistos)) {
        return false;
    }
    json->campo = "tiendas";
    return (size_t)cantidad == config->cantidad_tiendas - inicio ||
           fallar(json, "cantidad distinta de la longitud de detalle_tiendas");
}

static bool leer_cadenas(LectorJson *json, Configuracion *config) {
    if (!tomar(json, '[')) {
        return fallar(json, "se esperaba una lista de cadenas");
    }
    if (tomar(json, ']')) {
        return true;
    }
    do {
        if (config->cantidad_cadenas == CONFIG_MAX_CADENAS) {
            return fallar(json, "se excede el límite de cadenas");
        }
        if (!leer_cadena(json, config)) {
            return false;
        }
        ++config->cantidad_cadenas;
    } while (tomar(json, ','));
    return tomar(json, ']') || fallar(json, "se esperaba ',' o ']'");
}

static bool leer_interrupciones(LectorJson *json, ConfigInterrupciones *interrupciones) {
    const char *const campos[] = {"probabilidad_caida", "evaluar_cada_s",
                                  "caida_min_s", "caida_max_s", "recuperacion_auto"};
    double valores[4] = {0};
    unsigned vistos = 0;
    bool recuperar = false;
    if (!tomar(json, '{')) {
        return fallar(json, "se esperaba un objeto de interrupciones");
    }
    do {
        int campo = leer_campo(json, campos, CANTIDAD(campos), &vistos);
        if (campo < 0) {
            return false;
        }
        if (campo == 4) {
            recuperar = literal(json, "true");
            if (!recuperar && !literal(json, "false")) {
                return fallar(json, "se esperaba un booleano");
            }
        } else if (!leer_real(json, &valores[campo])) {
            return false;
        } else if (valores[campo] < 0 || (campo == 0 && valores[campo] > 1) ||
                   (campo == 1 && valores[campo] == 0)) {
            return fallar(json, "número fuera de rango");
        }
    } while (tomar(json, ','));
    if (!cerrar_objeto(json, campos, CANTIDAD(campos), vistos)) {
        return false;
    }
    json->campo = "caida_min_s";
    if (valores[2] > valores[3]) {
        return fallar(json, "supera caida_max_s");
    }
    *interrupciones = (ConfigInterrupciones){valores[0], valores[1], valores[2], valores[3], recuperar};
    return true;
}

static bool leer_raiz(LectorJson *json, Configuracion *config) {
    const char *const campos[] = {"cadenas", "tiendas_independientes",
                                  "detalle_tiendas_independientes",
                                  "interrupciones", "semilla_azar", clave_limite};
    unsigned vistos = 0;
    int64_t independientes = 0;
    size_t detalle_independientes = 0;
    if (!tomar(json, '{')) {
        return fallar(json, "se esperaba el objeto de configuración");
    }
    do {
        size_t antes = config->cantidad_tiendas;
        int64_t semilla;
        switch (leer_campo(json, campos, CANTIDAD(campos), &vistos)) {
        case 0:
            if (!leer_cadenas(json, config)) {
                return false;
            }
            break;
        case 1:
            if (!leer_entero(json, 0, CONFIG_MAX_TIENDAS, &independientes)) {
                return false;
            }
            break;
        case 2:
            if (!leer_tiendas(json, config, CONFIG_SIN_CADENA)) {
                return false;
            }
            detalle_independientes = config->cantidad_tiendas - antes;
            break;
        case 3:
            if (!leer_interrupciones(json, &config->interrupciones)) {
                return false;
            }
            break;
        case 4:
            if (!literal(json, "null")) {
                if (!leer_entero(json, 0, UINT_MAX, &semilla)) {
                    return false;
                }
                config->semilla_azar = (unsigned)semilla;
                config->semilla_configurada = true;
            }
            break;
        case 5:
            if (!leer_entero(json, 1, INT64_MAX, &config->limite_gubernamental_centavos)) {
                return false;
            }
            break;
        default:
            return false;
        }
    } while (tomar(json, ','));
    if (!cerrar_objeto(json, campos, CANTIDAD(campos), vistos)) {
        return false;
    }
    if ((size_t)independientes != detalle_independientes) {
        json->campo = "tiendas_independientes";
        return fallar(json, "cantidad distinta de la longitud de detalle_tiendas_independientes");
    }
    json->campo = "configuración";
    saltar_espacios(json);
    if (*json->actual) {
        return fallar(json, "datos después del objeto principal");
    }
    return config->cantidad_tiendas != 0 || fallar(json, "se requiere al menos una tienda");
}

// Rutas y datos repetidos

/*
 * Resuelve una ruta de historial a partir del directorio del JSON.
 * Devuelve true si obtiene una ruta válida dentro del límite de longitud.
 * Normaliza los componentes sin abrir archivos ni resolver enlaces simbólicos.
 */
static bool resolver_historial(char historial[CONFIG_MAX_RUTA + 1], const char *directorio) {
    char ruta[CONFIG_MAX_RUTA * 2 + 2];
    int longitud = historial[0] == '/'
                       ? snprintf(ruta, sizeof ruta, "%s", historial)
                       : snprintf(ruta, sizeof ruta, "%s/%s", directorio, historial);
    if (longitud < 0 || (size_t)longitud >= sizeof ruta || ruta[longitud - 1] == '/') {
        return false;
    }
    const char *ultimo = strrchr(ruta, '/') + 1;
    if (strcmp(ultimo, ".") == 0 || strcmp(ultimo, "..") == 0) {
        return false;
    }

    size_t usados = 0;
    char *siguiente;
    for (char *parte = strtok_r(ruta, "/", &siguiente); parte;
         parte = strtok_r(NULL, "/", &siguiente)) {
        if (strcmp(parte, ".") == 0) {
            continue;
        }
        if (strcmp(parte, "..") == 0) {
            while (usados > 0) {
                if (historial[--usados] == '/') {
                    break;
                }
            }
        } else {
            size_t largo = strlen(parte);
            if (usados + 1 + largo > CONFIG_MAX_RUTA) {
                return false;
            }
            historial[usados++] = '/';
            memcpy(historial + usados, parte, largo);
            usados += largo;
        }
    }
    historial[usados] = '\0';
    return usados != 0;
}

static bool validar_duplicados(Configuracion *config, const char *directorio,
                               char *error, size_t capacidad_error) {
    for (size_t i = 0; i < config->cantidad_cadenas; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(config->cadenas[i].id, config->cadenas[j].id) == 0) {
                snprintf(error, capacidad_error, "id de cadena repetido: %s", config->cadenas[i].id);
                return false;
            }
        }
    }
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        ConfigTienda *tienda = &config->tiendas[i];
        if (!resolver_historial(tienda->historial, directorio)) {
            snprintf(error, capacidad_error, "historial inválido para %s", tienda->id);
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            const ConfigTienda *anterior = &config->tiendas[j];
            const char *repetido = NULL;
            if (strcmp(tienda->id, anterior->id) == 0) {
                repetido = "id";
            } else if (strcmp(tienda->historial, anterior->historial) == 0) {
                repetido = "historial";
            } else if (tienda->puerto && tienda->puerto == anterior->puerto &&
                       strcmp(tienda->direccion, anterior->direccion) == 0) {
                repetido = "dirección y puerto";
            }
            if (repetido) {
                snprintf(error, capacidad_error, "%s repetido: %s y %s",
                         repetido, anterior->id, tienda->id);
                return false;
            }
        }
    }
    return true;
}

static bool directorio_del_json(const char *ruta, char directorio[CONFIG_MAX_RUTA + 1]) {
    const char *barra = strrchr(ruta, '/');
    size_t largo = barra ? (size_t)(barra - ruta) + 1 : 0;
    if (*ruta == '/') {
        if (largo > CONFIG_MAX_RUTA) {
            return false;
        }
        memcpy(directorio, ruta, largo);
        directorio[largo] = '\0';
        return true;
    }
    if (!getcwd(directorio, CONFIG_MAX_RUTA + 1)) {
        return false;
    }
    size_t usados = strlen(directorio);
    if (usados + 1 + largo > CONFIG_MAX_RUTA) {
        return false;
    }
    directorio[usados++] = '/';
    memcpy(directorio + usados, ruta, largo);
    directorio[usados + largo] = '\0';
    return true;
}

/*
 * Lee el archivo completo, comprueba sus objetos y verifica los datos repetidos.
 * Devuelve true y entrega la configuración solo después de validar todo.
 * Si falla, deja intacta la configuración recibida y escribe un diagnóstico.
 */
bool configuracion_cargar(const char *ruta, Configuracion *config,
                          char *error, size_t capacidad_error) {
    FILE *archivo = fopen(ruta, "rb");
    if (!archivo) {
        snprintf(error, capacidad_error, "%s: %s", ruta, strerror(errno));
        return false;
    }
    char datos[CONFIG_MAX_ARCHIVO + 1];
    // El byte adicional permite distinguir un archivo demasiado grande.
    size_t leidos = fread(datos, 1, sizeof datos, archivo);
    bool fallo = ferror(archivo) != 0;
    if (fclose(archivo) != 0 || fallo) {
        snprintf(error, capacidad_error, "%s: error de lectura o cierre", ruta);
        return false;
    }
    if (leidos == sizeof datos || memchr(datos, '\0', leidos)) {
        snprintf(error, capacidad_error, "%s: archivo demasiado grande o con bytes nulos", ruta);
        return false;
    }
    datos[leidos] = '\0';

    Configuracion nueva = {0};
    LectorJson json = {datos, datos, "configuración", error, capacidad_error};
    char directorio[CONFIG_MAX_RUTA + 1];
    if (!directorio_del_json(ruta, directorio)) {
        snprintf(error, capacidad_error, "%s: directorio demasiado largo o inaccesible", ruta);
        return false;
    }
    if (!leer_raiz(&json, &nueva) ||
        !validar_duplicados(&nueva, directorio, error, capacidad_error)) {
        return false;
    }
    *config = nueva;
    return true;
}

const ConfigTienda *configuracion_buscar(const Configuracion *config, const char *id) {
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        if (strcmp(config->tiendas[i].id, id) == 0) {
            return &config->tiendas[i];
        }
    }
    return NULL;
}
