#include "tienda_local.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    TAM_REGISTRO = sizeof "9223372036854775807 1 K \n" + CONFIG_MAX_ID,
    TAM_BLOQUE = 1024
};

static bool reservar_venta(TiendaLocal *tienda) {
    if (tienda->cantidad < tienda->capacidad) {
        return true;
    }
    if (tienda->capacidad > SIZE_MAX / sizeof *tienda->ventas / 2) {
        return false;
    }
    size_t capacidad = tienda->capacidad ? tienda->capacidad * 2 : 16;
    VentaLocal *ventas = realloc(tienda->ventas, capacidad * sizeof *ventas);
    if (!ventas) {
        return false;
    }
    tienda->ventas = ventas;
    tienda->capacidad = capacidad;
    return true;
}

static bool clave_valida(const char *clave) {
    return clave[0] && strlen(clave) <= CONFIG_MAX_ID &&
           clave[strspn(clave, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")] == '\0';
}

static size_t buscar_clave(const TiendaLocal *tienda, const char *clave) {
    for (size_t i = 0; i < tienda->cantidad; ++i) {
        if (strcmp(tienda->ventas[i].clave, clave) == 0) {
            return i + 1;
        }
    }
    return 0;
}

// Reconstrucción del historial

static ResultadoTiendaLocal aplicar_registro(TiendaLocal *tienda, const char *registro) {
    // Una venta posterior a la marca definitiva invalida el historial.
    if (tienda->cerrada_por_meta || registro[0] < '1' || registro[0] > '9') {
        return TIENDA_HISTORIAL_INVALIDO;
    }

    char *fin;
    errno = 0;
    intmax_t importe = strtoimax(registro, &fin, 10);
    if (errno == ERANGE || importe > INT64_MAX - tienda->ingreso_propio ||
        fin[0] != ' ' || (fin[1] != '0' && fin[1] != '1') ||
        (fin[2] != '\0' && (strncmp(fin + 2, " K ", 3) != 0 || !clave_valida(fin + 5)))) {
        return TIENDA_HISTORIAL_INVALIDO;
    }
    // Las ventas antiguas sin clave también conservan su posición.
    const char *clave = fin[2] ? fin + 5 : "";
    if (*clave && buscar_clave(tienda, clave)) {
        return TIENDA_HISTORIAL_INVALIDO;
    }
    if (!reservar_venta(tienda)) {
        return TIENDA_SIN_MEMORIA;
    }
    VentaLocal *venta = &tienda->ventas[tienda->cantidad++];
    venta->importe = (int64_t)importe;
    strcpy(venta->clave, clave);
    tienda->ingreso_propio += (int64_t)importe;
    tienda->cerrada_por_meta = fin[1] == '1';
    return TIENDA_OK;
}

/*
 * Reconstruye ventas y cierre en una instancia que todavía no atiende compras.
 * Devuelve TIENDA_OK si todos los registros terminan en salto de línea y son válidos.
 * Una lectura fallida o una última línea incompleta impide usar esa instancia.
 */
static ResultadoTiendaLocal reconstruir_historial(TiendaLocal *tienda) {
    char bloque[TAM_BLOQUE];
    char registro[TAM_REGISTRO];
    size_t usados = 0;
    ssize_t leidos;

    do {
        leidos = read(tienda->fd_historial, bloque, sizeof bloque);
        if (leidos < 0) {
            if (errno == EINTR) {
                continue;
            }
            return TIENDA_ERROR_LECTURA;
        }

        for (ssize_t i = 0; i < leidos; ++i) {
            char byte = bloque[i];
            if (byte == '\n') {
                registro[usados] = '\0';
                ResultadoTiendaLocal resultado = aplicar_registro(tienda, registro);
                if (resultado != TIENDA_OK) {
                    return resultado;
                }
                usados = 0;
            } else {
                if (byte == '\0' || usados == sizeof registro - 1) {
                    return TIENDA_HISTORIAL_INVALIDO;
                }
                registro[usados++] = byte;
            }
        }
    } while (leidos != 0);

    return usados == 0 ? TIENDA_OK : TIENDA_HISTORIAL_INVALIDO;
}

/*
 * Abre un archivo regular y reconstruye la tienda por orden de registro.
 * En modo histórico abre solo para lectura. Devuelve TIENDA_OK y entrega
 * el estado al llamador si el historial es válido para el modo solicitado.
 */
ResultadoTiendaLocal tienda_local_abrir_consulta(TiendaLocal *tienda, const char *ruta,
                                                 int64_t meta, bool historico) {
    if (tienda == NULL || tienda->fd_historial >= 0 ||
        ruta == NULL || ruta[0] == '\0' || meta <= 0) {
        return TIENDA_PARAMETRO_INVALIDO;
    }

    TiendaLocal nueva = TIENDA_LOCAL_INICIAL;
    nueva.meta = meta;
    int opciones = historico ? O_RDONLY | O_NONBLOCK : O_RDWR | O_CREAT | O_APPEND;
    nueva.fd_historial = open(ruta, opciones, 0600);
    if (nueva.fd_historial < 0) {
        return TIENDA_ERROR_APERTURA;
    }

    struct stat atributos;
    if (fstat(nueva.fd_historial, &atributos) < 0 || !S_ISREG(atributos.st_mode)) {
        close(nueva.fd_historial);
        return TIENDA_ERROR_APERTURA;
    }

    ResultadoTiendaLocal resultado = reconstruir_historial(&nueva);
    // La meta nueva solo se comprueba si el historial sigue abierto.
    if (resultado == TIENDA_OK && !historico && !nueva.cerrada_por_meta &&
        nueva.ingreso_propio >= meta) {
        resultado = TIENDA_META_INCOMPATIBLE;
    }
    if (resultado != TIENDA_OK) {
        close(nueva.fd_historial);
        free(nueva.ventas);
        return resultado;
    }

    *tienda = nueva;
    return TIENDA_OK;
}

ResultadoTiendaLocal tienda_local_abrir(TiendaLocal *tienda, const char *ruta, int64_t meta) {
    return tienda_local_abrir_consulta(tienda, ruta, meta, false);
}

// Admisión de compras

/*
 * Anexa el registro de una venta y actualiza el ingreso propio.
 * Devuelve TIENDA_OK cuando la escritura está completa. Si la escritura
 * falla, conserva el ingreso anterior y bloquea los registros nuevos.
 */
static ResultadoTiendaLocal registrar(TiendaLocal *tienda, int64_t importe, const char *clave) {
    if (tienda == NULL || tienda->fd_historial < 0) {
        return TIENDA_PARAMETRO_INVALIDO;
    }
    if (tienda->historial_por_verificar) {
        return TIENDA_HISTORIAL_POR_VERIFICAR;
    }
    if (tienda->cerrada_por_meta) {
        return TIENDA_CERRADA_POR_META;
    }
    if (importe <= 0) {
        return TIENDA_IMPORTE_INVALIDO;
    }
    if (importe > INT64_MAX - tienda->ingreso_propio) {
        return TIENDA_DESBORDAMIENTO;
    }

    int64_t ingreso_nuevo = tienda->ingreso_propio + importe;
    bool alcanza_meta = ingreso_nuevo >= tienda->meta;
    char registro[TAM_REGISTRO];
    // El importe y el cierre pertenecen al mismo registro de venta.
    int longitud = snprintf(registro, sizeof registro, "%" PRId64 " %d%s%s\n",
                            importe, alcanza_meta, *clave ? " K " : "", clave);
    if (longitud < 0 || (size_t)longitud >= sizeof registro) {
        return TIENDA_ERROR_ESCRITURA;
    }
    // Si falta memoria, se rechaza antes de escribir una venta que no se podría consultar.
    if (!reservar_venta(tienda)) {
        return TIENDA_SIN_MEMORIA;
    }

    size_t escritos = 0;
    size_t total = (size_t)longitud;
    while (escritos < total) {
        ssize_t cantidad = write(tienda->fd_historial, registro + escritos,
                                 total - escritos);
        if (cantidad < 0 && errno == EINTR) {
            continue;
        }
        if (cantidad <= 0) {
            // El archivo puede contener un registro parcial.
            tienda->historial_por_verificar = true;
            return TIENDA_ERROR_ESCRITURA;
        }
        escritos += (size_t)cantidad;
    }

    tienda->ingreso_propio = ingreso_nuevo;
    tienda->cerrada_por_meta = alcanza_meta;
    VentaLocal *venta = &tienda->ventas[tienda->cantidad++];
    venta->importe = importe;
    strcpy(venta->clave, clave);
    return TIENDA_OK;
}

ResultadoTiendaLocal tienda_local_registrar(TiendaLocal *tienda, int64_t importe) {
    return registrar(tienda, importe, "");
}

/*
 * Devuelve TIENDA_OK y el ID de la venta nueva o ya registrada con esa clave.
 * Si el importe difiere del guardado para esa clave, devuelve TIENDA_CONFLICTO.
 */
ResultadoTiendaLocal tienda_local_comprar(TiendaLocal *tienda, const char *clave, int64_t importe, size_t *id) {
    if (!tienda || tienda->fd_historial < 0 || !clave || !clave_valida(clave) || !id) {
        return TIENDA_PARAMETRO_INVALIDO;
    }
    *id = buscar_clave(tienda, clave);
    // Una venta ya registrada se puede confirmar incluso después del cierre.
    if (*id) {
        return tienda->ventas[*id - 1].importe == importe ? TIENDA_OK : TIENDA_CONFLICTO;
    }
    ResultadoTiendaLocal resultado = registrar(tienda, importe, clave);
    if (resultado == TIENDA_OK) {
        *id = tienda->cantidad;
    }
    return resultado;
}

ResultadoTiendaLocal tienda_local_cerrar(TiendaLocal *tienda) {
    if (tienda == NULL || tienda->fd_historial < 0) {
        return TIENDA_PARAMETRO_INVALIDO;
    }

    int descriptor = tienda->fd_historial;
    tienda->fd_historial = -1;
    free(tienda->ventas);
    tienda->ventas = NULL;
    tienda->cantidad = tienda->capacidad = 0;
    return close(descriptor) == 0 ? TIENDA_OK : TIENDA_ERROR_CIERRE;
}
