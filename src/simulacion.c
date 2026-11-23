#include "simulacion.h"
#include "configuracion.h"
#include "red.h"
#include "tienda_cadena.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    CAPACIDAD = CONFIG_MAX_SOLICITUDES_EN_VUELO + CONFIG_MAX_COMPRAS_EN_ESPERA,
    TRABAJADORES = 8,
    SIN_TRABAJO = -1, TRABAJO_BAJA = -2,
    ESPERA_REINTENTO = 250, SIN_PROGRESO_MS = 20000, ARRANQUE_MS = 8000,
    DETENCION_MS = 6000, LINEA_HIJO = CONFIG_MAX_TIENDAS * (CONFIG_MAX_ID + 1) + 128
};

_Static_assert(CONFIG_MAX_SOLICITUDES_EN_VUELO <= TRABAJADORES - 1, "Un trabajador se reserva para bajas");

typedef enum { INACTIVA, ARRANCANDO, ABIERTA, PAUSADA, CERRANDO, CERRADA, ERROR_TIENDA } Estado;

typedef struct {
    pid_t pid;
    int salida;
    // El proceso anuncia su estado. El historial se lee aparte para comprobar el cierre.
    Estado estado;
    EstadoArchivoTienda archivo;
    bool incorporada, cortada, matando, excluida, saturada, bloqueo_reportado;
    size_t primera, cursor_baja;
    /*
     * La vista es la lista ordenada de participantes que anuncia la tienda.
     * Cada bit corresponde a una tienda del JSON. orden conserva los lugares anunciados.
     */
    uint64_t vista;
    unsigned char orden[CONFIG_MAX_TIENDAS];
    size_t cantidad_vista;
    uint32_t version;
    int64_t proxima, duracion, volver, revisar_baja;
    char linea[LINEA_HIJO];
    size_t leidos;
} Hijo;

typedef struct {
    // La secuencia identifica la compra en el diario. Cero deja libre la plaza.
    int64_t secuencia, importe, reintentar;
    bool intentada; // El primer intento ya quedó anotado en el diario.
    bool en_vuelo;  // Conserva una plaza mientras falta el resultado de admisión.
    bool en_envio;  // Un trabajador está atendiendo el intento TCP actual.
} Compra;

typedef struct {
    size_t destino;
    char linea[RED_MAX_LINEA + 1];
} Peticion;

typedef struct {
    pid_t pid;
    int entrada, salida, trabajo;
} Trabajador;

typedef struct {
    Configuracion config;
    const char *ruta;
    FILE *diario;
    Hijo hijos[CONFIG_MAX_TIENDAS];
    Compra compras[CONFIG_MAX_TIENDAS][CAPACIDAD];
    Trabajador trabajadores[TRABAJADORES];
    int64_t secuencia, progreso;
    uint64_t excluidas_certificadas;
    size_t siguiente;
    bool error;
} Simulacion;

static volatile sig_atomic_t detener;

static void solicitar_detencion(int senal) {
    (void)senal;
    detener = 1;
}

static bool senales(void) {
    struct sigaction accion = {.sa_handler = solicitar_detencion};
    sigemptyset(&accion.sa_mask);
    if (sigaction(SIGINT, &accion, NULL) == -1 || sigaction(SIGTERM, &accion, NULL) == -1) {
        return false;
    }
    accion.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &accion, NULL) == 0;
}

static const ConfigTienda *tienda(const Simulacion *sim, size_t i) {
    return &sim->config.tiendas[i];
}

static const char *cadena(const Simulacion *sim, size_t i) {
    int grupo = tienda(sim, i)->cadena;
    return grupo == CONFIG_SIN_CADENA ? "-" : sim->config.cadenas[grupo].id;
}

static void clave_compra(char clave[CONFIG_MAX_ID + 1], int64_t secuencia) {
    snprintf(clave, CONFIG_MAX_ID + 1, "sim_%" PRId64, secuencia);
}

static bool cifra(const char *campo, int64_t *valor) {
    if (*campo < '0' || *campo > '9') {
        return false;
    }
    char *fin;
    errno = 0;
    intmax_t dato = strtoimax(campo, &fin, 10);
    if (errno == ERANGE || *fin || dato > INT64_MAX) {
        return false;
    }
    *valor = (int64_t)dato;
    return true;
}

static Compra *libre(Simulacion *sim, size_t i) {
    for (size_t j = 0; j < CAPACIDAD; ++j) {
        if (!sim->compras[i][j].secuencia) {
            return &sim->compras[i][j];
        }
    }
    return NULL;
}

static int compras_en_espera(const Simulacion *sim, size_t i) {
    int cantidad = 0;
    for (size_t j = 0; j < CAPACIDAD; ++j) {
        const Compra *compra = &sim->compras[i][j];
        cantidad += compra->secuencia > 0 && !compra->en_vuelo;
    }
    return cantidad;
}

// Diario del lanzador. Guarda la clave y el importe antes del primer envío.

static bool guardar(Simulacion *sim, const char *formato, ...) {
    va_list args;
    va_start(args, formato);
    int escrito = vfprintf(sim->diario, formato, args);
    va_end(args);
    if (escrito < 0 || fflush(sim->diario) == EOF) {
        perror("diario de simulación");
        sim->error = true;
        return false;
    }
    return true;
}

static bool terminar_compra(Simulacion *sim, Compra *compra) {
    // La plaza queda libre cuando el diario registra que ya no hay que reintentar.
    if (!guardar(sim, "F %" PRId64 "\n", compra->secuencia)) {
        return false;
    }
    *compra = (Compra){0};
    return true;
}

/*
 * Recupera del diario una compra generada, su primer intento, su fin o un corte.
 * Devuelve false si la línea contradice los registros anteriores o está incompleta.
 */
static bool registro(Simulacion *sim, char *linea) {
    size_t n = strlen(linea);
    if (!n || linea[n - 1] != '\n') {
        return false;
    }
    linea[n - 1] = '\0';
    char *resto;
    char *tipo = strtok_r(linea, " ", &resto);
    char *primero = strtok_r(NULL, " ", &resto);
    char *segundo = strtok_r(NULL, " ", &resto);
    char *tercero = strtok_r(NULL, " ", &resto);
    if (!tipo || !primero || strtok_r(NULL, " ", &resto)) {
        return false;
    }
    int64_t dato;
    bool finalizada = strcmp(tipo, "F") == 0;
    if ((finalizada || strcmp(tipo, "E") == 0) && !segundo && cifra(primero, &dato) && dato > 0) {
        for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
            for (size_t j = 0; j < CAPACIDAD; ++j) {
                Compra *compra = &sim->compras[i][j];
                if (compra->secuencia == dato) {
                    if (finalizada) {
                        *compra = (Compra){0};
                    } else if (compra->intentada) {
                        return false;
                    } else {
                        compra->intentada = true;
                    }
                    return true;
                }
            }
        }
        return false;
    }
    const ConfigTienda *destino = configuracion_buscar(&sim->config, primero);
    if (!destino) {
        return false;
    }
    size_t i = (size_t)(destino - sim->config.tiendas);
    if (strcmp(tipo, "P") == 0 && segundo && tercero && cifra(segundo, &dato) &&
        sim->secuencia < INT64_MAX && dato == sim->secuencia + 1) {
        int64_t importe;
        Compra *sitio = libre(sim, i);
        if (!sitio || !cifra(tercero, &importe) || importe <= 0) {
            return false;
        }
        *sitio = (Compra){.secuencia = dato, .importe = importe};
        sim->secuencia = dato;
        return true;
    }
    Hijo *hijo = &sim->hijos[i];
    if (strcmp(tipo, "B") == 0 && segundo && !tercero && cifra(segundo, &dato) &&
        dato <= INT_MAX) {
        hijo->cortada = true;
        hijo->duracion = dato;
        return true;
    }
    if (strcmp(tipo, "R") == 0 && !segundo && hijo->cortada) {
        hijo->cortada = false;
        return true;
    }
    return false;
}

