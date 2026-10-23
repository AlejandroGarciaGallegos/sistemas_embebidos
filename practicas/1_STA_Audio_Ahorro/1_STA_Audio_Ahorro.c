// PROGRAMA 1: vigia de audio con light sleep. Como el programa 1 de la
// clase 14 (microfono por UDP), pero el ESP32 duerme casi todo el tiempo
// sin perder el WiFi. Cada ESCUCHA_CADA_MS despierta, escucha un momento
// y mide el nivel; si hay ruido RUIDO_SEGUIDAS veces seguidas, manda a la
// PC solo el audio con ruido, hasta que pasan ESPERA_SILENCIO_MS sin
// ruido. El cliente es graficar_audio.py. Partes nuevas: "// >>>".

// ---------- 1) Importaciones ----------
#include "1_STA_Audio_Ahorro.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

// ---------- 2) Declaraciones ----------

#define WIFI_SSID "fr2.4g"
#define WIFI_PASS "GrayArrow123"

// IP fija: cada equipo usa la suya (201 a 211)
#define IP_ESP32 ESP_IP4TOADDR(192, 168, 8, 201)
#define IP_ROUTER ESP_IP4TOADDR(192, 168, 8, 1)
#define IP_MASCARA ESP_IP4TOADDR(255, 255, 255, 0)

// Microfono PDM integrado en la Sense (no hay que cablear)
#define MIC_PIN_CLK GPIO_NUM_42
#define MIC_PIN_DATA GPIO_NUM_41

// >>> LED de la XIAO (activo en bajo). Encendido = el vigia no duerme:
// >>> busca el WiFi, escucha (parpadeo de 0.1 s) o esta enviando
#define LED_GPIO GPIO_NUM_21
#define LED_ENCENDIDO 0
#define LED_APAGADO 1

// Audio: 16000 muestras/s de 16 bits, en bloques de 512 (32 ms)
#define AUDIO_FRECUENCIA 16000
#define AUDIO_MUESTRAS 512

// >>> El vigia: cada cuanto escucha, cuanto escucha y que es "ruido"
#define ESCUCHA_CADA_MS 750  // tiempo dormido entre escuchas
#define ESCUCHA_BLOQUES 3    // 3 x 32 ms = 0.1 s escuchando
#define NIVEL_UMBRAL 400     // arriba de esto es ruido (calibrar)
#define RUIDO_SEGUIDAS 3     // escuchas con ruido para despertar
#define ESPERA_SILENCIO_MS 4000 // escucha sin enviar tras el ultimo ruido

// >>> Energia: la CPU baja a 40 MHz cuando no hay trabajo, y si todas las
// >>> tareas esperan, el chip entra solo a light sleep
#define CPU_MAX_MHZ 160
#define CPU_MIN_MHZ 40

#define UDP_PUERTO 3333 // el mismo puerto en graficar_audio.py

// La tarea del vigia corre en el nucleo 1: el 0 es del WiFi
#define NUCLEO_ENVIO 1

static int16_t s_bloque[AUDIO_MUESTRAS]; // el ultimo bloque de audio

// Microfono: el canal I2S por el que llegan las muestras
static i2s_chan_handle_t s_microfono;

// UDP: un solo socket para todo, y la direccion de la ultima PC que
// escribio (se aprende con recvfrom; sin ella no hay a quien mandar)
static int s_socket = -1;
static struct sockaddr_in s_pc_direccion;
static volatile bool s_pc_conocida = false;

// Configuracion
static void energia_iniciar(void);
static void led_iniciar(void);
static void microfono_iniciar(void);
static void vigia_iniciar(void);
static void wifi_iniciar(void);
static void udp_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static void tarea_escuchar(void *arg);
static void tarea_vigia(void *arg);
static int escuchar_un_momento(void);
static void transmitir_hasta_silencio(void);
static int microfono_nivel(void);

// ---------- 3) Funcion principal ----------

void sta_audio_ahorro_app_main(void)
{
    // Si la NVS quedo de otro programa, se borra
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    energia_iniciar();
    led_iniciar();
    microfono_iniciar();
    vigia_iniciar();
    wifi_iniciar();
    udp_iniciar();
}

