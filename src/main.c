#include "configuracion.h"
#include "red.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 5) {
        fprintf(stderr, "Uso: %s config.json ID_TIENDA [ID_PAR IMPORTE_CENTAVOS]\n", argv[0]);
        return EXIT_FAILURE;
    }
    Configuracion config;
    char error[256];
    if (!configuracion_cargar(argv[1], &config, error, sizeof error)) {
        fprintf(stderr, "Configuración: %s\n", error);
        return EXIT_FAILURE;
    }
    const ConfigTienda *tienda = configuracion_buscar(&config, argv[2]);
    if (!tienda || tienda->cadena == CONFIG_SIN_CADENA) {
        fprintf(stderr, "ID de tienda de cadena inválido: %s\n", argv[2]);
        return EXIT_FAILURE;
    }

    const ConfigTienda *destino = NULL;
    intmax_t importe = 0;
    if (argc == 5) {
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
    ServidorRed servidor = {red_escuchar(tienda), &config, tienda};
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
