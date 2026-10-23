// PRACTICA: servir una pagina HTML desde el ESP32-S3, en modo punto de acceso

// ---------- 1) Importaciones ----------
#include "2_Servidor_web_AP.h"
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

// ---------- 2) Declaraciones ----------

#define WIFI_SSID "ESP32-CLASE"
#define WIFI_PASS "12345678"
#define WIFI_CANAL 1
#define WIFI_MAX_CONEXIONES 4

#define LED_GPIO 21 // LED de usuario de la XIAO
#define VALOR_SECRETO 42

static const char *TAG = "web";
static volatile uint8_t s_aleatorio = 0;

// Simbolos del HTML embebido (componente web_comun)
extern const char index_html_inicio[] asm("_binary_index_html_start");
extern const char index_html_fin[] asm("_binary_index_html_end");

// Configuracion
static void led_iniciar(void);
static void aleatorio_iniciar(void);
static void wifi_iniciar(void);
static void servidor_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                          int32_t id, void *data);
static esp_err_t handler_raiz(httpd_req_t *req);
static esp_err_t handler_aleatorio(httpd_req_t *req);
static esp_err_t handler_enviar(httpd_req_t *req);
static void tarea_aleatorio(void *arg);

// ---------- 3) Funcion principal ----------

void servidor_web_ap_app_main(void)
{
    // NVS de un programa anterior: se reinicia
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_iniciar();
    aleatorio_iniciar();
    wifi_iniciar();
    servidor_iniciar();
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
    xTaskCreate(tarea_aleatorio, "aleatorio", 2048, NULL, 5, NULL);
}

static void wifi_iniciar(void)
{
    ESP_ERROR_CHECK(esp_netif_init());                // TCP/IP
    ESP_ERROR_CHECK(esp_event_loop_create_default()); // Bucle de eventos
    esp_netif_create_default_wifi_ap();               // Objeto de red WiFi

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // Valores por defecto
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));                // Activa el WiFi

    // Arranca el proceso de eventos del wifi (aqui no hace falta el de IP)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventos_wifi, NULL, NULL));

    // Preparacion de la red WIFI que crea el propio ESP32
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .password = WIFI_PASS,
            .channel = WIFI_CANAL,
            .max_connection = WIFI_MAX_CONEXIONES,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));                // Modo
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));  // Config
    ESP_ERROR_CHECK(esp_wifi_start());                               // Radio

    ESP_LOGI(TAG, "Red creada: %s (clave: %s)", WIFI_SSID, WIFI_PASS);
    ESP_LOGI(TAG, "Abre en el navegador:  http://192.168.4.1");
}

static void servidor_iniciar(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t servidor = NULL;

    ESP_ERROR_CHECK(httpd_start(&servidor, &config));

    static const httpd_uri_t uri_raiz = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_raiz,
    };
    static const httpd_uri_t uri_aleatorio = {
        .uri = "/aleatorio",
        .method = HTTP_GET,
        .handler = handler_aleatorio,
    };
    static const httpd_uri_t uri_enviar = {
        .uri = "/enviar",
        .method = HTTP_GET,
        .handler = handler_enviar,
    };

    httpd_register_uri_handler(servidor, &uri_raiz);
    httpd_register_uri_handler(servidor, &uri_aleatorio);
    httpd_register_uri_handler(servidor, &uri_enviar);

    ESP_LOGI(TAG, "Servidor HTTP en el puerto %d", config.server_port);
}

// -- 4.2 Procesos --

static void eventos_wifi(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t e =
            *(wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Se conecto: " MACSTR, MAC2STR(e.mac));
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t e =
            *(wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Se desconecto: " MACSTR, MAC2STR(e.mac));
    }
}

static esp_err_t handler_raiz(httpd_req_t *req)
{
    // req es el enlace con el socket que conecta con el navegador
    // -1: EMBED_TXTFILES agrega un byte nulo que no es del HTML
    const size_t largo = (index_html_fin - index_html_inicio) - 1;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_inicio, largo);
}

static esp_err_t handler_aleatorio(httpd_req_t *req)
{
    char texto[4];

    snprintf(texto, sizeof(texto), "%d", s_aleatorio);
    return httpd_resp_sendstr(req, texto);
}

static esp_err_t handler_enviar(httpd_req_t *req)
{
    char consulta[32];
    char texto[8];
    bool coincide = false;

    // /enviar?valor=42 -> consulta = "valor=42", texto = "42"
    if (httpd_req_get_url_query_str(req, consulta, sizeof(consulta)) ==
            ESP_OK &&
        httpd_query_key_value(consulta, "valor", texto, sizeof(texto)) ==
            ESP_OK)
    {
        coincide = atoi(texto) == VALOR_SECRETO;
    }

    gpio_set_level(LED_GPIO, coincide ? 0 : 1);
    return httpd_resp_sendstr(req, coincide ? "1" : "0");
}

static void tarea_aleatorio(void *arg)
{
    while (true)
    {
        s_aleatorio = esp_random() % 256;
        ESP_LOGI(TAG, "Aleatorio: %d", s_aleatorio);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