// Al reanudar solo los intentos inciertos pueden ocupar las plazas en vuelo.
static bool cargar_capacidades(Simulacion *sim) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const ConfigCompras *config = &tienda(sim, i)->compras;
        int pendientes = compras_en_espera(sim, i), intentadas = 0;
        for (size_t j = 0; j < CAPACIDAD; ++j) {
            intentadas += sim->compras[i][j].intentada;
        }
        int excedente = pendientes - config->max_compras_en_espera;
        if (excedente > intentadas || excedente > config->max_solicitudes_en_vuelo) {
            fprintf(stderr, "%s: capacidad insuficiente para reanudar %d compras pendientes (%d con intento registrado). "
                    "Revise max_solicitudes_en_vuelo=%d y max_compras_en_espera=%d. Se conservan los archivos.\n",
                    tienda(sim, i)->id, pendientes, intentadas, config->max_solicitudes_en_vuelo,
                    config->max_compras_en_espera);
            return false;
        }
        for (size_t j = 0; j < CAPACIDAD && excedente > 0; ++j) {
            if (sim->compras[i][j].intentada) {
                sim->compras[i][j].en_vuelo = true;
                --excedente;
            }
        }
    }
    return true;
}

static bool cabecera(Simulacion *sim, bool reanudar) {
    char esperada[CONFIG_MAX_RUTA + 128], leida[sizeof esperada];
    snprintf(esperada, sizeof esperada, "SIMULACION1 %" PRId64 " %zu\n",
             sim->config.limite_gubernamental_centavos, sim->config.cantidad_tiendas);
    for (size_t i = 0; ; ++i) {
        if (reanudar) {
            if (!fgets(leida, sizeof leida, sim->diario) || strcmp(leida, esperada) != 0) {
                return false;
            }
        } else if (!guardar(sim, "%s", esperada)) {
            return false;
        }
        if (i == sim->config.cantidad_tiendas) {
            break;
        }
        const ConfigTienda *t = tienda(sim, i);
        snprintf(esperada, sizeof esperada, "T %s %c %s %s %u %s\n", t->id,
                 t->cadena < 0 ? 'I' : 'C', cadena(sim, i), t->direccion, (unsigned)t->puerto, t->historial);
    }
    return true;
}

/*
 * Abre el diario exclusivo del lanzador y comprueba el escenario.
 * Devuelve true si puede empezar a anexar o si reconstruyó los pendientes
 * dentro de las capacidades configuradas. El llamador cierra el diario abierto.
 */
static bool abrir_diario(Simulacion *sim, bool reanudar) {
    size_t longitud = strlen(sim->ruta) + sizeof ".simulacion";
    char *ruta = malloc(longitud);
    if (!ruta) {
        return false;
    }
    snprintf(ruta, longitud, "%s.simulacion", sim->ruta);
    int fd = open(ruta, O_RDWR | O_APPEND | O_CLOEXEC | O_NONBLOCK | (reanudar ? 0 : O_CREAT), 0600);
    if (fd == -1) {
        perror(ruta);
        free(ruta);
        return false;
    }
    struct stat datos;
    // El bloqueo de escritura impide que dos lanzadores usen a la vez el diario.
    struct flock bloqueo = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fstat(fd, &datos) == -1 || !S_ISREG(datos.st_mode) || fcntl(fd, F_SETLK, &bloqueo) == -1 ||
        ((datos.st_size != 0) != reanudar) || (sim->diario = fdopen(fd, "a+")) == NULL) {
        fprintf(stderr, "%s: diario ocupado, incompatible o requiere --reanudar. Se conserva.\n", ruta);
        close(fd);
        free(ruta);
        return false;
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        struct stat historial;
        if (stat(tienda(sim, i)->historial, &historial) == 0 && historial.st_dev == datos.st_dev &&
            historial.st_ino == datos.st_ino) {
            fprintf(stderr, "%s coincide con un historial de tienda. Se conserva.\n", ruta);
            free(ruta);
            return false;
        }
    }
    printf("DIARIO %s\n", ruta);
    free(ruta);
    if (reanudar && fseek(sim->diario, 0, SEEK_SET) != 0) {
        return false;
    }
    if (!cabecera(sim, reanudar)) {
        goto invalido;
    }
    if (reanudar) {
        char linea[128];
        while (fgets(linea, sizeof linea, sim->diario)) {
            if (!registro(sim, linea)) {
                goto invalido;
            }
        }
        if (ferror(sim->diario) || fseek(sim->diario, 0, SEEK_END) != 0) {
            goto invalido;
        }
        if (!cargar_capacidades(sim)) {
            return false;
        }
    }
    return true;

invalido:
    fprintf(stderr, "Diario incompleto o incompatible con el escenario. Se conserva.\n");
    return false;
}

// Procesos de tienda y trabajadores de compras.

static void cerrar_canales_heredados(const Simulacion *sim) {
    // El hijo solo conserva sus canales para no mantener abiertos los demás pipes.
    close(fileno(sim->diario));
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        if (sim->trabajadores[i].pid) {
            close(sim->trabajadores[i].entrada);
            close(sim->trabajadores[i].salida);
        }
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].salida >= 0) {
            close(sim->hijos[i].salida);
        }
    }
}

