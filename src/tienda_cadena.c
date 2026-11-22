#include "tienda_cadena.h"
#include "red.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { EMISOR = RED_RECEPTORES, TRABAJADORES = RED_RECEPTORES + 1,
       REINTENTO_MS = 500 };

// Cada pipe lleva registros de tamaño fijo.
typedef struct {
    size_t destino;
    size_t cantidad;
    unsigned char orden[CONFIG_MAX_TIENDAS];
    char linea[RED_MAX_LINEA + 1];
    char confirmacion[RED_MAX_LINEA + 1];
    bool inicio;
    bool exito;
} Mensaje;

_Static_assert(sizeof(Mensaje) <= _POSIX_PIPE_BUF, "Un mensaje debe caber en un pipe");

typedef struct {
    int desde_hilo[2];
    int hacia_hilo[2];
    Mensaje entrada, salida;
    size_t leidos, escritos;
    bool enviar;
} Canal;

typedef struct {
    ServidorRed red;
    int lectura, escritura;
    size_t cantidad_configurada;
} Trabajador;

typedef struct {
    size_t origen;
    int64_t secuencia, importe;
    char clave[CONFIG_MAX_ID + 1];
    size_t turno;          // Siguiente posición en orden. Cadena.cantidad indica fin.
    size_t confirmada_en;  // Posición de la última aplicación confirmada. Empieza en el origen.
    int64_t reintentar;    // Instante monotónico en ms. Cero permite intentar de inmediato.
} Venta;

/*
 * Solo el propietario accede al historial, la lista y los totales.
 * Los trabajadores reciben copias y leen la configuración fija.
 */
typedef struct {
    const Configuracion *config;
    const ConfigTienda *tienda;
    const char *id_cadena;
    int historial;
    bool historial_dudoso;
    // Cada posición de incorporación contiene un índice de config->tiendas.
    unsigned char orden[CONFIG_MAX_TIENDAS];
    size_t cantidad, cantidad_configurada, propia;
    bool lista;
    int64_t secuencia, ingreso_propio, total;
    Venta *ventas;
    // proxima es el punto de partida de la búsqueda circular. en_envio guarda
    // el índice de la venta en curso o SIZE_MAX si el emisor está libre.
    size_t usadas, capacidad, proxima, en_envio;
    bool incorporando;
} Cadena;

static volatile sig_atomic_t avisar_fin = -1;

// El byte de aviso despierta al propietario mientras espera en poll.
static void solicitar_fin(int senal) {
    (void)senal;
    int anterior = errno;
    char aviso = 0;
    if (avisar_fin >= 0) {
        (void)write(avisar_fin, &aviso, 1);
    }
    errno = anterior;
}

/*
 * Lee o escribe un registro en el extremo bloqueante del trabajador.
 * Devuelve true si lo completa y false si el pipe se cierra o falla.
 */
