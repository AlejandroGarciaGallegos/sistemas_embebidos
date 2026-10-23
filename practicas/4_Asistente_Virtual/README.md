# Practica 4: asistente virtual ESP32-S3 + Orange Pi

Primera version con microfono y cara OLED en la ESP, procesamiento en Debian y
respuesta escrita en la consola de la Orange Pi y en la OLED. No utiliza bocina.

Hay dos niveles de pruebas:

- **Sin modelos:** HTTP, WebSocket, expresiones, audio UDP y grabacion WAV.
- **Con modelos locales:** voz del microfono -> Whisper tiny -> Qwen -> texto.

El modo `--motor-ia prueba` devuelve un eco identificado como **PRUEBA SIN IA**.
Sirve para aislar problemas; no genera respuestas de un modelo. El modo por
defecto, `local`, utiliza Qwen de verdad.

## Archivos y componentes reutilizados

```text
4_Asistente_Virtual/
  4_Asistente_Virtual.c       firmware ESP-IDF / FreeRTOS
  4_Asistente_Virtual.h       entrada asistente_virtual_app_main()
  asistente_config.h         WiFi, pantalla, microfono y puertos
  CMakeLists.txt             registro del componente
  README.md                  esta guia
  orangepi/
    asistente.py             programa Python para Debian
    requirements.txt         cliente WebSocket, sin PyTorch ni interfaz grafica
    test_asistente.py        pruebas sin hardware ni modelos
```

La captura parte de `1_Microfono_UDP`, el control de
`2_Servidor_web_WebSocket` y la OLED de `1_Barras` / `3_Cores`.
Ya se registro la practica en los CMakeLists y en `main/practicas.h`.
`main/main.c` ya selecciona `asistente_virtual_app_main`.

## 1. Preparar y cargar la ESP (desde la PC)

Se presupone la misma XIAO ESP32S3 Sense y el mismo OLED de las practicas.

| Funcion | Pines/configuracion |
| --- | --- |
| Microfono PDM integrado | CLK GPIO42, DATA GPIO41, I2S0 |
| OLED I2C | SDA GPIO7, SCL GPIO44, direccion 7 bits 0x3C |
| Audio | 16000 Hz, mono, PCM de 16 bits |
| Control | HTTP puerto 80, WebSocket `/ws` |
| Audio por red | UDP puerto 3333 |

1. Edita **`asistente_config.h`** y escribe el SSID y la contrasena de la misma
   red que utiliza la Orange Pi. Esta practica tiene su propia configuracion;
   no hereda las macros de otra practica.
2. Si tu OLED es SH1106, cambia `OLED_ES_SH1106` a `1`. El valor inicial `0`
   conserva el controlador SSD1306 utilizado por tus practicas.
3. Para probar sin conectar la pantalla, cambia `OLED_HABILITADA` a `0`.
4. Abre una terminal **ESP-IDF** en la raiz `Freertos_demos`, donde esta el
   CMakeLists principal. No compiles solamente la subcarpeta de esta practica.

```powershell
idf.py build
idf.py -p COM4 flash monitor
```

`COM4` es el puerto configurado actualmente en el proyecto; cambialo si Windows
asigna otro. `build` compila; `flash` carga el firmware; `monitor` abre el puerto
serie. Sal del monitor con **Ctrl+]**.

Si cargaste la primera version y aparece `tcpip_send_msg_wait_sem (Invalid mbox)`
seguido de `Rebooting`, vuelve a compilar y cargar esta version corregida. El
fallo era abrir UDP/HTTP antes de inicializar TCP/IP, no una contrasena WiFi
incorrecta. Ahora `wifi_iniciar()` prepara `esp_netif` y el bucle de eventos
antes de `udp_iniciar()` y `servidor_iniciar()`. No hace falta borrar la flash.

En tu instalacion, si PowerShell no reconoce `idf.py`, puedes cargar su entorno:

```powershell
. C:\Espressif\tools\Microsoft.v6.1-beta1.PowerShell_profile.ps1
$env:PYTHONUTF8 = '1'
idf.py build
```

El monitor mostrara una linea como esta:

```text
IP ESP32: 192.168.1.50 | HTTP/WS: 80 | UDP: 3333
```

**Usa tu IP real** en todos los comandos siguientes. `192.168.1.50` es un ejemplo.
No necesitas crear un punto de acceso ni usar la practica del router.
Si cambias de practica, modifica `PRACTICA` en `main/main.c` como antes.

El proyecto ya incluye `CONFIG_HTTPD_WS_SUPPORT=y` en `sdkconfig.defaults`.
Si reutilizas otro sdkconfig con esa opcion apagada, habilita WebSocket en
`idf.py menuconfig` -> Component config -> HTTP Server.

## 2. Copiar la practica a la microSD (mediante Debian)

Con Debian arrancado y SSH habilitado en la Orange Pi, desde PowerShell en la
raiz del proyecto:

```powershell
scp -r .\practicas\4_Asistente_Virtual USUARIO@IP_ORANGE:~/
ssh USUARIO@IP_ORANGE
```

Sustituye `USUARIO` e `IP_ORANGE` por los datos de Debian. Los archivos quedan
en el sistema de archivos de la microSD. Tambien puedes copiarlos por otro
medio conservando la carpeta `orangepi` completa.

Los comandos restantes, salvo indicacion contraria, se ejecutan **en Debian**:

```bash
cd ~/4_Asistente_Virtual/orangepi
uname -m
python3 --version
free -h
df -h .
```

Para los motores de IA de esta guia utiliza Debian de 64 bits: `uname -m` debe
mostrar `aarch64`. El programa Python requiere Python 3.9 o posterior. Si aparece
`armv7l`, las pruebas de red siguen siendo utiles, pero estos comandos de
compilacion ARMv8 de los motores no corresponden a ese sistema de 32 bits.

Instala solamente las dependencias iniciales:

```bash
sudo apt update
sudo apt install -y python3 python3-venv python3-pip curl alsa-utils
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -r requirements.txt
python asistente.py --help
```

Al abrir otra terminal, repite `cd` y `source .venv/bin/activate`.
El programa no necesita escritorio, Qt, PyQtGraph, microfono USB ni bocina.

## 3. Pruebas indispensables antes de instalar IA

Ejecuta una sola instancia del cliente a la vez. La ESP acepta una Orange Pi
por WebSocket. Las opciones generales, como `--esp` y `--motor-ia`, se colocan
**antes** del nombre del modo.

### A. Red, control y llegada de audio

```bash
curl --noproxy '*' http://192.168.1.50/estado
python asistente.py --esp 192.168.1.50 diagnostico
```

El primer comando verifica HTTP. El segundo abre WebSocket, registra el receptor
UDP, solicita audio y comprueba 16 bloques. Debe terminar con:

```text
OK: HTTP, WebSocket, registro y recepcion de audio UDP.
```

Tambien muestra el estado del microfono, la version del protocolo y el RMS
promedio. Esto confirma transporte y lectura I2S, pero escuchar una grabacion
es lo que permite evaluar si el audio se entiende.

### B. Expresiones de la pantalla

```bash
python asistente.py --esp 192.168.1.50 pantalla
```

Recorre esperando, escuchando, transcribiendo, pensando, respuesta y error,
durante tres segundos por estado. La expresion de error es parte de la prueba.
La animacion se dibuja localmente, aunque la Orange Pi este ocupada.

### C. Grabar la voz y calibrar el nivel

```bash
python asistente.py --esp 192.168.1.50 grabar --segundos 8 --salida grabaciones/prueba.wav
```

Guarda silencio unos segundos, habla y vuelve a guardar silencio. La consola
muestra RMS y pico. Anota el RMS del ruido ambiental y el de tu voz. El umbral
para conversar debe quedar entre ambos; `300` es solo un punto de partida.

Puedes escuchar el archivo en cualquier PC o, si Debian tiene una salida de
audio configurada, con:

```bash
aplay grabaciones/prueba.wav
```

`aplay` es opcional y no utiliza una bocina en la ESP. Tambien puedes traer el
archivo a Windows, ejecutando en la PC:

```powershell
scp USUARIO@IP_ORANGE:~/4_Asistente_Virtual/orangepi/grabaciones/prueba.wav .\prueba.wav
```

No pases a reconocimiento si el archivo contiene silencio, cortes grandes,
voz saturada o ruido que impida entenderla.

### D. Respuesta escrita y envio a la OLED sin modelos

```bash
python asistente.py --esp 192.168.1.50 --motor-ia prueba texto "Hola desde la Orange Pi"
```

Debe mostrar **[PRUEBA SIN IA]** en consola y enviar el texto a la pantalla.
Pulsa Enter cuando termines de leerla. Para comprobar solo el Python sin ESP:

```bash
python asistente.py --motor-ia prueba texto "Hola"
```

## 4. Preparar los motores de IA local

El programa de control es Python. Para inferencia invoca los ejecutables de
**whisper.cpp** y **llama.cpp**: asi evita instalar PyTorch y puede liberar toda
la memoria del motor al cerrar su proceso. No utiliza servicios de pago ni APIs
externas. Se necesita Internet para instalar paquetes, clonar y descargar;
despues los modelos se ejecutan localmente.

Con 1 GB, empieza sin aplicaciones de escritorio pesadas y con compilacion
de **un solo trabajo**. Estos motores se cargan por separado durante cada turno.
La primera preparacion puede tardar bastante; no hay una velocidad prometida
para la Orange Pi.

Desde `~/4_Asistente_Virtual/orangepi`:

```bash
sudo apt install -y build-essential cmake git libssl-dev
mkdir -p motores modelos
git clone --depth 1 https://github.com/ggml-org/whisper.cpp.git motores/whisper.cpp
git clone --depth 1 https://github.com/ggml-org/llama.cpp.git motores/llama.cpp
```

No repitas `git clone` sobre una carpeta que ya contenga el repositorio.
Guarda las revisiones utilizadas si vas a comparar resultados:

```bash
git -C motores/whisper.cpp rev-parse HEAD
git -C motores/llama.cpp rev-parse HEAD
```

Compila Whisper (entrada de voz):

```bash
cmake -S motores/whisper.cpp -B motores/whisper.cpp/build \
  -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv8-a -DWHISPER_BUILD_TESTS=OFF
cmake --build motores/whisper.cpp/build --target whisper-cli -j 1
```

Compila el servidor de Qwen (generacion de respuestas):

```bash
cmake -S motores/llama.cpp -B motores/llama.cpp/build \
  -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=OFF \
  -DGGML_CPU_ARM_ARCH=armv8-a -DLLAMA_BUILD_TESTS=OFF
cmake --build motores/llama.cpp/build --target llama-server -j 1
```

El objetivo ARMv8 evita habilitar instrucciones adicionales que no tenga el
Cortex-A53. Los fuentes oficiales documentan
[la configuracion de CPU de llama.cpp](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cpu/CMakeLists.txt).

Descarga Whisper **tiny multilingue**, necesario para espanol, y Qwen:

```bash
curl -fL --retry 3 -o modelos/ggml-tiny.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.bin
curl -fL --retry 3 -o modelos/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf
```

No elijas `tiny.en`: es la variante de ingles. El archivo Qwen Q4_K_M oficial
ocupa aproximadamente 491 MB; es almacenamiento, no una garantia de consumo de
RAM. [Archivo oficial de Qwen](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/blob/main/qwen2.5-0.5b-instruct-q4_k_m.gguf).

Comprueba los binarios:

```bash
motores/whisper.cpp/build/bin/whisper-cli --help
motores/llama.cpp/build/bin/llama-server --help
```

Si el compilador termina con `Killed`, revisa `free -h` y la memoria disponible.
La compilacion del servidor tambien puede necesitar mas RAM que su ejecucion.
Puedes compilar en otro Debian ARM64 compatible y copiar **todo** el directorio
`build/bin` correspondiente, incluidas sus bibliotecas compartidas. No copies
binarios de Windows ni de una PC x86. Esta guia no cambia automaticamente swap
ni configuraciones del sistema.

## 5. Probar voz a texto y modelo por separado

Transcribe la grabacion previa, sin necesitar una ESP conectada:

```bash
python asistente.py transcribir grabaciones/prueba.wav
```

Luego prueba una pregunta escrita al modelo real, tambien sin ESP:

```bash
python asistente.py texto "Explica en dos frases para que sirve FreeRTOS"
```

Si falla cualquiera de estas dos pruebas, revisa ese motor antes de combinarlo
con el audio por red. En cada invocacion se muestra el error del ejecutable si
falta el modelo, el formato es incorrecto o no alcanza la memoria.

## 6. Conversacion completa, con respuesta por texto

```bash
python asistente.py --esp 192.168.1.50 conversar --umbral 300
```

1. Pulsa **Enter en Debian** para iniciar un turno.
2. Espera a que aparezca **Habla ahora**, formula una pregunta corta y calla.
3. Un segundo de silencio cierra la captura; se conservan las pausas internas
   y hasta 320 ms de audio anteriores al inicio detectado.
4. Whisper transcribe. Su proceso termina y libera memoria.
5. Se inicia llama-server en `127.0.0.1:8081`, se consulta Qwen y se detiene ese
   proceso. La ESP sigue animada y recibe latidos durante la inferencia.
6. La respuesta se imprime en consola y se envia a la OLED, que pagina dos
   lineas de 21 caracteres. La consola conserva acentos; la OLED los normaliza
   a ASCII para utilizar la fuente existente.
7. Pulsa Enter cuando termines de leer y luego Enter para otro turno. Escribe
   `q` en el siguiente indicador para salir o `limpiar` para borrar contexto.

Se conserva un solo intercambio previo y el contexto del modelo se limita a
1024 tokens. La respuesta inicial tiene un maximo de 96 tokens. Si se corta,
puedes subir el limite, hasta 256, mediante `--tokens`.

Los archivos `grabaciones/ultima_frase.wav` y `grabaciones/ultimo_turno.json`
se reemplazan con el ultimo turno. No se almacena un historial ilimitado.

La deteccion inicial es **por energia RMS**: no reconoce palabras de activacion
ni distingue habla de cualquier ruido fuerte. La activacion manual evita que
el ruido del cuarto genere consultas continuas durante estas primeras pruebas.

Ejemplos de ajustes (opciones generales antes del modo):

```bash
# Una captura y una respuesta, despues termina.
python asistente.py --esp 192.168.1.50 conversar --una-vez --umbral 300

# Voz suave: umbral menor; pausas mas largas antes de cerrar la frase.
python asistente.py --esp 192.168.1.50 conversar --umbral 150 --silencio 1.5

# Tiempo mayor de inferencia y respuestas un poco mas extensas.
python asistente.py --esp 192.168.1.50 --timeout 600 --tokens 128 conversar

# Reconocimiento real, pero respuesta de prueba sin cargar Qwen.
python asistente.py --esp 192.168.1.50 --motor-ia prueba conversar
```

Con `--una-vez`, al cerrar el cliente la OLED vuelve a indicar desconexion; la
respuesta completa permanece en la consola y en el JSON del ultimo turno.
Para detener cualquier modo utiliza **Ctrl+C**. El programa intenta detener
la captura y cerrar los procesos. Si se interrumpe la conexion, la ESP deja de
enviar audio al vencer el plazo de ocho segundos.

## 7. Si quieres probar el modelo en otra computadora de la LAN

Es una alternativa si la RAM o la latencia local impiden continuar. La Orange
Pi conserva captura y transcripcion; solamente cambia donde se genera texto.
Arranca un servidor llama.cpp con un modelo apropiado en esa otra computadora
y usa su URL, por ejemplo:

```bash
python asistente.py --esp 192.168.1.50 --llm-url http://192.168.1.60:8081 conversar
```

Con `--llm-url`, Python no carga ni detiene ese servidor: lo administras por
separado. Sin esa opcion, el funcionamiento predeterminado es local y por turno.

## 8. Protocolo y recuperacion

- La ESP escucha `/ws` y responde un JSON a cada comando de texto.
- El primer `PING` registra al cliente, compatible con el handshake automatico
  de ESP-IDF 6.1. No se depende de recibir el GET en el handler de usuario.
- Comandos: `PING`, `ESTADO|nombre|texto`, `TEXTO|respuesta`,
  `INICIAR|sesion` y `DETENER`.
- La Orange Pi envia `R|sesion` al puerto UDP 3333 desde su socket receptor.
  La ESP contesta `OK|sesion`; el registro se renueva cada segundo.
- Solo acepta ese registro desde la misma IP que abrio el WebSocket.
- El audio no empieza hasta recibir `INICIAR|sesion` por WebSocket.
- Cada datagrama contiene `AUDP`, sesion uint32, secuencia uint32, cantidad
  uint16 y frecuencia uint16: **16 bytes de cabecera big endian**, seguidos de
  **1024 bytes PCM little endian**. En Python: `struct.Struct("!4sIIHH")`.
- Se insertan bloques de silencio para huecos pequenos; paquetes atrasados o
  duplicados se descartan. Mas de ocho bloques consecutivos perdidos, una cola
  llena o mas del 10% de perdida invalidan la captura.
- Esta version descarta paquetes tardios; no implementa retransmision ni una
  ventana de reordenamiento. Muestra los contadores para evaluar tu red.
- Si una prueba pierde conexion, termina el comando, espera hasta ocho segundos
  y vuelvelo a ejecutar. La captura interrumpida se repite completa. No hay
  reconexion silenciosa en medio de una frase.

| Sintoma | Comprobacion |
| --- | --- |
| Invalid mbox y Rebooting en udp_iniciar | Cargar la version corregida: TCP/IP debe inicializarse antes de abrir sockets |
| HTTP no responde | IP del monitor, misma subred, aislamiento de clientes del punto de acceso |
| Error cliente_ocupado | Ya hay otro cliente; cierralo o espera a que venza el latido |
| HTTP funciona, no llega UDP | Puerto 3333, firewall, otra instancia, IP usada |
| Pantalla provoca reinicio | Cableado, alimentacion, direccion y controlador; probar con OLED_HABILITADA=0 |
| RMS casi cero y WAV silencioso | Microfono Sense, pines GPIO41/42, montaje de la expansion |
| Se activa con ruido | Subir --umbral y mantener activacion manual |
| Corta una pausa normal | Aumentar --silencio |
| No detecta tu voz | Medir RMS con grabar, bajar --umbral o acercarse al microfono |
| Whisper no encuentra modelo | Revisar ruta, usar --modelo-voz antes del modo |
| Qwen tarda o se cierra | RAM libre, menos aplicaciones, --timeout, --tokens; probar texto por separado |
| Illegal instruction en un motor | Recompilar ARMv8 para el Cortex-A53, sin binarios de otra arquitectura |

## 9. Pruebas del programa sin las placas

Desde `orangepi`, con el entorno virtual activo:

```bash
python -m unittest -v
```

Las pruebas usan HTTP, WebSocket y UDP en `127.0.0.1`, validan WAV, perdidas de
paquetes, sesiones antiguas, fin de frase y cierre del motor ante errores.
No descargan modelos. La transcripcion y las respuestas de esos ensayos estan
simuladas; la calidad real se verifica con los pasos 5 y 6.

Fuentes de los motores y de su interfaz:
[whisper.cpp CLI](https://github.com/ggml-org/whisper.cpp/tree/master/examples/cli),
[llama.cpp server](https://github.com/ggml-org/llama.cpp/tree/master/tools/server),
[websocket-client](https://github.com/websocket-client/websocket-client).
