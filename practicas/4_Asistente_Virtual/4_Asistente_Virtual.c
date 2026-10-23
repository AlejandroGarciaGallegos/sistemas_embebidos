/* ============================================================
 * Asistente virtual: primeras pruebas ESP32-S3 <-> Orange Pi.
 * Basado en 1_Microfono_UDP, 2_Servidor_web_WebSocket y 1_Barras.
 * Audio PCM por UDP; control y respuesta escrita por WebSocket.
 * No configura salida I2S ni bocina.
 * Organizacion: importaciones, declaraciones, app_main, funciones.
 * ============================================================ */

// ---------- 1) Importaciones ----------
#include "4_Asistente_Virtual.h"
#include "asistente_config.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "driver/i2s_pdm.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

// ---------- 2) Declaraciones ----------
#define TEXTO_MAX 384
#define CONTROL_MAX 768
#define AUDIO_CABECERA 16

typedef enum {
    ESPERANDO, ESCUCHANDO, TRANSCRIBIENDO, PENSANDO, RESPONDIENDO, ERROR_ESTADO
} estado_t;

// Todo lo compartido se copia bajo este cerrojo; nunca hay I/O dentro de el.
static portMUX_TYPE s_cerrojo = portMUX_INITIALIZER_UNLOCKED;
static struct {
    bool wifi, microfono, registrado, enviando;
    int cliente_fd;
    uint32_t cliente_ip, sesion, secuencia, enviados, errores_envio;
    int64_t ultimo_control, ultimo_registro;
    struct sockaddr_in destino;
    estado_t estado;
    int nivel;
    char ip[16];
    char texto[TEXTO_MAX + 1];
} s = {.cliente_fd = -1, .estado = ESPERANDO, .texto = "Conectando WiFi..."};

static const char *TAG = "asistente";
static const char *NOMBRES[] = {
    "esperando", "escuchando", "transcribiendo", "pensando", "respuesta", "error"
};
static int s_socket = -1;
static uint32_t s_arranque;
static i2s_chan_handle_t s_microfono;
#if OLED_HABILITADA
static u8g2_t s_oled;
#endif

static void wifi_iniciar(void);
static void eventos_wifi(void *, esp_event_base_t, int32_t, void *);
static void microfono_iniciar(void);
static void pantalla_iniciar(void);
static void servidor_iniciar(void);
static void udp_iniciar(void);
static void tarea_audio(void *);
static void tarea_udp(void *);
#if OLED_HABILITADA
static void tarea_pantalla(void *);
#endif
static void tarea_vigilancia(void *);
static esp_err_t handler_raiz(httpd_req_t *);
static esp_err_t handler_estado(httpd_req_t *);
static esp_err_t handler_ws(httpd_req_t *);
static void cliente_cerrar(httpd_handle_t, int);
static void estado_cambiar(estado_t estado, const char *texto);
static bool numero_leer(const char *texto, uint32_t *valor);
static int estado_json(char *salida, size_t capacidad);

