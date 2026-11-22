#include "red.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    char datos[512];
    size_t siguiente;
    size_t fin;
    int64_t recibido_en;
} LectorRed;

typedef enum {
    LINEA_COMPLETA,
    LINEA_LARGA,
    LINEA_INCOMPLETA,
    LINEA_FIN,
    LINEA_ERROR
} ResultadoLinea;

static const char tipo_prueba[] = "PRUEBA_VENTA ";
static const char recibido[] = "RECIBIDO\n";
static const char rechazado[] = "ERROR\n";

static int64_t reloj_ms(void) {
    struct timespec ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &ahora) == -1) {
        return -1;
    }
    return (int64_t)ahora.tv_sec * 1000 + ahora.tv_nsec / 1000000;
}

static int64_t nuevo_plazo(void) {
    int64_t ahora = reloj_ms();
    return ahora < 0 ? -1 : ahora + RED_PLAZO_MS;
}

static bool esperar(int fd, short evento, int64_t vence) {
    if (vence < 0) {
        return false;
    }
    struct pollfd objetivo = {.fd = fd, .events = evento};
    for (;;) {
        int64_t ahora = reloj_ms();
        if (ahora < 0) {
            return false;
        }
        if (ahora >= vence) {
            errno = ETIMEDOUT;
            return false;
        }
        int estado = poll(&objetivo, 1, (int)(vence - ahora));
        if (estado > 0) {
            ahora = reloj_ms();
            if (ahora < 0) {
                return false;
            }
            if (ahora < vence) {
                // La operación del socket informa EOF o el error concreto.
                return true;
            }
            errno = ETIMEDOUT;
            return false;
        }
        if (estado == -1 && errno != EINTR) {
            return false;
        }
    }
}

static bool preparar_conexion(int fd) {
    int opciones = fcntl(fd, F_GETFL);
    return opciones != -1 && fcntl(fd, F_SETFL, opciones | O_NONBLOCK) != -1;
}

static bool escribir_linea(int fd, const char *linea, size_t longitud, int64_t vence) {
    while (longitud != 0 && esperar(fd, POLLOUT, vence)) {
        ssize_t enviados = send(fd, linea, longitud, 0);
        if (enviados > 0) {
            linea += enviados;
            longitud -= (size_t)enviados;
        } else if (enviados == 0) {
            errno = EPIPE;
            return false;
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
    }
    return longitud == 0;
}

static ResultadoLinea leer_linea(int fd, LectorRed *lector, char linea[RED_MAX_LINEA + 1],
                                int64_t vence) {
    size_t usados = 0;
    while (usados < RED_MAX_LINEA) {
        if (lector->siguiente == lector->fin) {
            if (!esperar(fd, POLLIN, vence)) {
                return LINEA_ERROR;
            }
            ssize_t leidos = recv(fd, lector->datos, sizeof lector->datos, 0);
            if (leidos == 0) {
                return usados == 0 ? LINEA_FIN : LINEA_INCOMPLETA;
            }
            if (leidos == -1) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                return LINEA_ERROR;
            }
            lector->recibido_en = reloj_ms();
            if (lector->recibido_en < 0) {
                return LINEA_ERROR;
            }
            if (lector->recibido_en >= vence) {
                errno = ETIMEDOUT;
                return LINEA_ERROR;
            }
            lector->siguiente = 0;
            lector->fin = (size_t)leidos;
        }
        char byte = lector->datos[lector->siguiente++];
        linea[usados++] = byte;
        if (byte == '\n') {
            linea[usados] = '\0';
            // Reemplazar la línea por una trama inválida si contenía un NUL.
            if (memchr(linea, '\0', usados)) {
                linea[0] = '\0';
            }
            return LINEA_COMPLETA;
        }
    }
    return LINEA_LARGA;
}

static struct sockaddr_in direccion_de(const ConfigTienda *tienda) {
    struct sockaddr_in direccion = {.sin_family = AF_INET, .sin_port = htons(tienda->puerto)};
    inet_pton(AF_INET, tienda->direccion, &direccion.sin_addr);
    return direccion;
}

static int cerrar_por_error(int fd) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
}

int red_escuchar(const ConfigTienda *tienda) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    int reutilizar = 1;
    struct sockaddr_in direccion = direccion_de(tienda);
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reutilizar, sizeof reutilizar) == -1 ||
        bind(fd, (const struct sockaddr *)&direccion, sizeof direccion) == -1 ||
        listen(fd, SOMAXCONN) == -1) {
        return cerrar_por_error(fd);
    }
    return fd;
}

static int conectar(const ConfigTienda *destino) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (!preparar_conexion(fd)) {
        return cerrar_por_error(fd);
    }
    int64_t vence = nuevo_plazo();
    if (vence < 0) {
        return cerrar_por_error(fd);
    }
    struct sockaddr_in direccion = direccion_de(destino);
    if (connect(fd, (const struct sockaddr *)&direccion, sizeof direccion) == -1) {
        if ((errno != EINPROGRESS && errno != EINTR) || !esperar(fd, POLLOUT, vence)) {
            return cerrar_por_error(fd);
        }
        int error;
        socklen_t largo = sizeof error;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &largo) == -1) {
            return cerrar_por_error(fd);
        }
        if (error != 0) {
            errno = error;
            return cerrar_por_error(fd);
        }
    }
    return fd;
}

static bool trama_valida(const ServidorRed *servidor, char *linea) {
    size_t largo_tipo = sizeof tipo_prueba - 1;
    if (strncmp(linea, tipo_prueba, largo_tipo) != 0) {
        return false;
    }
    char *cadena = linea + largo_tipo;
    char *origen = strchr(cadena, ' ');
    if (!origen) {
        return false;
    }
    *origen++ = '\0';
    char *importe = strchr(origen, ' ');
    if (!importe) {
        return false;
    }
    *importe++ = '\0';
    const ConfigTienda *par = configuracion_buscar(servidor->config, origen);
    if (strcmp(cadena, servidor->config->cadenas[servidor->tienda->cadena].id) != 0 ||
        !par || par == servidor->tienda || par->cadena != servidor->tienda->cadena ||
        *importe < '1' || *importe > '9') {
        return false;
    }
    errno = 0;
    char *fin;
    intmax_t centavos = strtoimax(importe, &fin, 10);
    return errno != ERANGE && centavos <= INT64_MAX && strcmp(fin, "\n") == 0;
}

static void atender(int fd, const ServidorRed *servidor) {
    LectorRed lector = {0};
    char linea[RED_MAX_LINEA + 1];
    for (;;) {
        // Los bytes de la siguiente línea ya recibidos conservan su hora de llegada.
        int64_t vence = lector.siguiente < lector.fin
                           ? lector.recibido_en + RED_PLAZO_MS : nuevo_plazo();
        ResultadoLinea resultado = leer_linea(fd, &lector, linea, vence);
        if (resultado == LINEA_FIN || resultado == LINEA_ERROR) {
            return;
        }
        const char *respuesta = resultado == LINEA_COMPLETA && trama_valida(servidor, linea)
                                    ? recibido : rechazado;
        if (!escribir_linea(fd, respuesta, strlen(respuesta), nuevo_plazo()) ||
            resultado != LINEA_COMPLETA) {
            return;
        }
    }
}

void *red_recibir(void *contexto) {
    const ServidorRed *servidor = contexto;
    for (;;) {
        int fd = accept(servidor->escucha, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                // Otra conexión puede liberar recursos antes del siguiente intento.
                const struct timespec pausa = {.tv_nsec = 100000000};
                nanosleep(&pausa, NULL);
                continue;
            }
            perror("accept");
            exit(EXIT_FAILURE);
        }
        if (preparar_conexion(fd)) {
            atender(fd, servidor);
        }
        close(fd);
    }
}

bool red_enviar_prueba(const Configuracion *config, const ConfigTienda *origen,
                      const ConfigTienda *destino, int64_t importe,
                      char *error, size_t capacidad_error) {
    if (origen == destino || origen->cadena == CONFIG_SIN_CADENA ||
        origen->cadena != destino->cadena || importe <= 0) {
        snprintf(error, capacidad_error, "destino o importe inválido para %s", origen->id);
        return false;
    }
    char linea[RED_MAX_LINEA + 1];
    int longitud = snprintf(linea, sizeof linea, "%s%s %s %" PRId64 "\n", tipo_prueba,
                             config->cadenas[origen->cadena].id, origen->id, importe);
    if (longitud < 0 || longitud > RED_MAX_LINEA) {
        snprintf(error, capacidad_error, "mensaje demasiado largo");
        return false;
    }
    int fd = conectar(destino);
    if (fd == -1) {
        snprintf(error, capacidad_error, "conexión con %s: %s", destino->id, strerror(errno));
        return false;
    }
    bool exito = false;
    if (!escribir_linea(fd, linea, (size_t)longitud, nuevo_plazo())) {
        snprintf(error, capacidad_error, "envío a %s: %s", destino->id, strerror(errno));
    } else {
        LectorRed lector = {0};
        ResultadoLinea resultado = leer_linea(fd, &lector, linea, nuevo_plazo());
        if (resultado == LINEA_ERROR) {
            snprintf(error, capacidad_error, "respuesta de %s: %s", destino->id, strerror(errno));
        } else if (resultado == LINEA_COMPLETA && strcmp(linea, recibido) == 0) {
            exito = true;
        } else {
            snprintf(error, capacidad_error, "respuesta inválida o incompleta de %s", destino->id);
        }
    }
    close(fd);
    return exito;
}