// ---------- 4) Funciones ----------

// -- 4.1 Configuracion --

static void energia_iniciar(void)
{
    // >>> Administrador de energia: frecuencia variable y light sleep
    // >>> automatico cuando ninguna tarea tiene nada que hacer
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = CPU_MAX_MHZ,
        .min_freq_mhz = CPU_MIN_MHZ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
}

static void led_iniciar(void)
{
    // >>> Encendido desde el arranque: se apaga al conectarse al WiFi
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ENCENDIDO);
}

static void microfono_iniciar(void)
{
    // 1. el canal I2S, solo de recepcion (tx = NULL)
    i2s_chan_config_t canal_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&canal_cfg, NULL, &s_microfono));

    // 2. el modo PDM: frecuencia, formato de muestra y pines
    i2s_pdm_rx_config_t pdm_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_FRECUENCIA),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_PIN_CLK,
            .din = MIC_PIN_DATA,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_microfono, &pdm_cfg));

    // >>> 3. NO se arranca aqui: con el microfono encendido el chip no
    // >>> puede dormir. Se enciende solo mientras escucha o transmite
}

static void vigia_iniciar(void)
{
    // Tarea fijada al nucleo 1, para no quitarle CPU al WiFi (nucleo 0)
    xTaskCreatePinnedToCore(tarea_vigia, "vigia", 4096, NULL, 5,
                             NULL, NUCLEO_ENVIO);
}

static void wifi_iniciar(void)
{
    ESP_ERROR_CHECK(esp_netif_init());                // Pila TCP/IP
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Bucle de eventos
    esp_netif_t *interfaz = esp_netif_create_default_wifi_sta();

    // IP fija: se apaga el DHCP (pedir IP al router) y se pone la propia
    esp_netif_ip_info_t ip_fija = {
        .ip.addr = IP_ESP32,
        .gw.addr = IP_ROUTER,
        .netmask.addr = IP_MASCARA,
    };
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(interfaz));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(interfaz, &ip_fija));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // eventos_wifi se ejecuta con cada evento del WiFi
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventos_wifi, NULL, NULL));

    // Red del modem a la que se conecta (modo STA)
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // >>> Modem sleep: el radio se apaga entre los beacons del router y
    // >>> se enciende a tiempo para escucharlos: sigue conectado
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
}

static void udp_iniciar(void)
{
    // UDP: crea el socket de datagramas (SOCK_DGRAM)
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0)
    {
        return; // sin socket no hay nada que hacer
    }

    // UDP: lo enlaza al puerto 3333. No hay listen() ni accept():
    // desde aqui ya recibe datagramas de cualquiera
    struct sockaddr_in propia = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_PUERTO),
    };

    if (bind(s_socket, (struct sockaddr *)&propia, sizeof(propia)) != 0)
    {
        return; // sin puerto no se crea la tarea de escucha
    }

    xTaskCreate(tarea_escuchar, "escuchar_udp", 4096, NULL, 5, NULL);
}

// -- 4.2 Procesos --

// Conecta y reconecta. >>> El LED muestra si hay WiFi
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (id == WIFI_EVENT_STA_CONNECTED)
    {
        gpio_set_level(LED_GPIO, LED_APAGADO); // conectado: a dormir
    }
    else if (id == WIFI_EVENT_STA_DISCONNECTED)
    {
        gpio_set_level(LED_GPIO, LED_ENCENDIDO); // sin WiFi
        esp_wifi_connect();
    }
}

// Recibe "R" (registrarse): desde ahi se le manda el audio a esa PC
static void tarea_escuchar(void *arg)
{
    char mensaje[8];
    struct sockaddr_in origen;
    socklen_t largo_origen = sizeof(origen);

    while (true)
    {
        // UDP: espera un datagrama; recvfrom dice quien lo mando
        int recibidos = recvfrom(s_socket, mensaje, sizeof(mensaje) - 1, 0,
                                  (struct sockaddr *)&origen, &largo_origen);
        if (recibidos <= 0)
        {
            continue; // error del socket: se sigue escuchando
        }

        // UDP: se guarda quien escribio, para mandarle el audio
        s_pc_direccion = origen;
        s_pc_conocida = true;
    }
}