static bool iniciar_tienda(Simulacion *sim, size_t i, const char *entrada) {
    const ConfigTienda *t = tienda(sim, i);
    Hijo *hijo = &sim->hijos[i];
    int par[2];
    if (pipe(par) == -1) {
        return false;
    }
    if (!red_no_bloqueante(par[0])) {
        close(par[0]);
        close(par[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (setpgid(0, 0) == -1 || dup2(par[1], STDOUT_FILENO) == -1) {
            _exit(126);
        }
        close(par[0]);
        close(par[1]);
        cerrar_canales_heredados(sim);
        // Los reinicios usan la misma configuración validada al iniciar el ensayo.
        const ConfigTienda *par_inicial = entrada ? configuracion_buscar(&sim->config, entrada) : NULL;
        int estado = t->cadena == CONFIG_SIN_CADENA ? tienda_independiente_ejecutar(&sim->config, t)
                                                    : tienda_cadena_ejecutar(&sim->config, t, par_inicial);
        _exit(estado);
    }
    close(par[1]);
    if (pid < 0) {
        close(par[0]);
        return false;
    }
    // Separar el grupo deja que Ctrl+C llegue al lanzador y este ordene la salida.
    (void)setpgid(pid, pid);
    hijo->pid = pid;
    hijo->salida = par[0];
    hijo->estado = ARRANCANDO;
    hijo->incorporada = false;
    hijo->vista = 0;
    hijo->version = 0;
    hijo->leidos = 0;
    printf("PROCESO_TIENDA %s pid=%ld\n", t->id, (long)pid);
    return true;
}

static bool transferir(int fd, void *bloque, size_t tamano, bool salida) {
    char *datos = bloque;
    while (tamano) {
        ssize_t n = salida ? write(fd, datos, tamano) : read(fd, datos, tamano);
        if (n > 0) {
            datos += n;
            tamano -= (size_t)n;
        } else if (n == 0 || errno != EINTR) {
            return false;
        }
    }
    return true;
}

static void intercambiar_en_trabajador(const Simulacion *sim, int entrada, int salida) {
    Peticion peticion;
    while (transferir(entrada, &peticion, sizeof peticion, false)) {
        char respuesta[RED_MAX_LINEA + 1] = "", error[128];
        int64_t ahora = red_reloj_ms();
        // Una respuesta vacía deja el resultado de admisión sin confirmar.
        if (ahora < 0 || !red_intercambiar_hasta(tienda(sim, peticion.destino), peticion.linea, respuesta,
                                                error, sizeof error, ahora + RED_PLAZO_MS)) {
            respuesta[0] = '\0';
        }
        if (!transferir(salida, respuesta, sizeof respuesta, true)) {
            break;
        }
    }
}

static bool iniciar_trabajador(Simulacion *sim, size_t i) {
    int solicitud[2], respuesta[2];
    if (pipe(solicitud) == -1) {
        return false;
    }
    if (pipe(respuesta) == -1) {
        close(solicitud[0]);
        close(solicitud[1]);
        return false;
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (setpgid(0, 0) == -1) {
            _exit(126);
        }
        close(solicitud[1]);
        close(respuesta[0]);
        cerrar_canales_heredados(sim);
        intercambiar_en_trabajador(sim, solicitud[0], respuesta[1]);
        _exit(EXIT_SUCCESS);
    }
    close(solicitud[0]);
    close(respuesta[1]);
    if (pid < 0) {
        close(solicitud[1]);
        close(respuesta[0]);
        return false;
    }
    (void)setpgid(pid, pid);
    sim->trabajadores[i] = (Trabajador){pid, solicitud[1], respuesta[0], SIN_TRABAJO};
    return true;
}

static void aviso_hijo(Simulacion *sim, size_t i, char *linea, int64_t ahora) {
    Hijo *hijo = &sim->hijos[i];
    const char *id = tienda(sim, i)->id;
    if (strncmp(linea, "LISTA ", 6) == 0) {
        char *resto;
        (void)strtok_r(linea + 6, " ", &resto);
        uint64_t vista = 0;
        size_t primera = CONFIG_MAX_TIENDAS;
        hijo->cantidad_vista = 0;
        for (char *nombre = strtok_r(NULL, " ", &resto); nombre; nombre = strtok_r(NULL, " ", &resto)) {
            const ConfigTienda *par = configuracion_buscar(&sim->config, nombre);
            if (par && par->cadena == tienda(sim, i)->cadena && hijo->cantidad_vista < CONFIG_MAX_TIENDAS) {
                size_t j = (size_t)(par - sim->config.tiendas);
                hijo->orden[hijo->cantidad_vista++] = (unsigned char)j;
                if (!vista) {
                    primera = j;
                }
                vista |= UINT64_C(1) << j;
            }
        }
        hijo->primera = primera;
        hijo->vista = vista;
        hijo->version = 0;
    } else if (sscanf(linea, "VISTA %*s %" SCNu32, &hijo->version) == 1) {
        return;
    } else if (strncmp(linea, "INCORPORADA ", 12) == 0) {
        hijo->incorporada = true;
        return;
    } else if (strncmp(linea, "INDEPENDIENTE ", 14) == 0) {
        hijo->incorporada = true;
    } else if (strncmp(linea, "CAMBIO_EN_CURSO ", 16) == 0 && hijo->estado == ABIERTA) {
        hijo->estado = PAUSADA;
        return;
    } else if (strncmp(linea, "CAMBIO_CANCELADO ", 16) == 0 && hijo->estado == PAUSADA) {
        hijo->estado = ABIERTA;
        return;
    } else if (strncmp(linea, "CIERRE_EN_CURSO ", 16) == 0 ||
               strncmp(linea, "CIERRE_PREPARADO ", 17) == 0 ||
               strncmp(linea, "CERRADA_META ", 13) == 0) {
        hijo->estado = CERRANDO;
        return;
    } else if (strncmp(linea, "APLICADA ", 9) == 0) {
        sim->progreso = ahora;
        return;
    } else {
        return;
    }
    if (hijo->estado == ARRANCANDO || hijo->estado == INACTIVA) {
        hijo->estado = ABIERTA;
        hijo->proxima = ahora + tienda(sim, i)->compras.intervalo_ms;
        if (hijo->cortada && guardar(sim, "R %s\n", id)) {
            hijo->cortada = false;
            printf("RECUPERADA %s\n", id);
        }
    } else if (hijo->estado == PAUSADA) {
        hijo->estado = ABIERTA;
    }
}

static void leer_hijo(Simulacion *sim, size_t i, int64_t ahora) {
    Hijo *hijo = &sim->hijos[i];
    char bloque[4096];
    ssize_t n;
    while (hijo->salida >= 0 && (n = read(hijo->salida, bloque, sizeof bloque)) > 0) {
        for (ssize_t k = 0; k < n; ++k) {
            if (bloque[k] == '\n') {
                hijo->linea[hijo->leidos] = '\0';
                puts(hijo->linea);
                aviso_hijo(sim, i, hijo->linea, ahora);
                hijo->leidos = 0;
            } else if (hijo->leidos < sizeof hijo->linea - 1) {
                hijo->linea[hijo->leidos++] = bloque[k];
            }
        }
    }
}

static void acreditar(Simulacion *sim, size_t i) {
    Hijo *hijo = &sim->hijos[i];
    uint64_t excluidas;
    hijo->archivo = tienda_inspeccionar_historial(&sim->config, tienda(sim, i), NULL, 0, NULL, &excluidas);
    if (hijo->archivo == ARCHIVO_CERRADO) {
        hijo->estado = CERRADA;
        sim->excluidas_certificadas |= excluidas;
    }
}

/*
 * Recoge los hijos locales que terminaron y revisa su historial.
 * El estado de salida ayuda a distinguir corte, error y detención del ensayo.
 * Solo la evidencia guardada acredita un cierre comercial.
 */
static void recoger_tiendas(Simulacion *sim, int64_t ahora, bool saliendo) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        leer_hijo(sim, i, ahora);
        if (!hijo->pid) {
            continue;
        }
        int estado;
        pid_t termino = waitpid(hijo->pid, &estado, WNOHANG);
        if (termino == -1 && errno != EINTR) {
            hijo->pid = 0;
            sim->error = true;
        }
        if (termino <= 0) {
            continue;
        }
        leer_hijo(sim, i, ahora);
        close(hijo->salida);
        hijo->salida = -1;
        hijo->pid = 0;
        acreditar(sim, i);
        if (hijo->estado == CERRADA) {
            sim->progreso = ahora;
            printf("CIERRE_ACREDITADO %s\n", tienda(sim, i)->id);
            if (!saliendo && !hijo->matando && (!WIFEXITED(estado) || WEXITSTATUS(estado) != EXIT_SUCCESS)) {
                printf("ERROR_PROCESO %s estado=%d cierre_acreditado\n", tienda(sim, i)->id, estado);
                sim->error = true;
            }
        } else if ((WIFSIGNALED(estado) && WTERMSIG(estado) == SIGKILL && (hijo->matando || !saliendo)) ||
                   (!saliendo && WIFEXITED(estado) && WEXITSTATUS(estado) == EXIT_SUCCESS)) {
            if (!hijo->matando) {
                hijo->duracion = (int64_t)(sim->config.interrupciones.caida_min_s * 1000);
            }
            hijo->estado = INACTIVA;
            hijo->cortada = true;
            hijo->volver = ahora + hijo->duracion;
            hijo->revisar_baja = ahora;
            hijo->cursor_baja = 0;
            guardar(sim, "B %s %" PRId64 "\n", tienda(sim, i)->id, hijo->duracion);
            printf("CORTE_COMPROBADO %s\n", tienda(sim, i)->id);
        } else if (!saliendo) {
            hijo->estado = ERROR_TIENDA;
            sim->error = true;
            printf("ERROR_PROCESO %s estado=%d\n", tienda(sim, i)->id, estado);
        }
        hijo->matando = false;
    }
}

static void esperar_eventos(const Simulacion *sim, int ms) {
    struct pollfd fds[CONFIG_MAX_TIENDAS + TRABAJADORES];
    size_t n = 0;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].salida >= 0) {
            fds[n++] = (struct pollfd){sim->hijos[i].salida, POLLIN, 0};
        }
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        if (sim->trabajadores[i].pid && sim->trabajadores[i].trabajo != SIN_TRABAJO) {
            fds[n++] = (struct pollfd){sim->trabajadores[i].salida, POLLIN, 0};
        }
    }
    (void)poll(fds, n, ms);
}

