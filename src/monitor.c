#include "monitor.h"
#include "red.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const ConfigTienda *tienda;
    bool (*local)(void *, const char *, char *);
    void *contexto;
    char error[256];
} Consulta;

static bool pedir(Consulta *consulta, const char *linea, char *respuesta) {
    return consulta->local ? consulta->local(consulta->contexto, linea, respuesta)
                           : red_intercambiar(consulta->tienda, linea, respuesta,
                                              consulta->error, sizeof consulta->error);
}

static bool campos_exactos(char *linea, char **campos, size_t cantidad) {
    char *fin = strchr(linea, '\n');
    if (!fin || fin[1]) {
        return false;
    }
    *fin = '\0';
    for (size_t i = 0; i < cantidad; ++i) {
        campos[i] = linea;
        char *espacio = strchr(linea, ' ');
        if (!*linea || espacio == linea || (espacio == NULL) != (i + 1 == cantidad)) {
            return false;
        }
        if (espacio) {
            *espacio = '\0';
            linea = espacio + 1;
        }
    }
    return true;
}

static bool leer_cifra(const char *texto, int64_t *valor) {
    if (*texto < '0' || *texto > '9') {
        return false;
    }
    char *fin;
    errno = 0;
    intmax_t cifra = strtoimax(texto, &fin, 10);
    if (errno == ERANGE || *fin || cifra > INT64_MAX) {
        return false;
    }
    *valor = (int64_t)cifra;
    return true;
}

static const char *describir_estado(const char *estado) {
    if (strlen(estado) != 1) {
        return NULL;
    }
    switch (*estado) {
    case 'A': return "ABIERTA";
    case 'R': return "RECUPERANDO";
    case 'C': return "CIERRE_EN_CURSO";
    case 'P': return "CIERRE_PREPARADO";
    case 'M':
    case 'm': return "CERRADA_META";
    case 'H': return "HISTORIAL_POR_VERIFICAR";
    case 'I': return "SIN_CIERRE_REGISTRADO";
    default: return NULL;
    }
}

static void observacion(const Consulta *consulta, const char *cadena, const char *tipo) {
    char hora[32] = "no_disponible";
    time_t ahora = time(NULL);
    struct tm utc;
    if (ahora != (time_t)-1 && gmtime_r(&ahora, &utc)) {
        strftime(hora, sizeof hora, "%Y-%m-%dT%H:%M:%SZ", &utc);
    }
    const ConfigTienda *tienda = consulta->tienda;
    printf("%s tienda=%s", tipo, tienda->id);
    if (tienda->cadena != CONFIG_SIN_CADENA) {
        printf(" cadena=%s", cadena);
    }
    if (consulta->local) {
        printf(" fuente=historial:%s", tienda->historial);
    } else {
        printf(" fuente=tcp:%s:%u", tienda->direccion, (unsigned)tienda->puerto);
    }
    printf(" observado=%s\n", hora);
}

/* Enumera solo el prefijo de ventas propias que existía al responder el estado. */
static bool mostrar_ventas(Consulta *consulta, const char *cadena, int64_t ultima, int64_t propio) {
    int64_t suma = 0;
    for (int64_t i = 0; i < ultima; ++i) {
        char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1], *campos[3];
        snprintf(linea, sizeof linea, "VENTA_PROPIA %s %s %" PRId64 "\n",
                 consulta->tienda->id, cadena, i + 1);
        int64_t id, importe;
        if (!pedir(consulta, linea, respuesta) || !campos_exactos(respuesta, campos, 3) ||
            strcmp(campos[0], "PROPIA") != 0 || !leer_cifra(campos[1], &id) || id != i + 1 ||
            !leer_cifra(campos[2], &importe) || importe <= 0 || importe > propio - suma) {
            return false;
        }
        suma += importe;
        printf("VENTA_PROPIA id=%" PRId64 " centavos=%" PRId64 "\n", id, importe);
    }
    return suma == propio;
}

