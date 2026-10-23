// PRACTICA: el ESP32 funcionando como un router de verdad, con las tres
// garantias de un router domestico:
//   1) los dispositivos del AP navegan a internet a traves de la STA (NAT)
//   2) los dispositivos del AP NO ven la red de la STA (aislamiento)
//   3) los dispositivos de la red de la STA NO ven el servidor del AP

// ---------- 1) Importaciones ----------
#include "3_Servidor_web_Router.h"
#include "aislar_lan_estado.h" // estado compartido con el filtro de lwIP
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "driver/gpio.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>

// ---------- 2) Declaraciones ----------

// Red a la que el ESP32 se conecta (STA) -- la misma de la practica 1
#define WIFI_STA_SSID "fr2.4g"
#define WIFI_STA_PASS "GrayArrow123"

// Red que el ESP32 crea (AP) -- la misma de la practica 2
#define WIFI_AP_SSID "ESP32-CLASE"
#define WIFI_AP_PASS "12345678"
#define WIFI_AP_CANAL 1
#define WIFI_AP_MAX_CONEXIONES 4

#define LED_GPIO 21 // LED de usuario de la XIAO
#define VALOR_SECRETO 42

#define DHCPS_OPCION_DNS 0x02 // opcion 6 del protocolo DHCP: servidor DNS

static const char *TAG = "web";
static volatile uint8_t s_aleatorio = 0;
static bool s_internet_activo = false;
static uint32_t s_red_ap = 0;     // subred del AP, para la pieza 3
static uint32_t s_mascara_ap = 0; // mascara de esa subred

// El estado que lee el filtro de lwIP (declarado en aislar_lan_estado.h).
// Empieza "apagado": mientras la STA no tenga IP no hay nada que aislar.
//
// Cuando OTRA practica es la activa, el enlazador (con --gc-sections)
// descarta estas variables por "no usadas": no puede ver que
// aislar_lan_hook.h, compilado dentro de lwIP, las necesita. Se evita
// forzando el simbolo con "-u" desde el CMakeLists.txt de esta practica
// (el mismo mecanismo que usa el propio ESP-IDF para casos identicos).
bool g_aislar_activo = false;
uint32_t g_ip_sta = 0;
uint32_t g_red_sta = 0;
uint32_t g_mascara_sta = 0;

static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

// Simbolos del HTML embebido (componente web_comun)
extern const char index_html_inicio[] asm("_binary_index_html_start");
extern const char index_html_fin[] asm("_binary_index_html_end");

// Configuracion
static void led_iniciar(void);
static void aleatorio_iniciar(void);
static void wifi_iniciar(void);
static void servidor_iniciar(void);
static void ap_compartir_internet_aislado(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                          int32_t id, void *data);
static bool cliente_es_ap(httpd_req_t *req);
static esp_err_t handler_raiz(httpd_req_t *req);
static esp_err_t handler_aleatorio(httpd_req_t *req);
static esp_err_t handler_enviar(httpd_req_t *req);
static void tarea_aleatorio(void *arg);

// ---------- 3) Funcion principal ----------

void servidor_web_router_app_main(void)
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

    // Los dos objetos de red: uno para el AP, otro para la STA
    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // Valores por defecto
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));                // Activa el WiFi

    // Arranca los procesos basados en eventos del wifi y la ip
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &eventos_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &eventos_wifi, NULL, NULL));

    // wifi_config_t es un UNION: .ap y .sta comparten la misma memoria, por
    // eso no caben los dos en una sola variable como en las otras practicas
    // -- hacen falta dos variables y dos llamadas a esp_wifi_set_config
    wifi_config_t config_ap = {
        .ap = {
            .ssid = WIFI_AP_SSID,
            .ssid_len = strlen(WIFI_AP_SSID),
            .password = WIFI_AP_PASS,
            .channel = WIFI_AP_CANAL,
            .max_connection = WIFI_AP_MAX_CONEXIONES,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    wifi_config_t config_sta = {
        .sta = {
            .ssid = WIFI_STA_SSID,
            .password = WIFI_STA_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));            // Modo
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config_ap));   // AP
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config_sta)); // STA
    ESP_ERROR_CHECK(esp_wifi_start());                              // Radio

    // IP propia del AP (siempre 192.168.4.1 por defecto): la pieza 3 la usa
    // para reconocer "esta conexion si entro por el AP"
    esp_netif_ip_info_t info_ap;
    esp_netif_get_ip_info(s_netif_ap, &info_ap);
    s_mascara_ap = info_ap.netmask.addr;
    s_red_ap = info_ap.ip.addr & s_mascara_ap;

    ESP_LOGI(TAG, "Red creada: %s (clave: %s)", WIFI_AP_SSID, WIFI_AP_PASS);
    ESP_LOGI(TAG, "IP desde el AP:      http://192.168.4.1");
    ESP_LOGI(TAG, "  -> usa esta si estas conectado a %s", WIFI_AP_SSID);
    ESP_LOGI(TAG, "Conectando a %s ...", WIFI_STA_SSID);
}