// Las cadenas nuevas incorporan sus tiendas en orden. Las demás recuperan la lista por TCP.
static bool arrancar(Simulacion *sim) {
    const char *primera[CONFIG_MAX_CADENAS] = {0};
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        const ConfigTienda *t = tienda(sim, i);
        if (hijo->estado == CERRADA || hijo->cortada) {
            continue;
        }
        const char *entrada = t->cadena >= 0 && hijo->archivo == ARCHIVO_NUEVO ? primera[t->cadena] : NULL;
        if (!iniciar_tienda(sim, i, entrada)) {
            return false;
        }
        if (t->cadena < 0 || hijo->archivo != ARCHIVO_NUEVO) {
            continue;
        }
        int64_t vence = red_reloj_ms() + ARRANQUE_MS;
        while (!hijo->incorporada && !sim->error) {
            int64_t ahora = red_reloj_ms();
            if (ahora < 0 || ahora >= vence) {
                fprintf(stderr, "No se completó el alta inicial de %s.\n", t->id);
                return false;
            }
            recoger_tiendas(sim, ahora, false);
            if (!hijo->incorporada) {
                esperar_eventos(sim, 20);
            }
        }
        if (sim->error) {
            return false;
        }
        if (!primera[t->cadena]) {
            primera[t->cadena] = t->id;
        }
    }
    return true;
}

// El intervalo de compras avanza aunque falten respuestas de admisión.

static uint64_t sortear_importe(void) {
    uint64_t valor = 0;
    for (size_t i = 0; i < 5; ++i) {
        valor = (valor << 15) | (unsigned)(rand() & 0x7fff);
    }
    return valor & (uint64_t)INT64_MAX;
}

static void generar(Simulacion *sim, int64_t ahora) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas && !detener && !sim->error; ++i) {
        Hijo *hijo = &sim->hijos[i];
        const ConfigCompras *config = &tienda(sim, i)->compras;
        if (hijo->estado != ABIERTA || hijo->matando || ahora < hijo->proxima) {
            continue;
        }
        // El intervalo avanza aun si no queda sitio para generar otra compra.
        hijo->proxima = ahora + config->intervalo_ms;
        Compra *sitio = libre(sim, i);
        if (!sitio || compras_en_espera(sim, i) >= config->max_compras_en_espera) {
            if (!hijo->saturada) {
                printf("CAPACIDAD_COMPRAS %s\n", tienda(sim, i)->id);
            }
            hijo->saturada = true;
            continue;
        }
        hijo->saturada = false;
        if (sim->secuencia == INT64_MAX) {
            fprintf(stderr, "Se agotaron las claves de la simulación.\n");
            sim->error = true;
            return;
        }
        int64_t amplitud = config->importe_max_centavos - config->importe_min_centavos + 1;
        int64_t importe = config->importe_min_centavos + (int64_t)(sortear_importe() % (uint64_t)amplitud);
        int64_t secuencia = sim->secuencia + 1;
        // Se registra la intención antes de que la compra entre a la cola.
        if (!guardar(sim, "P %s %" PRId64 " %" PRId64 "\n", tienda(sim, i)->id, secuencia, importe)) {
            return;
        }
        *sitio = (Compra){.secuencia = secuencia, .importe = importe};
        sim->secuencia = secuencia;
        printf("COMPRA_GENERADA %s sim_%" PRId64 " %" PRId64 "\n", tienda(sim, i)->id, secuencia, importe);
    }
}

static bool entregar(Simulacion *sim, Trabajador *trabajador, Peticion *peticion, int trabajo) {
    if (!transferir(trabajador->entrada, peticion, sizeof *peticion, true)) {
        fprintf(stderr, "Se perdió un trabajador TCP de la simulación.\n");
        sim->error = true;
        return false;
    }
    trabajador->trabajo = trabajo;
    return true;
}

/*
 * Devuelve el índice de la compra más antigua que se puede enviar ahora
 * o SIN_TRABAJO. Un reintento que sigue en vuelo conserva su plaza.
 */
static int proxima_compra(const Simulacion *sim, size_t i, int64_t ahora) {
    const Hijo *hijo = &sim->hijos[i];
    if (hijo->estado != ABIERTA || hijo->matando) {
        return SIN_TRABAJO;
    }
    int en_vuelo = 0;
    for (int j = 0; j < CAPACIDAD; ++j) {
        en_vuelo += sim->compras[i][j].en_vuelo;
    }
    int limite = tienda(sim, i)->compras.max_solicitudes_en_vuelo;
    int elegida = SIN_TRABAJO;
    for (int j = 0; j < CAPACIDAD; ++j) {
        const Compra *compra = &sim->compras[i][j];
        if (compra->secuencia && !compra->en_envio && compra->reintentar <= ahora &&
            (compra->en_vuelo || en_vuelo < limite) &&
            (elegida == SIN_TRABAJO || compra->secuencia < sim->compras[i][elegida].secuencia)) {
            elegida = j;
        }
    }
    return elegida;
}

static void enviar_compras(Simulacion *sim, int64_t ahora) {
    size_t tiendas = sim->config.cantidad_tiendas;
    // El primer trabajador queda disponible para avisar bajas aunque haya compras pendientes.
    for (size_t w = 1; w < TRABAJADORES && !detener && !sim->error; ++w) {
        Trabajador *trabajador = &sim->trabajadores[w];
        if (trabajador->trabajo != SIN_TRABAJO) {
            continue;
        }
        for (size_t k = 0; k < tiendas; ++k) {
            size_t i = (sim->siguiente + k) % tiendas;
            int j = proxima_compra(sim, i, ahora);
            if (j == SIN_TRABAJO) {
                continue;
            }
            Compra *compra = &sim->compras[i][j];
            Peticion peticion = {.destino = i};
            char clave[CONFIG_MAX_ID + 1];
            clave_compra(clave, compra->secuencia);
            snprintf(peticion.linea, sizeof peticion.linea, "COMPRA %s %" PRId64 "\n", clave, compra->importe);
            // Si se interrumpe el primer envío, el diario conserva la clave y el intento.
            if (!compra->intentada && !guardar(sim, "E %" PRId64 "\n", compra->secuencia)) {
                return;
            }
            compra->intentada = true;
            if (entregar(sim, trabajador, &peticion, (int)i * CAPACIDAD + j)) {
                compra->en_vuelo = true;
                compra->en_envio = true;
                sim->siguiente = (i + 1) % tiendas;
                printf("COMPRA_INTENTO %s %s\n", tienda(sim, i)->id, clave);
            }
            break;
        }
    }
}

static bool admision(char *respuesta) {
    if (strncmp(respuesta, "ADMITIDA ", 9) != 0) {
        return false;
    }
    char *lf = strchr(respuesta, '\n');
    int64_t id;
    if (!lf || lf[1]) {
        return false;
    }
    *lf = '\0';
    return cifra(respuesta + 9, &id) && id > 0;
}

static void recibir_resultados(Simulacion *sim, int64_t ahora) {
    for (size_t w = 0; w < TRABAJADORES; ++w) {
        Trabajador *trabajador = &sim->trabajadores[w];
        if (!trabajador->pid || trabajador->trabajo == SIN_TRABAJO) {
            continue;
        }
        struct pollfd fd = {.fd = trabajador->salida, .events = POLLIN};
        if (poll(&fd, 1, 0) <= 0) {
            continue;
        }
        char respuesta[RED_MAX_LINEA + 1];
        int trabajo = trabajador->trabajo;
        trabajador->trabajo = SIN_TRABAJO;
        if (!transferir(fd.fd, respuesta, sizeof respuesta, false)) {
            sim->error = true;
            return;
        }
        if (trabajo == TRABAJO_BAJA) {
            continue;
        }
        // La división recupera la tienda y el resto recupera la plaza de la compra.
        size_t i = (size_t)trabajo / CAPACIDAD;
        Compra *compra = &sim->compras[i][trabajo % CAPACIDAD];
        compra->en_envio = false;
        if (admision(respuesta)) {
            printf("COMPRA_CONFIRMADA %s sim_%" PRId64 "\n", tienda(sim, i)->id, compra->secuencia);
            terminar_compra(sim, compra);
            sim->progreso = ahora;
        } else if (strcmp(respuesta, "CONFLICTO\n") == 0) {
            fprintf(stderr, "Conflicto de clave sim_%" PRId64 " en %s.\n", compra->secuencia, tienda(sim, i)->id);
            sim->error = true;
        } else if (strcmp(respuesta, "DESBORDAMIENTO\n") == 0) {
            printf("COMPRA_RECHAZADA %s sim_%" PRId64 " DESBORDAMIENTO\n", tienda(sim, i)->id, compra->secuencia);
            terminar_compra(sim, compra);
        } else {
            if (sim->hijos[i].pid && sim->hijos[i].estado != CERRADA &&
                (strcmp(respuesta, "CIERRE_EN_CURSO\n") == 0 || strcmp(respuesta, "CERRADA_META\n") == 0 ||
                 strcmp(respuesta, "ERROR_HISTORIAL\n") == 0)) {
                sim->hijos[i].estado = CERRANDO;
            }
            // Si la espera está llena, el intento incierto conserva su plaza en vuelo.
            if (compras_en_espera(sim, i) < tienda(sim, i)->compras.max_compras_en_espera) {
                compra->en_vuelo = false;
            }
            compra->reintentar = ahora + ESPERA_REINTENTO;
        }
    }
}

