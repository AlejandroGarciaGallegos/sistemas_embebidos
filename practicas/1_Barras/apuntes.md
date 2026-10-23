# Apuntes: Barras - scheduling multicore con FreeRTOS

## El experimento

Tres tareas (`vTask1`, `vTask2`, `vTask3`) hacen lo mismo: un bloque de
computo pesado (`trabajar()`) y despues avanzan un pixel su barra en el
OLED. Ninguna se bloquea nunca por si sola.

La asignacion de cores es a proposito **desigual**:

| Tarea  | Core | Comparte core con |
|--------|------|--------------------|
| vTask1 | 0    | nadie              |
| vTask2 | 1    | vTask3             |
| vTask3 | 1    | vTask2             |

Se fija con `xTaskCreatePinnedToCore(..., core)`. Por eso la barra 1
avanza mas rapido que las otras dos: tiene un core para ella sola,
mientras 2 y 3 se reparten el mismo core.

## xTaskCreatePinnedToCore: los parametros

```c
BaseType_t xTaskCreatePinnedToCore( TaskFunction_t pxTaskCode,
                                    const char * const pcName,
                                    const uint32_t usStackDepth,
                                    void * const pvParameters,
                                    UBaseType_t uxPriority,
                                    TaskHandle_t * const pxCreatedTask,
                                    const BaseType_t xCoreID )
```

Con la llamada real del programa como referencia:

```c
xTaskCreatePinnedToCore(vTask1, "vTask1", STACK_SIZE, NULL, 1, NULL, 0);
```

| Parametro       | Que es                                                                 | En esta llamada |
|-----------------|-------------------------------------------------------------------------|------------------|
| `pxTaskCode`    | La funcion que la tarea va a correr (su bucle `while(1)`).             | `vTask1` |
| `pcName`        | Nombre de la tarea, solo para depuracion (logs, `vTaskList`). No cambia el comportamiento. | `"vTask1"` |
| `usStackDepth`  | Tamano de la pila de la tarea. En ESP-IDF se da en **bytes** (a diferencia del FreeRTOS estandar, que lo pide en words). | `STACK_SIZE` (1024*2 = 2048 bytes) |
| `pvParameters`  | Puntero generico que se le pasa a la funcion al arrancar (lo recibe como su argumento `pvParameters`). | `NULL`: `vTask1` no necesita ningun dato de entrada |
| `uxPriority`    | Prioridad de la tarea (numero mayor = mas prioridad). Con la misma prioridad, dos tareas del mismo core se turnan por time slicing (ver arriba). | `1` |
| `pxCreatedTask` | Si no es `NULL`, aqui se guarda un handle para poder manejar la tarea despues (borrarla, suspenderla, etc). | `NULL`: no necesitamos controlarla despues de crearla |
| `xCoreID`       | El core al que se fija la tarea: `0`, `1`, o `tskNO_AFFINITY` para dejar que el planificador decida. Esto es lo que arma la tabla de arriba. | `0` |

La funcion regresa `pdPASS` si pudo crear la tarea (por ejemplo, si
alcanzo la memoria para la pila) o `pdFAIL` si no. En el programa no se
revisa ese valor de retorno porque es una demo, pero en un proyecto
real conviene verificarlo.

## Donde corre app_main (la tarea "main")

`app_main()` no corre "fuera" del sistema de tareas: ESP-IDF la
arranca dentro de una tarea mas, llamada `main`, creada automaticamente
al iniciar. Esa tarea `main` es la que ejecuta `barras_app_main()` y,
dentro de ella, `tareas_crear()` — o sea, **quien crea a `vTask1`,
`vTask2`, `vTask3` y `oled_mostrar` es la propia tarea `main`, que
sigue corriendo hasta que `tareas_crear()` termina.**

Por defecto, en este proyecto la tarea `main` esta fijada al **core 0**
(`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y` en el `sdkconfig`) con
**prioridad 1** (fija en el codigo de ESP-IDF, no aparece en
menuconfig).

Esto importa porque puede volverse una trampa: si dentro de
`tareas_crear()` se crea, ANTES que las demas, una tarea fijada al
core 0 con prioridad **mayor** que 1 y que nunca cede el CPU (como
`vTask1` cuando se le sube la prioridad en el Ejercicio 2 de
`ejercicios.md`), esa tarea interrumpe a `main` en cuanto se crea — y
como `main` esta en el mismo core y `vTask1` nunca cede, `main` nunca
recupera el control. El resultado: `tareas_crear()` se queda a medias,
`vTask2`, `vTask3` y `oled_mostrar` **nunca llegan a crearse**, y la
pantalla se congela en lo ultimo que tenia (porque la tarea que la
dibuja no existe, no porque no le toque turno).

Dos formas de evitarlo sin perder el punto del ejercicio (que
`vTask1` tenga mas prioridad):

- Crear `vTask1` **al final** de `tareas_crear()`, despues de las
  demas, para que no alcance a interrumpir su propia creacion.
- O cambiarle la afinidad a la tarea `main` desde `menuconfig`, para
  que ya no este atrapada en el core 0 (ver siguiente seccion).

### CONFIG_ESP_MAIN_TASK_AFFINITY: los tres valores

Se cambia en `idf.py menuconfig` -> `Component config` ->
`ESP System Settings` -> `Main task core affinity`. Esta opcion
controla exactamente a que core se fija la tarea `main` — igual que
el ultimo parametro de `xTaskCreatePinnedToCore` lo hace para
cualquier otra tarea, solo que aqui se configura una sola vez para
todo el proyecto en vez de pasarse como argumento. Tiene tres
valores:

- `CPU0` (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0`): fija `main` al core
  0. Es el valor por defecto, y el que tiene este proyecto.
- `CPU1` (`CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1`): fija `main` al core
  1.
- `No affinity` (`CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY`): no la
  fija a ningun core; el planificador la puede mover.

Con `CPU0` (el default), `main` queda atrapada en el core 0 para
siempre: si una tarea de mayor prioridad en ese mismo core nunca cede
el CPU, `main` no tiene a donde escapar y se queda esperando para
siempre — esto es justo lo que pasa en el Ejercicio 5 de
`ejercicios.md`. Con `No affinity`, en cambio, en cuanto la
interrumpen en el core 0, el planificador la puede reanudar en el
core 1 (si esta libre) y sigue corriendo con normalidad, aunque la
tarea que la interrumpio nunca ceda el CPU en su propio core.

**Comprobado:** con `vTask1` en prioridad 2 / core 0 (la config que
trababa la pantalla), cambiar `Main task core affinity` a
`No affinity` y volver a flashear destraba el sistema — `main`
termina de correr `tareas_crear()` normalmente y `oled_mostrar`
vuelve a dibujar.

## Por que las tareas 2 y 3 se turnan solas

`vTask2` y `vTask3` tienen la misma prioridad (1). FreeRTOS en el
ESP32 usa *time slicing*: si dos tareas de igual prioridad estan listas
en el mismo core, el planificador las va turnando en cada tick del
reloj del sistema, sin que el codigo tenga que pedirlo. Por eso, aunque
quitamos cualquier cesion explicita de CPU, las barras 2 y 3 se siguen
repartiendo el core 1 de forma razonablemente pareja.

Eso NO aplica hacia abajo en prioridad: una tarea de prioridad mas baja
(como la tarea IDLE) nunca corre mientras haya alguna tarea de
prioridad igual o mayor lista en su core.

## La tarea IDLE y el Watchdog

Cada core tiene su propia tarea IDLE (la de menor prioridad posible,
0). Corre "cuando no hay nada mas que hacer" y, entre otras cosas,
avisa al Task Watchdog Timer (TWDT) que el sistema sigue vivo.

En este experimento, el core 1 nunca se queda sin una tarea de
prioridad 1 lista (si `vTask2` no esta lista, `vTask3` si, o viceversa),
asi que IDLE1 practicamente nunca corre. El TWDT lo detecta y reporta:

```
E (35599) task_wdt: Task watchdog got triggered...
E (35599) task_wdt:  - IDLE1 (CPU 1)
```

Esto **no es un bug del programa**: es la consecuencia esperada de
saturar un core con tareas que nunca ceden. Es justo lo que el
experimento quiere mostrar.

## Por que desactivamos el Watchdog aqui

El TWDT es una herramienta de deteccion de cuelgues (una tarea
verdaderamente trabada). En esta practica el "cuelgue" de IDLE1 es
intencional y esperado, no un error real, y el tema que se esta viendo
es scheduling/prioridades/cores, no manejo de watchdogs. Dejarlo
prendido solo agrega ruido en el log que distrae del punto.

Ademas, no hay costo real por apagarlo aqui: el programa no borra
tareas ni usa ahorro de energia (light sleep), que son los otros
trabajos que depende de que IDLE corra. Y aunque estuviera prendido,
el panic esta desactivado en este proyecto
(`CONFIG_ESP_TASK_WDT_PANIC` no activo), asi que el chip no se
reinicia solo; el error se repite en el log sin mas consecuencia. Si
esa opcion estuviera activa, el ESP32 se reiniciaria cada vez que el
watchdog se disparara.

**Importante:** esto es valido para esta demo. En un proyecto real
(o en practicas donde una tarea si se puede trabar de verdad, por un
periferico que no responde, por ejemplo) el watchdog conviene dejarlo
activo, porque ahi si esta cumpliendo su proposito.

### Como se desactivo

```
idf.py menuconfig
```

`Component config` -> `ESP System Settings` -> `Task Watchdog Timer`
-> desmarcar **"Initialize Task Watchdog Timer on startup"** -> guardar
(`S`) y salir.

Esto pone `CONFIG_ESP_TASK_WDT_INIT` en `n` dentro del `sdkconfig` del
proyecto.

## Codigo: que se quito y por que

El programa original cedia CPU cada 8 pixeles de avance
(`CEDER_CADA`), con un `vTaskDelay(1)` dentro de cada `vTaskx`, para
darle un hueco a IDLE y alimentar al watchdog. Con el watchdog
desactivado esa cesion ya no cumple ningun proposito en esta practica,
asi que se quito de las tres tareas (`vTask1`, `vTask2`, `vTask3`):
ya no hay variable `paso` ni `if (++paso % CEDER_CADA == 0)`. Las
tareas quedaron como el bucle puro: computo + avance de posicion, sin
nada de manejo de CPU de por medio.

## Codigo: se quito el escaneo I2C

`i2c_escanear()` solo servia para diagnosticar el cableado del OLED
(recorrer 0x03-0x77 y ver quien responde). No aporta nada al tema de
scheduling/cores, asi que se quito por completo: la funcion, su
prototipo, el `#define ACK_CHECK_EN`, la llamada en `barras_app_main`
y el `#include "driver/gpio.h"` (solo se usaba ahi). El componente
tampoco necesita ya `esp_driver_gpio` en su `CMakeLists.txt`.

Si alguna vez hay que verificar cableado nuevo, esa funcion sigue
disponible en la version anterior de la practica (o se puede volver a
escribir; no es codigo dificil de recuperar).
