#include "configuracion.h"
#include "monitor.h"
#include "red.h"
#include "simulacion.h"
#include "tienda_cadena.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc >= 3 && strcmp(argv[2], "--simular") == 0) {
        bool reanudar = argc == 4 && strcmp(argv[3], "--reanudar") == 0;
        if (argc == 3 || reanudar) {
            return simulacion_ejecutar(argv[1], reanudar);
        }
        fprintf(stderr, "Uso: %s config.json --simular [--reanudar]\n", argv[0]);
        return EXIT_FAILURE;
    }
    bool modo_cadena = argc >= 4 && strcmp(argv[3], "--cadena") == 0;
    bool solo_cierre = argc == 4 && strcmp(argv[3], "--consultar-cierre") == 0;
    bool modo_independiente = argc == 4 && strcmp(argv[3], "--independiente") == 0;
    bool modo_monitor = argc >= 4 && strcmp(argv[3], "--monitor") == 0;
    bool ventas = false, historico = false;
    int opcion;
    for (opcion = 4; modo_monitor && opcion < argc; ++opcion) {
        if (!ventas && strcmp(argv[opcion], "--ventas") == 0) {
            ventas = true;
        } else if (!historico && strcmp(argv[opcion], "--historial") == 0) {
            historico = true;
        } else {
            break;
        }
    }
    bool uso_valido = modo_monitor ? opcion == argc
                     : argc == 3 || argc == 5 || (argc == 4 && (modo_cadena || solo_cierre || modo_independiente));
    if (!uso_valido) {
        fprintf(stderr, "Uso: %s config.json ID_TIENDA [ID_PAR IMPORTE_CENTAVOS | --cadena [PAR_INICIAL] | "
                        "--independiente | --monitor [--ventas] [--historial] | --consultar-cierre]\n"
                        "     %s config.json --simular [--reanudar]\n", argv[0], argv[0]);
        return EXIT_FAILURE;
    }
    Configuracion config;
    char error[256];
    if (!configuracion_cargar(argv[1], &config, error, sizeof error)) {
        fprintf(stderr, "Configuración: %s\n", error);
        return EXIT_FAILURE;
    }
    const ConfigTienda *tienda = configuracion_buscar(&config, argv[2]);
    if (!tienda || (!modo_monitor && (tienda->cadena == CONFIG_SIN_CADENA) != modo_independiente)) {
        fprintf(stderr, "ID de tienda inválido para este modo: %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    if (modo_independiente && !tienda->puerto) {
        fprintf(stderr, "%s requiere direccion y puerto para escuchar.\n", tienda->id);
        return EXIT_FAILURE;
    }

    const ConfigTienda *destino = NULL;
    intmax_t importe = 0;
    if (modo_cadena && argc == 5) {
        destino = configuracion_buscar(&config, argv[4]);
        if (!destino || destino == tienda || destino->cadena != tienda->cadena) {
            fprintf(stderr, "Par inicial de cadena inválido\n");
            return EXIT_FAILURE;
        }
    } else if (!modo_monitor && argc == 5) {
        destino = configuracion_buscar(&config, argv[3]);
        char *fin;
        errno = 0;
        importe = strtoimax(argv[4], &fin, 10);
        if (!destino || destino == tienda || destino->cadena != tienda->cadena ||
            errno == ERANGE || importe <= 0 || importe > INT64_MAX ||
            argv[4][0] < '1' || argv[4][0] > '9' || *fin != '\0') {
            fprintf(stderr, "Destino o importe en centavos inválido\n");
            return EXIT_FAILURE;
        }
    }

    // Un cierre remoto se procesa como error de I/O en el hilo que envía.
    struct sigaction accion = {.sa_handler = SIG_IGN};
    if (sigemptyset(&accion.sa_mask) == -1 || sigaction(SIGPIPE, &accion, NULL) == -1) {
        perror("SIGPIPE");
        return EXIT_FAILURE;
    }
    if (modo_cadena) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return tienda_cadena_ejecutar(&config, tienda, destino);
    }
    if (solo_cierre) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return tienda_cadena_consultar_cierre(&config, tienda);
    }
    if (modo_monitor) {
        return historico ? tienda_consultar_historial(&config, tienda, ventas)
                         : monitor_mostrar(&config, tienda, ventas, NULL, NULL);
    }
    if (modo_independiente) {
        setvbuf(stdout, NULL, _IOLBF, 0);
        return tienda_independiente_ejecutar(&config, tienda);
    }
    ServidorRed servidor = {.escucha = red_escuchar(tienda), .config = &config, .tienda = tienda};
    if (servidor.escucha == -1) {
        fprintf(stderr, "%s, %s:%u: %s\n", tienda->id, tienda->direccion,
                (unsigned)tienda->puerto, strerror(errno));
        return EXIT_FAILURE;
    }

    pthread_t receptores[RED_RECEPTORES];
    for (size_t i = 0; i < RED_RECEPTORES; ++i) {
        int estado = pthread_create(&receptores[i], NULL, red_recibir, &servidor);
        if (estado != 0) {
            fprintf(stderr, "pthread_create: %s\n", strerror(estado));
            exit(EXIT_FAILURE);
        }
    }
    printf("ESCUCHANDO %s %s:%u\n", tienda->id, tienda->direccion, (unsigned)tienda->puerto);
    fflush(stdout);
    if (destino) {
        if (red_enviar_prueba(&config, tienda, destino, (int64_t)importe, error, sizeof error)) {
            printf("RECIBIDO %s\n", destino->id);
            fflush(stdout);
        } else {
            fprintf(stderr, "%s\n", error);
        }
    }
    for (size_t i = 0; i < RED_RECEPTORES; ++i) {
        pthread_join(receptores[i], NULL);
    }
    close(servidor.escucha);
    return EXIT_SUCCESS;
}
