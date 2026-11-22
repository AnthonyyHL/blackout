#ifndef CONFIGURACION_H
#define CONFIGURACION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    CONFIG_MAX_CADENAS = 32,
    CONFIG_MAX_TIENDAS = 64,
    CONFIG_MAX_ID = 32,
    CONFIG_MAX_RUTA = 512,
    CONFIG_MAX_ARCHIVO = 65536,
    CONFIG_SIN_CADENA = -1
};

typedef struct {
    char id[CONFIG_MAX_ID + 1];
    int64_t meta;
} ConfigCadena;

typedef struct {
    char id[CONFIG_MAX_ID + 1];
    char direccion[16];
    uint16_t puerto;
    char historial[CONFIG_MAX_RUTA + 1];
    int cadena;
    int64_t meta;
} ConfigTienda;

typedef struct {
    ConfigCadena cadenas[CONFIG_MAX_CADENAS];
    ConfigTienda tiendas[CONFIG_MAX_TIENDAS];
    size_t cantidad_cadenas;
    size_t cantidad_tiendas;
} Configuracion;

bool configuracion_cargar(const char *ruta, Configuracion *config,
                          char *error, size_t capacidad_error);
const ConfigTienda *configuracion_buscar(const Configuracion *config, const char *id);

#endif