// ---------- 3) Funcion principal ----------
void asistente_virtual_app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_arranque = esp_random();

    pantalla_iniciar();
    // esp_netif_init() prepara lwIP dentro de wifi_iniciar(). Debe ejecutarse
    // ANTES de socket()/httpd_start(), incluso si aun no se ha obtenido IP.
    // Abrir UDP primero causa "tcpip_send_msg_wait_sem: Invalid mbox" y reinicios.
    ESP_LOGI(TAG, "Arranque: inicializando TCP/IP y WiFi antes de los sockets");
    wifi_iniciar();
    microfono_iniciar();
    udp_iniciar();
    servidor_iniciar();

    ESP_ERROR_CHECK(xTaskCreate(tarea_vigilancia, "vigilar", 3072, NULL, 2,
                               NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_LOGI(TAG, "Configurar Orange Pi con la IP que aparezca abajo.");
}

// ---------- 4) Configuracion ----------
static void estado_cambiar(estado_t estado, const char *texto)
{
    portENTER_CRITICAL(&s_cerrojo);
    s.estado = estado;
    snprintf(s.texto, sizeof(s.texto), "%.*s", TEXTO_MAX, texto);
    portEXIT_CRITICAL(&s_cerrojo);
}

static void wifi_iniciar(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, eventos_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, eventos_wifi, NULL, NULL));
    wifi_config_t red = {.sta = {
        .ssid = ASISTENTE_WIFI_SSID, .password = ASISTENTE_WIFI_PASS,
    }};
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &red));
    ESP_ERROR_CHECK(esp_wifi_start());
    // En las primeras pruebas buscamos continuidad del audio.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static void eventos_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Conectando a la red WiFi: %s", ASISTENTE_WIFI_SSID);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        estado_cambiar(ESPERANDO, "WiFi asociado. Esperando IP...");
        ESP_LOGI(TAG, "WiFi asociado; esperando direccion IP por DHCP...");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *evento = data;
        unsigned motivo = evento->reason;
        portENTER_CRITICAL(&s_cerrojo);
        s.wifi = false;
        s.enviando = false;
        s.registrado = false;
        portEXIT_CRITICAL(&s_cerrojo);
        estado_cambiar(ERROR_ESTADO, "Sin WiFi. Reconectando...");
        ESP_LOGW(TAG, "WiFi desconectado (motivo=%u). Reintentando...", motivo);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        portENTER_CRITICAL(&s_cerrojo);
        s.wifi = true;
        memcpy(s.ip, ip, sizeof(ip));
        portEXIT_CRITICAL(&s_cerrojo);
        estado_cambiar(ESPERANDO, "Lista. Esperando Orange Pi");
        ESP_LOGI(TAG, "IP ESP32: %s | HTTP/WS: %d | UDP: %d", ip,
                 CONTROL_PUERTO_HTTP, AUDIO_PUERTO_UDP);
    }
}

static void microfono_iniciar(void)
{
    i2s_chan_config_t canal = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&canal, NULL, &s_microfono);
    if (err == ESP_OK) {
        i2s_pdm_rx_config_t pdm = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUDIO_FRECUENCIA),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(
                I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {.clk = MIC_PIN_CLK, .din = MIC_PIN_DATA},
        };
        err = i2s_channel_init_pdm_rx_mode(s_microfono, &pdm);
    }
    if (err == ESP_OK) err = i2s_channel_enable(s_microfono);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Microfono: %s. Se mantienen disponibles las pruebas de red.",
                 esp_err_to_name(err));
        return;
    }
    portENTER_CRITICAL(&s_cerrojo);
    s.microfono = true;
    portEXIT_CRITICAL(&s_cerrojo);
}

static void udp_iniciar(void)
{
    s_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_ERROR_CHECK(s_socket >= 0 ? ESP_OK : ESP_FAIL);
    struct sockaddr_in propia = {
        .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(AUDIO_PUERTO_UDP),
    };
    ESP_ERROR_CHECK(bind(s_socket, (struct sockaddr *)&propia, sizeof(propia)) == 0
                    ? ESP_OK : ESP_FAIL);
    struct timeval espera = {.tv_sec = 1};
    setsockopt(s_socket, SOL_SOCKET, SO_RCVTIMEO, &espera, sizeof(espera));
    ESP_ERROR_CHECK(xTaskCreate(tarea_udp, "registro_udp", 3072, NULL, 4,
                               NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    if (s.microfono) {
        ESP_ERROR_CHECK(xTaskCreatePinnedToCore(tarea_audio, "audio", 4096, NULL,
                         5, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    }
}

static void servidor_iniciar(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONTROL_PUERTO_HTTP;
    cfg.stack_size = 6144;
    cfg.close_fn = cliente_cerrar;
    cfg.recv_wait_timeout = 3;
    cfg.send_wait_timeout = 3;
    httpd_handle_t servidor = NULL;
    ESP_ERROR_CHECK(httpd_start(&servidor, &cfg));
    const httpd_uri_t raiz = {.uri = "/", .method = HTTP_GET, .handler = handler_raiz};
    const httpd_uri_t estado = {.uri = "/estado", .method = HTTP_GET, .handler = handler_estado};
    const httpd_uri_t ws = {.uri = "/ws", .method = HTTP_GET,
                            .handler = handler_ws, .is_websocket = true};
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &raiz));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &estado));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &ws));
}

