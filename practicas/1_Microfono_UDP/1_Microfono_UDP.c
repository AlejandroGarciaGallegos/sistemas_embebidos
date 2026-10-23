// PROGRAMA 1: microfono por UDP. Como 3_Aleatorio_UDP, pero en lugar de un
// aleatorio manda bloques de audio del microfono de la Sense. El cliente es
// graficar_audio.py. Partes clave marcadas con "// >>>".

// ---------- 1) Importaciones ----------
#include "1_Microfono_UDP.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "driver/i2s_pdm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------- 2) Declaraciones ----------

// #define WIFI_SSID "fr2.4g"
// #define WIFI_PASS "GrayArrow123"

// #define WIFI_SSID "IZZI-3B6C"
// #define WIFI_PASS "nfPRK96pL6kxe73HEr"

#define WIFI_SSID "HUAWEInova14"
#define WIFI_PASS "12345678"

// >>> Microfono PDM integrado en la Sense (no hay que cablear)
#define MIC_PIN_CLK GPIO_NUM_42
#define MIC_PIN_DATA GPIO_NUM_41

// >>> Audio: 16000 muestras/s de 16 bits, en bloques de 512 (32 ms)
#define AUDIO_FRECUENCIA 16000
#define AUDIO_MUESTRAS 512

#define UDP_PUERTO 3333 // el mismo puerto en graficar_audio.py

// La tarea que manda corre en el nucleo 1: el 0 es del WiFi.
// No hace falta periodo: el microfono entrega un bloque cada 32 ms.
#define NUCLEO_ENVIO 1

static const char *TAG = "microfono";
static int16_t s_bloque[AUDIO_MUESTRAS]; // el ultimo bloque de audio

// >>> Microfono: el canal I2S por el que llegan las muestras
static i2s_chan_handle_t s_microfono;

// >>> UDP: un solo socket para todo, y la direccion de la ultima PC que
// >>> escribio (se aprende con recvfrom; sin ella no hay a quien mandar)
static int s_socket = -1;
static struct sockaddr_in s_pc_direccion;
static volatile bool s_pc_conocida = false;

// Configuracion
static void microfono_iniciar(void);
static void audio_iniciar(void);
static void wifi_iniciar(void);
static void udp_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static void tarea_escuchar(void *arg);
static void tarea_audio(void *arg);

// ---------- 3) Funcion principal ----------

void microfono_udp_app_main(void)
{
    // Si la NVS quedo de otro programa, se borra
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    microfono_iniciar();
    audio_iniciar();
    wifi_iniciar();
    udp_iniciar();
}

// ---------- 4) Funciones ----------

// -- 4.1 Configuracion --

static void microfono_iniciar(void)
{
    // >>> 1. el canal I2S, solo de recepcion (tx = NULL)
    i2s_chan_config_t canal_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&canal_cfg, NULL, &s_microfono));

    // >>> 2. el modo PDM: frecuencia, formato de muestra y pines
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

    // >>> 3. arranca: desde aqui el DMA va juntando muestras solo
    ESP_ERROR_CHECK(i2s_channel_enable(s_microfono));
}

static void audio_iniciar(void)
{
    // Tarea fijada al nucleo 1, para no quitarle CPU al WiFi (nucleo 0)
    xTaskCreatePinnedToCore(tarea_audio, "audio", 4096, NULL, 5,
                             NULL, NUCLEO_ENVIO);
}

static void wifi_iniciar(void)
{
    ESP_ERROR_CHECK(esp_netif_init());                // Pila TCP/IP
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Bucle de eventos
    esp_netif_create_default_wifi_sta();              // Interfaz WiFi

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // eventos_wifi se ejecuta con cada evento del WiFi y al obtener IP
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventos_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventos_wifi, NULL, NULL));

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

    ESP_LOGI(TAG, "Conectando a %s ...", WIFI_SSID);
}

static void udp_iniciar(void)
{
    // >>> UDP: crea el socket de datagramas (SOCK_DGRAM)
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_socket < 0)
    {
        ESP_LOGE(TAG, "No se pudo crear el socket UDP");
        return;
    }

    // >>> UDP: lo enlaza al puerto 3333. No hay listen() ni accept():
    // >>> desde aqui ya recibe datagramas de cualquiera
    struct sockaddr_in propia = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(UDP_PUERTO),
    };

    if (bind(s_socket, (struct sockaddr *)&propia, sizeof(propia)) != 0)
    {
        ESP_LOGE(TAG, "No se pudo enlazar el puerto %d", UDP_PUERTO);
        return;
    }

    xTaskCreate(tarea_escuchar, "escuchar_udp", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "Escuchando UDP en el puerto %d", UDP_PUERTO);
}

// -- 4.2 Procesos --

static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "Sin conexion, reintentando...");
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t e = *(ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Usa esta IP en graficar_audio.py:  " IPSTR,
                 IP2STR(&e.ip_info.ip));
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
        // >>> UDP: espera un datagrama; recvfrom dice quien lo mando
        int recibidos = recvfrom(s_socket, mensaje, sizeof(mensaje) - 1, 0,
                                  (struct sockaddr *)&origen, &largo_origen);
        if (recibidos <= 0)
        {
            continue; // error del socket: se sigue escuchando
        }

        // >>> UDP: se guarda quien escribio, para mandarle el audio
        s_pc_direccion = origen;
        s_pc_conocida = true;
        ESP_LOGI(TAG, "Mandando audio a %s", inet_ntoa(origen.sin_addr));
    }
}

// Lee un bloque del microfono y lo manda por UDP
static void tarea_audio(void *arg)
{
    while (true)
    {
        // >>> Espera a que el microfono junte un bloque completo (32 ms).
        // >>> Mientras espera suelta el nucleo: no hace falta vTaskDelay
        size_t leidos = 0;
        i2s_channel_read(s_microfono, s_bloque, sizeof(s_bloque), &leidos,
                         portMAX_DELAY);

        // Si nadie ha escrito todavia, no hay a quien mandarlo
        if (s_pc_conocida)
        {
            // >>> UDP: manda un datagrama a esa direccion.
            // >>> Sin conexion y sin confirmacion: si se pierde, nadie se entera
            sendto(s_socket, s_bloque, leidos, 0,
                   (struct sockaddr *)&s_pc_direccion, sizeof(s_pc_direccion));
        }
    }
}