// >>> El vigia: duerme, escucha un momento y decide si transmite
static void tarea_vigia(void *arg)
{
    int seguidas = 0; // escuchas seguidas con ruido

    while (true)
    {
        // >>> 1. duerme: con todas las tareas esperando, el chip entra
        // >>> solo a light sleep (y el WiFi sigue conectado)
        vTaskDelay(pdMS_TO_TICKS(ESCUCHA_CADA_MS));

        // >>> 2. escucha un momento y cuenta si hubo ruido
        if (escuchar_un_momento() > NIVEL_UMBRAL)
        {
            seguidas = seguidas + 1;
        }
        else
        {
            seguidas = 0; // el silencio rompe la racha
        }

        // >>> 3. ruido sostenido: transmite hasta que vuelva el silencio
        if (seguidas >= RUIDO_SEGUIDAS)
        {
            transmitir_hasta_silencio();
            seguidas = 0;
        }
    }
}

// >>> Enciende el microfono ESCUCHA_BLOQUES bloques y devuelve el nivel
// >>> mas alto que oyo
static int escuchar_un_momento(void)
{
    int nivel_max = 0;
    size_t leidos = 0;

    gpio_set_level(LED_GPIO, LED_ENCENDIDO); // >>> escuchando
    i2s_channel_enable(s_microfono);
    for (int i = 0; i < ESCUCHA_BLOQUES; i++)
    {
        i2s_channel_read(s_microfono, s_bloque, sizeof(s_bloque), &leidos,
                         portMAX_DELAY);
        int nivel = microfono_nivel();
        if (i > 0 && nivel > nivel_max) // el 1er bloque se descarta: el
        {                               // microfono apenas se encendio
            nivel_max = nivel;
        }
    }
    i2s_channel_disable(s_microfono); // apagado: el chip ya puede dormir
    gpio_set_level(LED_GPIO, LED_APAGADO);

    return nivel_max;
}

// >>> Manda solo los bloques con ruido. Termina cuando pasan
// >>> ESPERA_SILENCIO_MS sin ruido (escuchando, pero sin enviar)
static void transmitir_hasta_silencio(void)
{
    size_t leidos = 0;
    int64_t ultimo_ruido_us = esp_timer_get_time();

    gpio_set_level(LED_GPIO, LED_ENCENDIDO); // >>> fase de envio
    i2s_channel_enable(s_microfono);
    while (esp_timer_get_time() - ultimo_ruido_us <
           ESPERA_SILENCIO_MS * 1000LL)
    {
        i2s_channel_read(s_microfono, s_bloque, sizeof(s_bloque), &leidos,
                         portMAX_DELAY);

        // Bloque sin ruido: no se envia, solo sigue contando el silencio
        if (microfono_nivel() <= NIVEL_UMBRAL)
        {
            continue;
        }

        // Bloque con ruido: reinicia la espera y se manda a la PC
        ultimo_ruido_us = esp_timer_get_time();
        if (s_pc_conocida)
        {
            sendto(s_socket, s_bloque, leidos, 0,
                   (struct sockaddr *)&s_pc_direccion, sizeof(s_pc_direccion));
        }
    }
    i2s_channel_disable(s_microfono);
    gpio_set_level(LED_GPIO, LED_APAGADO); // >>> de vuelta a dormir
}

// >>> Nivel del bloque: cuanto se aleja la onda de su centro (0 a 32767)
static int microfono_nivel(void)
{
    // 1. el centro: el promedio de las muestras
    int32_t suma = 0;
    for (int i = 0; i < AUDIO_MUESTRAS; i++)
    {
        suma = suma + s_bloque[i];
    }
    int32_t centro = suma / AUDIO_MUESTRAS;

    // 2. la mayor distancia al centro
    int nivel = 0;
    for (int i = 0; i < AUDIO_MUESTRAS; i++)
    {
        int distancia = abs(s_bloque[i] - centro);
        if (distancia > nivel)
        {
            nivel = distancia;
        }
    }
    return nivel;
}