static void pantalla_iniciar(void)
{
#if OLED_HABILITADA
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = OLED_PIN_SDA;
    hal.bus.i2c.scl = OLED_PIN_SCL;
    u8g2_esp32_hal_init(hal);
#if OLED_ES_SH1106
    u8g2_Setup_sh1106_i2c_128x64_noname_f(&s_oled, U8G2_R0,
        u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#else
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&s_oled, U8G2_R0,
        u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);
#endif
    u8x8_SetI2CAddress(&s_oled.u8x8, OLED_DIRECCION << 1);
    u8g2_InitDisplay(&s_oled);
    u8g2_SetPowerSave(&s_oled, 0);
    ESP_ERROR_CHECK(xTaskCreate(tarea_pantalla, "cara_oled", 4096, NULL, 2,
                               NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
#endif
}

// ---------- 5) Control HTTP / WebSocket ----------
static bool numero_leer(const char *texto, uint32_t *valor)
{
    if (!texto || *texto < '0' || *texto > '9') return false;
    errno = 0;
    char *fin;
    unsigned long n = strtoul(texto, &fin, 10);
    if (errno || *fin || n == 0 || n > UINT32_MAX) return false;
    *valor = (uint32_t)n;
    return true;
}

static int estado_json(char *salida, size_t capacidad)
{
    portENTER_CRITICAL(&s_cerrojo);
    bool wifi = s.wifi, mic = s.microfono, audio = s.enviando, udp = s.registrado;
    uint32_t sesion = s.sesion, enviados = s.enviados, errores = s.errores_envio;
    estado_t estado = s.estado;
    portEXIT_CRITICAL(&s_cerrojo);
    return snprintf(salida, capacidad,
        "{\"ok\":true,\"version\":1,\"arranque\":%" PRIu32 ","
        "\"wifi\":%s,\"microfono\":%s,\"audio\":%s,\"udp\":%s,"
        "\"sesion\":%" PRIu32 ",\"paquetes\":%" PRIu32 ",\"errores_envio\":%" PRIu32 ","
        "\"estado\":\"%s\",\"frecuencia\":%d,\"muestras\":%d}",
        s_arranque, wifi ? "true" : "false", mic ? "true" : "false",
        audio ? "true" : "false", udp ? "true" : "false", sesion, enviados,
        errores, NOMBRES[estado], AUDIO_FRECUENCIA, AUDIO_MUESTRAS);
}

static esp_err_t handler_raiz(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req,
        "Practica 4: asistente virtual\nGET /estado: diagnostico JSON\n"
        "WebSocket /ws: control desde asistente.py\nUDP 3333: audio PCM\n"
        "Esta version responde por texto y no utiliza bocina.\n");
}

static esp_err_t handler_estado(httpd_req_t *req)
{
    char json[512];
    estado_json(json, sizeof(json));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static void cliente_cerrar(httpd_handle_t servidor, int fd)
{
    bool era_cliente;
    portENTER_CRITICAL(&s_cerrojo);
    era_cliente = s.cliente_fd == fd;
    if (era_cliente) {
        s.cliente_fd = -1;
        s.enviando = false;
        s.registrado = false;
    }
    portEXIT_CRITICAL(&s_cerrojo);
    if (era_cliente) estado_cambiar(ESPERANDO, "Orange Pi desconectada");
    close(fd); // al instalar close_fn, HTTPD delega aqui el cierre real
}

static esp_err_t handler_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // Compatibilidad con versiones que invocan el handler durante el GET.
        // ESP-IDF 6.1 resuelve el handshake sin llamar aqui: el registro real
        // se hace con el primer frame (PING), no depende de recibir este GET.
        return ESP_OK;
    }

    httpd_ws_frame_t trama = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &trama, 0);
    if (err != ESP_OK) return err;
    if (trama.len > CONTROL_MAX || trama.type != HTTPD_WS_TYPE_TEXT || !trama.final)
        return ESP_FAIL;
    char mensaje[CONTROL_MAX + 1];
    trama.payload = (uint8_t *)mensaje;
    if (trama.len) {
        err = httpd_ws_recv_frame(req, &trama, trama.len);
        if (err != ESP_OK) return err;
    }
    mensaje[trama.len] = 0;
    // El cliente Python normaliza el texto para esta fuente ASCII del OLED.
    for (size_t i = 0; i < trama.len; i++) {
        if ((unsigned char)mensaje[i] < 32 || (unsigned char)mensaje[i] > 126)
            mensaje[i] = ' ';
    }
    struct sockaddr_in remoto;
    socklen_t tam = sizeof(remoto);
    if (getpeername(fd, (struct sockaddr *)&remoto, &tam) != 0) return ESP_FAIL;
    portENTER_CRITICAL(&s_cerrojo);
    bool ocupado = s.cliente_fd >= 0 && s.cliente_fd != fd;
    if (!ocupado) {
        s.cliente_fd = fd;
        s.cliente_ip = remoto.sin_addr.s_addr;
        s.ultimo_control = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_cerrojo);
    if (ocupado) {
        char rechazo[] = "{\"ok\":false,\"error\":\"cliente_ocupado\"}";
        httpd_ws_frame_t salida = {.type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)rechazo, .len = strlen(rechazo)};
        httpd_ws_send_frame(req, &salida);
        return ESP_FAIL; // cierra solo el segundo cliente, sin afectar al primero
    }

    const char *error = NULL;
    if (strcmp(mensaje, "PING") == 0) {
        // El estado completo sirve tambien como respuesta al latido.
    } else if (strcmp(mensaje, "DETENER") == 0) {
        portENTER_CRITICAL(&s_cerrojo);
        s.enviando = false;
        portEXIT_CRITICAL(&s_cerrojo);
    } else if (strncmp(mensaje, "INICIAR|", 8) == 0) {
        uint32_t sesion;
        if (!numero_leer(mensaje + 8, &sesion)) error = "sesion_invalida";
        else {
            portENTER_CRITICAL(&s_cerrojo);
            if (!s.microfono) error = "microfono_no_disponible";
            else if (!s.wifi || !s.registrado || s.sesion != sesion)
                error = "registrar_udp_primero";
            else {
                s.secuencia = 0;
                s.enviados = 0;
                s.errores_envio = 0;
                s.enviando = true;
            }
            portEXIT_CRITICAL(&s_cerrojo);
            if (!error) estado_cambiar(ESCUCHANDO, "Habla ahora...");
        }
    } else if (strncmp(mensaje, "TEXTO|", 6) == 0) {
        estado_cambiar(RESPONDIENDO, mensaje + 6);
        ESP_LOGI(TAG, "Respuesta: %.*s", TEXTO_MAX, mensaje + 6);
    } else if (strncmp(mensaje, "ESTADO|", 7) == 0) {
        char *nombre = mensaje + 7;
        char *texto = strchr(nombre, '|');
        if (!texto) error = "estado_sin_texto";
        else {
            *texto++ = 0;
            int i;
            for (i = 0; i < 6; i++) if (strcmp(nombre, NOMBRES[i]) == 0) break;
            if (i == 6) error = "estado_invalido";
            else estado_cambiar((estado_t)i, texto);
        }
    } else error = "comando_desconocido";

    char respuesta[512];
    if (error) snprintf(respuesta, sizeof(respuesta), "{\"ok\":false,\"error\":\"%s\"}", error);
    else estado_json(respuesta, sizeof(respuesta));
    httpd_ws_frame_t salida = {.type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)respuesta, .len = strlen(respuesta)};
    return httpd_ws_send_frame(req, &salida);
}

