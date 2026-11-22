#include "tienda_local.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    TAM_REGISTRO = sizeof "9223372036854775807 1\n",
    TAM_BLOQUE = 1024
};

static bool aplicar_registro(TiendaLocal *tienda, const char *registro) {
    if (tienda->cerrada_por_meta || registro[0] < '1' || registro[0] > '9') {
        return false;
    }

    char *fin;
    errno = 0;
    intmax_t importe = strtoimax(registro, &fin, 10);
    if (errno == ERANGE || importe > INT64_MAX - tienda->ingreso_propio ||
        fin[0] != ' ' || (fin[1] != '0' && fin[1] != '1') || fin[2] != '\0') {
        return false;
    }

    tienda->ingreso_propio += (int64_t)importe;
    tienda->cerrada_por_meta = fin[1] == '1';
    return true;
}

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
                if (!aplicar_registro(tienda, registro)) {
                    return TIENDA_HISTORIAL_INVALIDO;
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

ResultadoTiendaLocal tienda_local_abrir(TiendaLocal *tienda, const char *ruta,
                                        int64_t meta) {
    if (tienda == NULL || tienda->fd_historial >= 0 ||
        ruta == NULL || ruta[0] == '\0' || meta <= 0) {
        return TIENDA_PARAMETRO_INVALIDO;
    }

    TiendaLocal nueva = TIENDA_LOCAL_INICIAL;
    nueva.meta = meta;
    nueva.fd_historial = open(ruta, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (nueva.fd_historial < 0) {
        return TIENDA_ERROR_APERTURA;
    }

    struct stat atributos;
    if (fstat(nueva.fd_historial, &atributos) < 0 || !S_ISREG(atributos.st_mode)) {
        close(nueva.fd_historial);
        return TIENDA_ERROR_APERTURA;
    }

    ResultadoTiendaLocal resultado = reconstruir_historial(&nueva);
    // La marca histórica mantiene el cierre aunque cambie la meta de prueba.
    if (resultado == TIENDA_OK && !nueva.cerrada_por_meta &&
        nueva.ingreso_propio >= meta) {
        resultado = TIENDA_META_INCOMPATIBLE;
    }
    if (resultado != TIENDA_OK) {
        close(nueva.fd_historial);
        return resultado;
    }

    *tienda = nueva;
    return TIENDA_OK;
}

ResultadoTiendaLocal tienda_local_registrar(TiendaLocal *tienda, int64_t importe) {
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
    int longitud = snprintf(registro, sizeof registro, "%" PRId64 " %d\n",
                            importe, alcanza_meta);
    if (longitud < 0 || (size_t)longitud >= sizeof registro) {
        return TIENDA_ERROR_ESCRITURA;
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
            // Mantiene el total previo y evita anexar detrás de un registro parcial.
            tienda->historial_por_verificar = true;
            return TIENDA_ERROR_ESCRITURA;
        }
        escritos += (size_t)cantidad;
    }

    tienda->ingreso_propio = ingreso_nuevo;
    tienda->cerrada_por_meta = alcanza_meta;
    return TIENDA_OK;
}

ResultadoTiendaLocal tienda_local_cerrar(TiendaLocal *tienda) {
    if (tienda == NULL || tienda->fd_historial < 0) {
        return TIENDA_PARAMETRO_INVALIDO;
    }

    int descriptor = tienda->fd_historial;
    tienda->fd_historial = -1;
    return close(descriptor) == 0 ? TIENDA_OK : TIENDA_ERROR_CIERRE;
}