/*
 * Devuelve true si todos los cierres están acreditados y no quedan envíos en curso.
 * Una cadena puede acreditar el cierre para una tienda excluida. Las compras
 * cuyo resultado no se puede comprobar quedan pendientes en el diario.
 */
static bool todos_cerrados(Simulacion *sim) {
    bool todos = true;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if (!hijo->pid && hijo->estado != CERRADA &&
            (sim->excluidas_certificadas & (UINT64_C(1) << i))) {
            hijo->excluida = true;
            hijo->estado = CERRADA;
            printf("CIERRE_CADENA_ACREDITADO %s excluida\n", tienda(sim, i)->id);
        }
        if (hijo->estado != CERRADA) {
            todos = false;
            continue;
        }
        for (size_t j = 0; j < CAPACIDAD; ++j) {
            Compra *compra = &sim->compras[i][j];
            if (!compra->secuencia) {
                continue;
            }
            if (compra->en_envio) {
                todos = false;
                continue;
            }
        if (compra->reintentar == INT64_MAX) {
                continue;
            }
            char clave[CONFIG_MAX_ID + 1];
            bool registrada;
            clave_compra(clave, compra->secuencia);
            EstadoArchivoTienda archivo = tienda_inspeccionar_historial(&sim->config, tienda(sim, i),
                                                                         clave, compra->importe, &registrada, NULL);
            if (archivo == ARCHIVO_CERRADO || (hijo->excluida && archivo == ARCHIVO_ABIERTO)) {
                printf("COMPRA_RESUELTA_HISTORIAL %s %s %s\n", tienda(sim, i)->id, clave,
                       registrada ? "registrada" : "sin_registro");
                terminar_compra(sim, compra);
            } else {
                // Queda en el diario y se evita volver a consultar el mismo historial.
                compra->reintentar = INT64_MAX;
            }
        }
    }
    return todos;
}

// Cortes de procesos hijos y avisos de baja a las supervivientes

static double sorteo(void) {
    return rand() / ((double)RAND_MAX + 1.0);
}

static void sortear_cortes(Simulacion *sim) {
    ConfigInterrupciones config = sim->config.interrupciones;
    if (config.probabilidad_caida == 0) {
        return;
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas && !detener; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if ((hijo->estado != ABIERTA && hijo->estado != PAUSADA) || !hijo->pid || hijo->matando || hijo->cortada) {
            continue;
        }
        double valor = sorteo();
        if (valor >= config.probabilidad_caida) {
            continue;
        }
        hijo->duracion = (int64_t)(1000 * (config.caida_min_s + sorteo() * (config.caida_max_s - config.caida_min_s)));
        if (kill(hijo->pid, SIGKILL) == 0) {
            hijo->matando = true;
        }
    }
}

/*
 * Comprueba si la tienda ya está fuera de la lista de las supervivientes.
 * Si aún figura, intenta avisar juntas las bajas comprobadas para esa versión.
 * El envío del aviso por sí solo no confirma que se instaló la lista.
 */
static bool baja_instalada(Simulacion *sim, size_t i, int64_t ahora) {
    int grupo = tienda(sim, i)->cadena;
    Hijo *baja = &sim->hijos[i];
    if (grupo < 0) {
        return true;
    }
    uint64_t bajas = 0, vista = 0;
    uint32_t version = 0;
    const Hijo *referencia = NULL;
    bool instalada = true, vistas_estables = true;
    for (size_t j = 0; j < sim->config.cantidad_tiendas; ++j) {
        const Hijo *par = &sim->hijos[j];
        if (tienda(sim, j)->cadena != grupo) {
            continue;
        }
        if (!par->pid && par->cortada && par->estado != CERRADA) {
            bajas |= UINT64_C(1) << j;
        } else if (par->pid) {
            if (!par->version || (par->vista & (UINT64_C(1) << i))) {
                instalada = false;
            }
            if (!par->version || !par->vista || par->matando || par->estado == PAUSADA ||
                (referencia && (version != par->version || vista != par->vista ||
                                par->cantidad_vista != referencia->cantidad_vista ||
                                memcmp(par->orden, referencia->orden, par->cantidad_vista) != 0))) {
                vistas_estables = false;
            }
            vista = par->vista;
            version = par->version;
            referencia = par;
        }
    }
    if (!vistas_estables) {
        return false;
    }
    bajas &= vista;
    if (instalada) {
        return bajas == 0;
    }
    Trabajador *control = &sim->trabajadores[0];
    if (!control->pid || control->trabajo != SIN_TRABAJO || ahora < baja->revisar_baja) {
        return false;
    }
    for (size_t k = 0; k < sim->config.cantidad_tiendas; ++k) {
        size_t j = (baja->cursor_baja + k) % sim->config.cantidad_tiendas;
        Hijo *par = &sim->hijos[j];
        if (i == j || grupo != tienda(sim, j)->cadena || !par->pid) {
            continue;
        }
        if (par->primera == j || (bajas & (UINT64_C(1) << par->primera))) {
            // Si cayó la primera, solo la primera superviviente acepta la propuesta.
            Peticion peticion = {.destino = j};
            uint64_t posiciones = 0;
            // Convierte los bits por tienda en bits por posición de la lista.
            for (size_t pos = 0; pos < par->cantidad_vista; ++pos) {
                if (bajas & (UINT64_C(1) << par->orden[pos])) {
                    posiciones |= UINT64_C(1) << pos;
                }
            }
            snprintf(peticion.linea, sizeof peticion.linea, "BAJAS_ENSAYO %s %" PRIu32 " %" PRIu64 "\n",
                     cadena(sim, i), version, posiciones);
            entregar(sim, control, &peticion, TRABAJO_BAJA);
            baja->cursor_baja = (j + 1) % sim->config.cantidad_tiendas;
            baja->revisar_baja = ahora + ESPERA_REINTENTO;
            break;
        }
    }
    return instalada;
}

static void reiniciar_tienda(Simulacion *sim, size_t i) {
    printf("REINICIO %s\n", tienda(sim, i)->id);
    if (!iniciar_tienda(sim, i, NULL)) {
        sim->error = true;
    }
}

// Sin supervivientes, se espera a que puedan volver juntas las tiendas pendientes.
static void reiniciar_cadena_caida(Simulacion *sim, int grupo, int64_t ahora) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const Hijo *hijo = &sim->hijos[i];
        if (tienda(sim, i)->cadena == grupo && hijo->estado != CERRADA &&
            (!hijo->cortada || ahora < hijo->volver || hijo->archivo == ARCHIVO_BLOQUEADO)) {
            return;
        }
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas && !sim->error; ++i) {
        if (tienda(sim, i)->cadena == grupo && sim->hijos[i].estado != CERRADA) {
            reiniciar_tienda(sim, i);
        }
    }
}

static void tramitar_bajas(Simulacion *sim, int64_t ahora, bool permitir_retorno) {
    bool hay_supervivientes[CONFIG_MAX_CADENAS] = {false};
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        int grupo = tienda(sim, i)->cadena;
        if (grupo >= 0 && sim->hijos[i].pid) {
            hay_supervivientes[grupo] = true;
        }
    }
    bool reiniciar = permitir_retorno && sim->config.interrupciones.recuperacion_auto;
    for (size_t grupo = 0; reiniciar && grupo < sim->config.cantidad_cadenas && !sim->error; ++grupo) {
        if (!hay_supervivientes[grupo]) {
            reiniciar_cadena_caida(sim, (int)grupo, ahora);
        }
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if (hijo->pid || !hijo->cortada || hijo->estado == CERRADA) {
            continue;
        }
        if (!baja_instalada(sim, i, ahora)) {
            continue;
        }
        if (hijo->archivo == ARCHIVO_BLOQUEADO) {
            if (!hijo->bloqueo_reportado) {
                fprintf(stderr, "%s: el corte dejó un historial pendiente o inválido.\n", tienda(sim, i)->id);
                hijo->bloqueo_reportado = true;
            }
        } else if (reiniciar && ahora >= hijo->volver &&
                   (tienda(sim, i)->cadena < 0 || hay_supervivientes[tienda(sim, i)->cadena])) {
            reiniciar_tienda(sim, i);
        }
    }
}

static bool preparar_archivos(Simulacion *sim, bool reanudar) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const ConfigTienda *t = tienda(sim, i);
        if (!t->puerto || strncmp(t->direccion, "127.", 4) != 0 || !t->compras.intervalo_ms) {
            fprintf(stderr, "%s requiere un punto TCP de loopback y el objeto compras para --simular.\n", t->id);
            return false;
        }
        if (reanudar) {
            acreditar(sim, i);
        } else {
            struct stat datos;
            if (stat(t->historial, &datos) == 0) {
                if (!S_ISREG(datos.st_mode) || datos.st_size) {
                    fprintf(stderr, "%s: historial existente. Se conserva. Use --reanudar para continuar.\n", t->historial);
                    return false;
                }
            } else if (errno != ENOENT) {
                perror(t->historial);
                return false;
            }
        }
    }
    return true;
}

static bool comprobar_reanudacion(Simulacion *sim, bool reanudar) {
    if (!reanudar) {
        return true;
    }
    (void)todos_cerrados(sim);
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if (hijo->estado != CERRADA && !hijo->cortada && hijo->archivo == ARCHIVO_BLOQUEADO) {
            fprintf(stderr, "%s: historial inválido, incompatible o con cierre pendiente. Se conserva.\n",
                    tienda(sim, i)->historial);
            return false;
        }
    }
    return true;
}

static bool crear_directorios(const Simulacion *sim) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        char ruta[CONFIG_MAX_RUTA + 1];
        strcpy(ruta, tienda(sim, i)->historial);
        for (char *p = ruta + 1; (p = strchr(p, '/')); ++p) {
            *p = '\0';
            if (mkdir(ruta, 0700) == -1 && errno != EEXIST) {
                perror(ruta);
                return false;
            }
            *p = '/';
        }
    }
    return true;
}

static bool hay_trabajo(const Simulacion *sim) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const Hijo *hijo = &sim->hijos[i];
        if (hijo->pid || (hijo->cortada && hijo->estado != CERRADA && hijo->archivo != ARCHIVO_BLOQUEADO &&
                         sim->config.interrupciones.recuperacion_auto)) {
            return true;
        }
    }
    return false;
}

static int espera_siguiente(const Simulacion *sim, int64_t ahora) {
    int64_t espera = 20; // Vuelve a consultar waitpid aunque ningún pipe tenga datos.
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].estado == ABIERTA && sim->hijos[i].proxima > ahora &&
            sim->hijos[i].proxima - ahora < espera) {
            espera = sim->hijos[i].proxima - ahora;
        }
    }
    return (int)espera;
}

// Atención de eventos y detención del ensayo

/*
 * Alterna el seguimiento de hijos, los cortes y la generación de compras.
 * Devuelve true al acreditar los cierres. La detención, el error y la
 * falta de progreso interrumpen la atención con los pendientes disponibles.
 */
static bool ejecutar(Simulacion *sim) {
    int64_t ahora = red_reloj_ms();
    if (ahora < 0) {
        return false;
    }
    ConfigInterrupciones config = sim->config.interrupciones;
    int64_t intervalo = (int64_t)(config.evaluar_cada_s * 1000);
    if (!intervalo) {
        intervalo = 1;
    }
    int64_t evaluar = ahora + intervalo;
    int64_t max_sin_progreso = SIN_PROGRESO_MS + (int64_t)(config.caida_max_s * 1000);
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        sim->hijos[i].volver = ahora + sim->hijos[i].duracion;
        if (tienda(sim, i)->compras.intervalo_ms + (int64_t)SIN_PROGRESO_MS > max_sin_progreso) {
            max_sin_progreso = tienda(sim, i)->compras.intervalo_ms + (int64_t)SIN_PROGRESO_MS;
        }
    }
    sim->progreso = ahora;
    while (!detener && !sim->error && ahora >= 0) {
        recoger_tiendas(sim, ahora, false);
        recibir_resultados(sim, ahora);
        if (todos_cerrados(sim) && !sim->error) {
            puts("SIMULACION_CIERRES_ACREDITADOS");
            return true;
        }
        if (!hay_trabajo(sim) || ahora - sim->progreso >= max_sin_progreso) {
            puts("SIMULACION_SIN_PROGRESO");
            break;
        }
        tramitar_bajas(sim, ahora, !detener);
        if (ahora >= evaluar) {
            sortear_cortes(sim);
            evaluar = ahora + intervalo;
        }
        enviar_compras(sim, ahora);
        generar(sim, ahora);
        enviar_compras(sim, ahora);
        esperar_eventos(sim, espera_siguiente(sim, ahora));
        ahora = red_reloj_ms();
    }
    return false;
}

static bool cambio_en_curso(Simulacion *sim, int64_t ahora) {
    bool pendiente = false;
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        pendiente |= sim->trabajadores[i].pid && sim->trabajadores[i].trabajo != SIN_TRABAJO;
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if (hijo->matando || (hijo->pid && (hijo->estado == ARRANCANDO || hijo->estado == PAUSADA ||
                                           hijo->estado == CERRANDO)) ||
            (!hijo->pid && hijo->cortada && hijo->estado != CERRADA && !baja_instalada(sim, i, ahora))) {
            pendiente = true;
        }
    }
    return pendiente;
}

static void recoger_trabajadores(Simulacion *sim) {
    for (size_t w = 0; w < TRABAJADORES; ++w) {
        Trabajador *trabajador = &sim->trabajadores[w];
        if (trabajador->pid && waitpid(trabajador->pid, NULL, WNOHANG) == trabajador->pid) {
            trabajador->pid = 0;
            if (trabajador->salida >= 0) {
                close(trabajador->salida);
            }
            trabajador->salida = -1;
        }
    }
}

static bool quedan_hijos(const Simulacion *sim) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].pid) {
            return true;
        }
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        if (sim->trabajadores[i].pid) {
            return true;
        }
    }
    return false;
}

/*
 * En una detención normal da un plazo a los cambios ya iniciados.
 * Después cierra los pipes y pide terminar los hijos. Si alguno sigue
 * registrado al vencer el plazo, fuerza la salida e intenta recogerlo.
 */
static void limpiar(Simulacion *sim, bool estable) {
    int64_t ahora = red_reloj_ms();
    int64_t vence = ahora + DETENCION_MS;
    while (estable && ahora >= 0 && ahora < vence) {
        recoger_tiendas(sim, ahora, true);
        recibir_resultados(sim, ahora);
        if (!cambio_en_curso(sim, ahora)) {
            break;
        }
        esperar_eventos(sim, 20);
        ahora = red_reloj_ms();
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        if (sim->trabajadores[i].pid) {
            close(sim->trabajadores[i].entrada);
        }
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].pid && !sim->hijos[i].matando) {
            kill(sim->hijos[i].pid, SIGTERM);
        }
    }
    vence = red_reloj_ms() + DETENCION_MS;
    while (quedan_hijos(sim)) {
        ahora = red_reloj_ms();
        if (ahora < 0 || ahora >= vence) {
            break;
        }
        recoger_tiendas(sim, ahora, true);
        recibir_resultados(sim, ahora);
        recoger_trabajadores(sim);
        if (quedan_hijos(sim)) {
            esperar_eventos(sim, 20);
        }
    }
    // Después del plazo solo se fuerza la salida de los hijos que siguen registrados.
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].pid) {
            fprintf(stderr, "SALIDA_FORZADA %s\n", tienda(sim, i)->id);
            kill(sim->hijos[i].pid, SIGKILL);
        }
    }
    for (size_t i = 0; i < TRABAJADORES; ++i) {
        if (sim->trabajadores[i].pid) {
            kill(sim->trabajadores[i].pid, SIGKILL);
        }
    }
    vence = red_reloj_ms() + 1000;
    while (quedan_hijos(sim) && (ahora = red_reloj_ms()) >= 0 && ahora < vence) {
        recoger_tiendas(sim, ahora, true);
        recoger_trabajadores(sim);
        if (quedan_hijos(sim)) {
            esperar_eventos(sim, 20);
        }
    }
    if (quedan_hijos(sim)) {
        fprintf(stderr, "Quedaron hijos sin recoger después del plazo.\n");
        sim->error = true;
    }
}

static void informar_pendientes(const Simulacion *sim) {
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const Hijo *hijo = &sim->hijos[i];
        if (hijo->estado != CERRADA) {
            const char *causa = hijo->cortada ? "corte" : hijo->estado == CERRANDO ? "cierre_pendiente"
                              : hijo->estado == ERROR_TIENDA ? "error_proceso" : "sin_cierre_acreditado";
            printf("TIENDA_PENDIENTE %s %s\n", tienda(sim, i)->id, causa);
        }
        for (size_t j = 0; j < CAPACIDAD; ++j) {
            const Compra *compra = &sim->compras[i][j];
            if (compra->secuencia) {
                printf("COMPRA_PENDIENTE %s sim_%" PRId64 " %" PRId64 "\n",
                       tienda(sim, i)->id, compra->secuencia, compra->importe);
            }
        }
    }
}

// Importes del reporte en centavos, con agrupación de miles.
static void importe_reporte(FILE *archivo, int64_t centavos) {
    char entero[24];
    snprintf(entero, sizeof entero, "%" PRId64, centavos / 100);
    size_t largo = strlen(entero);
    for (size_t i = 0; i < largo; ++i) {
        if (i && (largo - i) % 3 == 0) {
            fputc('.', archivo);
        }
        fputc(entero[i], archivo);
    }
    fprintf(archivo, ",%02" PRId64, centavos % 100);
}

static void fila_reporte(FILE *archivo, const ConfigTienda *t, const Hijo *hijo,
                         const ResumenHistorial *r, bool excluida) {
    const char *estado = r->cerrada ? "Cerrada por meta"
                       : excluida ? "Excluida del cierre de cadena"
                       : !r->disponible ? "Historial no disponible"
                       : r->cierre_pendiente ? "Cierre pendiente"
                       : hijo->cortada ? "Interrumpida por corte"
                       : r->recuperacion_pendiente ? "Recuperación pendiente"
                       : "Sin cierre acreditado";
    fprintf(archivo, "| `%s` | %s | ", t->id, estado);
    if (r->disponible) {
        fprintf(archivo, "%zu | ", r->ventas_propias);
        importe_reporte(archivo, r->ingreso_propio);
    } else {
        fputs("No disponible | No disponible", archivo);
    }
    if (t->cadena != CONFIG_SIN_CADENA) {
        fputs(" | ", archivo);
        if (r->disponible) {
            importe_reporte(archivo, r->total_conocido);
        } else {
            fputs("No disponible", archivo);
        }
    }
    fputs(" |\n", archivo);
}

static void cadena_reporte(FILE *archivo, const Simulacion *sim, int grupo,
                           const ResumenHistorial resumen[CONFIG_MAX_TIENDAS]) {
    const EvidenciaCierre *certificado = NULL;
    bool discrepancia = false, incompleto = false;
    size_t copias = 0;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        const EvidenciaCierre *c = &resumen[i].certificado;
        if (tienda(sim, i)->cadena != grupo || !c->version) {
            continue;
        }
        if (!c->items) {
            incompleto = true;
        } else if (!certificado) {
            certificado = c;
        } else if (!tienda_certificados_coinciden(certificado, c)) {
            discrepancia = true;
        }
        copias += c->items != NULL;
    }
    uint64_t fuera = 0;
    if (certificado && !discrepancia && !incompleto) {
        for (size_t i = certificado->participantes; i < certificado->participantes + certificado->excluidas; ++i) {
            fuera |= UINT64_C(1) << certificado->miembros[i];
        }
    }

    fprintf(archivo, "\n## Cadena `%s`\n\n", sim->config.cadenas[grupo].id);
    fputs("| Tienda | Estado | Ventas propias | Ingreso propio | Total conocido |\n"
          "| --- | --- | ---: | ---: | ---: |\n", archivo);
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (tienda(sim, i)->cadena == grupo) {
            fila_reporte(archivo, tienda(sim, i), &sim->hijos[i], &resumen[i],
                         (fuera & (UINT64_C(1) << i)) != 0);
        }
    }
    if (discrepancia || incompleto || !certificado) {
        fputs("\n**Total certificado, ventas conciliadas, participantes y excluidas:** No disponible. ", archivo);
        fputs(discrepancia ? "Discrepancia entre los certificados leídos.\n"
              : incompleto ? "No se pudieron comparar todos los certificados leídos.\n"
              : "No hay un certificado definitivo disponible.\n", archivo);
        return;
    }
    fputs("\n- **Total certificado:** ", archivo);
    importe_reporte(archivo, certificado->total);
    fprintf(archivo, "\n- **Ventas conciliadas:** %zu\n- **Participantes:** ", certificado->cantidad);
    for (size_t i = 0; i < certificado->participantes; ++i) {
        fprintf(archivo, "%s`%s`", i ? ", " : "", tienda(sim, certificado->miembros[i])->id);
    }
    fputs("\n- **Excluidas:** ", archivo);
    if (!certificado->excluidas) {
        fputs("Ninguna", archivo);
    }
    for (size_t i = 0; i < certificado->excluidas; ++i) {
        fprintf(archivo, "%s`%s`", i ? ", " : "",
                tienda(sim, certificado->miembros[certificado->participantes + i])->id);
    }
    fprintf(archivo, "\n- **Copias disponibles del certificado:** %zu\n", copias);
}

static bool mismo_archivo(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static bool datos_directorio(char *ruta, struct stat *datos) {
    char *barra = strrchr(ruta, '/');
    if (!barra || barra == ruta) {
        return stat(barra ? "/" : ".", datos) == 0;
    }
    *barra = '\0';
    bool leido = stat(ruta, datos) == 0;
    *barra = '/';
    return leido;
}

// También protege rutas de historial aún ausentes bajo el mismo directorio.
static bool destino_reporte_seguro(const Simulacion *sim, char *ruta, const struct stat *diario) {
    struct stat destino, otro, padre;
    bool existe = lstat(ruta, &destino) == 0;
    if (existe ? !S_ISREG(destino.st_mode) : errno != ENOENT) {
        return false;
    }
    if ((existe && (mismo_archivo(&destino, diario) ||
                    (stat(sim->ruta, &otro) == 0 && mismo_archivo(&destino, &otro)))) ||
        !datos_directorio(ruta, &padre)) {
        return false;
    }
    const char *nombre = strrchr(ruta, '/');
    nombre = nombre ? nombre + 1 : ruta;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        char historial[CONFIG_MAX_RUTA + 1];
        strcpy(historial, tienda(sim, i)->historial);
        if (existe && stat(historial, &otro) == 0 && mismo_archivo(&destino, &otro)) {
            return false;
        }
        if (strcmp(nombre, strrchr(historial, '/') + 1) == 0 &&
            datos_directorio(historial, &otro) && mismo_archivo(&padre, &otro)) {
            return false;
        }
    }
    return true;
}

static void generar_reporte(const Simulacion *sim, int resultado, bool error, bool lectura_segura,
                           const struct stat *diario, const ResumenHistorial resumen[CONFIG_MAX_TIENDAS]) {
    size_t largo = strlen(sim->ruta) + sizeof ".resultados.md";
    char *ruta = malloc(largo);
    char *temporal = malloc(largo + sizeof ".XXXXXX");
    if (!ruta || !temporal) {
        fprintf(stderr, "No se pudo preparar la ruta del reporte.\n");
        free(ruta);
        free(temporal);
        return;
    }
    snprintf(ruta, largo, "%s.resultados.md", sim->ruta);
    snprintf(temporal, largo + sizeof ".XXXXXX", "%s.XXXXXX", ruta);
    int fd = mkstemp(temporal);
    FILE *archivo = fd == -1 ? NULL : fdopen(fd, "w");
    if (!archivo) {
        fprintf(stderr, "No se pudo abrir el reporte %s: %s\n", ruta, strerror(errno));
        if (fd != -1) {
            close(fd);
            unlink(temporal);
        }
        goto salir;
    }
    char fecha[32] = "No disponible";
    time_t ahora = time(NULL);
    struct tm utc;
    if (ahora != (time_t)-1 && gmtime_r(&ahora, &utc)) {
        strftime(fecha, sizeof fecha, "%Y-%m-%d %H:%M:%S UTC", &utc);
    }
    fprintf(archivo, "# Resultados de BlackOut\n\n**Fecha de generación:** %s\n\n**Resultado:** %s\n\n",
            fecha, resultado == EXIT_SUCCESS ? "Cierres acreditados por el ensayo."
                 : resultado == 130 ? "Parcial. Detención solicitada."
                 : error ? "Parcial. El ensayo terminó con un error."
                         : "Parcial. El ensayo terminó con cierres pendientes.");
    fputs("**Límite gubernamental:** ", archivo);
    importe_reporte(archivo, sim->config.limite_gubernamental_centavos);
    fputs("\n\nEl ingreso propio corresponde a la tienda. El total conocido es su copia del total de cadena. "
          "El total certificado se muestra una vez por cadena.\n", archivo);
    if (!lectura_segura) {
        fputs("\nLa lectura de historiales quedó pendiente porque faltaban procesos por recoger.\n", archivo);
    }
    for (size_t i = 0; i < sim->config.cantidad_cadenas; ++i) {
        cadena_reporte(archivo, sim, (int)i, resumen);
    }
    bool independientes = false;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (tienda(sim, i)->cadena == CONFIG_SIN_CADENA) {
            if (!independientes) {
                fputs("\n## Tiendas independientes\n\n| Tienda | Estado | Ventas propias | Ingreso propio |\n"
                      "| --- | --- | ---: | ---: |\n", archivo);
                independientes = true;
            }
            fila_reporte(archivo, tienda(sim, i), &sim->hijos[i], &resumen[i], false);
        }
    }
    fputs("\n## Compras pendientes\n\n", archivo);
    bool hay_pendientes = false;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        for (size_t j = 0; j < CAPACIDAD; ++j) {
            const Compra *compra = &sim->compras[i][j];
            if (compra->secuencia) {
                fprintf(archivo, "- `%s`, `sim_%" PRId64 "`, importe ", tienda(sim, i)->id, compra->secuencia);
                importe_reporte(archivo, compra->importe);
                fputs(compra->intentada ? " (resultado sin confirmar).\n" : " (sin envío registrado).\n", archivo);
                hay_pendientes = true;
            }
        }
    }
    if (!hay_pendientes) {
        fputs("No quedaron compras pendientes en el lanzador.\n", archivo);
    }
    bool confirmaciones = false;
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (resumen[i].ventas_sin_confirmacion) {
            if (!confirmaciones) {
                fputs("\n## Ventas sin confirmación completa\n\n"
                      "Según la última lista guardada en el historial de cada tienda.\n\n", archivo);
                confirmaciones = true;
            }
            fprintf(archivo, "- `%s`: %zu\n", tienda(sim, i)->id, resumen[i].ventas_sin_confirmacion);
        }
    }
    bool escrito = !ferror(archivo);
    if (fclose(archivo) == EOF || !escrito) {
        fprintf(stderr, "No se pudo escribir el reporte %s.\n", ruta);
    } else if (!diario || !destino_reporte_seguro(sim, ruta, diario)) {
        fprintf(stderr, "Se conservó %s: destino inseguro para el reporte.\n", ruta);
    } else if (rename(temporal, ruta) == -1) {
        fprintf(stderr, "No se pudo publicar el reporte %s: %s\n", ruta, strerror(errno));
    } else {
        printf("REPORTE_RESULTADOS %s\n", ruta);
        goto salir;
    }
    unlink(temporal);

salir:
    free(temporal);
    free(ruta);
}

int simulacion_ejecutar(const char *ruta, bool reanudar) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    Simulacion sim = {.ruta = ruta};
    char error[256];
    if (!configuracion_cargar(ruta, &sim.config, error, sizeof error)) {
        fprintf(stderr, "Configuración: %s\n", error);
        return EXIT_FAILURE;
    }
    if (sim.config.interrupciones.evaluar_cada_s > INT_MAX / 1000.0 ||
        sim.config.interrupciones.caida_max_s > INT_MAX / 1000.0) {
        fprintf(stderr, "Las esperas de interrupciones deben caber en INT_MAX milisegundos.\n");
        return EXIT_FAILURE;
    }
    for (size_t i = 0; i < sim.config.cantidad_tiendas; ++i) {
        sim.hijos[i].salida = -1;
    }
    if (!preparar_archivos(&sim, reanudar) || !abrir_diario(&sim, reanudar)) {
        if (sim.diario) {
            fclose(sim.diario);
        }
        return EXIT_FAILURE;
    }
    unsigned semilla = sim.config.semilla_configurada ? sim.config.semilla_azar : (unsigned)time(NULL);
    srand(semilla);
    printf("SEMILLA_SIMULACION %u\n", semilla);
    bool exito = false;
    if (!senales() || !comprobar_reanudacion(&sim, reanudar) || !crear_directorios(&sim) || !arrancar(&sim)) {
        fprintf(stderr, "No se pudo completar el arranque de la simulación.\n");
        sim.error = true;
    }
    for (size_t i = 0; i < TRABAJADORES && !sim.error && !detener; ++i) {
        if (!iniciar_trabajador(&sim, i)) {
            perror("trabajador de simulación");
            sim.error = true;
        }
    }
    if (!sim.error && !detener) {
        exito = ejecutar(&sim);
    }
    limpiar(&sim, detener && !sim.error);
    informar_pendientes(&sim);
    bool detencion_solicitada = detener;
    // Lee los historiales mientras el lanzador conserva el bloqueo del diario.
    bool lectura_segura = !quedan_hijos(&sim);
    ResumenHistorial resumen[CONFIG_MAX_TIENDAS] = {0};
    for (size_t i = 0; lectura_segura && i < sim.config.cantidad_tiendas; ++i) {
        resumen[i] = tienda_resumir_historial(&sim.config, tienda(&sim, i));
    }
    struct stat diario;
    bool diario_identificado = fstat(fileno(sim.diario), &diario) == 0;
    bool error_final = fclose(sim.diario) == EOF || sim.error;
    // 130 sigue la convención 128 + SIGINT (2) y también se usa para SIGTERM.
    int resultado = error_final ? EXIT_FAILURE : detencion_solicitada ? 130 : exito ? EXIT_SUCCESS : EXIT_FAILURE;
    if (resultado == 130) {
        puts("SIMULACION_DETENIDA");
    }
    generar_reporte(&sim, resultado, error_final, lectura_segura, diario_identificado ? &diario : NULL, resumen);
    for (size_t i = 0; i < sim.config.cantidad_tiendas; ++i) {
        free(resumen[i].certificado.items);
    }
    return resultado;
}