/* Coteja la respuesta del protocolo de cierre antes de mostrar su alcance. */
static bool mostrar_cierre(Consulta *consulta, const Configuracion *config, const char *cadena) {
    char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1], *campos[6];
    snprintf(linea, sizeof linea, "PRUEBA_CIERRE %s 0\n", cadena);
    if (!pedir(consulta, linea, respuesta)) {
        return false;
    }
    if (strcmp(respuesta, "ERROR\n") == 0) {
        puts("CIERRE_GUARDADO sin_detalle_de_certificacion");
        return true;
    }
    int64_t version, total, ventas, activas, excluidas;
    if (!campos_exactos(respuesta, campos, 6) || strcmp(campos[0], "CERRADA") != 0 ||
        !leer_cifra(campos[1], &version) || version <= 0 || version > UINT32_MAX ||
        !leer_cifra(campos[2], &total) || total <= 0 ||
        !leer_cifra(campos[3], &ventas) || ventas <= 0 ||
        !leer_cifra(campos[4], &activas) || activas <= 0 || activas > CONFIG_MAX_TIENDAS ||
        !leer_cifra(campos[5], &excluidas) || excluidas > CONFIG_MAX_TIENDAS - activas) {
        return false;
    }
    const ConfigTienda *miembros[CONFIG_MAX_TIENDAS];
    uint64_t vistos = 0;
    for (int64_t i = 0; i < activas + excluidas; ++i) {
        snprintf(linea, sizeof linea, "CIERRE_MIEMBRO %s %" PRId64 "\n", cadena, i);
        if (!pedir(consulta, linea, respuesta) || !campos_exactos(respuesta, campos, 1)) {
            return false;
        }
        const ConfigTienda *par = configuracion_buscar(config, campos[0]);
        if (!par || par->cadena != consulta->tienda->cadena) {
            return false;
        }
        uint64_t bit = UINT64_C(1) << (par - config->tiendas);
        if (vistos & bit) {
            return false;
        }
        vistos |= bit;
        miembros[i] = par;
    }
    printf("CIERRE_CERTIFICADO version=%" PRId64 " total_centavos=%" PRId64 " ventas=%" PRId64 "\n",
           version, total, ventas);
    for (int64_t i = 0; i < activas + excluidas; ++i) {
        printf("%s %s\n", i < activas ? "PARTICIPANTE" : "EXCLUIDA", miembros[i]->id);
    }
    return true;
}

int monitor_mostrar(const Configuracion *config, const ConfigTienda *tienda, bool ventas,
                    bool (*local)(void *, const char *, char *), void *contexto) {
    bool independiente = tienda->cadena == CONFIG_SIN_CADENA;
    const char *cadena = independiente ? "-" : config->cadenas[tienda->cadena].id;
    if (!local && !tienda->puerto) {
        fprintf(stderr, "%s requiere direccion y puerto para una consulta TCP.\n", tienda->id);
        return EXIT_FAILURE;
    }
    Consulta consulta = {.tienda = tienda, .local = local, .contexto = contexto};
    char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1], *campos[7];
    snprintf(linea, sizeof linea, "ESTADO %s %s\n", tienda->id, cadena);
    if (!pedir(&consulta, linea, respuesta)) {
        observacion(&consulta, cadena, local ? "HISTORICO_NO_DISPONIBLE" : "INACCESIBLE");
        fprintf(stderr, "%s\n", consulta.error);
        return EXIT_FAILURE;
    }
    const char *estado;
    int64_t propio, ultima, total = 0, pendientes = 0;
    if (!campos_exactos(respuesta, campos, 7) || strcmp(campos[0], "ESTADO") != 0 ||
        strcmp(campos[1], tienda->id) != 0 || !(estado = describir_estado(campos[2])) ||
        !leer_cifra(campos[3], &propio) || !leer_cifra(campos[6], &ultima) || ultima > propio ||
        (independiente ? strcmp(campos[4], "-") != 0 || strcmp(campos[5], "-") != 0
                       : !leer_cifra(campos[4], &total) || total < propio ||
                         (strcmp(campos[5], "-") != 0 &&
                          (!leer_cifra(campos[5], &pendientes) || pendientes > ultima)))) {
        fprintf(stderr, "Respuesta de estado inválida para %s.\n", tienda->id);
        return EXIT_FAILURE;
    }
    char estado_declarado = *campos[2];
    observacion(&consulta, cadena, local || estado_declarado == 'm' ? "HISTORICO" : "ACTUAL");
    printf("ESTADO %s ingreso_propio_centavos=%" PRId64 " ventas_propias=%" PRId64,
           estado, propio, ultima);
    if (!independiente) {
        printf(" total_conocido_centavos=%" PRId64, total);
        if (strcmp(campos[5], "-") != 0) {
            printf(" pendientes_confirmacion=%" PRId64, pendientes);
        }
    }
    putchar('\n');
    bool completo = true;
    if (estado_declarado == 'M' || estado_declarado == 'm') {
        if (independiente) {
            puts("CIERRE_GUARDADO marca_historial_independiente");
        } else if (!mostrar_cierre(&consulta, config, cadena)) {
            fprintf(stderr, "Detalle de cierre no disponible para %s.\n", tienda->id);
            completo = false;
        }
    }
    if (ventas && !mostrar_ventas(&consulta, cadena, ultima, propio)) {
        fprintf(stderr, "Enumeración incompleta para %s.\n", tienda->id);
        completo = false;
    }
    return completo ? EXIT_SUCCESS : EXIT_FAILURE;
}
