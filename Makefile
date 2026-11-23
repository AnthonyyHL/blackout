CC = gcc
CPPFLAGS += -Isrc -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic

OBJETO = build/tienda_local.o
OBJETO_PRUEBA = build/tienda_local_prueba.o
PRUEBAS = bin/tienda_local_test
RED = build/configuracion.o build/red.o
CADENA = build/tienda_cadena.o build/recuperacion.o build/cierre.o
MONITOR = build/monitor.o $(OBJETO)
SIMULACION = build/simulacion.o
PRUEBAS_ETAPA_3 = bin/configuracion_test bin/red_test

.PHONY: all test
all: bin/blackout

test: all $(PRUEBAS) $(PRUEBAS_ETAPA_3) bin/tienda_cadena_test
	./$(PRUEBAS)
	./bin/configuracion_test
	./bin/red_test
	./bin/tienda_cadena_test

$(OBJETO): src/tienda_local.c src/tienda_local.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(OBJETO_PRUEBA): src/tienda_local.c src/tienda_local.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dread=prueba_leer -Dwrite=prueba_escribir -c $< -o $@

$(PRUEBAS): tests/tienda_local_test.c src/tienda_local.h $(OBJETO_PRUEBA) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(OBJETO_PRUEBA) $(LDLIBS) -o $@

build/configuracion.o: src/configuracion.c src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/red.o: src/red.c src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/red_prueba.o: src/red.c src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dconnect=prueba_conectar -Dpoll=prueba_esperar \
		-Dsend=prueba_enviar_bytes -c $< -o $@

build/recuperacion.o: src/recuperacion.c src/recuperacion.h src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/cierre.o: src/cierre.c src/cierre.h src/recuperacion.h src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/monitor.o: src/monitor.c src/monitor.h src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

build/tienda_cadena.o: src/tienda_cadena.c src/tienda_cadena.h src/cierre.h src/recuperacion.h src/red.h src/configuracion.h src/monitor.h src/tienda_local.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@

build/simulacion.o: src/simulacion.c src/simulacion.h src/red.h src/configuracion.h src/tienda_cadena.h src/cierre.h src/recuperacion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

bin/blackout: src/main.c src/configuracion.h src/tienda_cadena.h src/monitor.h src/simulacion.h $(RED) $(CADENA) $(MONITOR) $(SIMULACION) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(filter %.o,$^) -pthread $(LDLIBS) -o $@

bin/configuracion_test: tests/configuracion_test.c src/configuracion.h build/configuracion.o | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/configuracion.o $(LDLIBS) -o $@

bin/red_test: tests/red_test.c src/red.h src/configuracion.h build/configuracion.o build/red_prueba.o | bin bin/blackout
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< build/configuracion.o build/red_prueba.o $(LDLIBS) -o $@

build/fallos_cadena.o: tests/tienda_cadena_test.c src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSIMULAR_FALLOS -c $< -o $@

build/red_cadena_prueba.o: src/red.c src/red.h src/configuracion.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dsend=prueba_perder_ack -c $< -o $@

build/tienda_cadena_prueba.o: src/tienda_cadena.c src/tienda_cadena.h src/cierre.h src/recuperacion.h src/red.h src/configuracion.h src/monitor.h src/tienda_local.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Dread=prueba_leer_pipe -Dwrite=prueba_escribir_pipe -pthread -c $< -o $@

bin/blackout_cadena_prueba: src/main.c src/tienda_cadena.h src/red.h src/monitor.h src/simulacion.h build/configuracion.o \
                           build/red_cadena_prueba.o build/tienda_cadena_prueba.o build/recuperacion.o build/cierre.o \
                           build/fallos_cadena.o $(MONITOR) $(SIMULACION) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(filter %.o,$^) -pthread $(LDLIBS) -o $@

bin/tienda_cadena_test: tests/tienda_cadena_test.c src/red.h $(RED) bin/blackout bin/blackout_cadena_prueba | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(RED) -pthread $(LDLIBS) -o $@

build bin:
	mkdir -p $@
