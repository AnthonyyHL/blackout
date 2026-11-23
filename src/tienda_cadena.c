#include "tienda_cadena.h"
#include "red.h"
#include "recuperacion.h"
#include "cierre.h"
#include "monitor.h"
#include "tienda_local.h"

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
       REINTENTO_MS = 500, CAMPOS = CONFIG_MAX_TIENDAS + 5,
       MAX_REGISTRO = CAMPOS * (CONFIG_MAX_ID + 1) };

// Cada pipe lleva registros de tamaño fijo.
typedef struct {
    size_t destino;
    size_t cantidad;
    unsigned char orden[CONFIG_MAX_TIENDAS];
    char linea[RED_MAX_LINEA + 1];
    char confirmacion[RED_MAX_LINEA + 1]; // Respuesta que confirma la aplicación en el par.
    CambioLista cambio;
    enum TrabajoCierre cierre;
    bool inicio;
    bool buscar;
    bool exito;
} Mensaje;

_Static_assert(sizeof(Mensaje) <= _POSIX_PIPE_BUF, "Un mensaje debe caber en un pipe");

typedef struct {
    // Cada trabajador tiene un pipe por sentido. El propietario conserva el avance.
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
    uint64_t confirmadas; // Un bit por tienda del JSON cuando confirma la aplicación.
    int64_t reintentar;   // Próximo intento en el reloj monotónico. Cero permite intentarlo ya.
    bool cotejada; // El cierre ya comprobó esta venta y su importe.
} Venta;

/*
 * Solo el propietario accede al historial, la lista y los totales.
 * Los trabajadores reciben copias y leen la configuración fija.
 */
typedef struct {
    const Configuracion *config;
    const ConfigTienda *tienda;
    const char *id_cadena;
    TiendaLocal *local;
    int historial;
    bool historial_dudoso, historico; // Escritura fallida / consulta de archivo.
    // La lista ordenada guarda índices de config->tiendas.
    unsigned char orden[CONFIG_MAX_TIENDAS];
    size_t cantidad, cantidad_configurada, propia;
    bool lista;
    uint32_t version, ultima_propuesta; // Cero en version indica que aún no hay lista acordada.
    CambioLista cambio, encargo; // Propuesta preparada y propuesta por enviar al emisor.
    uint64_t verificados; // Un bit por posición comprobada de la lista propuesta.
    bool reiniciada, control_en_curso;
    int64_t buscar_desde;
    int64_t secuencia, ingreso_propio, total;
    Venta *ventas;
    /*
     * proxima indica dónde retomar la búsqueda circular de ventas pendientes.
     * en_envio guarda el índice de la venta en curso o SIZE_MAX si no hay una.
     */
    size_t usadas, capacidad, proxima, en_envio;
    bool incorporando;
    bool cerrando, conciliando, definitiva, solo_lectura, salir, informo_pendiente;
    size_t cotejadas;
    EvidenciaCierre cierre, borrador; // Cierre preparado o guardado y copia por comprobar.
} Cadena;

static size_t turno_pendiente(const Cadena *cadena, const Venta *venta, size_t *previa);

static volatile sig_atomic_t avisar_fin = -1;

// SIGINT y SIGTERM despiertan al propietario con un byte en el pipe.
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

/*
 * Entrega una copia de la solicitud al propietario y espera su respuesta.
 * Devuelve false si el canal se cierra o el registro no se puede transferir.
 */
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