// Copia el DNS de la STA al DHCP del AP, activa el NAT, y ACTIVA EL FILTRO
// que aisla al AP de la subred de la STA (pieza 2) -- sin este filtro,
// cualquier dispositivo conectado al AP podria ver el resto de la red de
// casa, no solo salir a internet a traves de ella.
static void ap_compartir_internet_aislado(void)
{
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns);

    uint8_t opcion_dns = DHCPS_OPCION_DNS;
    esp_netif_dhcps_stop(s_netif_ap);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                                            ESP_NETIF_DOMAIN_NAME_SERVER,
                                            &opcion_dns, sizeof(opcion_dns)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN,
                                            &dns));
    esp_netif_dhcps_start(s_netif_ap);

    esp_netif_set_default_netif(s_netif_sta);
    if (esp_netif_napt_enable(s_netif_ap) != ESP_OK)
    {
        ESP_LOGE(TAG, "No se pudo activar el NAT en el AP");
        return;
    }

    g_aislar_activo = true;
    ESP_LOGI(TAG, "Internet compartido, con la LAN de casa aislada del AP");
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
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGW(TAG, "STA sin conexion, reintentando...");
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t e =
            *(wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Se conecto al AP: " MACSTR, MAC2STR(e.mac));
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t e =
            *(wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Se desconecto del AP: " MACSTR, MAC2STR(e.mac));
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t e = *(ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "IP desde el router:  " IPSTR, IP2STR(&e.ip_info.ip));
        ESP_LOGI(TAG, "  -> NO sirve para abrir la pagina, es solo");
        ESP_LOGI(TAG, "     informativa (el ESP32 sale a internet por ahi)");
        ESP_LOGI(TAG, "Pagina solo desde el AP:  http://192.168.4.1");

        // Con la IP de la STA ya se puede calcular su subred (IP con la
        // mascara aplicada): es lo que el filtro usa para reconocerla, y
        // tambien para reconocer el netif de la STA (ver aislar_lan_hook.h)
        g_ip_sta = e.ip_info.ip.addr;
        g_mascara_sta = e.ip_info.netmask.addr;
        g_red_sta = e.ip_info.ip.addr & g_mascara_sta;

        if (!s_internet_activo)
        {
            ap_compartir_internet_aislado();
            s_internet_activo = true;
        }
    }
}

// Pieza 3: se prueba en cada peticion, dentro de los handlers -- no en
// open_fn. El primer intento comparaba justo ahi, al abrir la conexion
// (con getsockname primero, despues con getpeername): las dos veces la
// direccion volvio en 0.0.0.0, verificado con logs de depuracion. Mover la
// verificacion a los handlers no bastaba por si solo: seguia dando cero.
// La causa real, tambien encontrada con logs, es otra: este proyecto tiene
// IPv6 habilitado (CONFIG_LWIP_IPV6), asi que getpeername no llena una
// struct sockaddr_in -- llena una sockaddr_in6, con la IPv4 disfrazada de
// "direccion IPv6 mapeada" (::ffff:a.b.c.d). Interpretar esos bytes como
// si fueran una sockaddr_in leia el campo equivocado, y por eso siempre
// daba cero.
static bool cliente_es_ap(httpd_req_t *req)
{
    int sockfd = httpd_req_to_sockfd(req);
    struct sockaddr_storage cliente = {0};
    socklen_t len = sizeof(cliente);

    if (sockfd < 0 ||
        getpeername(sockfd, (struct sockaddr *)&cliente, &len) != 0)
    {
        return false; // no se pudo verificar: por seguridad, se rechaza
    }

    uint32_t ip_cliente;

    if (cliente.ss_family == AF_INET)
    {
        ip_cliente = ((struct sockaddr_in *)&cliente)->sin_addr.s_addr;
    }
    else if (cliente.ss_family == AF_INET6)
    {
        struct in6_addr *v6 = &((struct sockaddr_in6 *)&cliente)->sin6_addr;
        if (!IN6_IS_ADDR_V4MAPPED(v6))
        {
            return false; // IPv6 real (no mapeada): no es de esta subred
        }
        // Los ultimos 4 bytes de una direccion IPv4-mapeada son la IPv4
        memcpy(&ip_cliente, &v6->s6_addr[12], sizeof(ip_cliente));
    }
    else
    {
        return false;
    }

    return (ip_cliente & s_mascara_ap) == s_red_ap;
}

static esp_err_t handler_raiz(httpd_req_t *req)
{
    if (!cliente_es_ap(req))
    {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
        return ESP_FAIL;
    }

    // req es el enlace con el socket que conecta con el navegador
    // -1: EMBED_TXTFILES agrega un byte nulo que no es del HTML
    const size_t largo = (index_html_fin - index_html_inicio) - 1;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_inicio, largo);
}

static esp_err_t handler_aleatorio(httpd_req_t *req)
{
    if (!cliente_es_ap(req))
    {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
        return ESP_FAIL;
    }

    char texto[4];

    snprintf(texto, sizeof(texto), "%d", s_aleatorio);
    return httpd_resp_sendstr(req, texto);
}

static esp_err_t handler_enviar(httpd_req_t *req)
{
    if (!cliente_es_ap(req))
    {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, NULL);
        return ESP_FAIL;
    }

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
