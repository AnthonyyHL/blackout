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
    CONFIG_MAX_SOLICITUDES_EN_VUELO = 7,
    CONFIG_MAX_COMPRAS_EN_ESPERA = 32,
    CONFIG_SIN_CADENA = -1
};

typedef struct {
    char id[CONFIG_MAX_ID + 1];
} ConfigCadena;

typedef struct {
    int64_t importe_min_centavos, importe_max_centavos;
    int intervalo_ms;
    int max_solicitudes_en_vuelo, max_compras_en_espera;
} ConfigCompras;

typedef struct {
    double probabilidad_caida, evaluar_cada_s, caida_min_s, caida_max_s;
    bool recuperacion_auto;
} ConfigInterrupciones;

typedef struct {
    char id[CONFIG_MAX_ID + 1];
    char direccion[16];
    uint16_t puerto;
    char historial[CONFIG_MAX_RUTA + 1];
    int cadena; // Índice en cadenas. CONFIG_SIN_CADENA identifica una independiente.
    ConfigCompras compras;
} ConfigTienda;

typedef struct {
    ConfigCadena cadenas[CONFIG_MAX_CADENAS];
    ConfigTienda tiendas[CONFIG_MAX_TIENDAS];
    size_t cantidad_cadenas;
    size_t cantidad_tiendas;
    int64_t limite_gubernamental_centavos;
    ConfigInterrupciones interrupciones;
    unsigned semilla_azar;
    bool semilla_configurada; // Distingue null de una semilla explícita, incluso cero.
} Configuracion;

bool configuracion_cargar(const char *ruta, Configuracion *config,
                          char *error, size_t capacidad_error);
const ConfigTienda *configuracion_buscar(const Configuracion *config, const char *id);

#endif