static bool transferir(int fd, Mensaje *mensaje, bool escribir) {
    char *datos = (char *)mensaje;
    size_t hechos = 0;
    while (hechos < sizeof *mensaje) {
        ssize_t n = escribir ? write(fd, datos + hechos, sizeof *mensaje - hechos)
                             : read(fd, datos + hechos, sizeof *mensaje - hechos);
        if (n > 0) {
            hechos += (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

static bool pasar_al_propietario(void *contexto, const char *linea, char *respuesta) {
    Trabajador *hilo = contexto;
    Mensaje mensaje = {0};
    strcpy(mensaje.linea, linea);
    if (!transferir(hilo->escritura, &mensaje, true) ||
        !transferir(hilo->lectura, &mensaje, false)) {
        return false;
    }
    strcpy(respuesta, mensaje.linea);
    return true;
}

static void *recibir(void *contexto) {
    Trabajador *hilo = contexto;
    red_recibir(&hilo->red);
    close(hilo->escritura);
    close(hilo->lectura);
    return NULL;
}

static size_t separar(char *linea, char *campos[6]) {
    char *fin = strchr(linea, '\n');
    if (!fin || fin[1] != '\0') {
        return 0;
    }
    *fin = '\0';
    size_t cantidad = 0;
    while (cantidad < 6 && *linea && *linea != ' ') {
        campos[cantidad++] = linea;
        char *espacio = strchr(linea, ' ');
        if (!espacio) {
            return cantidad;
        }
        *espacio = '\0';
        linea = espacio + 1;
    }
    return 0;
}

static bool numero(const char *campo, int64_t *valor) {
    if (*campo < '0' || *campo > '9' || (campo[0] == '0' && campo[1])) {
        return false;
    }
    errno = 0;
    char *fin;
    intmax_t resultado = strtoimax(campo, &fin, 10);
    if (errno == ERANGE || *fin || resultado > INT64_MAX) {
        return false;
    }
    *valor = (int64_t)resultado;
    return true;
}

static size_t posicion(const Cadena *cadena, size_t tienda) {
    for (size_t i = 0; i < cadena->cantidad; ++i) {
        if (cadena->orden[i] == tienda) {
            return i;
        }
    }
    return cadena->cantidad;
}

static int64_t siguiente(const char *campo, size_t maximo) {
    int64_t valor;
    return numero(campo, &valor) && (uint64_t)valor <= maximo ? valor : -1;
}

static bool solicitar(Trabajador *hilo, const ConfigTienda *destino,
                      const char *linea, char *respuesta) {
    char error[256];
    return !red_fin_solicitado(hilo->lectura) &&
           red_intercambiar(destino, linea, respuesta, error, sizeof error);
}

static bool consultar_posicion(Trabajador *hilo, const ConfigTienda *par, size_t indice,
                               const ConfigTienda **integrante) {
    char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1], *campos[6];
    const Configuracion *config = hilo->red.config;
    snprintf(linea, sizeof linea, "ORDEN %s %zu\n",
             config->cadenas[par->cadena].id, indice);
    if (!solicitar(hilo, par, linea, respuesta)) {
        return false;
    }
    size_t cantidad = separar(respuesta, campos);
    if (cantidad == 2 && strcmp(campos[0], "FIN") == 0 &&
        siguiente(campos[1], CONFIG_MAX_TIENDAS) == (int64_t)indice) {
        *integrante = NULL;
        return true;
    }
    if (cantidad != 3 || strcmp(campos[0], "POS") != 0 ||
        siguiente(campos[1], CONFIG_MAX_TIENDAS) != (int64_t)indice) {
        return false;
    }
    *integrante = configuracion_buscar(config, campos[2]);
    return *integrante && (*integrante)->cadena == hilo->red.tienda->cadena;
}

// Incorporación y envíos TCP

/*
 * Pide el alta a los integrantes anteriores y solicita sellar la lista si
 * es la última incorporación. Devuelve true si los pares aceptan y deja
 * el orden acordado en mensaje.
 */
static bool incorporarse(Trabajador *hilo, Mensaje *mensaje) {
    const Configuracion *config = hilo->red.config;
    const ConfigTienda *propia = hilo->red.tienda;
    const ConfigTienda *entrada = &config->tiendas[mensaje->destino];
    bool vistos[CONFIG_MAX_TIENDAS] = {false};
    size_t cantidad = 0;
    bool fin = false;
    while (cantidad < hilo->cantidad_configurada) {
        const ConfigTienda *par;
        if (!consultar_posicion(hilo, entrada, cantidad, &par)) {
            return false;
        }
        if (!par) {
            fin = true;
            break;
        }
        size_t indice = (size_t)(par - config->tiendas);
        if (par == propia || vistos[indice]) {
            return false;
        }
        vistos[indice] = true;
        mensaje->orden[cantidad++] = (unsigned char)indice;
    }
    if (!fin || cantidad == 0 || !vistos[mensaje->destino]) {
        return false;
    }
    // Se comprueba también FIN para detectar si algún par conoce una lista más larga.
    for (size_t j = 0; j < cantidad; ++j) {
        const ConfigTienda *par = &config->tiendas[mensaje->orden[j]];
        for (size_t i = 0; i <= cantidad; ++i) {
            const ConfigTienda *declarada;
            const ConfigTienda *esperada = i < cantidad ? &config->tiendas[mensaje->orden[i]] : NULL;
            if (!consultar_posicion(hilo, par, i, &declarada) || declarada != esperada) {
                return false;
            }
        }
    }
    char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1];
    const char *cadena = config->cadenas[propia->cadena].id;
    snprintf(linea, sizeof linea, "ALTA %s %s %zu\n", cadena, propia->id, cantidad);
    for (size_t i = 0; i < cantidad; ++i) {
        if (!solicitar(hilo, &config->tiendas[mensaje->orden[i]], linea, respuesta) ||
            strcmp(respuesta, "ALTA_OK\n") != 0) {
            return false;
        }
    }
    if (cantidad + 1 == hilo->cantidad_configurada) {
        snprintf(linea, sizeof linea, "SELLAR %s %s %zu\n", cadena, propia->id, cantidad + 1);
        for (size_t i = 0; i < cantidad; ++i) {
            if (!solicitar(hilo, &config->tiendas[mensaje->orden[i]], linea, respuesta) ||
                strcmp(respuesta, "LISTA_OK\n") != 0) {
                return false;
            }
        }
    }
    mensaje->orden[cantidad] = (unsigned char)(propia - config->tiendas);
    mensaje->cantidad = cantidad + 1;
    return true;
}

/*
 * Atiende encargos de red y devuelve el resultado al propietario por el pipe.
 * Un envío de venta solo se confirma si coincide la respuesta de aplicación.
 * Devuelve NULL cuando se cierra el canal o falla la transferencia.
 */
static void *emitir(void *contexto) {
    Trabajador *hilo = contexto;
    Mensaje trabajo;
    while (transferir(hilo->lectura, &trabajo, false)) {
        if (trabajo.inicio) {
            trabajo.exito = incorporarse(hilo, &trabajo);
        } else {
            char respuesta[RED_MAX_LINEA + 1];
            const ConfigTienda *par = &hilo->red.config->tiendas[trabajo.destino];
            trabajo.exito = solicitar(hilo, par, trabajo.linea, respuesta) &&
                            strcmp(respuesta, trabajo.confirmacion) == 0;
        }
        if (!transferir(hilo->escritura, &trabajo, true)) {
            break;
        }
    }
    close(hilo->escritura);
    close(hilo->lectura);
    return NULL;
}

static void anunciar_lista(const Cadena *cadena) {
    printf("LISTA %s", cadena->tienda->id);
    for (size_t i = 0; i < cadena->cantidad; ++i) {
        printf(" %s", cadena->config->tiendas[cadena->orden[i]].id);
    }
    puts("");
}

// Registro y aplicación de ventas

/*
 * Anexa una venta validada y actualiza los totales después de escribir la línea
 * completa. Devuelve NULL si la aplica o un mensaje de error si falla.
 */
static const char *registrar(Cadena *cadena, Venta venta) {
    if (cadena->historial_dudoso) {
        return "ERROR_HISTORIAL\n";
    }
    if (venta.importe > INT64_MAX - cadena->total) {
        return "DESBORDAMIENTO\n";
    }
    if (cadena->usadas == cadena->capacidad) {
        if (cadena->capacidad > SIZE_MAX / 2 / sizeof venta) {
            return "SIN_ESPACIO\n";
        }
        size_t nueva = cadena->capacidad ? cadena->capacidad * 2 : 32;
        Venta *ventas = realloc(cadena->ventas, nueva * sizeof venta);
        if (!ventas) {
            return "SIN_ESPACIO\n";
        }
        cadena->ventas = ventas;
        cadena->capacidad = nueva;
    }
    const char *origen = cadena->config->tiendas[venta.origen].id;
    char linea[RED_MAX_LINEA + 1];
    int largo = snprintf(linea, sizeof linea, "APLICADA %s %s %" PRId64 " %" PRId64 "\n",
                         cadena->id_cadena, origen, venta.secuencia, venta.importe);
    if (largo < 0 || largo > RED_MAX_LINEA) {
        return "ERROR_HISTORIAL\n";
    }
    size_t usados = 0;
    while (usados < (size_t)largo) {
        ssize_t n = write(cadena->historial, linea + usados, (size_t)largo - usados);
        if (n > 0) {
            usados += (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            // Si falla la escritura, el archivo puede quedar con una línea incompleta.
            cadena->historial_dudoso = true;
            return "ERROR_HISTORIAL\n";
        }
    }
    cadena->ventas[cadena->usadas++] = venta;
    cadena->total += venta.importe;
    if (venta.origen == cadena->propia) {
        cadena->secuencia = venta.secuencia;
        cadena->ingreso_propio += venta.importe;
    }
    printf("APLICADA %s %s %" PRId64 " %" PRId64 " propio=%" PRId64 " total=%" PRId64 "\n",
           cadena->tienda->id, origen, venta.secuencia, venta.importe,
           cadena->ingreso_propio, cadena->total);
    return NULL;
}

static bool clave_valida(const char *clave) {
    const char *permitidos = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    return strlen(clave) <= CONFIG_MAX_ID && clave[strspn(clave, permitidos)] == '\0';
}

static void compra(Cadena *cadena, char *campos[6], char *respuesta) {
    int64_t importe;
    if (!clave_valida(campos[1]) || !numero(campos[2], &importe) || importe == 0) {
        strcpy(respuesta, "ERROR\n");
        return;
    }
    // Reutiliza la secuencia original si se repiten la clave y el importe.
    for (size_t i = 0; i < cadena->usadas; ++i) {
        const Venta *venta = &cadena->ventas[i];
        if (venta->origen == cadena->propia && strcmp(venta->clave, campos[1]) == 0) {
            if (venta->importe == importe) {
                snprintf(respuesta, RED_MAX_LINEA + 1, "ADMITIDA %" PRId64 "\n", venta->secuencia);
            } else {
                strcpy(respuesta, "CONFLICTO\n");
            }
            return;
        }
    }
    if (cadena->secuencia == INT64_MAX) {
        strcpy(respuesta, "DESBORDAMIENTO\n");
        return;
    }
    Venta venta = {.origen = cadena->propia, .secuencia = cadena->secuencia + 1,
                   .importe = importe, .turno = cadena->orden[0] == cadena->propia ? 1 : 0,
                   .confirmada_en = posicion(cadena, cadena->propia)};
    strcpy(venta.clave, campos[1]);
    const char *error = registrar(cadena, venta);
    if (error) {
        strcpy(respuesta, error);
    } else {
        snprintf(respuesta, RED_MAX_LINEA + 1, "ADMITIDA %" PRId64 "\n", venta.secuencia);
        if (venta.turno == cadena->cantidad) {
            printf("DIFUNDIDA %s %" PRId64 "\n", cadena->tienda->id, venta.secuencia);
        }
    }
}

/*
 * Devuelve la posición de quien aplica justo antes del destinatario. El
 * primero sigue al origen. El resto salta la posición del origen en la lista.
 */
static size_t predecesora(size_t turno, size_t origen) {
    if (turno == 0 || (turno == 1 && origen == 0)) {
        return origen;
    }
    return turno - (turno - 1 == origen ? 2 : 1);
}

static void confirmacion(char *texto, const Cadena *cadena, const Venta *venta, size_t turno) {
    snprintf(texto, RED_MAX_LINEA + 1, "APLICADA %s %s %" PRId64 " %" PRId64 " %zu\n",
             cadena->id_cadena, cadena->config->tiendas[venta->origen].id,
             venta->secuencia, venta->importe, turno);
}

/*
 * Aplica la venta remota si le toca y escribe la respuesta en el búfer.
 * Si ya se aplicó el mismo ID e importe, confirma sin volver a registrar ni sumar.
 * Si cambia el importe para ese ID, responde CONFLICTO.
 */
static void aplicar_remota(Cadena *cadena, char *campos[6], char *respuesta) {
    const ConfigTienda *origen = configuracion_buscar(cadena->config, campos[2]);
    Venta venta = {.turno = cadena->cantidad};
    int64_t anterior = siguiente(campos[5], cadena->cantidad - 1);
    if (!origen || origen == cadena->tienda || origen->cadena != cadena->tienda->cadena ||
        !numero(campos[3], &venta.secuencia) || venta.secuencia == 0 ||
        !numero(campos[4], &venta.importe) || venta.importe == 0) {
        strcpy(respuesta, "ERROR\n");
        return;
    }
    venta.origen = (size_t)(origen - cadena->config->tiendas);
    size_t turno = posicion(cadena, cadena->propia);
    if (anterior != (int64_t)predecesora(turno, posicion(cadena, venta.origen))) {
        strcpy(respuesta, "FUERA_DE_TURNO\n");
        return;
    }
    for (size_t i = 0; i < cadena->usadas; ++i) {
        const Venta *anterior_venta = &cadena->ventas[i];
        if (anterior_venta->origen == venta.origen && anterior_venta->secuencia == venta.secuencia) {
            if (anterior_venta->importe == venta.importe) {
                confirmacion(respuesta, cadena, &venta, turno);
            } else {
                strcpy(respuesta, "CONFLICTO\n");
            }
            return;
        }
    }
    const char *error = registrar(cadena, venta);
    if (error) {
        strcpy(respuesta, error);
    } else {
        confirmacion(respuesta, cadena, &venta, turno);
    }
}

static void atender(Cadena *cadena, char *linea, char *respuesta) {
    char *campos[6];
    size_t cantidad = separar(linea, campos);
    strcpy(respuesta, "ERROR\n");
    if (cantidad == 3 && strcmp(campos[0], "COMPRA") == 0) {
        if (cadena->lista) {
            compra(cadena, campos, respuesta);
        } else {
            strcpy(respuesta, "ORDEN_NO_LISTO\n");
        }
        return;
    }
    if (cantidad < 3 || strcmp(campos[1], cadena->id_cadena) != 0) {
        return;
    }
    if (cantidad == 3 && strcmp(campos[0], "ORDEN") == 0) {
        int64_t indice = siguiente(campos[2], cadena->cantidad);
        if (indice >= 0 && !cadena->incorporando) {
            if ((size_t)indice == cadena->cantidad) {
                snprintf(respuesta, RED_MAX_LINEA + 1, "FIN %zu\n", cadena->cantidad);
            } else {
                const char *id = cadena->config->tiendas[cadena->orden[indice]].id;
                snprintf(respuesta, RED_MAX_LINEA + 1, "POS %" PRId64 " %s\n", indice, id);
            }
        }
    } else if (cantidad == 4 && (strcmp(campos[0], "ALTA") == 0 ||
                                strcmp(campos[0], "SELLAR") == 0)) {
        const ConfigTienda *nueva = configuracion_buscar(cadena->config, campos[2]);
        int64_t indice = siguiente(campos[3], cadena->cantidad_configurada);
        if (cadena->incorporando || !nueva || nueva->cadena != cadena->tienda->cadena || indice < 0) {
            return;
        }
        size_t nueva_tienda = (size_t)(nueva - cadena->config->tiendas);
        size_t lugar = posicion(cadena, nueva_tienda);
        if (strcmp(campos[0], "ALTA") == 0) {
            if ((size_t)indice == cadena->cantidad && lugar == cadena->cantidad &&
                cadena->cantidad < cadena->cantidad_configurada && !cadena->lista) {
                cadena->orden[cadena->cantidad++] = (unsigned char)nueva_tienda;
                strcpy(respuesta, "ALTA_OK\n");
            }
        } else if ((size_t)indice == cadena->cantidad_configurada &&
                   cadena->cantidad == cadena->cantidad_configurada &&
                   lugar == cadena->cantidad - 1) {
            if (!cadena->lista) {
                cadena->lista = true;
                anunciar_lista(cadena);
            }
            strcpy(respuesta, "LISTA_OK\n");
        }
    } else if (cantidad == 6 && strcmp(campos[0], "VENTA") == 0) {
        if (cadena->lista) {
            aplicar_remota(cadena, campos, respuesta);
        } else {
            strcpy(respuesta, "ORDEN_NO_LISTO\n");
        }
    }
}

// Difusión de pendientes

/*
 * Instala la lista tras el alta o actualiza el turno de la venta en curso.
 * Si el intento falla, conserva el turno y aplaza solo esa venta.
 */
static void terminar_envio(Cadena *cadena, const Mensaje *resultado) {
    if (cadena->incorporando) {
        cadena->incorporando = false;
        cadena->cantidad = resultado->cantidad;
        memcpy(cadena->orden, resultado->orden, cadena->cantidad);
        printf("INCORPORADA %s\n", cadena->tienda->id);
        if (cadena->cantidad == cadena->cantidad_configurada) {
            cadena->lista = true;
            anunciar_lista(cadena);
        }
        return;
    }
    Venta *venta = &cadena->ventas[cadena->en_envio];
    if (resultado->exito) {
        venta->confirmada_en = venta->turno;
        ++venta->turno;
        if (venta->turno < cadena->cantidad && cadena->orden[venta->turno] == cadena->propia) {
            ++venta->turno;
        }
        venta->reintentar = 0;
        if (venta->turno == cadena->cantidad) {
            printf("DIFUNDIDA %s %" PRId64 "\n", cadena->tienda->id, venta->secuencia);
        }
    } else {
        if (venta->reintentar == 0) {
            printf("PENDIENTE %s %" PRId64 " %s\n", cadena->tienda->id, venta->secuencia,
                   cadena->config->tiendas[cadena->orden[venta->turno]].id);
        }
        venta->reintentar = red_reloj_ms() + REINTENTO_MS;
    }
    cadena->en_envio = SIZE_MAX;
}

/*
 * Si el emisor está libre, programa la siguiente venta elegible en el
 * recorrido circular. Devuelve los ms hasta el próximo reintento, -1 si basta
 * esperar un evento o -2 si falla el reloj.
 */
static int programar(Cadena *cadena, Canal *emisor) {
    if (!cadena->lista || cadena->en_envio != SIZE_MAX || emisor->enviar) {
        return -1;
    }
    int espera = -1;
    int64_t ahora = red_reloj_ms();
    if (ahora < 0) {
        return -2;
    }
    for (size_t i = 0; i < cadena->usadas; ++i) {
        size_t indice = (cadena->proxima + i) % cadena->usadas;
        Venta *venta = &cadena->ventas[indice];
        if (venta->origen != cadena->propia || venta->turno == cadena->cantidad) {
            continue;
        }
        if (venta->reintentar > ahora) {
            int resto = (int)(venta->reintentar - ahora);
            if (espera == -1 || resto < espera) {
                espera = resto;
            }
            continue;
        }
        Mensaje salida = {.destino = cadena->orden[venta->turno]};
        // El primer recibo es el registro propio. Los siguientes provienen del ACK cotejado.
        snprintf(salida.linea, sizeof salida.linea, "VENTA %s %s %" PRId64 " %" PRId64 " %zu\n",
                 cadena->id_cadena, cadena->tienda->id, venta->secuencia, venta->importe,
                 venta->confirmada_en);
        confirmacion(salida.confirmacion, cadena, venta, venta->turno);
        emisor->salida = salida;
        emisor->enviar = true;
        cadena->en_envio = indice;
        cadena->proxima = (indice + 1) % cadena->usadas;
        return -1;
    }
    return espera;
}

// Atención de pipes

// Devuelve 1 si completa el registro, 0 si falta leer y -1 si el pipe se cierra o falla.
static int recibir_canal(Canal *canal) {
    char *datos = (char *)&canal->entrada;
    ssize_t n = read(canal->desde_hilo[0], datos + canal->leidos,
                     sizeof canal->entrada - canal->leidos);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    if (n <= 0) {
        return -1;
    }
    canal->leidos += (size_t)n;
    if (canal->leidos < sizeof canal->entrada) {
        return 0;
    }
    canal->leidos = 0;
    return 1;
}

static bool enviar_canal(Canal *canal) {
    const char *datos = (const char *)&canal->salida;
    ssize_t n = write(canal->hacia_hilo[1], datos + canal->escritos,
                      sizeof canal->salida - canal->escritos);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        return true;
    }
    if (n <= 0) {
        return false;
    }
    canal->escritos += (size_t)n;
    if (canal->escritos == sizeof canal->salida) {
        canal->enviar = false;
        canal->escritos = 0;
    }
    return true;
}

/*
 * Atiende los pipes listos y programa reintentos. Si un mensaje queda a medias,
 * guarda el avance de ese canal y sigue atendiendo los demás. Devuelve true
 * si se solicita salir y false si falla el servicio.
 */
static bool atender_canales(Cadena *cadena, Canal canales[TRABAJADORES], int fin) {
    for (;;) {
        int espera = programar(cadena, &canales[EMISOR]);
        if (espera < -1) {
            return false;
        }
        struct pollfd fds[2 * TRABAJADORES + 1];
        fds[0] = (struct pollfd){.fd = fin, .events = POLLIN};
        for (size_t i = 0; i < TRABAJADORES; ++i) {
            fds[1 + 2 * i] = (struct pollfd){.fd = canales[i].desde_hilo[0], .events = POLLIN};
            fds[2 + 2 * i] = (struct pollfd){.fd = canales[i].enviar ? canales[i].hacia_hilo[1] : -1,
                                            .events = POLLOUT};
        }
        int listos = poll(fds, 2 * TRABAJADORES + 1, espera);
        if (listos < 0 && errno != EINTR) {
            return false;
        }
        if (listos <= 0) {
            continue;
        }
        if (fds[0].revents) {
            return true;
        }
        for (size_t i = 0; i < TRABAJADORES; ++i) {
            Canal *canal = &canales[i];
            if (fds[2 + 2 * i].revents && !enviar_canal(canal)) {
                return false;
            }
            if (!fds[1 + 2 * i].revents) {
                continue;
            }
            int estado = recibir_canal(canal);
            if (estado < 0) {
                return false;
            }
            if (estado == 1) {
                if (i == EMISOR) {
                    if (cadena->incorporando && !canal->entrada.exito) {
                        fprintf(stderr, "No se completó la incorporación de %s\n", cadena->tienda->id);
                        return false;
                    }
                    terminar_envio(cadena, &canal->entrada);
                } else {
                    atender(cadena, canal->entrada.linea, canal->salida.linea);
                    canal->enviar = true;
                }
            }
        }
    }
}

// Arranque y terminación

/*
 * Abre o crea un historial regular vacío para iniciar el contador. Devuelve
 * el descriptor o -1 si no se puede usar. Conserva los archivos con datos.
 */
static int abrir_historial(const char *ruta) {
    int fd = open(ruta, O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK, 0600);
    if (fd == -1) {
        perror(ruta);
        return -1;
    }
    struct stat datos;
    if (fstat(fd, &datos) == -1 || !S_ISREG(datos.st_mode) || datos.st_size != 0) {
        fprintf(stderr, "El historial de cadena debe ser un archivo regular vacío: %s\n", ruta);
        close(fd);
        return -1;
    }
    return fd;
}

static bool preparar_senales(int fin[2]) {
    if (pipe(fin) == -1 || !red_no_bloqueante(fin[1])) {
        return false;
    }
    avisar_fin = fin[1];
    struct sigaction accion = {.sa_handler = solicitar_fin};
    sigemptyset(&accion.sa_mask);
    return sigaction(SIGINT, &accion, NULL) == 0 && sigaction(SIGTERM, &accion, NULL) == 0;
}

int tienda_cadena_ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                          const ConfigTienda *entrada) {
    int escucha = red_escuchar(tienda);
    if (escucha == -1) {
        perror("escucha de cadena");
        return EXIT_FAILURE;
    }
    if (!red_no_bloqueante(escucha)) {
        perror("escucha no bloqueante");
        close(escucha);
        return EXIT_FAILURE;
    }
    Cadena cadena = {.config = config, .tienda = tienda, .id_cadena = config->cadenas[tienda->cadena].id,
                     .propia = (size_t)(tienda - config->tiendas), .en_envio = SIZE_MAX};
    cadena.historial = abrir_historial(tienda->historial);
    if (cadena.historial == -1) {
        close(escucha);
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        if (config->tiendas[i].cadena == tienda->cadena) {
            ++cadena.cantidad_configurada;
        }
    }

    Canal canales[TRABAJADORES] = {0};
    Trabajador trabajadores[TRABAJADORES];
    pthread_t hilos[TRABAJADORES];
    size_t iniciados = 0;
    int fin[2] = {-1, -1};
    int estado = EXIT_FAILURE;
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        canales[i].desde_hilo[0] = canales[i].desde_hilo[1] = -1;
        canales[i].hacia_hilo[0] = canales[i].hacia_hilo[1] = -1;
    }
    if (!preparar_senales(fin)) {
        perror("señales de cadena");
        goto cerrar;
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        Canal *canal = &canales[i];
        if (pipe(canal->desde_hilo) == -1 || pipe(canal->hacia_hilo) == -1 ||
            !red_no_bloqueante(canal->desde_hilo[0]) || !red_no_bloqueante(canal->hacia_hilo[1])) {
            perror("pipe de cadena");
            goto cerrar;
        }
        Trabajador *hilo = &trabajadores[i];
        *hilo = (Trabajador){
            .red = {escucha, config, tienda, canal->hacia_hilo[0], pasar_al_propietario, hilo},
            .lectura = canal->hacia_hilo[0], .escritura = canal->desde_hilo[1],
            .cantidad_configurada = cadena.cantidad_configurada
        };
        int error = pthread_create(&hilos[i], NULL, i == EMISOR ? emitir : recibir, hilo);
        if (error) {
            fprintf(stderr, "pthread_create: %s\n", strerror(error));
            goto cerrar;
        }
        ++iniciados;
    }

    if (entrada) {
        cadena.incorporando = true;
        canales[EMISOR].salida = (Mensaje){.destino = (size_t)(entrada - config->tiendas),
                                         .inicio = true};
        canales[EMISOR].enviar = true;
    } else {
        cadena.orden[0] = (unsigned char)cadena.propia;
        cadena.cantidad = 1;
        printf("INCORPORADA %s\n", tienda->id);
        if (cadena.cantidad_configurada == 1) {
            cadena.lista = true;
            anunciar_lista(&cadena);
        }
    }
    if (atender_canales(&cadena, canales, fin[0])) {
        estado = EXIT_SUCCESS;
    } else {
        fprintf(stderr, "Se interrumpió el servicio de cadena de %s\n", tienda->id);
    }

cerrar:
    // El cierre de las salidas despierta a los hilos. Se esperan antes
    // de cerrar el socket y los extremos restantes.
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        close(canales[i].hacia_hilo[1]);
    }
    for (size_t i = 0; i < iniciados; ++i) {
        pthread_join(hilos[i], NULL);
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        close(canales[i].desde_hilo[0]);
        if (i >= iniciados) {
            close(canales[i].desde_hilo[1]);
            close(canales[i].hacia_hilo[0]);
        }
    }
    avisar_fin = -1;
    close(fin[0]);
    close(fin[1]);
    close(escucha);
    if (close(cadena.historial) == -1) {
        perror("cierre del historial");
        estado = EXIT_FAILURE;
    }
    free(cadena.ventas);
    return estado;
}
