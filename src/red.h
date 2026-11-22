#ifndef RED_H
#define RED_H

#include "configuracion.h"

enum { RED_MAX_LINEA = 128, RED_PLAZO_MS = 3000, RED_RECEPTORES = 2 };

typedef struct {
    int escucha;
    const Configuracion *config;
    const ConfigTienda *tienda;
    int fin; // Extremo de lectura del pipe que avisa al receptor al cerrarse.
    // Recibe una línea completa y dispone de RED_MAX_LINEA + 1 bytes para la respuesta.
    bool (*atender_local)(void *canal, const char *linea, char *respuesta);
    void *canal;
} ServidorRed;

int red_escuchar(const ConfigTienda *tienda);
bool red_no_bloqueante(int fd);
int64_t red_reloj_ms(void);
bool red_fin_solicitado(int fd);
void *red_recibir(void *contexto);
/*
 * Envía una línea terminada en LF y espera otra con plazos acotados.
 * Devuelve true si recibe una línea completa en respuesta, que admite
 * RED_MAX_LINEA + 1 bytes. Si falla, devuelve false y escribe el error.
 */
bool red_intercambiar(const ConfigTienda *destino, const char *linea, char *respuesta,
                      char *error, size_t capacidad_error);
bool red_enviar_prueba(const Configuracion *config, const ConfigTienda *origen,
                      const ConfigTienda *destino, int64_t importe,
                      char *error, size_t capacidad_error);

#endif