// ---------- 6) Audio y registro UDP ----------
static void tarea_udp(void *arg)
{
    char mensaje[64];
    while (true) {
        struct sockaddr_in origen;
        socklen_t tam = sizeof(origen);
        int n = recvfrom(s_socket, mensaje, sizeof(mensaje) - 1, 0,
                         (struct sockaddr *)&origen, &tam);
        if (n <= 0) continue;
        mensaje[n] = 0;
        uint32_t sesion;
        if (strncmp(mensaje, "R|", 2) || !numero_leer(mensaje + 2, &sesion)) continue;
        portENTER_CRITICAL(&s_cerrojo);
        bool permitido = s.wifi && s.cliente_fd >= 0 && origen.sin_addr.s_addr == s.cliente_ip;
        if (permitido) {
            if (s.sesion != sesion) s.enviando = false;
            s.destino = origen;
            s.sesion = sesion;
            s.registrado = true;
            s.ultimo_registro = esp_timer_get_time();
        }
        portEXIT_CRITICAL(&s_cerrojo);
        if (permitido) {
            char respuesta[32];
            int len = snprintf(respuesta, sizeof(respuesta), "OK|%" PRIu32, sesion);
            sendto(s_socket, respuesta, len, MSG_DONTWAIT, (struct sockaddr *)&origen, tam);
        }
    }
}

