# BlackOut

Proyecto de Sistemas Operativos · Anthony Herrera León

BlackOut simula tiendas de cadena e independientes. Cada tienda lleva su propio historial. Las integrantes de una cadena intercambian ventas individuales por TCP y calculan localmente el total que conocen.

## Compilar en Ubuntu

Desde la raíz del proyecto, instala GNU GCC y GNU Make si faltan. El programa usa C11, Pthreads y las interfaces POSIX de Linux.

```sh
sudo apt update
sudo apt install build-essential
gcc --version
make CC=gcc
```

El ejecutable se genera en `bin/blackout` y los objetos en `build/`. Las suites completas pasaron en Ubuntu 24.04.5 LTS (`aarch64`) con GNU GCC 13.3.0. En ese entorno también se verificaron los cortes superpuestos, la detención y reanudación, y el monitoreo en vivo e histórico.

## Ejecutar la simulación

El programa arranca ejecutando el siguiente comando:

```sh
./bin/blackout config.json --simular
```

El ejemplo arranca dos cadenas y una tienda independiente. El lanzador genera compras, simula cortes según la configuración y recoge a sus procesos hijos. Las tiendas intercambian ventas y acuerdan los cierres entre ellas. Cada tienda de cadena aplica sus ventas primero y luego las difunde según el orden de incorporación de las demás participantes.

`SIMULACION_CIERRES_ACREDITADOS` indica que el ensayo acreditó los cierres. Si se agotan las posibilidades de avance o vence el plazo sin progreso, el lanzador informa las tiendas pendientes. Un ensayo que termina con pendientes carece de un cierre acreditado para esas tiendas.

## Configuración

Edita `config.json` antes de arrancar. Mantén las cantidades de tiendas en concordancia con sus listas. Cada tienda necesita un ID, un punto TCP de loopback y una ruta de historial propios. Los puntos TCP y las rutas deben ser distintos entre tiendas. Las rutas relativas se resuelven desde el directorio del JSON.

| Clave | Uso |
| --- | --- |
| `limite_gubernamental_centavos` | Entero positivo en la raíz. Cada cadena lo compara con su total y cada independiente con su ingreso propio. El ejemplo usa 100000 centavos como valor ilustrativo. |
| `compras.importe_min_centavos`, `compras.importe_max_centavos` | Enteros positivos por tienda, con mínimo menor o igual al máximo. |
| `compras.intervalo_ms` | Intervalo de generación por tienda, en milisegundos enteros positivos. |
| `compras.max_solicitudes_en_vuelo` | De 1 a 7 por tienda. Cuenta las solicitudes cuyo resultado se espera. |
| `compras.max_compras_en_espera` | De 1 a 32 por tienda. Cuenta las compras que esperan su primer envío o un reintento. |
| `interrupciones.probabilidad_caida` | Probabilidad entre 0 y 1 por tienda elegible en cada evaluación. El valor 0 desactiva los cortes. |
| `interrupciones.evaluar_cada_s` | Intervalo positivo entre evaluaciones, en segundos. |
| `interrupciones.caida_min_s`, `interrupciones.caida_max_s` | Duración del corte en segundos, con `0 ≤ mínimo ≤ máximo`. |
| `interrupciones.recuperacion_auto` | Con `true`, el lanzador intenta reiniciar una tienda cortada cuando corresponde. Con `false`, las bajas comprobadas permanecen fuera del ensayo. |
| `semilla_azar` | Semilla para los sorteos. Con las mismas entradas, orden y biblioteca de C se pueden repetir los sorteos. La planificación concurrente puede variar. |

Ambas capacidades de compra valen 2 si se omiten. Si se llena la espera, se omite esa oportunidad de generación. Las compras existentes conservan su clave e importe al reintentar. Las ventas pendientes de difusión se gestionan por separado dentro de cada tienda. Los tiempos de corte deben caber en `INT_MAX` milisegundos una vez convertidos.

## Consultar una tienda

Desde otra terminal, en el mismo directorio, consulta una tienda de cadena (`A`) o una independiente (`F`).

```sh
./bin/blackout config.json A --monitor --ventas
./bin/blackout config.json F --monitor
```

`ACTUAL` identifica la observación por TCP. La respuesta incluye el estado comercial y los ingresos disponibles. `--ventas` enumera las ventas propias. `INACCESIBLE` indica que la consulta no obtuvo respuesta. El estado comercial solo se acredita con una respuesta o con evidencia guardada que se pueda consultar.

Con la tienda detenida, consulta su historial íntegro y accesible en modo de solo lectura.

```sh
./bin/blackout config.json A --monitor --historial --ventas
./bin/blackout config.json F --monitor --historial
```

`HISTORICO` identifica la fuente y la hora de consulta. Cuando existe un certificado de cadena, su total aparece por separado del total conocido en el historial de la tienda.

## Detener, reanudar o preparar otro ensayo

Pulsa **Ctrl+C** en la terminal del lanzador para detener la generación y recoger las tiendas. Esta detención conserva las claves de compras inciertas y es reanudable. Un cierre por límite gubernamental es definitivo.

```sh
./bin/blackout config.json --simular --reanudar
```

Los historiales se guardan en las rutas `historial` del JSON, bajo `datos/` en el ejemplo. El diario se guarda junto al JSON como `config.json.simulacion`. Para reanudar, conserva los historiales y el diario, y usa las mismas identidades, direcciones, rutas y límite común. Si un registro está incompleto o la evidencia de un cambio o cierre es insuficiente, el programa conserva los archivos y puede rechazar la reanudación.

El modo nuevo rechaza historiales o un diario anteriores si contienen datos. Para otro ensayo, copia `config.json` a un directorio nuevo sin los historiales ni el diario. Las rutas de historial relativas del ejemplo se resolverán en ese directorio. Inicia desde la raíz con `./bin/blackout ruta-del-ensayo/config.json --simular`. Conserva el directorio anterior si vas a reanudarlo o consultarlo.

## Alcance de la demostración

- Cada tienda es un proceso. El lanzador usa `waitpid` para comprobar la terminación de sus hijos locales y las supervivientes acuerdan su lista activa. Un vencimiento TCP solo indica inaccesibilidad. La demostración usa direcciones conocidas y comunicación entre supervivientes.
- La simulación termina el proceso de tienda al cerrar por meta. El computador sigue encendido. La consulta en vivo necesita un servicio accesible y la histórica requiere un archivo accesible.
- La recuperación se verificó con historiales íntegros. Las particiones de red, las bajas remotas y la durabilidad frente a pérdida eléctrica quedan fuera de la verificación.
- El cierre de cadena certifica las ventas admitidas que se pudieron conciliar. Si una venta aparece después en el historial de una tienda excluida, se conserva como no incluida y el cierre permanece definitivo.