static size_t separar(char *linea, char *campos[CAMPOS]) {
    char *fin = strchr(linea, '\n');
    if (!fin || fin[1] != '\0') {
        return 0;
    }
    *fin = '\0';
    size_t cantidad = 0;
    while (cantidad < CAMPOS && *linea && *linea != ' ') {
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

static size_t lugar_en(const unsigned char *orden, size_t cantidad, size_t tienda) {
    for (size_t i = 0; i < cantidad; ++i) {
        if (orden[i] == tienda) {
            return i;
        }
    }
    return cantidad;
}

static size_t posicion(const Cadena *cadena, size_t tienda) {
    return lugar_en(cadena->orden, cadena->cantidad, tienda);
}

static int64_t siguiente(const char *campo, size_t maximo) {
    int64_t valor;
    return numero(campo, &valor) && (uint64_t)valor <= maximo ? valor : -1;
}

static uint64_t leer_bajas(const char *campo) {
    if (*campo < '1' || *campo > '9') {
        return 0;
    }
    errno = 0;
    char *fin;
    uintmax_t bajas = strtoumax(campo, &fin, 10);
    return errno == ERANGE || *fin || bajas > UINT64_MAX ? 0 : (uint64_t)bajas;
}

static bool solicitar(Trabajador *hilo, const ConfigTienda *destino,
                      const char *linea, char *respuesta) {
    char error[256];
    return !red_fin_solicitado(hilo->lectura) &&
           red_intercambiar(destino, linea, respuesta, error, sizeof error);
}

static bool consultar_posicion(Trabajador *hilo, const ConfigTienda *par, size_t indice,
                               const ConfigTienda **integrante) {
    char linea[RED_MAX_LINEA + 1], respuesta[RED_MAX_LINEA + 1], *campos[CAMPOS];
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
 * Pide a las tiendas anteriores que acepten el alta. Si la lista alcanza
 * el tamaño configurado, les pide guardarla como lista inicial.
 * Devuelve true si las demás aceptan y deja el orden en el mensaje.
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
 * Un envío de venta se da por confirmado si la respuesta de aplicación coincide
 * con la venta enviada y con la posición esperada de la tienda que responde.
 * Devuelve NULL cuando se cierra el canal o falla la transferencia.
 */
static void *emitir(void *contexto) {
    Trabajador *hilo = contexto;
    Mensaje trabajo;
    while (transferir(hilo->lectura, &trabajo, false)) {
        if (trabajo.inicio) {
            trabajo.exito = incorporarse(hilo, &trabajo);
        } else if (trabajo.buscar) {
            trabajo.exito = cierre_buscar(hilo->red.config, hilo->red.tienda, hilo->lectura);
            if (!trabajo.exito) {
                trabajo.exito = recuperacion_buscar(hilo->red.config, hilo->red.tienda,
                                                    &trabajo.cambio, hilo->lectura);
            }
        } else if (trabajo.cierre) {
            trabajo.exito = cierre_ejecutar(hilo->red.config, hilo->red.tienda,
                                             &trabajo.cambio, trabajo.cierre, hilo->lectura);
            if (!trabajo.exito && trabajo.cierre == AVISAR_CIERRE) {
                trabajo.exito = cierre_buscar(hilo->red.config, hilo->red.tienda, hilo->lectura);
            }
        } else if (trabajo.cambio.tipo) {
            trabajo.exito = recuperacion_acordar(hilo->red.config, hilo->red.tienda,
                                                 &trabajo.cambio, hilo->lectura);
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
 * Anexa un registro completo y devuelve true si se escriben todos sus bytes.
 * Si falla, bloquea escrituras posteriores porque la última línea puede quedar incompleta.
 */
static bool anexar(Cadena *cadena, const char *linea) {
    if (cadena->historial_dudoso) {
        return false;
    }
    size_t largo = strlen(linea);
    size_t usados = 0;
    while (usados < largo) {
        ssize_t n = write(cadena->historial, linea + usados, largo - usados);
        if (n > 0) {
            usados += (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            cadena->historial_dudoso = true;
            return false;
        }
    }
    return true;
}

static bool suspender(Cadena *cadena) {
    if (cadena->cerrando) {
        return !cadena->historial_dudoso;
    }
    // La admisión se detiene aunque falle la escritura de la marca.
    cadena->cerrando = true;
    if (!anexar(cadena, "CIERRE_EN_CURSO\n")) {
        fprintf(stderr, "ERROR_HISTORIAL_CIERRE %s: no se pudo guardar CIERRE_EN_CURSO. Compras nuevas bloqueadas.\n",
                cadena->tienda->id);
        return false;
    }
    printf("CIERRE_EN_CURSO %s\n", cadena->tienda->id);
    return true;
}

static bool reservar(Cadena *cadena) {
    if (cadena->usadas == cadena->capacidad) {
        if (cadena->capacidad > SIZE_MAX / 2 / sizeof *cadena->ventas) {
            return false;
        }
        size_t nueva = cadena->capacidad ? cadena->capacidad * 2 : 32;
        Venta *ventas = realloc(cadena->ventas, nueva * sizeof *ventas);
        if (!ventas) {
            return false;
        }
        cadena->ventas = ventas;
        cadena->capacidad = nueva;
    }
    return true;
}

static void incorporar_venta(Cadena *cadena, Venta venta) {
    cadena->ventas[cadena->usadas++] = venta;
    cadena->total += venta.importe;
    if (venta.origen == cadena->propia) {
        cadena->secuencia = venta.secuencia;
        cadena->ingreso_propio += venta.importe;
    }
}

static Venta *venta_por_id(Cadena *cadena, size_t origen, int64_t secuencia) {
    for (size_t i = 0; i < cadena->usadas; ++i) {
        if (cadena->ventas[i].origen == origen && cadena->ventas[i].secuencia == secuencia) {
            return &cadena->ventas[i];
        }
    }
    return NULL;
}

static size_t tienda_de(const Cadena *cadena, const char *id) {
    const ConfigTienda *tienda = configuracion_buscar(cadena->config, id);
    return tienda && tienda->cadena == cadena->tienda->cadena
               ? (size_t)(tienda - cadena->config->tiendas) : CONFIG_MAX_TIENDAS;
}

/*
 * Anexa una venta validada y actualiza los totales después de escribir la línea
 * completa. Devuelve NULL si la aplica. Si falla la marca de cierre posterior,
 * informa el fallo por separado y mantiene bloqueadas las compras nuevas.
 */
static const char *registrar(Cadena *cadena, Venta venta) {
    if (cadena->historial_dudoso) {
        return "ERROR_HISTORIAL\n";
    }
    if (venta.importe > INT64_MAX - cadena->total) {
        return "DESBORDAMIENTO\n";
    }
    if (!reservar(cadena)) {
        return "SIN_ESPACIO\n";
    }
    const char *origen = cadena->config->tiendas[venta.origen].id;
    char linea[MAX_REGISTRO];
    int largo = snprintf(linea, sizeof linea, "APLICADA %s %s %" PRId64 " %" PRId64 "%s%s\n",
                         cadena->id_cadena, origen, venta.secuencia, venta.importe,
                         *venta.clave ? " " : "", venta.clave);
    if (largo < 0 || (size_t)largo >= sizeof linea || !anexar(cadena, linea)) {
        return "ERROR_HISTORIAL\n";
    }
    incorporar_venta(cadena, venta);
    printf("APLICADA %s %s %" PRId64 " %" PRId64 " propio=%" PRId64 " total=%" PRId64 "\n",
           cadena->tienda->id, origen, venta.secuencia, venta.importe,
           cadena->ingreso_propio, cadena->total);
    if (cadena->total >= cadena->config->limite_gubernamental_centavos) {
        suspender(cadena);
    }
    return NULL;
}

static bool clave_valida(const char *clave) {
    const char *permitidos = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-";
    return strlen(clave) <= CONFIG_MAX_ID && clave[strspn(clave, permitidos)] == '\0';
}

static const Venta *venta_por_clave(const Cadena *cadena, const char *clave) {
    for (size_t i = 0; i < cadena->usadas; ++i) {
        const Venta *venta = &cadena->ventas[i];
        if (venta->origen == cadena->propia && strcmp(venta->clave, clave) == 0) {
            return venta;
        }
    }
    return NULL;
}

/*
 * Responde a una compra propia con su secuencia o con el motivo de rechazo.
 * Una compra nueva se admite después de anexar la venta al historial.
 */
static void compra(Cadena *cadena, char *campos[CAMPOS], char *respuesta) {
    int64_t importe;
    if (!clave_valida(campos[1]) || !numero(campos[2], &importe) || importe == 0) {
        strcpy(respuesta, "ERROR\n");
        return;
    }
    // Un reintento idéntico recupera la secuencia incluso con la admisión suspendida.
    const Venta *anterior = venta_por_clave(cadena, campos[1]);
    if (anterior) {
        if (anterior->importe == importe) {
            snprintf(respuesta, RED_MAX_LINEA + 1, "ADMITIDA %" PRId64 "\n", anterior->secuencia);
        } else {
            strcpy(respuesta, "CONFLICTO\n");
        }
        return;
    }
    if (cadena->historial_dudoso) {
        strcpy(respuesta, "ERROR_HISTORIAL\n");
        return;
    }
    if (cadena->cerrando) {
        strcpy(respuesta, "CIERRE_EN_CURSO\n");
        return;
    }
    if (cadena->secuencia == INT64_MAX) {
        strcpy(respuesta, "DESBORDAMIENTO\n");
        return;
    }
    Venta venta = {.origen = cadena->propia, .secuencia = cadena->secuencia + 1,
                   .importe = importe};
    strcpy(venta.clave, campos[1]);
    const char *error = registrar(cadena, venta);
    if (error) {
        strcpy(respuesta, error);
    } else {
        snprintf(respuesta, RED_MAX_LINEA + 1, "ADMITIDA %" PRId64 "\n", venta.secuencia);
        if (cadena->cantidad == 1) {
            printf("DIFUNDIDA %s %" PRId64 "\n", cadena->tienda->id, venta.secuencia);
        }
    }
}

/*
 * Devuelve quién debe haber aplicado la venta antes del destinatario.
 * La primera tienda sigue al origen. Las demás toman el lugar anterior
 * de la lista y saltan el del origen, que ya aplicó primero.
 */
static size_t predecesora(size_t turno, size_t origen) {
    if (turno == 0 || (turno == 1 && origen == 0)) {
        return origen;
    }
    return turno - (turno - 1 == origen ? 2 : 1);
}

static void confirmacion(char *texto, const Cadena *cadena, const Venta *venta, size_t turno,
                         uint32_t version) {
    const char *id = cadena->config->tiendas[venta->origen].id;
    if (version == 1) {
        snprintf(texto, RED_MAX_LINEA + 1, "APLICADA %s %s %" PRId64 " %" PRId64 " %zu\n",
                 cadena->id_cadena, id, venta->secuencia, venta->importe, turno);
    } else {
        snprintf(texto, RED_MAX_LINEA + 1, "HECHA %s %s %" PRId64 " %" PRId64 " %zu %" PRIu32 "\n",
                 cadena->id_cadena, id, venta->secuencia, venta->importe, turno, version);
    }
}

static uint64_t marcados(size_t cantidad) {
    // El caso de 64 posiciones evita desplazar un entero por todo su ancho.
    return cantidad == 64 ? UINT64_MAX : (UINT64_C(1) << cantidad) - 1;
}

/*
 * Comprueba que la venta llegue con la posición previa esperada.
 * Si la venta ya se aplicó con ese identificador e importe, repite la
 * confirmación. Si el importe cambió, responde CONFLICTO.
 */
static void aplicar_remota(Cadena *cadena, char *campos[CAMPOS], char *respuesta,
                           const unsigned char *orden, size_t cantidad, uint32_t version) {
    Venta venta = {.origen = tienda_de(cadena, campos[2])};
    int64_t anterior = siguiente(campos[5], cantidad);
    if (venta.origen == CONFIG_MAX_TIENDAS || venta.origen == cadena->propia ||
        !numero(campos[3], &venta.secuencia) || venta.secuencia == 0 ||
        !numero(campos[4], &venta.importe) || venta.importe == 0) {
        strcpy(respuesta, "ERROR\n");
        return;
    }
    size_t turno = lugar_en(orden, cantidad, cadena->propia);
    size_t origen = lugar_en(orden, cantidad, venta.origen);
    if (turno == cantidad || anterior != (int64_t)predecesora(turno, origen)) {
        strcpy(respuesta, "FUERA_DE_TURNO\n");
        return;
    }
    const Venta *anterior_venta = venta_por_id(cadena, venta.origen, venta.secuencia);
    if (anterior_venta) {
        if (anterior_venta->importe == venta.importe) {
            confirmacion(respuesta, cadena, &venta, turno, version);
        } else {
            strcpy(respuesta, "CONFLICTO\n");
        }
        return;
    }
    if (cadena->cierre.version || cadena->definitiva) {
        strcpy(respuesta, "CIERRE_PREPARADO\n");
        return;
    }
    const char *error = registrar(cadena, venta);
    if (error) {
        strcpy(respuesta, error);
    } else {
        confirmacion(respuesta, cadena, &venta, turno, version);
    }
}

// Cambios de participantes

static bool instalar(Cadena *cadena, const CambioLista *cambio) {
    char linea[MAX_REGISTRO];
    int usados = snprintf(linea, sizeof linea, "VISTA %" PRIu32, cambio->version);
    for (size_t i = 0; i < cambio->cantidad; ++i) {
        usados += snprintf(linea + usados, sizeof linea - (size_t)usados,
                           " %s", cadena->config->tiendas[cambio->orden[i]].id);
    }
    snprintf(linea + usados, sizeof linea - (size_t)usados, "\n");
    // La nueva lista se usa solo después de escribir su registro.
    if (!anexar(cadena, linea)) {
        return false;
    }
    cadena->version = cambio->version;
    cadena->cantidad = cambio->cantidad;
    memcpy(cadena->orden, cambio->orden, cambio->cantidad);
    cadena->lista = true;
    cadena->reiniciada = false;
    cadena->conciliando = false;
    for (size_t i = 0; i < cadena->usadas; ++i) {
        cadena->ventas[i].reintentar = 0;
    }
    anunciar_lista(cadena);
    printf("VISTA %s %" PRIu32 "\n", cadena->tienda->id, cadena->version);
    return true;
}

static bool formar_cambio(const Cadena *cadena, CambioLista *cambio, char tipo, size_t objetivo) {
    if (cadena->version == 0 || cadena->version == UINT32_MAX ||
        cadena->ultima_propuesta == UINT32_MAX ||
        objetivo >= CONFIG_MAX_TIENDAS) {
        return false;
    }
    bool presente = posicion(cadena, objetivo) < cadena->cantidad;
    if (presente == (tipo == 'R')) {
        return false;
    }
    uint32_t ultima = cadena->ultima_propuesta > cadena->version ? cadena->ultima_propuesta : cadena->version;
    *cambio = (CambioLista){.version = ultima + 1, .base = cadena->version,
                           .tipo = tipo, .objetivo = objetivo};
    for (size_t i = 0; i < cadena->cantidad; ++i) {
        if (tipo != 'B' || cadena->orden[i] != objetivo) {
            cambio->orden[cambio->cantidad++] = cadena->orden[i];
        }
    }
    if (tipo == 'R' && cambio->cantidad < CONFIG_MAX_TIENDAS) {
        cambio->orden[cambio->cantidad++] = (unsigned char)objetivo;
    }
    return cambio->cantidad > 0 &&
           (tipo != 'R' || cambio->orden[cambio->cantidad - 1] == objetivo);
}

/*
 * Propone retirar varias tiendas de la lista actual sin cambiar el orden de las demás.
 * Devuelve false si las posiciones de baja son inválidas o se retira a esta tienda.
 */
static bool formar_bajas(const Cadena *cadena, CambioLista *cambio, uint64_t bajas) {
    if (!bajas || !cadena->version || cadena->version == UINT32_MAX ||
        cadena->ultima_propuesta == UINT32_MAX) {
        return false;
    }
    if (bajas & ~marcados(cadena->cantidad)) {
        return false;
    }
    uint32_t ultima = cadena->ultima_propuesta > cadena->version ? cadena->ultima_propuesta : cadena->version;
    *cambio = (CambioLista){.version = ultima + 1, .base = cadena->version, .tipo = 'M', .bajas = bajas};
    for (size_t i = 0; i < cadena->cantidad; ++i) {
        if (!(bajas & (UINT64_C(1) << i))) {
            cambio->orden[cambio->cantidad++] = cadena->orden[i];
        }
    }
    return lugar_en(cambio->orden, cambio->cantidad, cadena->propia) < cambio->cantidad;
}

static bool preparar_cambio(Cadena *cadena, uint32_t version, uint32_t base, char tipo,
                            const char *destino, size_t cantidad) {
    size_t objetivo = tipo == 'M' ? 0 : tienda_de(cadena, destino);
    uint64_t bajas = tipo == 'M' ? leer_bajas(destino) : 0;
    bool baja = tipo == 'B' || tipo == 'M';
    if (cantidad == 0 || cantidad > cadena->cantidad_configurada || version <= base ||
        base < cadena->version ||
        objetivo >= CONFIG_MAX_TIENDAS || cadena->cierre.version ||
        (tipo == 'M' && (!bajas || strncmp(cadena->tienda->direccion, "127.", 4) != 0)) ||
        (cadena->cerrando && (!baja || (tipo == 'B' && objetivo == cadena->orden[0]) ||
                             (bajas & UINT64_C(1))))) {
        return false;
    }
    if (cadena->cambio.version) {
        return cadena->cambio.version == version && cadena->cambio.tipo == tipo &&
               cadena->cambio.base == base && cadena->cambio.objetivo == objetivo &&
               cadena->cambio.cantidad == cantidad && cadena->cambio.bajas == bajas;
    }
    if (version <= cadena->ultima_propuesta) {
        return false;
    }
    CambioLista propuesta;
    if (tipo == 'R' && objetivo == cadena->propia && cadena->reiniciada) {
        propuesta = (CambioLista){.version = version, .tipo = tipo,
                                   .objetivo = objetivo, .cantidad = cantidad};
        // La tienda que retorna ocupa el final. Los demás lugares llegan por MIEMBRO.
        memset(propuesta.orden, UCHAR_MAX, sizeof propuesta.orden);
        propuesta.orden[cantidad - 1] = (unsigned char)objetivo;
    } else if (!(tipo == 'M' ? formar_bajas(cadena, &propuesta, bajas)
                             : formar_cambio(cadena, &propuesta, tipo, objetivo)) || propuesta.base != base ||
               propuesta.cantidad != cantidad ||
               lugar_en(propuesta.orden, cantidad, cadena->propia) == cantidad) {
        return false;
    }
    propuesta.version = version;
    propuesta.base = base;
    char linea[RED_MAX_LINEA];
    snprintf(linea, sizeof linea, "PREPARA %" PRIu32 " %c %s %" PRIu32 "\n",
             version, tipo, destino, base);
    if (!anexar(cadena, linea)) {
        return false;
    }
    cadena->cambio = propuesta;
    cadena->ultima_propuesta = version;
    cadena->verificados = 0;
    printf("CAMBIO_EN_CURSO %s\n", cadena->tienda->id);
    return true;
}

// El aviso de baja inicia un acuerdo. La lista cambia solo al instalarlo.
static bool atender_cambio(Cadena *cadena, char *campos[CAMPOS], size_t campos_usados,
                           char *respuesta) {
    const char *orden = campos[0];
    if (campos_usados == 3 && strcmp(orden, "VIGENTE") == 0 && cadena->version) {
        size_t par = tienda_de(cadena, campos[2]);
        if (par < CONFIG_MAX_TIENDAS) {
            snprintf(respuesta, RED_MAX_LINEA + 1, "VIGENTE %" PRIu32 " %s %d %d\n",
                     cadena->version, cadena->config->tiendas[cadena->orden[0]].id,
                     posicion(cadena, par) < cadena->cantidad,
                     cadena->lista && !cadena->cambio.version && !cadena->cerrando);
        }
        return true;
    }
    if (campos_usados == 4 && strcmp(orden, "BAJAS_ENSAYO") == 0) {
        uint64_t bajas = leer_bajas(campos[3]);
        CambioLista propuesta;
        if (cadena->lista && !cadena->cambio.version && !cadena->encargo.tipo && !cadena->cierre.version &&
            (!cadena->control_en_curso || cadena->cerrando) &&
            strncmp(cadena->tienda->direccion, "127.", 4) == 0 &&
            siguiente(campos[2], UINT32_MAX) == cadena->version &&
            (!cadena->cerrando || !(bajas & UINT64_C(1))) &&
            formar_bajas(cadena, &propuesta, bajas) && propuesta.orden[0] == cadena->propia) {
            cadena->encargo = propuesta;
            strcpy(respuesta, "CAMBIO_SOLICITADO\n");
        } else {
            strcpy(respuesta, "CAMBIO_NO_DISPONIBLE\n");
        }
        return true;
    }
    if (campos_usados == 3 && (strcmp(orden, "BAJA_ENSAYO") == 0 ||
                               strcmp(orden, "RETORNO") == 0 || strcmp(orden, "REANUDAR") == 0)) {
        char tipo = strcmp(orden, "BAJA_ENSAYO") == 0 ? 'B' : strcmp(orden, "RETORNO") == 0 ? 'R' : 'I';
        size_t par = tienda_de(cadena, campos[2]);
        CambioLista propuesta;
        if (cadena->lista && !cadena->cambio.version &&
            (!cadena->control_en_curso || (cadena->cerrando && tipo == 'B')) &&
            !cadena->encargo.tipo && par != cadena->propia &&
            !cadena->cierre.version &&
            (!cadena->cerrando || (tipo == 'B' && par != cadena->orden[0])) &&
            (tipo != 'B' || strncmp(cadena->tienda->direccion, "127.", 4) == 0) &&
            formar_cambio(cadena, &propuesta, tipo, par) && propuesta.orden[0] == cadena->propia) {
            cadena->encargo = propuesta;
            strcpy(respuesta, "CAMBIO_SOLICITADO\n");
        } else {
            strcpy(respuesta, "CAMBIO_NO_DISPONIBLE\n");
        }
        return true;
    }
    CambioLista *cambio = &cadena->cambio;
    int64_t version = siguiente(campos[2], UINT32_MAX);
    if (campos_usados == 7 && strcmp(orden, "PREPARAR") == 0 && version > 0 &&
        strlen(campos[3]) == 1 && strchr("BRIM", campos[3][0])) {
        int64_t cantidad = siguiente(campos[5], CONFIG_MAX_TIENDAS);
        int64_t base = siguiente(campos[6], UINT32_MAX);
        if (cantidad > 0 && base > 0 && preparar_cambio(cadena, (uint32_t)version,
                                           (uint32_t)base, campos[3][0],
                                           campos[4], (size_t)cantidad)) {
            strcpy(respuesta, "PREPARADA\n");
        }
        return true;
    }
    if (campos_usados == 3 && strcmp(orden, "INSTALAR") == 0 &&
        version > 0 && version == cadena->version && cadena->lista) {
        strcpy(respuesta, "INSTALADA\n");
        return true;
    }
    if (campos_usados == 3 && strcmp(orden, "CANCELAR") == 0 &&
        version > cadena->version && (!cambio->version || cambio->version == version)) {
        if (!cambio->version && version <= cadena->ultima_propuesta) {
            strcpy(respuesta, "CANCELADA\n");
            return true;
        }
        char linea[64];
        snprintf(linea, sizeof linea, "CANCELA %" PRId64 "\n", version);
        if (anexar(cadena, linea)) {
            cambio->version = 0;
            cadena->ultima_propuesta = (uint32_t)version;
            printf("CAMBIO_CANCELADO %s\n", cadena->tienda->id);
            strcpy(respuesta, "CANCELADA\n");
        }
        return true;
    }
    if (version <= 0 || version != cambio->version) {
        return false;
    }
    if (campos_usados == 5 && strcmp(orden, "MIEMBRO") == 0) {
        int64_t indice = siguiente(campos[3], cambio->cantidad - 1);
        size_t par = tienda_de(cadena, campos[4]);
        if (indice >= 0 && par < CONFIG_MAX_TIENDAS &&
            (cambio->orden[indice] == UCHAR_MAX || cambio->orden[indice] == par)) {
            size_t anterior = lugar_en(cambio->orden, cambio->cantidad, par);
            if (anterior == cambio->cantidad || anterior == (size_t)indice) {
                cambio->orden[indice] = (unsigned char)par;
                cadena->verificados |= UINT64_C(1) << indice;
                strcpy(respuesta, "VERIFICADO\n");
            }
        }
        return true;
    }
    if (campos_usados == 3 && strcmp(orden, "INSTALAR") == 0 &&
        cadena->verificados == marcados(cambio->cantidad) && instalar(cadena, cambio)) {
        cambio->version = 0;
        strcpy(respuesta, "INSTALADA\n");
        return true;
    }
    return false;
}

// Cierre comercial y evidencia persistente

static size_t cantidad_miembros(const EvidenciaCierre *cierre) {
    return cierre->participantes + cierre->excluidas;
}

static bool guardar_cabecera(Cadena *cadena, const char *tipo, const EvidenciaCierre *cierre) {
    char linea[MAX_REGISTRO];
    int largo = snprintf(linea, sizeof linea, "%s %" PRIu32 " %" PRId64 " %zu %zu", tipo,
                         cierre->version, cierre->total, cierre->cantidad, cierre->participantes);
    for (size_t i = 0; i < cantidad_miembros(cierre); ++i) {
        largo += snprintf(linea + largo, sizeof linea - (size_t)largo,
                          " %s", cadena->config->tiendas[cierre->miembros[i]].id);
    }
    snprintf(linea + largo, sizeof linea - (size_t)largo, "\n");
    return anexar(cadena, linea);
}

static EvidenciaCierre propuesta_cierre(const Cadena *cadena) {
    EvidenciaCierre cierre = {.version = cadena->version, .total = cadena->total,
                              .cantidad = cadena->usadas, .participantes = cadena->cantidad};
    memcpy(cierre.miembros, cadena->orden, cadena->cantidad);
    for (size_t i = 0; i < cadena->config->cantidad_tiendas; ++i) {
        if (cadena->config->tiendas[i].cadena == cadena->tienda->cadena &&
            posicion(cadena, i) == cadena->cantidad) {
            cierre.miembros[cierre.participantes + cierre.excluidas++] = (unsigned char)i;
        }
    }
    return cierre;
}

static ItemCierre item(const Cadena *cadena, size_t fila) {
    if (cadena->cierre.items) {
        return cadena->cierre.items[fila];
    }
    const Venta *venta = &cadena->ventas[fila];
    return (ItemCierre){venta->origen, venta->secuencia, venta->importe};
}

static bool misma_cabecera(const EvidenciaCierre *a, const EvidenciaCierre *b) {
    return a->version == b->version && a->total == b->total && a->cantidad == b->cantidad &&
           a->participantes == b->participantes && a->excluidas == b->excluidas &&
           memcmp(a->miembros, b->miembros, cantidad_miembros(a)) == 0;
}

static bool guardar_decision(Cadena *cadena) {
    // Repetir la decisión permite confirmar de nuevo sin anexar otra marca.
    if (!cadena->definitiva && !anexar(cadena, "CERRADA_META\n")) {
        return false;
    }
    if (!cadena->definitiva) {
        printf("CERRADA_META %s total=%" PRId64 " ventas=%zu\n",
               cadena->tienda->id, cadena->cierre.total, cadena->cierre.cantidad);
    }
    cadena->definitiva = true;
    return true;
}

static bool leer_item(Cadena *cadena, char **campos, ItemCierre *dato) {
    dato->origen = tienda_de(cadena, campos[0]);
    return dato->origen < CONFIG_MAX_TIENDAS &&
           numero(campos[1], &dato->secuencia) && dato->secuencia > 0 &&
           numero(campos[2], &dato->importe) && dato->importe > 0;
}

static const ItemCierre *buscar_item(const EvidenciaCierre *cierre, const ItemCierre *dato) {
    for (size_t i = 0; i < cierre->items_leidos; ++i) {
        if (cierre->items[i].origen == dato->origen &&
            cierre->items[i].secuencia == dato->secuencia) {
            return &cierre->items[i];
        }
    }
    return NULL;
}

static bool agregar_item(EvidenciaCierre *cierre, ItemCierre dato) {
    if (cierre->items_leidos == cierre->cantidad || buscar_item(cierre, &dato)) {
        return false;
    }
    cierre->items[cierre->items_leidos++] = dato;
    return true;
}

static bool evidencia_completa(const EvidenciaCierre *cierre) {
    if (cierre->miembros_leidos != cantidad_miembros(cierre) || cierre->items_leidos != cierre->cantidad) {
        return false;
    }
    int64_t suma = 0;
    for (size_t i = 0; i < cierre->cantidad; ++i) {
        if (cierre->items[i].importe > INT64_MAX - suma) {
            return false;
        }
        suma += cierre->items[i].importe;
    }
    return suma == cierre->total;
}

/*
 * Comprueba el certificado recibido antes de registrar el cierre.
 * Devuelve true si guarda la decisión. Las ventas propias ausentes del
 * certificado se marcan aparte y conservan su registro original.
 */
static bool guardar_importacion(Cadena *cadena) {
    EvidenciaCierre *borrador = &cadena->borrador;
    if (!evidencia_completa(borrador)) {
        return false;
    }
    if (cadena->cierre.version) {
        if (!misma_cabecera(&cadena->cierre, borrador)) {
            return false;
        }
        for (size_t i = 0; i < borrador->cantidad; ++i) {
            const ItemCierre *dato = &borrador->items[i];
            const Venta *venta = venta_por_id(cadena, dato->origen, dato->secuencia);
            if (!venta || venta->importe != dato->importe) {
                return false;
            }
        }
        return guardar_decision(cadena);
    }
    if (borrador->version < cadena->version ||
        lugar_en(borrador->miembros + borrador->participantes, borrador->excluidas,
                 cadena->propia) == borrador->excluidas ||
        !guardar_cabecera(cadena, "CIERRE_IMPORTADO", borrador)) {
        return false;
    }
    char linea[MAX_REGISTRO];
    for (size_t i = 0; i < borrador->cantidad; ++i) {
        const ItemCierre *dato = &borrador->items[i];
        snprintf(linea, sizeof linea, "CIERRE_VENTA %s %" PRId64 " %" PRId64 "\n",
                 cadena->config->tiendas[dato->origen].id, dato->secuencia, dato->importe);
        if (!anexar(cadena, linea)) {
            return false;
        }
    }
    for (size_t i = 0; i < cadena->usadas; ++i) {
        const Venta *venta = &cadena->ventas[i];
        ItemCierre dato = {venta->origen, venta->secuencia, venta->importe};
        if (venta->origen == cadena->propia && !buscar_item(borrador, &dato)) {
            snprintf(linea, sizeof linea, "NO_INCLUIDA %s %" PRId64 " %" PRId64 "\n",
                     cadena->tienda->id, venta->secuencia, venta->importe);
            if (!anexar(cadena, linea)) {
                return false;
            }
        }
    }
    cadena->cierre = *borrador;
    *borrador = (EvidenciaCierre){0};
    return guardar_decision(cadena);
}

static bool atender_evidencia(Cadena *cadena, char *campos[CAMPOS], size_t n, char *respuesta) {
    EvidenciaCierre *cierre = &cadena->cierre;
    const char *cmd = campos[0];
    if (cadena->definitiva && cierre->version && n == 3) {
        int64_t fila;
        if (strcmp(cmd, "PRUEBA_CIERRE") == 0) {
            snprintf(respuesta, RED_MAX_LINEA + 1, "CERRADA %" PRIu32 " %" PRId64 " %zu %zu %zu\n",
                     cierre->version, cierre->total, cierre->cantidad, cierre->participantes, cierre->excluidas);
            return true;
        }
        if (strcmp(cmd, "CIERRE_MIEMBRO") == 0 &&
            (fila = siguiente(campos[2], cantidad_miembros(cierre))) >= 0) {
            if ((size_t)fila == cantidad_miembros(cierre)) {
                strcpy(respuesta, "FIN\n");
            } else {
                snprintf(respuesta, RED_MAX_LINEA + 1, "%s\n",
                         cadena->config->tiendas[cierre->miembros[fila]].id);
            }
            return true;
        }
        if (strcmp(cmd, "CIERRE_DATO") == 0 && (fila = siguiente(campos[2], cierre->cantidad)) >= 0) {
            if ((size_t)fila == cierre->cantidad) {
                strcpy(respuesta, "FIN\n");
            } else {
                ItemCierre dato = item(cadena, (size_t)fila);
                snprintf(respuesta, RED_MAX_LINEA + 1, "%s %" PRId64 " %" PRId64 "\n",
                         cadena->config->tiendas[dato.origen].id, dato.secuencia, dato.importe);
            }
            return true;
        }
    }
    if (cadena->solo_lectura || cadena->definitiva || (!cadena->reiniciada && !cadena->cierre.version)) {
        return false;
    }
    EvidenciaCierre *borrador = &cadena->borrador;
    if (n == 7 && strcmp(cmd, "EVIDENCIA_INICIO") == 0) {
        int64_t version = siguiente(campos[2], UINT32_MAX), total;
        int64_t ventas = siguiente(campos[4], SIZE_MAX / sizeof(ItemCierre));
        int64_t activas = siguiente(campos[5], CONFIG_MAX_TIENDAS);
        int64_t fuera = siguiente(campos[6], CONFIG_MAX_TIENDAS);
        if (version <= 0 || !numero(campos[3], &total) || !total || ventas <= 0 ||
            activas <= 0 || fuera < 0 || activas + fuera != (int64_t)cadena->cantidad_configurada) {
            return true;
        }
        ItemCierre *items = calloc((size_t)ventas, sizeof *items);
        if (items) {
            free(borrador->items);
            *borrador = (EvidenciaCierre){.version = (uint32_t)version, .total = total,
                                         .cantidad = (size_t)ventas, .participantes = (size_t)activas,
                                         .excluidas = (size_t)fuera, .items = items};
            strcpy(respuesta, "CONTINUA\n");
        }
        return true;
    }
    if (!borrador->version) {
        return false;
    }
    if (n == 3 && strcmp(cmd, "EVIDENCIA_MIEMBRO") == 0) {
        size_t par = tienda_de(cadena, campos[2]);
        if (par < CONFIG_MAX_TIENDAS && borrador->miembros_leidos < cantidad_miembros(borrador) &&
            lugar_en(borrador->miembros, borrador->miembros_leidos, par) == borrador->miembros_leidos) {
            borrador->miembros[borrador->miembros_leidos++] = (unsigned char)par;
            strcpy(respuesta, "CONTINUA\n");
        }
        return true;
    }
    if (n == 5 && strcmp(cmd, "EVIDENCIA_VENTA") == 0) {
        ItemCierre dato;
        if (leer_item(cadena, campos + 2, &dato) && agregar_item(borrador, dato)) {
            strcpy(respuesta, "CONTINUA\n");
        }
        return true;
    }
    if (n == 3 && strcmp(cmd, "EVIDENCIA_FIN") == 0 && guardar_importacion(cadena)) {
        strcpy(respuesta, "GUARDADA\n");
        cadena->salir = true;
        return true;
    }
    return false;
}

/*
 * Atiende los pasos del cierre desde el propietario. Devuelve true si el
 * mensaje queda resuelto, incluso si se rechaza. Solo confirma la preparación
 * cuando se cotejaron todas las ventas y se escribió el registro.
 */
static bool atender_cierre(Cadena *cadena, char *campos[CAMPOS], size_t n, char *respuesta) {
    const char *cmd = campos[0];
    if (atender_evidencia(cadena, campos, n, respuesta)) {
        return true;
    }
    int64_t version = siguiente(campos[n - 1], UINT32_MAX);
    if (cadena->solo_lectura) {
        return true;
    }
    if (n == 4 && strcmp(cmd, "AVISO_CIERRE") == 0 &&
        siguiente(campos[2], UINT32_MAX) == cadena->version && cadena->cantidad &&
        cadena->orden[0] == cadena->propia &&
        posicion(cadena, tienda_de(cadena, campos[3])) < cadena->cantidad) {
        if (suspender(cadena)) {
            strcpy(respuesta, "AVISADA\n");
        }
        return true;
    }
    if (n == 3 && strcmp(cmd, "SALIR_CIERRE") == 0 && cadena->definitiva &&
        version == cadena->cierre.version) {
        strcpy(respuesta, "SALIDA\n");
        cadena->salir = true;
        return true;
    }
    if (n == 3 && strcmp(cmd, "DECIDIR_CIERRE") == 0 && version > 0 &&
        version == cadena->cierre.version && guardar_decision(cadena)) {
        strcpy(respuesta, "GUARDADA\n");
        return true;
    }
    if (cadena->definitiva) {
        return true;
    }
    if (n == 3 && strcmp(cmd, "CONGELAR_CIERRE") == 0 && version > 0 &&
        version == cadena->version && !cadena->cambio.version && suspender(cadena)) {
        cadena->conciliando = true;
        cadena->cotejadas = 0;
        for (size_t i = 0; i < cadena->usadas; ++i) {
            cadena->ventas[i].cotejada = false;
        }
        strcpy(respuesta, "CONGELADA\n");
        return true;
    }
    if (!cadena->conciliando || cadena->historial_dudoso) {
        return false;
    }
    if (n == 6 && strcmp(cmd, "COTEJA") == 0 && version == cadena->version) {
        ItemCierre dato;
        if (leer_item(cadena, campos + 2, &dato)) {
            Venta *venta = venta_por_id(cadena, dato.origen, dato.secuencia);
            if (venta && venta->importe == dato.importe) {
                // Consultar de nuevo la misma venta no aumenta el recuento.
                if (!venta->cotejada) {
                    venta->cotejada = true;
                    ++cadena->cotejadas;
                }
                strcpy(respuesta, "COINCIDE\n");
            } else {
                strcpy(respuesta, "CONFLICTO\n");
            }
        }
        return true;
    }
    if (n == 3 && strcmp(cmd, "RESUMEN_CIERRE") == 0 && version == cadena->version) {
        snprintf(respuesta, RED_MAX_LINEA + 1, "RESUMEN %zu %" PRId64 "\n", cadena->usadas, cadena->total);
        return true;
    }
    if (n == 5 && strcmp(cmd, "PREPARAR_CIERRE") == 0 &&
        siguiente(campos[2], UINT32_MAX) == cadena->version) {
        int64_t total, cantidad = siguiente(campos[3], cadena->usadas);
        if (cantidad != (int64_t)cadena->usadas || cadena->cotejadas != cadena->usadas ||
            !numero(campos[4], &total) || total != cadena->total ||
            (!cadena->cierre.version && total < cadena->config->limite_gubernamental_centavos)) {
            strcpy(respuesta, "CONFLICTO\n");
        } else if (cadena->cierre.version) {
            strcpy(respuesta, "PREPARADA\n");
        } else {
            EvidenciaCierre cierre = propuesta_cierre(cadena);
            if (guardar_cabecera(cadena, "CIERRE_PREPARADO", &cierre)) {
                cadena->cierre = cierre;
                printf("CIERRE_PREPARADO %s\n", cadena->tienda->id);
                strcpy(respuesta, "PREPARADA\n");
            }
        }
        return true;
    }
    return false;
}

static char estado_monitoreado(const Cadena *cadena) {
    if (cadena->local) {
        if (cadena->local->historial_por_verificar) {
            return 'H';
        }
        return cadena->local->cerrada_por_meta ? 'M' : cadena->historico ? 'I' : 'A';
    }
    if (cadena->historial_dudoso) {
        return 'H';
    }
    if (cadena->definitiva) {
        return cadena->solo_lectura ? 'm' : 'M';
    }
    if (cadena->cierre.version) {
        return 'P';
    }
    if (cadena->cerrando) {
        return 'C';
    }
    if (cadena->historico) {
        return cadena->reiniciada ? 'R' : 'I';
    }
    return cadena->lista && !cadena->reiniciada && !cadena->cambio.version ? 'A' : 'R';
}

// Cuenta las ventas propias con alguna confirmación de aplicación pendiente.
static size_t pendientes_propias(const Cadena *cadena) {
    size_t pendientes = 0;
    for (size_t i = 0; i < cadena->usadas; ++i) {
        const Venta *venta = &cadena->ventas[i];
        size_t previa;
        if (venta->origen == cadena->propia && turno_pendiente(cadena, venta, &previa) < cadena->cantidad) {
            ++pendientes;
        }
    }
    return pendientes;
}

// El último identificador propio del resumen fija hasta dónde se listan las ventas.
static bool atender_monitor(Cadena *cadena, char *campos[CAMPOS], size_t cantidad, char *respuesta) {
    if (!cantidad || (strcmp(campos[0], "ESTADO") != 0 && strcmp(campos[0], "VENTA_PROPIA") != 0)) {
        return false;
    }
    bool resumen = strcmp(campos[0], "ESTADO") == 0;
    if (cantidad != (resumen ? 3u : 4u) || strcmp(campos[1], cadena->tienda->id) != 0 ||
        strcmp(campos[2], cadena->id_cadena) != 0) {
        return true;
    }
    int64_t ultima = cadena->local ? (int64_t)cadena->local->cantidad : cadena->secuencia;
    if (resumen) {
        char total[24] = "-", pendientes[24] = "-";
        int64_t propio = cadena->local ? cadena->local->ingreso_propio : cadena->ingreso_propio;
        if (!cadena->local) {
            snprintf(total, sizeof total, "%" PRId64, cadena->total);
            if (!cadena->historico && cadena->version && !cadena->definitiva) {
                snprintf(pendientes, sizeof pendientes, "%zu", pendientes_propias(cadena));
            }
        }
        snprintf(respuesta, RED_MAX_LINEA + 1, "ESTADO %s %c %" PRId64 " %s %s %" PRId64 "\n",
                 cadena->tienda->id, estado_monitoreado(cadena), propio, total, pendientes, ultima);
    } else {
        int64_t indice;
        if (numero(campos[3], &indice) && indice > 0 && indice <= ultima) {
            const Venta *venta = cadena->local ? NULL : venta_por_id(cadena, cadena->propia, indice);
            if (cadena->local || venta) {
                int64_t importe = cadena->local ? cadena->local->ventas[indice - 1].importe : venta->importe;
                snprintf(respuesta, RED_MAX_LINEA + 1, "PROPIA %" PRId64 " %" PRId64 "\n", indice, importe);
            }
        }
    }
    return true;
}

static void atender(Cadena *cadena, char *linea, char *respuesta) {
    char *campos[CAMPOS];
    size_t cantidad = separar(linea, campos);
    strcpy(respuesta, "ERROR\n");
    if (atender_monitor(cadena, campos, cantidad, respuesta)) {
        return;
    }
    if (cadena->local) {
        int64_t importe;
        if (cantidad == 3 && strcmp(campos[0], "COMPRA") == 0 && numero(campos[2], &importe) && importe > 0) {
            size_t id;
            ResultadoTiendaLocal resultado = tienda_local_comprar(cadena->local, campos[1], importe, &id);
            if (resultado == TIENDA_OK) {
                snprintf(respuesta, RED_MAX_LINEA + 1, "ADMITIDA %zu\n", id);
                cadena->salir = cadena->local->cerrada_por_meta;
            } else if (resultado == TIENDA_CONFLICTO) {
                strcpy(respuesta, "CONFLICTO\n");
            } else if (resultado == TIENDA_CERRADA_POR_META) {
                strcpy(respuesta, "CERRADA_META\n");
            } else if (resultado == TIENDA_ERROR_ESCRITURA || resultado == TIENDA_HISTORIAL_POR_VERIFICAR) {
                strcpy(respuesta, "ERROR_HISTORIAL\n");
            } else if (resultado == TIENDA_DESBORDAMIENTO) {
                strcpy(respuesta, "DESBORDAMIENTO\n");
            }
        }
        return;
    }
    if (cantidad == 3 && strcmp(campos[0], "COMPRA") == 0) {
        if (cadena->solo_lectura || cadena->definitiva) {
            strcpy(respuesta, "CERRADA_META\n");
        } else if (cadena->cerrando || (cadena->lista && !cadena->cambio.version)) {
            compra(cadena, campos, respuesta);
        } else {
            strcpy(respuesta, cadena->cambio.version ? "CAMBIO_EN_CURSO\n" : "ORDEN_NO_LISTO\n");
        }
        return;
    }
    if (cantidad < 3 || strcmp(campos[1], cadena->id_cadena) != 0) {
        return;
    }
    if (atender_cierre(cadena, campos, cantidad, respuesta) ||
        atender_cambio(cadena, campos, cantidad, respuesta)) {
        return;
    }
    uint32_t inventario = cadena->cambio.version ? cadena->cambio.version
                                                : cadena->conciliando ? cadena->version : 0;
    if (cantidad == 4 && strcmp(campos[0], "FICHA") == 0 && inventario &&
        siguiente(campos[3], UINT32_MAX) == inventario) {
        int64_t indice = siguiente(campos[2], cadena->usadas);
        if (indice >= 0 && (size_t)indice == cadena->usadas) {
            strcpy(respuesta, "FIN\n");
        } else if (indice >= 0) {
            const Venta *venta = &cadena->ventas[indice];
            snprintf(respuesta, RED_MAX_LINEA + 1, "FICHA %s %" PRId64 " %" PRId64 "\n",
                     cadena->config->tiendas[venta->origen].id, venta->secuencia, venta->importe);
        }
    } else if (cantidad == 5 && strcmp(campos[0], "TIENE") == 0 && inventario &&
               siguiente(campos[4], UINT32_MAX) == inventario) {
        size_t origen = tienda_de(cadena, campos[2]);
        int64_t secuencia;
        if (origen < CONFIG_MAX_TIENDAS && numero(campos[3], &secuencia) && secuencia > 0) {
            const Venta *venta = venta_por_id(cadena, origen, secuencia);
            if (venta) {
                snprintf(respuesta, RED_MAX_LINEA + 1, "CONOCE %" PRId64 "\n", venta->importe);
            } else {
                strcpy(respuesta, "FALTA\n");
            }
        }
    } else if (cantidad == 7 && strcmp(campos[0], "RECUP") == 0) {
        if (cadena->cambio.version && cadena->cambio.tipo != 'B' && cadena->cambio.tipo != 'M' &&
            siguiente(campos[6], UINT32_MAX) == cadena->cambio.version &&
            cadena->verificados == marcados(cadena->cambio.cantidad)) {
            aplicar_remota(cadena, campos, respuesta, cadena->cambio.orden,
                           cadena->cambio.cantidad, cadena->cambio.version);
        } else if (cadena->conciliando && !cadena->cambio.version &&
                   siguiente(campos[6], UINT32_MAX) == cadena->version) {
            aplicar_remota(cadena, campos, respuesta, cadena->orden, cadena->cantidad, cadena->version);
        } else {
            strcpy(respuesta, "VISTA_INCORRECTA\n");
        }
    } else if (cantidad == 3 && strcmp(campos[0], "ORDEN") == 0) {
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
        if (cadena->version) {
            return;
        }
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
            CambioLista inicial = {.version = 1, .cantidad = cadena->cantidad};
            memcpy(inicial.orden, cadena->orden, cadena->cantidad);
            if (instalar(cadena, &inicial)) {
                strcpy(respuesta, "LISTA_OK\n");
            }
        }
    } else if ((cantidad == 6 && strcmp(campos[0], "VENTA") == 0) ||
               (cantidad == 7 && strcmp(campos[0], "VENTA2") == 0)) {
        int64_t version = cantidad == 6 ? 1 : siguiente(campos[6], UINT32_MAX);
        if (version != cadena->version || cadena->cambio.version || cadena->conciliando) {
            strcpy(respuesta, "VISTA_INCORRECTA\n");
        } else if (cadena->lista) {
            aplicar_remota(cadena, campos, respuesta, cadena->orden, cadena->cantidad, cadena->version);
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
            CambioLista inicial = {.version = 1, .cantidad = cadena->cantidad};
            memcpy(inicial.orden, cadena->orden, cadena->cantidad);
            if (!instalar(cadena, &inicial)) {
                fprintf(stderr, "No se guardó la lista inicial\n");
            }
        }
        return;
    }
    if (resultado->buscar || resultado->cambio.tipo) {
        cadena->control_en_curso = false;
        cadena->buscar_desde = red_reloj_ms() + REINTENTO_MS;
        if (!resultado->buscar && !resultado->exito) {
            printf("CAMBIO_PENDIENTE %s\n", cadena->tienda->id);
        }
        return;
    }
    if (resultado->cierre) {
        cadena->control_en_curso = false;
        cadena->buscar_desde = red_reloj_ms() + REINTENTO_MS;
        if (resultado->cierre != AVISAR_CIERRE && resultado->exito && cadena->definitiva) {
            cadena->salir = true;
        } else if (!resultado->exito && !cadena->informo_pendiente) {
            printf("CIERRE_PENDIENTE %s\n", cadena->tienda->id);
            cadena->informo_pendiente = true;
        }
        return;
    }
    Venta *venta = &cadena->ventas[cadena->en_envio];
    cadena->en_envio = SIZE_MAX;
    /*
     * Las confirmaciones de difusión quedan en pausa durante un cambio de lista
     * o la conciliación. Una respuesta de otra versión también se descarta.
     */
    if (resultado->cambio.version != cadena->version || cadena->cambio.version || cadena->conciliando) {
        return;
    }
    const char *id_destino = cadena->config->tiendas[resultado->destino].id;
    if (resultado->exito) {
        char registro[RED_MAX_LINEA + 1];
        snprintf(registro, sizeof registro, "CONFIRMADA %" PRIu32 " %s %" PRId64 " %s\n",
                 cadena->version, cadena->config->tiendas[venta->origen].id,
                 venta->secuencia, id_destino);
        if (!anexar(cadena, registro)) {
            return;
        }
        venta->confirmadas |= UINT64_C(1) << resultado->destino;
        venta->reintentar = 0;
        bool difundida = true;
        for (size_t i = 0; i < cadena->cantidad; ++i) {
            size_t par = cadena->orden[i];
            if (par != cadena->propia && par != venta->origen &&
                !(venta->confirmadas & (UINT64_C(1) << par))) {
                difundida = false;
            }
        }
        if (difundida && venta->origen == cadena->propia) {
            printf("DIFUNDIDA %s %" PRId64 "\n", cadena->tienda->id, venta->secuencia);
        }
    } else {
        if (venta->reintentar == 0) {
            printf("PENDIENTE %s %" PRId64 " %s\n", cadena->tienda->id, venta->secuencia, id_destino);
        }
        venta->reintentar = red_reloj_ms() + REINTENTO_MS;
    }
}

/*
 * Devuelve la primera posición que falta confirmar y deja en previa quién
 * aplicó justo antes. Si el origen quedó fuera, previa empieza en cantidad.
 * Devuelve cantidad si ninguna tienda de la lista necesita confirmación.
 */
static size_t turno_pendiente(const Cadena *cadena, const Venta *venta, size_t *previa) {
    *previa = posicion(cadena, venta->origen);
    for (size_t i = 0; i < cadena->cantidad; ++i) {
        size_t par = cadena->orden[i];
        if (par == venta->origen) {
            continue;
        }
        if (par != cadena->propia && !(venta->confirmadas & (UINT64_C(1) << par))) {
            return i;
        }
        *previa = i;
    }
    return cadena->cantidad;
}

/*
 * Si el emisor está libre, programa la siguiente venta elegible en el
 * recorrido circular. Devuelve los ms hasta el próximo reintento, -1 si basta
 * esperar un evento o -2 si falla el reloj.
 */
static int programar(Cadena *cadena, Canal *emisor) {
    if (cadena->solo_lectura || cadena->salir || cadena->historial_dudoso || cadena->cambio.version ||
        cadena->control_en_curso || cadena->en_envio != SIZE_MAX || emisor->enviar) {
        return -1;
    }
    int espera = -1;
    int64_t ahora = red_reloj_ms();
    if (ahora < 0) {
        return -2;
    }
    if (cadena->encargo.tipo && cadena->cierre.version) {
        cadena->encargo.tipo = 0;
    }
    if (cadena->encargo.tipo) {
        emisor->salida = (Mensaje){.cambio = cadena->encargo};
        cadena->encargo.tipo = 0;
        cadena->control_en_curso = emisor->enviar = true;
        return -1;
    }
    if (cadena->cerrando && cadena->version) {
        if (cadena->buscar_desde > ahora) {
            return (int)(cadena->buscar_desde - ahora);
        }
        bool primera = cadena->orden[0] == cadena->propia;
        if (cadena->definitiva && !primera) {
            return -1;
        }
        enum TrabajoCierre trabajo = !primera ? AVISAR_CIERRE
                                   : cadena->definitiva ? DISTRIBUIR_CIERRE : CONCILIAR_CIERRE;
        emisor->salida = (Mensaje){.cierre = trabajo, .cambio = {.version = cadena->version,
                                                               .cantidad = cadena->cantidad}};
        memcpy(emisor->salida.cambio.orden, cadena->orden, cadena->cantidad);
        cadena->control_en_curso = emisor->enviar = true;
        return -1;
    }
    if (cadena->reiniciada) {
        if (cadena->buscar_desde > ahora) {
            return (int)(cadena->buscar_desde - ahora);
        }
        emisor->salida = (Mensaje){.buscar = true, .cambio = {.version = cadena->version,
                                                             .base = cadena->ultima_propuesta,
                                                             .cantidad = cadena->cantidad}};
        memcpy(emisor->salida.cambio.orden, cadena->orden, cadena->cantidad);
        cadena->control_en_curso = emisor->enviar = true;
        return -1;
    }
    if (!cadena->lista) {
        return -1;
    }
    for (size_t i = 0; i < cadena->usadas; ++i) {
        size_t indice = (cadena->proxima + i) % cadena->usadas;
        Venta *venta = &cadena->ventas[indice];
        // Mientras el origen siga activo, queda a cargo de difundir su venta.
        if (venta->origen != cadena->propia && posicion(cadena, venta->origen) < cadena->cantidad) {
            continue;
        }
        size_t previa;
        size_t turno = turno_pendiente(cadena, venta, &previa);
        if (turno == cadena->cantidad) {
            continue;
        }
        if (venta->reintentar > ahora) {
            int resto = (int)(venta->reintentar - ahora);
            if (espera == -1 || resto < espera) {
                espera = resto;
            }
            continue;
        }
        Mensaje salida = {.destino = cadena->orden[turno], .cambio = {.version = cadena->version}};
        const char *origen = cadena->config->tiendas[venta->origen].id;
        if (cadena->version == 1) {
            snprintf(salida.linea, sizeof salida.linea, "VENTA %s %s %" PRId64 " %" PRId64 " %zu\n",
                     cadena->id_cadena, origen, venta->secuencia, venta->importe, previa);
        } else {
            snprintf(salida.linea, sizeof salida.linea,
                     "VENTA2 %s %s %" PRId64 " %" PRId64 " %zu %" PRIu32 "\n",
                     cadena->id_cadena, origen, venta->secuencia, venta->importe, previa, cadena->version);
        }
        confirmacion(salida.confirmacion, cadena, venta, turno, cadena->version);
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
static bool atender_canales(Cadena *cadena, Canal canales[TRABAJADORES], size_t activos, int fin) {
    for (;;) {
        // Entrega las respuestas ya formadas antes de cerrar los pipes.
        if (cadena->salir && !canales[0].enviar && !canales[1].enviar) {
            return true;
        }
        int espera = activos == TRABAJADORES ? programar(cadena, &canales[EMISOR]) : -1;
        if (espera < -1) {
            return false;
        }
        struct pollfd fds[2 * TRABAJADORES + 1];
        fds[0] = (struct pollfd){.fd = fin, .events = POLLIN};
        // Vigila la escritura solo cuando hay algo que entregar a ese trabajador.
        for (size_t i = 0; i < activos; ++i) {
            fds[1 + 2 * i] = (struct pollfd){.fd = canales[i].desde_hilo[0], .events = POLLIN};
            fds[2 + 2 * i] = (struct pollfd){.fd = canales[i].enviar ? canales[i].hacia_hilo[1] : -1,
                                            .events = POLLOUT};
        }
        int listos = poll(fds, 2 * activos + 1, espera);
        if (listos < 0 && errno != EINTR) {
            return false;
        }
        if (listos <= 0) {
            continue;
        }
        if (fds[0].revents) {
            return true;
        }
        for (size_t i = 0; i < activos; ++i) {
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

// Reconstrucción del historial

static bool leer_venta(Cadena *cadena, char *campos[CAMPOS], size_t cantidad) {
    if (cantidad < 5 || cantidad > 6 || !cadena->version ||
        strcmp(campos[1], cadena->id_cadena) != 0) {
        return false;
    }
    Venta venta = {.origen = tienda_de(cadena, campos[2])};
    if (venta.origen == CONFIG_MAX_TIENDAS ||
        !numero(campos[3], &venta.secuencia) || venta.secuencia <= 0 ||
        !numero(campos[4], &venta.importe) || venta.importe <= 0 ||
        venta.importe > INT64_MAX - cadena->total || venta_por_id(cadena, venta.origen, venta.secuencia)) {
        return false;
    }
    if (venta.origen == cadena->propia) {
        // Las ventas propias conservan la secuencia y una clave única por compra.
        if (cantidad != 6 || !clave_valida(campos[5]) || cadena->secuencia == INT64_MAX ||
            venta.secuencia != cadena->secuencia + 1) {
            return false;
        }
        for (size_t i = 0; i < cadena->usadas; ++i) {
            if (cadena->ventas[i].origen == cadena->propia &&
                strcmp(cadena->ventas[i].clave, campos[5]) == 0) {
                return false;
            }
        }
        strcpy(venta.clave, campos[5]);
    } else if (cantidad != 5) {
        return false;
    }
    if (!reservar(cadena)) {
        return false;
    }
    incorporar_venta(cadena, venta);
    return true;
}

static bool leer_cabecera_cierre(Cadena *cadena, char *campos[CAMPOS], size_t n, bool importada) {
    if (n != 5 + cadena->cantidad_configurada || n < 6 || cadena->cierre.version) {
        return false;
    }
    int64_t version = siguiente(campos[1], UINT32_MAX), total;
    int64_t ventas = siguiente(campos[3], SIZE_MAX / sizeof(ItemCierre));
    int64_t activas = siguiente(campos[4], n - 5);
    if (version <= 0 || !numero(campos[2], &total) || !total || ventas <= 0 || activas <= 0) {
        return false;
    }
    EvidenciaCierre cierre = {.version = (uint32_t)version, .total = total,
                              .cantidad = (size_t)ventas, .participantes = (size_t)activas,
                              .excluidas = n - 5 - (size_t)activas, .miembros_leidos = n - 5};
    uint64_t vistos = 0;
    for (size_t i = 0; i < cantidad_miembros(&cierre); ++i) {
        size_t par = tienda_de(cadena, campos[i + 5]);
        if (par == CONFIG_MAX_TIENDAS || (vistos & (UINT64_C(1) << par))) {
            return false;
        }
        vistos |= UINT64_C(1) << par;
        cierre.miembros[i] = (unsigned char)par;
    }
    if (importada) {
        if (version < cadena->version ||
            lugar_en(cierre.miembros + cierre.participantes, cierre.excluidas,
                     cadena->propia) == cierre.excluidas) {
            return false;
        }
        cierre.items = calloc(cierre.cantidad, sizeof *cierre.items);
        if (!cierre.items) {
            return false;
        }
    } else {
        EvidenciaCierre esperada = propuesta_cierre(cadena);
        if (!cadena->cerrando || !misma_cabecera(&cierre, &esperada)) {
            return false;
        }
    }
    cadena->cierre = cierre;
    cadena->cerrando = true;
    return true;
}

static bool leer_registro(Cadena *cadena, char *linea, uint32_t *preparada) {
    char *campos[CAMPOS];
    size_t cantidad = separar(linea, campos);
    if (!cantidad) {
        return false;
    }
    if (cantidad == 1 && strcmp(campos[0], "CIERRE_EN_CURSO") == 0 && !cadena->cerrando) {
        cadena->cerrando = true;
        return true;
    }
    if (strcmp(campos[0], "CIERRE_PREPARADO") == 0 || strcmp(campos[0], "CIERRE_IMPORTADO") == 0) {
        return !*preparada && leer_cabecera_cierre(cadena, campos, cantidad,
                                                  strcmp(campos[0], "CIERRE_IMPORTADO") == 0);
    }
    if (cadena->cierre.version) {
        if (cadena->cierre.items && cantidad == 4) {
            ItemCierre dato;
            if (!leer_item(cadena, campos + 1, &dato)) {
                return false;
            }
            if (strcmp(campos[0], "CIERRE_VENTA") == 0) {
                return agregar_item(&cadena->cierre, dato);
            }
            if (strcmp(campos[0], "NO_INCLUIDA") == 0 &&
                cadena->cierre.items_leidos == cadena->cierre.cantidad) {
                const Venta *venta = venta_por_id(cadena, dato.origen, dato.secuencia);
                return dato.origen == cadena->propia && venta && venta->importe == dato.importe &&
                       !buscar_item(&cadena->cierre, &dato);
            }
        }
        return false;
    }
    if (strcmp(campos[0], "APLICADA") == 0) {
        return leer_venta(cadena, campos, cantidad);
    }
    int64_t version = cantidad > 1 ? siguiente(campos[1], UINT32_MAX) : -1;
    if (version <= 0) {
        return false;
    }
    if (cantidad == 5 && strcmp(campos[0], "PREPARA") == 0 && cadena->version &&
        version > cadena->version && version > cadena->ultima_propuesta && !*preparada &&
        siguiente(campos[4], UINT32_MAX) >= cadena->version &&
        siguiente(campos[4], UINT32_MAX) < version && strlen(campos[2]) == 1 &&
        strchr("BRIM", campos[2][0])) {
        if (campos[2][0] == 'M') {
            CambioLista propuesta;
            if (!formar_bajas(cadena, &propuesta, leer_bajas(campos[3])) ||
                siguiente(campos[4], UINT32_MAX) != cadena->version) {
                return false;
            }
        } else if (tienda_de(cadena, campos[3]) == CONFIG_MAX_TIENDAS) {
            return false;
        }
        *preparada = (uint32_t)version;
        cadena->ultima_propuesta = (uint32_t)version;
        return true;
    }
    if (cantidad == 2 && strcmp(campos[0], "CANCELA") == 0 &&
        (version == *preparada ||
         (!*preparada && version > cadena->version && version > cadena->ultima_propuesta))) {
        *preparada = 0;
        cadena->ultima_propuesta = (uint32_t)version;
        return true;
    }
    if (cantidad >= 3 && strcmp(campos[0], "VISTA") == 0 && version > cadena->version &&
        ((!cadena->version && version == 1) || version == *preparada)) {
        uint64_t vistos = 0;
        for (size_t i = 2; i < cantidad; ++i) {
            size_t tienda = tienda_de(cadena, campos[i]);
            if (tienda == CONFIG_MAX_TIENDAS || (vistos & (UINT64_C(1) << tienda))) {
                return false;
            }
            vistos |= UINT64_C(1) << tienda;
            cadena->orden[i - 2] = (unsigned char)tienda;
        }
        if (!(vistos & (UINT64_C(1) << cadena->propia))) {
            return false;
        }
        cadena->cantidad = cantidad - 2;
        cadena->version = (uint32_t)version;
        *preparada = 0;
        return true;
    }
    if (cantidad == 5 && strcmp(campos[0], "CONFIRMADA") == 0 && version == cadena->version &&
        !*preparada) {
        size_t origen = tienda_de(cadena, campos[2]), destino = tienda_de(cadena, campos[4]);
        int64_t secuencia;
        if (origen < CONFIG_MAX_TIENDAS && destino < CONFIG_MAX_TIENDAS && destino != origen &&
            destino != cadena->propia && posicion(cadena, destino) < cadena->cantidad &&
            numero(campos[3], &secuencia)) {
            Venta *venta = venta_por_id(cadena, origen, secuencia);
            if (venta) {
                venta->confirmadas |= UINT64_C(1) << destino;
                return true;
            }
        }
    }
    return false;
}

/*
 * Lee el historial completo y reconstruye los datos del propietario.
 * Devuelve true si la evidencia permite el modo solicitado. Un cierre o
 * una ronda inconclusa puede permitir consulta sin permitir reanudar ventas.
 */
static bool reconstruir(Cadena *cadena, const char *cabecera) {
    int fd = dup(cadena->historial);
    FILE *archivo = fd == -1 ? NULL : fdopen(fd, "r");
    if (!archivo) {
        if (fd != -1) {
            close(fd);
        }
        perror("lectura del historial");
        return false;
    }
    char linea[MAX_REGISTRO];
    if (!fgets(linea, sizeof linea, archivo) || strcmp(linea, cabecera) != 0) {
        fprintf(stderr, "Historial incompatible o identidad distinta. Se conserva el archivo.\n");
        fclose(archivo);
        return false;
    }
    uint32_t preparada = 0;
    bool valido = true;
    bool cerrada = false;
    bool registros = false;
    while (valido && fgets(linea, sizeof linea, archivo)) {
        registros = true;
        if (strcmp(linea, "CERRADA_META\n") == 0) {
            cerrada = true;
            break;
        }
        valido = leer_registro(cadena, linea, &preparada);
    }
    // La marca final debe cerrar el archivo y, si se importó, el certificado completo.
    if (cerrada && (fgetc(archivo) != EOF ||
                    (cadena->cierre.items && !evidencia_completa(&cadena->cierre)))) {
        valido = false;
    }
    if (ferror(archivo)) {
        valido = false;
    }
    if (fclose(archivo) != 0) {
        valido = false;
    }
    if (!valido || (cadena->cierre.items && !cerrada)) {
        fprintf(stderr, "Historial inválido o registro incompleto. Se conserva el archivo.\n");
        return false;
    }
    if (cerrada) {
        if (cadena->historico || (cadena->solo_lectura && cadena->cierre.version && !preparada)) {
            cadena->definitiva = true;
            return true;
        }
        fprintf(stderr, "Tienda cerrada por meta. Se conserva el historial.\n");
        return false;
    }
    if (cadena->historico) {
        cadena->reiniciada = preparada || !cadena->version;
        return true;
    }
    if (cadena->solo_lectura) {
        fprintf(stderr, "El historial no acredita una decisión de cierre.\n");
        return false;
    }
    // El historial carece del umbral original. Una ronda pendiente queda para revisión.
    if (cadena->cerrando) {
        fprintf(stderr, "Cierre pendiente en el historial. No se puede reanudar. Se conserva el archivo.\n");
        return false;
    }
    if (cadena->total >= cadena->config->limite_gubernamental_centavos) {
        fprintf(stderr, "El total del historial abierto (%" PRId64 ") alcanza el límite gubernamental (%" PRId64 "). Se conserva el archivo.\n",
                cadena->total, cadena->config->limite_gubernamental_centavos);
        return false;
    }
    if (!registros) {
        return true;
    }
    if (preparada || !cadena->version) {
        fprintf(stderr, "Cambio de lista inconcluso o sin vista acordada. Recuperación pendiente.\n");
        return false;
    }
    cadena->reiniciada = true;
    return true;
}

static int abrir_historial(Cadena *cadena) {
    const char *ruta = cadena->tienda->historial;
    int opciones = cadena->solo_lectura ? O_RDONLY : O_RDWR | O_CREAT | O_APPEND;
    int fd = open(ruta, opciones | O_NONBLOCK, 0600);
    if (fd == -1) {
        perror(ruta);
        return -1;
    }
    struct stat datos;
    if (fstat(fd, &datos) == -1 || !S_ISREG(datos.st_mode)) {
        fprintf(stderr, "El historial debe ser un archivo regular: %s\n", ruta);
        close(fd);
        return -1;
    }
    cadena->historial = fd;
    char cabecera[RED_MAX_LINEA];
    snprintf(cabecera, sizeof cabecera, "HISTORIAL5 %s %s\n", cadena->id_cadena, cadena->tienda->id);
    bool valido = datos.st_size == 0 && !cadena->solo_lectura ? anexar(cadena, cabecera)
                                                             : reconstruir(cadena, cabecera);
    if (!valido) {
        close(fd);
        free(cadena->ventas);
        free(cadena->cierre.items);
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

static Cadena iniciar_propietario(const Configuracion *config, const ConfigTienda *tienda) {
    Cadena cadena = {.config = config, .tienda = tienda,
                     .id_cadena = tienda->cadena == CONFIG_SIN_CADENA ? "-" : config->cadenas[tienda->cadena].id,
                     .propia = (size_t)(tienda - config->tiendas), .en_envio = SIZE_MAX};
    for (size_t i = 0; i < config->cantidad_tiendas; ++i) {
        if (config->tiendas[i].cadena == tienda->cadena) {
            ++cadena.cantidad_configurada;
        }
    }
    return cadena;
}

// Arranque y terminación del proceso de tienda

static int ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                    const ConfigTienda *entrada, bool solo_lectura) {
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
    Cadena cadena = iniciar_propietario(config, tienda);
    cadena.solo_lectura = solo_lectura;
    TiendaLocal local = TIENDA_LOCAL_INICIAL;
    if (tienda->cadena == CONFIG_SIN_CADENA) {
        ResultadoTiendaLocal resultado = tienda_local_abrir_consulta(
            &local, tienda->historial, config->limite_gubernamental_centavos, false);
        if (resultado != TIENDA_OK || local.cerrada_por_meta) {
            if (resultado == TIENDA_META_INCOMPATIBLE) {
                fprintf(stderr, "%s: el ingreso del historial abierto alcanza el límite gubernamental (%" PRId64 "). Se conserva el archivo.\n",
                        tienda->id, config->limite_gubernamental_centavos);
            } else {
                fprintf(stderr, "%s: %s (%d).\n", tienda->id, local.cerrada_por_meta ? "Tienda cerrada por meta"
                                            : "No se pudo reconstruir el historial independiente", (int)resultado);
            }
            if (resultado == TIENDA_OK) {
                tienda_local_cerrar(&local);
            }
            close(escucha);
            return EXIT_FAILURE;
        }
        cadena.local = &local;
    } else {
        cadena.historial = abrir_historial(&cadena);
    }
    if (!cadena.local && cadena.historial == -1) {
        close(escucha);
        return EXIT_FAILURE;
    }

    Canal canales[TRABAJADORES] = {0};
    Trabajador trabajadores[TRABAJADORES];
    pthread_t hilos[TRABAJADORES];
    size_t iniciados = 0;
    size_t activos = cadena.local || solo_lectura ? RED_RECEPTORES : TRABAJADORES;
    int fin[2] = {-1, -1};
    int estado = EXIT_FAILURE;
    for (size_t i = 0; i < activos; ++i) {
        canales[i].desde_hilo[0] = canales[i].desde_hilo[1] = -1;
        canales[i].hacia_hilo[0] = canales[i].hacia_hilo[1] = -1;
    }
    if (!preparar_senales(fin)) {
        perror("señales de cadena");
        goto cerrar;
    }
    for (size_t i = 0; i < activos; ++i) {
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

    if (cadena.local) {
        printf("INDEPENDIENTE %s\n", tienda->id);
    } else if (cadena.solo_lectura) {
        printf("CONSULTA_CIERRE %s\n", tienda->id);
    } else if (cadena.reiniciada) {
        printf("RECUPERANDO %s propio=%" PRId64 " total=%" PRId64 "\n",
               tienda->id, cadena.ingreso_propio, cadena.total);
    } else if (entrada) {
        cadena.incorporando = true;
        canales[EMISOR].salida = (Mensaje){.destino = (size_t)(entrada - config->tiendas),
                                         .inicio = true};
        canales[EMISOR].enviar = true;
    } else {
        cadena.orden[0] = (unsigned char)cadena.propia;
        cadena.cantidad = 1;
        printf("INCORPORADA %s\n", tienda->id);
        if (cadena.cantidad_configurada == 1) {
            CambioLista inicial = {.version = 1, .cantidad = 1, .orden = {(unsigned char)cadena.propia}};
            if (!instalar(&cadena, &inicial)) {
                fprintf(stderr, "No se guardó la lista inicial\n");
                goto cerrar;
            }
        }
    }
    if (atender_canales(&cadena, canales, activos, fin[0])) {
        estado = EXIT_SUCCESS;
    } else {
        fprintf(stderr, "Se interrumpió el servicio de cadena de %s\n", tienda->id);
    }

cerrar:
    /*
     * Cerrar los pipes hacia los trabajadores les indica que deben salir.
     * Después de esperar a los hilos se cierran el socket y los extremos restantes.
     */
    for (size_t i = 0; i < activos; ++i) {
        close(canales[i].hacia_hilo[1]);
    }
    for (size_t i = 0; i < iniciados; ++i) {
        pthread_join(hilos[i], NULL);
    }
    for (size_t i = 0; i < activos; ++i) {
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
    if (cadena.local ? tienda_local_cerrar(&local) != TIENDA_OK : close(cadena.historial) == -1) {
        perror("cierre del historial");
        estado = EXIT_FAILURE;
    }
    free(cadena.ventas);
    free(cadena.cierre.items);
    free(cadena.borrador.items);
    return estado;
}

int tienda_cadena_ejecutar(const Configuracion *config, const ConfigTienda *tienda,
                          const ConfigTienda *entrada) {
    return ejecutar(config, tienda, entrada, false);
}

int tienda_cadena_consultar_cierre(const Configuracion *config, const ConfigTienda *tienda) {
    return ejecutar(config, tienda, NULL, true);
}

int tienda_independiente_ejecutar(const Configuracion *config, const ConfigTienda *tienda) {
    return ejecutar(config, tienda, NULL, false);
}

static bool consultar_historial(void *contexto, const char *linea, char *respuesta) {
    char copia[RED_MAX_LINEA + 1];
    strcpy(copia, linea);
    atender(contexto, copia, respuesta);
    return true;
}

int tienda_consultar_historial(const Configuracion *config, const ConfigTienda *tienda, bool ventas) {
    Cadena cadena = iniciar_propietario(config, tienda);
    cadena.solo_lectura = cadena.historico = true;
    TiendaLocal local = TIENDA_LOCAL_INICIAL;
    if (tienda->cadena == CONFIG_SIN_CADENA) {
        ResultadoTiendaLocal resultado = tienda_local_abrir_consulta(
            &local, tienda->historial, config->limite_gubernamental_centavos, true);
        if (resultado != TIENDA_OK) {
            fprintf(stderr, "No se pudo leer el historial de %s (%d).\n", tienda->id, (int)resultado);
            return EXIT_FAILURE;
        }
        cadena.local = &local;
    } else if (abrir_historial(&cadena) == -1) {
        return EXIT_FAILURE;
    }
    int resultado = monitor_mostrar(config, tienda, ventas, consultar_historial, &cadena);
    if (cadena.local ? tienda_local_cerrar(&local) != TIENDA_OK : close(cadena.historial) == -1) {
        perror("cierre del historial");
        resultado = EXIT_FAILURE;
    }
    free(cadena.ventas);
    free(cadena.cierre.items);
    return resultado;
}

/*
 * Clasifica el historial para el lanzador sin modificarlo. Si se solicitan,
 * informa si la clave e importe coinciden y qué tiendas quedaron fuera del cierre.
 * Devuelve ARCHIVO_BLOQUEADO cuando la evidencia no permite un arranque seguro.
 */
static EstadoArchivoTienda inspeccionar_historial(const Configuracion *config, const ConfigTienda *tienda,
                                                 const char *clave, int64_t importe, bool *registrada,
                                                 uint64_t *excluidas, ResumenHistorial *resumen) {
    struct stat datos;
    if (registrada) {
        *registrada = false;
    }
    if (excluidas) {
        *excluidas = 0;
    }
    if (stat(tienda->historial, &datos) == -1) {
        return errno == ENOENT ? ARCHIVO_NUEVO : ARCHIVO_BLOQUEADO;
    }
    if (!S_ISREG(datos.st_mode)) {
        return ARCHIVO_BLOQUEADO;
    }
    // Un historial vacío de una independiente permite informar cero ventas.
    if (!datos.st_size && (!resumen || tienda->cadena != CONFIG_SIN_CADENA)) {
        return ARCHIVO_NUEVO;
    }
    int64_t limite = config->limite_gubernamental_centavos;
    if (tienda->cadena == CONFIG_SIN_CADENA) {
        TiendaLocal local = TIENDA_LOCAL_INICIAL;
        if (tienda_local_abrir_consulta(&local, tienda->historial, limite, true) != TIENDA_OK) {
            return ARCHIVO_BLOQUEADO;
        }
        EstadoArchivoTienda estado = local.cerrada_por_meta ? ARCHIVO_CERRADO
                                    : local.ingreso_propio >= limite ? ARCHIVO_BLOQUEADO : ARCHIVO_ABIERTO;
        for (size_t i = 0; registrada && i < local.cantidad; ++i) {
            if (strcmp(local.ventas[i].clave, clave) == 0) {
                *registrada = local.ventas[i].importe == importe;
                break;
            }
        }
        if (resumen) {
            *resumen = (ResumenHistorial){.disponible = true, .cerrada = local.cerrada_por_meta,
                                        .ventas_propias = local.cantidad, .ingreso_propio = local.ingreso_propio};
        }
        if (tienda_local_cerrar(&local) != TIENDA_OK) {
            if (resumen) {
                resumen->disponible = false;
            }
            return ARCHIVO_BLOQUEADO;
        }
        return estado;
    }
    Cadena cadena = iniciar_propietario(config, tienda);
    cadena.solo_lectura = cadena.historico = true;
    if (abrir_historial(&cadena) == -1) {
        return ARCHIVO_BLOQUEADO;
    }
    EstadoArchivoTienda estado = cadena.definitiva ? ARCHIVO_CERRADO
                                : cadena.cerrando || cadena.total >= limite ? ARCHIVO_BLOQUEADO
                                : cadena.version ? ARCHIVO_ABIERTO
                                : datos.st_size == (off_t)(strlen(cadena.id_cadena) + strlen(tienda->id) +
                                                          sizeof "HISTORIAL5  \n" - 1)
                                  ? ARCHIVO_NUEVO : ARCHIVO_BLOQUEADO;
    if (cadena.reiniciada && cadena.version) {
        estado = ARCHIVO_BLOQUEADO;
    }
    if (registrada) {
        const Venta *venta = venta_por_clave(&cadena, clave);
        *registrada = venta && venta->importe == importe;
    }
    if (excluidas && estado == ARCHIVO_CERRADO) {
        for (size_t i = cadena.cierre.participantes; i < cantidad_miembros(&cadena.cierre); ++i) {
            *excluidas |= UINT64_C(1) << cadena.cierre.miembros[i];
        }
    }
    if (resumen) {
        *resumen = (ResumenHistorial){.disponible = true, .cerrada = cadena.definitiva,
            .cierre_pendiente = cadena.cerrando && !cadena.definitiva,
            .recuperacion_pendiente = cadena.reiniciada, .ventas_propias = (size_t)cadena.secuencia,
            .ingreso_propio = cadena.ingreso_propio, .total_conocido = cadena.total};
        if (!cadena.definitiva && cadena.version && !cadena.reiniciada) {
            resumen->ventas_sin_confirmacion = pendientes_propias(&cadena);
        }
        if (cadena.definitiva && cadena.cierre.version) {
            resumen->certificado = cadena.cierre;
            if (cadena.cierre.items) {
                cadena.cierre.items = NULL;
            } else {
                ItemCierre *items = malloc(cadena.cierre.cantidad * sizeof *items);
                resumen->certificado.items = items;
                if (items) {
                    for (size_t i = 0; i < cadena.cierre.cantidad; ++i) {
                        items[i] = item(&cadena, i);
                    }
                    resumen->certificado.items_leidos = cadena.cierre.cantidad;
                }
            }
        }
    }
    free(cadena.ventas);
    free(cadena.cierre.items);
    if (close(cadena.historial) == -1) {
        if (resumen) {
            free(resumen->certificado.items);
            *resumen = (ResumenHistorial){0};
        }
        return ARCHIVO_BLOQUEADO;
    }
    return estado;
}

EstadoArchivoTienda tienda_inspeccionar_historial(const Configuracion *config, const ConfigTienda *tienda,
                                                  const char *clave, int64_t importe, bool *registrada,
                                                  uint64_t *excluidas) {
    return inspeccionar_historial(config, tienda, clave, importe, registrada, excluidas, NULL);
}

ResumenHistorial tienda_resumir_historial(const Configuracion *config, const ConfigTienda *tienda) {
    ResumenHistorial resumen = {0};
    inspeccionar_historial(config, tienda, NULL, 0, NULL, NULL, &resumen);
    return resumen;
}

// El orden de registro puede diferir entre tiendas. Se cotejan identificadores e importes.
bool tienda_certificados_coinciden(const EvidenciaCierre *a, const EvidenciaCierre *b) {
    if (!misma_cabecera(a, b)) {
        return false;
    }
    for (size_t i = 0; i < a->cantidad; ++i) {
        const ItemCierre *otra = buscar_item(b, &a->items[i]);
        if (!otra || otra->importe != a->items[i].importe) {
            return false;
        }
    }
    return true;
}