static void tarea_audio(void *arg)
{
    // Cabecera de red: magic[4], sesion u32, secuencia u32, muestras u16, Hz u16.
    // Los campos numericos de cabecera son big endian; el PCM es little endian.
    uint8_t paquete[AUDIO_CABECERA + AUDIO_MUESTRAS * sizeof(int16_t)];
    int16_t muestras[AUDIO_MUESTRAS];
    size_t pendientes = 0;
    memcpy(paquete, "AUDP", 4);
    const uint16_t cantidad = htons(AUDIO_MUESTRAS), hz = htons(AUDIO_FRECUENCIA);
    memcpy(paquete + 12, &cantidad, 2);
    memcpy(paquete + 14, &hz, 2);
    while (true) {
        size_t leidos = 0;
        esp_err_t err = i2s_channel_read(s_microfono, (uint8_t *)muestras + pendientes,
                           sizeof(muestras) - pendientes, &leidos, 1000);
        pendientes += leidos;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "Lectura I2S: %s", esp_err_to_name(err));
            pendientes = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (pendientes < sizeof(muestras)) continue;
        pendientes = 0;
        int32_t suma = 0;
        int nivel = 0;
        for (int i = 0; i < AUDIO_MUESTRAS; i++) suma += muestras[i];
        int centro = suma / AUDIO_MUESTRAS;
        for (int i = 0; i < AUDIO_MUESTRAS; i++) {
            int distancia = abs(muestras[i] - centro);
            if (distancia > nivel) nivel = distancia;
        }
        portENTER_CRITICAL(&s_cerrojo);
        s.nivel = nivel;
        bool enviar = s.enviando && s.wifi && s.registrado;
        struct sockaddr_in destino = s.destino;
        uint32_t sesion = htonl(s.sesion), secuencia = htonl(s.secuencia);
        if (enviar) s.secuencia++;
        portEXIT_CRITICAL(&s_cerrojo);
        if (!enviar) continue;
        memcpy(paquete + 4, &sesion, 4);
        memcpy(paquete + 8, &secuencia, 4);
        memcpy(paquete + AUDIO_CABECERA, muestras, sizeof(muestras));
        int n = sendto(s_socket, paquete, sizeof(paquete), MSG_DONTWAIT,
                       (struct sockaddr *)&destino, sizeof(destino));
        portENTER_CRITICAL(&s_cerrojo);
        if (n == sizeof(paquete)) s.enviados++;
        else s.errores_envio++;
        portEXIT_CRITICAL(&s_cerrojo);
    }
}

