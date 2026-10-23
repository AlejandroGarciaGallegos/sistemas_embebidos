// PROGRAMA 1: HTTP. La pagina pide el aleatorio; el ESP32 solo contesta.
// Las partes clave estan marcadas con "// >>>".

// ---------- 1) Importaciones ----------
#include "1_Servidor_web_STA.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

// ---------- 2) Declaraciones ----------

#define WIFI_SSID "fr2.4g"
#define WIFI_PASS "GrayArrow123"

#define LED_GPIO 21 // LED de usuario de la XIAO
#define VALOR_SECRETO 42

static const char *TAG = "web";
static volatile uint8_t s_aleatorio = 0;

// Pagina HTML embebida (componente web_comun)
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

void servidor_web_sta_app_main(void)
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

static void servidor_iniciar(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t servidor = NULL;

    // >>> TCP: crea el socket que escucha en el puerto 80.
    // >>> Por dentro hace socket(), bind(), listen() y accept().
    ESP_ERROR_CHECK(httpd_start(&servidor, &config));

    // >>> HTTP: cada ruta (URI) se atiende con su funcion (handler)
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
        ESP_LOGI(TAG, "Abre en el navegador:  http://" IPSTR,
                 IP2STR(&e.ip_info.ip));
    }
}

// >>> HTTP: GET / -> responde con la pagina
static esp_err_t handler_raiz(httpd_req_t *req)
{
    // -1: EMBED_TXTFILES agrega un byte nulo al final
    const size_t largo = (index_html_fin - index_html_inicio) - 1;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_inicio, largo);
}

// >>> HTTP: GET /aleatorio -> responde con el ultimo aleatorio.
// >>> El ESP32 solo lo manda cuando la pagina lo pide.
static esp_err_t handler_aleatorio(httpd_req_t *req)
{
    char texto[4];

    snprintf(texto, sizeof(texto), "%d", s_aleatorio);
    return httpd_resp_sendstr(req, texto);
}

// >>> HTTP: GET /enviar?valor=42 -> lee el valor de la URI y enciende el LED
static esp_err_t handler_enviar(httpd_req_t *req)
{
    char consulta[32];
    char texto[8];
    bool coincide = false;

    // "valor=42" -> "42"
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

// Genera un aleatorio por segundo. No lo manda: espera a que lo pidan.
static void tarea_aleatorio(void *arg)
{
    int64_t anterior_us = 0; // 0: todavia no hay uno anterior

    while (true)
    {
        s_aleatorio = esp_random() % 256;

        int64_t ahora_us = esp_timer_get_time();
        if (anterior_us == 0)
        {
            ESP_LOGI(TAG, "Aleatorio: %d", s_aleatorio);
        }
        else
        {
            double periodo_ms = (ahora_us - anterior_us) / 1000.0;
            ESP_LOGI(TAG, "Aleatorio: %d  (cada %.2f ms)  (%.1f Hz)",
                     s_aleatorio, periodo_ms, 1000.0 / periodo_ms);
        }
        anterior_us = ahora_us;

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
