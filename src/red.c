#include "red.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
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

int64_t red_reloj_ms(void) {
    struct timespec ahora;
    if (clock_gettime(CLOCK_MONOTONIC, &ahora) == -1) {
        return -1;
    }
    return (int64_t)ahora.tv_sec * 1000 + ahora.tv_nsec / 1000000;
}

static int64_t nuevo_plazo(void) {
    int64_t ahora = red_reloj_ms();
    return ahora < 0 ? -1 : ahora + RED_PLAZO_MS;
}

static int64_t plazo_hasta(int64_t limite) {
    int64_t plazo = nuevo_plazo();
    return limite >= 0 && limite < plazo ? limite : plazo;
}

static bool esperar(int fd, short evento, int64_t vence) {
    if (vence < 0) {
        return false;
    }
    struct pollfd objetivo = {.fd = fd, .events = evento};
    for (;;) {
        int64_t ahora = red_reloj_ms();
        if (ahora < 0) {
            return false;
        }
        if (ahora >= vence) {
            errno = ETIMEDOUT;
            return false;
        }
        int estado = poll(&objetivo, 1, (int)(vence - ahora));
        if (estado > 0) {
            ahora = red_reloj_ms();
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

bool red_no_bloqueante(int fd) {
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
            lector->recibido_en = red_reloj_ms();
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

static int conectar(const ConfigTienda *destino, int64_t limite) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        return -1;
    }
    if (!red_no_bloqueante(fd)) {
        return cerrar_por_error(fd);
    }
    int64_t vence = plazo_hasta(limite);
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

bool red_fin_solicitado(int fd) {
    struct pollfd entrada = {.fd = fd, .events = POLLIN};
    return poll(&entrada, 1, 0) > 0 && (entrada.revents & (POLLHUP | POLLERR | POLLNVAL));
}

static void atender(int fd, const ServidorRed *servidor) {
    LectorRed lector = {0};
    char linea[RED_MAX_LINEA + 1];
    while (!red_fin_solicitado(servidor->fin)) {
        // Los bytes de la siguiente línea ya recibidos conservan su hora de llegada.
        int64_t vence = lector.siguiente < lector.fin
                           ? lector.recibido_en + RED_PLAZO_MS : nuevo_plazo();
        ResultadoLinea resultado = leer_linea(fd, &lector, linea, vence);
        if (resultado == LINEA_FIN || resultado == LINEA_ERROR) {
            return;
        }
        char respuesta[RED_MAX_LINEA + 1] = "ERROR\n";
        if (resultado == LINEA_COMPLETA && *linea &&
            !servidor->atender_local(servidor->canal, linea, respuesta)) {
            return;
        }
        if (!escribir_linea(fd, respuesta, strlen(respuesta), nuevo_plazo()) ||
            resultado != LINEA_COMPLETA) {
            return;
        }
    }
}

void red_recibir(const ServidorRed *servidor) {
    for (;;) {
        struct pollfd entradas[] = {
            {.fd = servidor->escucha, .events = POLLIN},
            {.fd = servidor->fin, .events = POLLIN}
        };
        int estado = poll(entradas, 2, -1);
        if (estado < 0 && errno == EINTR) {
            continue;
        }
        if (estado < 0 || entradas[1].revents || !(entradas[0].revents & POLLIN)) {
            return;
        }
        int fd = accept(servidor->escucha, NULL, NULL);
        if (fd == -1) {
            // Si el otro receptor ya aceptó la conexión, se vuelve a esperar.
            if (errno == EINTR || errno == ECONNABORTED || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
                // Otra conexión puede liberar recursos antes del siguiente intento.
                const struct timespec pausa = {.tv_nsec = 100000000};
                nanosleep(&pausa, NULL);
                continue;
            }
            perror("accept");
            return;
        }
        if (red_no_bloqueante(fd)) {
            atender(fd, servidor);
        }
        close(fd);
    }
}

bool red_intercambiar(const ConfigTienda *destino, const char *linea, char *respuesta,
                      char *error, size_t capacidad_error) {
    return red_intercambiar_hasta(destino, linea, respuesta, error, capacidad_error, -1);
}

bool red_intercambiar_hasta(const ConfigTienda *destino, const char *linea, char *respuesta,
                            char *error, size_t capacidad_error, int64_t limite) {
    size_t longitud = strlen(linea);
    if (longitud == 0 || longitud > RED_MAX_LINEA || linea[longitud - 1] != '\n') {
        snprintf(error, capacidad_error, "mensaje demasiado largo");
        return false;
    }
    if (limite >= 0 && red_reloj_ms() >= limite) {
        snprintf(error, capacidad_error, "plazo agotado");
        return false;
    }
    int fd = conectar(destino, limite);
    if (fd == -1) {
        snprintf(error, capacidad_error, "conexión con %s: %s", destino->id, strerror(errno));
        return false;
    }
    bool exito = false;
    if (!escribir_linea(fd, linea, longitud, plazo_hasta(limite))) {
        snprintf(error, capacidad_error, "envío a %s: %s", destino->id, strerror(errno));
    } else {
        LectorRed lector = {0};
        ResultadoLinea resultado = leer_linea(fd, &lector, respuesta, plazo_hasta(limite));
        if (resultado == LINEA_ERROR) {
            snprintf(error, capacidad_error, "respuesta de %s: %s", destino->id, strerror(errno));
        } else if (resultado == LINEA_COMPLETA && *respuesta) {
            exito = true;
        } else {
            snprintf(error, capacidad_error, "respuesta inválida o incompleta de %s", destino->id);
        }
    }
    close(fd);
    return exito;
}