static void tarea_vigilancia(void *arg)
{
    while (true) {
        int64_t ahora = esp_timer_get_time();
        int cerrar = -1;
        bool vencio = false;
        portENTER_CRITICAL(&s_cerrojo);
        if (s.registrado && ahora - s.ultimo_registro > CLIENTE_ESPERA_MS * 1000LL) {
            s.registrado = false;
            vencio = s.enviando;
            s.enviando = false;
        }
        if (s.cliente_fd >= 0 && ahora - s.ultimo_control > CLIENTE_ESPERA_MS * 1000LL) {
            cerrar = s.cliente_fd;
            s.enviando = false;
        }
        portEXIT_CRITICAL(&s_cerrojo);
        // shutdown despierta al servidor; close_fn realiza el cierre y libera al cliente.
        if (cerrar >= 0) shutdown(cerrar, SHUT_RDWR);
        if (vencio) estado_cambiar(ERROR_ESTADO, "Registro UDP vencido. Repite prueba");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------- 7) Cara OLED: una sola tarea utiliza u8g2 ----------
#if OLED_HABILITADA
static void tarea_pantalla(void *arg)
{
    unsigned cuadro = 0, pagina = 0;
    char anterior[TEXTO_MAX + 1] = "";
    while (true) {
        char texto[TEXTO_MAX + 1], ip[16];
        portENTER_CRITICAL(&s_cerrojo);
        estado_t estado = s.estado;
        bool conectado = s.cliente_fd >= 0;
        int nivel = s.nivel;
        memcpy(texto, s.texto, sizeof(texto));
        memcpy(ip, s.ip, sizeof(ip));
        portEXIT_CRITICAL(&s_cerrojo);
        if (strcmp(anterior, texto)) {
            memcpy(anterior, texto, sizeof(texto));
            pagina = 0;
        }
        u8g2_ClearBuffer(&s_oled);
        u8g2_SetFont(&s_oled, u8g2_font_6x10_tr);
        u8g2_DrawStr(&s_oled, 0, 9, conectado ? NOMBRES[estado] : ip);
        int mirada = estado == PENSANDO ? ((cuadro / 5) % 3) * 3 - 3 : 0;
        bool parpadeo = cuadro % 43 == 0;
        for (int ojo = 0; ojo < 2; ojo++) {
            int x = 35 + ojo * 43 + mirada;
            if (estado == ERROR_ESTADO) {
                u8g2_DrawLine(&s_oled, x, 21, x + 13, 34);
                u8g2_DrawLine(&s_oled, x + 13, 21, x, 34);
            } else {
                u8g2_DrawRBox(&s_oled, x, parpadeo ? 27 : 19, 16,
                              parpadeo ? 3 : 17, 2);
            }
        }
        if (estado == ESCUCHANDO) {
            int ancho = nivel / 150;
            if (ancho > 110) ancho = 110;
            u8g2_DrawBox(&s_oled, 9, 39, ancho, 2);
        } else if (estado == RESPONDIENDO) {
            u8g2_DrawLine(&s_oled, 53, 37, 63, 40);
            u8g2_DrawLine(&s_oled, 63, 40, 73, 37);
        }
        // Dos renglones de 21 caracteres, avanzando cada ~2 s.
        size_t largo = strlen(texto);
        size_t paginas = largo ? (largo + 41) / 42 : 1;
        size_t inicio = (pagina % paginas) * 42;
        for (int renglon = 0; renglon < 2; renglon++) {
            char linea[22] = {0};
            size_t pos = inicio + renglon * 21;
            if (pos < largo) snprintf(linea, sizeof(linea), "%.21s", texto + pos);
            u8g2_DrawStr(&s_oled, 0, 51 + renglon * 12, linea);
        }
        u8g2_SendBuffer(&s_oled);
        cuadro++;
        if (cuadro % 20 == 0) pagina++;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif
