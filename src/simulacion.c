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
    CAPACIDAD = 4, RECEPCIONES_POR_TIENDA = 2, TRABAJADORES = 8,
    SIN_TRABAJO = -1, TRABAJO_BAJA = -2,
    ESPERA_REINTENTO = 250, SIN_PROGRESO_MS = 20000, ARRANQUE_MS = 8000,
    DETENCION_MS = 6000, LINEA_HIJO = CONFIG_MAX_TIENDAS * (CONFIG_MAX_ID + 1) + 128
};

typedef enum { INACTIVA, ARRANCANDO, ABIERTA, PAUSADA, CERRANDO, CERRADA, ERROR_TIENDA } Estado;

typedef struct {
    pid_t pid;
    int salida;
    Estado estado;
    EstadoArchivoTienda archivo;
    bool incorporada, cortada, matando, excluida, saturada, bloqueo_reportado;
    size_t primera, cursor_baja;
    uint64_t vista;
    int64_t proxima, duracion, volver, revisar_baja;
    char linea[LINEA_HIJO];
    size_t leidos;
} Hijo;

typedef struct {
    // La secuencia global también identifica la compra persistida en el diario.
    int64_t secuencia, importe, reintentar;
    bool en_envio;
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

// Diario del lanzador. Cada intención se completa antes de enviarla a una tienda.

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
    if (!guardar(sim, "F %" PRId64 "\n", compra->secuencia)) {
        return false;
    }
    *compra = (Compra){0};
    return true;
}

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
    if (strcmp(tipo, "F") == 0 && !segundo && cifra(primero, &dato) && dato > 0) {
        for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
            for (size_t j = 0; j < CAPACIDAD; ++j) {
                if (sim->compras[i][j].secuencia == dato) {
                    sim->compras[i][j] = (Compra){0};
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
    if (strcmp(tipo, "B") == 0 && segundo && !tercero && !hijo->cortada && cifra(segundo, &dato) &&
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
    }
    return true;

invalido:
    fprintf(stderr, "Diario incompleto o incompatible con el escenario. Se conserva.\n");
    return false;
}

// Procesos de tienda y trabajadores de compras.

static void cerrar_canales_heredados(const Simulacion *sim) {
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
        for (char *nombre = strtok_r(NULL, " ", &resto); nombre; nombre = strtok_r(NULL, " ", &resto)) {
            const ConfigTienda *par = configuracion_buscar(&sim->config, nombre);
            if (par && par->cadena == tienda(sim, i)->cadena) {
                size_t j = (size_t)(par - sim->config.tiendas);
                if (!vista) {
                    primera = j;
                }
                vista |= UINT64_C(1) << j;
            }
        }
        hijo->primera = primera;
        hijo->vista = vista;
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
        } else if (hijo->matando && WIFSIGNALED(estado) && WTERMSIG(estado) == SIGKILL) {
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

// Alta escalonada para historiales nuevos. Los existentes acuerdan la vista por TCP.
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

// Compras. Los vencimientos de cada tienda avanzan aunque queden ACK por recibir.

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
        hijo->proxima = ahora + config->intervalo_ms;
        Compra *sitio = libre(sim, i);
        if (!sitio) {
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

static int proxima_compra(const Simulacion *sim, size_t i, int64_t ahora) {
    const Hijo *hijo = &sim->hijos[i];
    if (hijo->estado != ABIERTA || hijo->matando) {
        return SIN_TRABAJO;
    }
    size_t enviadas = 0;
    int elegida = SIN_TRABAJO;
    for (int j = 0; j < CAPACIDAD; ++j) {
        const Compra *compra = &sim->compras[i][j];
        if (compra->en_envio) {
            ++enviadas;
        } else if (compra->secuencia && compra->reintentar <= ahora &&
                   (elegida == SIN_TRABAJO || compra->secuencia < sim->compras[i][elegida].secuencia)) {
            elegida = j;
        }
    }
    return enviadas < RECEPCIONES_POR_TIENDA ? elegida : SIN_TRABAJO;
}

static void enviar_compras(Simulacion *sim, int64_t ahora) {
    size_t tiendas = sim->config.cantidad_tiendas;
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
            if (entregar(sim, trabajador, &peticion, (int)i * CAPACIDAD + j)) {
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
            compra->reintentar = ahora + ESPERA_REINTENTO;
        }
    }
}

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
                compra->reintentar = INT64_MAX;
            }
        }
    }
    return todos;
}

// Cortes del ensayo. B acredita waitpid. El acuerdo real se hace entre las tiendas.

static double sorteo(void) {
    return rand() / ((double)RAND_MAX + 1.0);
}

static void sortear_cortes(Simulacion *sim) {
    ConfigInterrupciones config = sim->config.interrupciones;
    if (config.probabilidad_caida == 0) {
        return;
    }
    bool ocupada[CONFIG_MAX_CADENAS] = {false};
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        Hijo *hijo = &sim->hijos[i];
        if (tienda(sim, i)->cadena >= 0 && (hijo->cortada || hijo->matando || hijo->estado == ARRANCANDO)) {
            ocupada[tienda(sim, i)->cadena] = true;
        }
    }
    for (size_t i = 0; i < sim->config.cantidad_tiendas && !detener; ++i) {
        Hijo *hijo = &sim->hijos[i];
        int grupo = tienda(sim, i)->cadena;
        if (hijo->estado != ABIERTA || !hijo->pid || hijo->matando) {
            continue;
        }
        double valor = sorteo();
        if (valor >= config.probabilidad_caida) {
            continue;
        }
        if (grupo >= 0 && ocupada[grupo]) {
            printf("CORTE_OMITIDO_MODELO %s\n", tienda(sim, i)->id);
            continue;
        }
        hijo->duracion = (int64_t)(1000 * (config.caida_min_s + sorteo() * (config.caida_max_s - config.caida_min_s)));
        if (kill(hijo->pid, SIGKILL) == 0) {
            hijo->matando = true;
            if (grupo >= 0) {
                ocupada[grupo] = true;
            }
        }
    }
}

static bool baja_instalada(Simulacion *sim, size_t i, int64_t ahora) {
    int grupo = tienda(sim, i)->cadena;
    bool instalada = true;
    Hijo *baja = &sim->hijos[i];
    for (size_t k = 0; grupo >= 0 && k < sim->config.cantidad_tiendas; ++k) {
        size_t j = (baja->cursor_baja + k) % sim->config.cantidad_tiendas;
        Hijo *par = &sim->hijos[j];
        if (i == j || grupo != tienda(sim, j)->cadena || !par->pid) {
            continue;
        }
        if (!par->vista) {
            instalada = false;
            continue;
        }
        if (!(par->vista & (UINT64_C(1) << i))) {
            continue;
        }
        instalada = false;
        Trabajador *control = &sim->trabajadores[0];
        if (control->pid && control->trabajo == SIN_TRABAJO && ahora >= baja->revisar_baja &&
            (par->primera == j || par->primera == i)) {
            // Si cayó la primera, solo su sucesora en la vista aceptará la propuesta.
            Peticion peticion = {.destino = j};
            snprintf(peticion.linea, sizeof peticion.linea, "BAJA_ENSAYO %s %s\n", cadena(sim, i), tienda(sim, i)->id);
            entregar(sim, control, &peticion, TRABAJO_BAJA);
            baja->cursor_baja = (j + 1) % sim->config.cantidad_tiendas;
            baja->revisar_baja = ahora + ESPERA_REINTENTO;
        }
    }
    return instalada;
}

static void tramitar_bajas(Simulacion *sim, int64_t ahora, bool permitir_retorno) {
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
        } else if (permitir_retorno && sim->config.interrupciones.recuperacion_auto && ahora >= hijo->volver) {
            printf("REINICIO %s\n", tienda(sim, i)->id);
            if (!iniciar_tienda(sim, i, NULL)) {
                sim->error = true;
            }
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
    int64_t espera = 20;  // waitpid se comprueba también si no hay actividad en pipes.
    for (size_t i = 0; i < sim->config.cantidad_tiendas; ++i) {
        if (sim->hijos[i].estado == ABIERTA && sim->hijos[i].proxima > ahora &&
            sim->hijos[i].proxima - ahora < espera) {
            espera = sim->hijos[i].proxima - ahora;
        }
    }
    return (int)espera;
}

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

/* Primero deja estabilizar cambios ya iniciados. Luego solicita y acota la salida. */
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
    if (fclose(sim.diario) == EOF || sim.error) {
        return EXIT_FAILURE;
    }
    if (detener) {
        puts("SIMULACION_DETENIDA");
        return 130;
    }
    return exito ? EXIT_SUCCESS : EXIT_FAILURE;
}
