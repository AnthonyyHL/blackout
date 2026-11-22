#ifndef RED_H
#define RED_H

#include "configuracion.h"

enum { RED_MAX_LINEA = 128, RED_PLAZO_MS = 3000, RED_RECEPTORES = 2 };

typedef struct {
    int escucha;
    const Configuracion *config;
    const ConfigTienda *tienda;
} ServidorRed;

int red_escuchar(const ConfigTienda *tienda);
void *red_recibir(void *contexto);
bool red_enviar_prueba(const Configuracion *config, const ConfigTienda *origen,
                      const ConfigTienda *destino, int64_t importe,
                      char *error, size_t capacidad_error);

#endif
