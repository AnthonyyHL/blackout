#include "configuracion.h"
#include "monitor.h"
#include "simulacion.h"
#include "tienda_cadena.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    bool uso_valido = (modo_monitor && opcion == argc) || (modo_cadena && argc <= 5) ||
                      solo_cierre || modo_independiente;
    if (!uso_valido) {
        fprintf(stderr, "Uso: %s config.json ID_TIENDA {--cadena [PAR_INICIAL] | "
                        "--independiente | --monitor [--ventas] [--historial] | --consultar-cierre}\n"
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
    if (modo_cadena && argc == 5) {
        destino = configuracion_buscar(&config, argv[4]);
        if (!destino || destino == tienda || destino->cadena != tienda->cadena) {
            fprintf(stderr, "Par inicial de cadena inválido\n");
            return EXIT_FAILURE;
        }
    }

    // Un cierre remoto se procesa como error de I/O en el hilo que envía.
    struct sigaction accion = {.sa_handler = SIG_IGN};
    if (sigemptyset(&accion.sa_mask) == -1 || sigaction(SIGPIPE, &accion, NULL) == -1) {
        perror("SIGPIPE");
        return EXIT_FAILURE;
    }
    if (modo_monitor) {
        return historico ? tienda_consultar_historial(&config, tienda, ventas)
                         : monitor_mostrar(&config, tienda, ventas, NULL, NULL);
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (modo_cadena) {
        return tienda_cadena_ejecutar(&config, tienda, destino);
    }
    if (solo_cierre) {
        return tienda_cadena_consultar_cierre(&config, tienda);
    }
    return tienda_independiente_ejecutar(&config, tienda);
}
