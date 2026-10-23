// PROGRAMA 3: UDP. Sin pagina web (un navegador no recibe UDP): el cliente
// son escuchar.py y enviar.py. Partes clave marcadas con "// >>>".

// ---------- 1) Importaciones ----------
#include "3_Aleatorio_UDP.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------- 2) Declaraciones ----------

//#define WIFI_SSID "fr2.4g"
//#define WIFI_PASS "GrayArrow123"
#define WIFI_SSID "HUAWEInova14"
#define WIFI_PASS "12345678"

#define LED_GPIO 21 // LED de usuario de la XIAO
#define VALOR_SECRETO 42

#define UDP_PUERTO 3333 // el mismo puerto en escuchar.py y enviar.py

// La tarea que manda corre en el nucleo 1: el 0 es del WiFi.
// Periodo en microsegundos (vTaskDelay no baja de 1 tick).
#define PERIODO_ENVIO_US 50
#define NUCLEO_ENVIO 1

static const char *TAG = "udp";
static volatile uint8_t s_aleatorio = 0;

// >>> UDP: un solo socket para todo, y la direccion de la ultima PC que
// >>> escribio (se aprende con recvfrom; sin ella no hay a quien mandar)
static int s_socket = -1;
static struct sockaddr_in s_pc_direccion;
static volatile bool s_pc_conocida = false;

// Configuracion
static void led_iniciar(void);
static void aleatorio_iniciar(void);
static void wifi_iniciar(void);
static void udp_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static void tarea_escuchar(void *arg);
static void tarea_aleatorio(void *arg);

// ---------- 3) Funcion principal ----------

void aleatorio_udp_app_main(void)
{
    // Si la NVS quedo de otro programa, se borra
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_iniciar();
    aleatorio_iniciar();
    wifi_iniciar();
    udp_iniciar();
}

// ---------- 4) Funciones ----------

// -- 4.1 Configuracion --

static void led_iniciar(void)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 1); // apagado (activo en bajo)
}

static void aleatorio_iniciar(void)
{
    // Tarea fijada al nucleo 1, para no quitarle CPU al WiFi (nucleo 0)
    xTaskCreatePinnedToCore(tarea_aleatorio, "aleatorio", 4096, NULL, 5,
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
        ESP_LOGI(TAG, "Usa esta IP en escuchar.py y enviar.py:  " IPSTR,
                 IP2STR(&e.ip_info.ip));
    }
}

// Recibe "R" (registrarse) o un valor (42 enciende el LED)
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
        mensaje[recibidos] = '\0';

        // >>> UDP: se guarda quien escribio, para mandarle el aleatorio
        s_pc_direccion = origen;
        s_pc_conocida = true;

        // "R" solo registra: no cuenta como intento ni toca el LED
        if (strcmp(mensaje, "R") == 0)
        {
            continue;
        }

        bool coincide = atoi(mensaje) == VALOR_SECRETO;
        gpio_set_level(LED_GPIO, coincide ? 0 : 1);
        ESP_LOGI(TAG, "Recibido \"%s\" de %s: %s", mensaje,
                 inet_ntoa(origen.sin_addr), coincide ? "LED on" : "LED off");
    }
}

// Genera un aleatorio cada PERIODO_ENVIO_US y lo manda por UDP
static void tarea_aleatorio(void *arg)
{
    int64_t siguiente_us = esp_timer_get_time();
    int64_t ultimo_ceder_us = siguiente_us;

    while (true)
    {
        s_aleatorio = esp_random() % 256;

        // Si nadie ha escrito todavia, no hay a quien mandarlo
        if (s_pc_conocida)
        {
            // >>> UDP: manda un datagrama a esa direccion.
            // >>> Sin conexion y sin confirmacion: si se pierde, nadie se entera
            char texto[4];
            int largo = snprintf(texto, sizeof(texto), "%d", s_aleatorio);
            sendto(s_socket, texto, largo, 0,
                   (struct sockaddr *)&s_pc_direccion, sizeof(s_pc_direccion));
        }

        siguiente_us += PERIODO_ENVIO_US;

        // Cada 200 ms se suelta el nucleo un instante: si no, la tarea
        // inactiva del nucleo 1 nunca corre y el watchdog reinicia la placa
        if (esp_timer_get_time() - ultimo_ceder_us > 200000)
        {
            vTaskDelay(1);
            ultimo_ceder_us = esp_timer_get_time();
        }

        // Espera activa hasta el siguiente periodo
        while (esp_timer_get_time() < siguiente_us)
        {
        }
    }
}
