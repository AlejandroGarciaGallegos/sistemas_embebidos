// PROGRAMA 2: WebSocket. La misma pagina del programa 1, pero el ESP32
// empuja el aleatorio sin que se lo pidan. Partes clave marcadas con "// >>>".

// ---------- 1) Importaciones ----------
#include "2_Servidor_web_WebSocket.h"
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

// La tarea que manda corre en el nucleo 1: el 0 es del WiFi.
// Periodo en microsegundos (vTaskDelay no baja de 1 tick).
#define PERIODO_ENVIO_US 2000
#define NUCLEO_ENVIO 1

static const char *TAG = "web";
static volatile uint8_t s_aleatorio = 0;
static httpd_handle_t s_servidor = NULL; // para enviar desde la tarea

// Pagina HTML embebida (componente web_comun)
extern const char index_html_inicio[] asm("_binary_index_ws_html_start");
extern const char index_html_fin[] asm("_binary_index_ws_html_end");

// Configuracion
static void led_iniciar(void);
static void aleatorio_iniciar(void);
static void wifi_iniciar(void);
static void servidor_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static esp_err_t handler_raiz(httpd_req_t *req);
static esp_err_t handler_ws(httpd_req_t *req);
static esp_err_t handler_enviar(httpd_req_t *req);
static void ws_enviar_a_todos(void);
static void tarea_aleatorio(void *arg);

// ---------- 3) Funcion principal ----------

void servidor_web_websocket_app_main(void)
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

static void servidor_iniciar(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t servidor = NULL;

    // >>> TCP: crea el socket que escucha en el puerto 80 (igual que el programa 1)
    ESP_ERROR_CHECK(httpd_start(&servidor, &config));

    static const httpd_uri_t uri_raiz = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_raiz,
    };
    // >>> WEBSOCKET: la ruta /ws acepta el cambio de HTTP a WebSocket.
    // >>> Con is_websocket, el servidor contesta solo el "101 Switching Protocols".
    static const httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = handler_ws,
        .is_websocket = true,
    };
    static const httpd_uri_t uri_enviar = {
        .uri = "/enviar",
        .method = HTTP_GET,
        .handler = handler_enviar,
    };

    httpd_register_uri_handler(servidor, &uri_raiz);
    httpd_register_uri_handler(servidor, &uri_ws);
    httpd_register_uri_handler(servidor, &uri_enviar);
    s_servidor = servidor;

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

// GET / -> responde con la pagina
static esp_err_t handler_raiz(httpd_req_t *req)
{
    // -1: EMBED_TXTFILES agrega un byte nulo al final
    const size_t largo = (index_html_fin - index_html_inicio) - 1;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_inicio, largo);
}

// >>> WEBSOCKET: atiende la conexion /ws
static esp_err_t handler_ws(httpd_req_t *req)
{
    // El GET es el saludo: el 101 ya se envio; la conexion queda abierta
    if (req->method == HTTP_GET)
    {
        return ESP_OK;
    }

    // Mensaje de la pagina (esta no manda ninguno): se lee y se descarta
    uint8_t mensaje[8];
    httpd_ws_frame_t trama = {.payload = mensaje};
    return httpd_ws_recv_frame(req, &trama, sizeof(mensaje));
}

// GET /enviar?valor=42 -> igual que el programa 1 (sigue siendo HTTP)
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

// >>> WEBSOCKET: el ESP32 empuja el aleatorio a cada pagina abierta,
// >>> sin que se lo pidan
static void ws_enviar_a_todos(void)
{
    char texto[4];
    int clientes[CONFIG_LWIP_MAX_SOCKETS];
    size_t n = CONFIG_LWIP_MAX_SOCKETS;

    if (s_servidor == NULL ||
        httpd_get_client_list(s_servidor, &n, clientes) != ESP_OK)
    {
        return; // el servidor aun no arranca
    }

    // Un mensaje (frame) de texto con el numero
    httpd_ws_frame_t trama = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)texto,
        .len = snprintf(texto, sizeof(texto), "%d", s_aleatorio),
    };

    // clientes[] son sockets: solo se envia a los que ya son WebSocket
    for (size_t i = 0; i < n; i++)
    {
        if (httpd_ws_get_fd_info(s_servidor, clientes[i]) ==
            HTTPD_WS_CLIENT_WEBSOCKET)
        {
            httpd_ws_send_data(s_servidor, clientes[i], &trama);
        }
    }
}

// Genera un aleatorio cada PERIODO_ENVIO_US y lo manda por WebSocket
static void tarea_aleatorio(void *arg)
{
    int64_t anterior_us = 0; // 0: todavia no hay uno anterior
    int64_t siguiente_us = esp_timer_get_time();
    int64_t ultimo_ceder_us = siguiente_us;

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

        ws_enviar_a_todos();

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
