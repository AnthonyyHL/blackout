#ifndef RED_H
#define RED_H

#include "configuracion.h"

// El máximo incluye el salto de línea. Los plazos se miden en milisegundos.
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
// El socket de escucha debe ser no bloqueante y el canal local debe estar listo.
void red_recibir(const ServidorRed *servidor);
/*
 * Envía una línea con salto de línea al final y espera otra con plazos acotados.
 * Devuelve true al recibir una línea completa en el búfer de respuesta
 * de RED_MAX_LINEA + 1 bytes. El llamador interpreta el contenido.
 * Si falla, devuelve false y escribe el error.
 */
bool red_intercambiar(const ConfigTienda *destino, const char *linea, char *respuesta,
                      char *error, size_t capacidad_error);
bool red_intercambiar_hasta(const ConfigTienda *destino, const char *linea, char *respuesta,
                            char *error, size_t capacidad_error, int64_t limite);

// Forma la solicitud y la envía antes del plazo si el canal de trabajo sigue abierto.
bool red_pedir(const ConfigTienda *par, int fin, int64_t vence, char *respuesta,
               const char *formato, ...);

#endif
