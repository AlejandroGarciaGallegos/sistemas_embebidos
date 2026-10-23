// PROGRAMA 2: camara por UDP. Como 3_Aleatorio_UDP, pero en lugar de un
// aleatorio manda fotos JPEG de la camara de la Sense, una por datagrama.
// El cliente es ver_video.py. Partes clave marcadas con "// >>>".

// ---------- 1) Importaciones ----------
#include "2_Camara_UDP.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "esp_camera.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------- 2) Declaraciones ----------

// #define WIFI_SSID "fr2.4g"
// #define WIFI_PASS "GrayArrow123"

// #define WIFI_SSID "IZZI-3B6C"
// #define WIFI_PASS "nfPRK96pL6kxe73HEr"

#define WIFI_SSID "HUAWEInova14"
#define WIFI_PASS "12345678"

// >>> Camara de la Sense (pines internos, no hay que cablear)
#define CAM_PIN_XCLK 10 // reloj que el ESP32 le da a la camara
#define CAM_PIN_SDA 40  // configuracion (tipo I2C)
#define CAM_PIN_SCL 39
#define CAM_PIN_D7 48 // 8 lineas de datos: un byte por pulso de PCLK
#define CAM_PIN_D6 11
#define CAM_PIN_D5 12
#define CAM_PIN_D4 14
#define CAM_PIN_D3 16
#define CAM_PIN_D2 18
#define CAM_PIN_D1 17
#define CAM_PIN_D0 15
#define CAM_PIN_VSYNC 38 // inicio de cuadro
#define CAM_PIN_HREF 47  // inicio de renglon
#define CAM_PIN_PCLK 13  // un pulso por byte

// >>> Los tres valores de la camara (ver README, seccion 5)
#define CAM_TAMANO FRAMESIZE_HVGA // resolucion, de QQVGA a QXGA
#define CAM_CALIDAD 20 // JPEG: 0 = nitida y pesada, 63 = ligera
#define CAM_XCLK_HZ 20000000 // reloj del sensor; 20 MHz es lo probado

#define UDP_PUERTO 3333 // el mismo puerto en ver_video.py

// La tarea que manda corre en el nucleo 1: el 0 es del WiFi.
// No hace falta periodo: la camara entrega una foto cuando la tiene.
#define NUCLEO_ENVIO 1

static const char *TAG = "camara";

// >>> UDP: un solo socket para todo, y la direccion de la ultima PC que
// >>> escribio (se aprende con recvfrom; sin ella no hay a quien mandar)
static int s_socket = -1;
static struct sockaddr_in s_pc_direccion;
static volatile bool s_pc_conocida = false;

// Configuracion
static void camara_iniciar(void);
static void video_iniciar(void);
static void wifi_iniciar(void);
static void udp_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static void tarea_escuchar(void *arg);
static void tarea_video(void *arg);

// ---------- 3) Funcion principal ----------

void camara_udp_app_main(void)
{
    // Si la NVS quedo de otro programa, se borra
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    camara_iniciar();
    video_iniciar();
    wifi_iniciar();
    udp_iniciar();
}

// ---------- 4) Funciones ----------

// -- 4.1 Configuracion --

static void camara_iniciar(void)
{
    // >>> 1. pines, reloj y formato de la foto
    camera_config_t cam_cfg = {
        .pin_pwdn = -1, // la Sense no los usa
        .pin_reset = -1,
        .pin_xclk = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SDA,
        .pin_sccb_scl = CAM_PIN_SCL,
        .pin_d7 = CAM_PIN_D7,
        .pin_d6 = CAM_PIN_D6,
        .pin_d5 = CAM_PIN_D5,
        .pin_d4 = CAM_PIN_D4,
        .pin_d3 = CAM_PIN_D3,
        .pin_d2 = CAM_PIN_D2,
        .pin_d1 = CAM_PIN_D1,
        .pin_d0 = CAM_PIN_D0,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,

        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0, // el XCLK se genera con el LEDC
        .ledc_channel = LEDC_CHANNEL_0,

        .pixel_format = PIXFORMAT_JPEG, // la camara ya entrega JPEG
        .frame_size = CAM_TAMANO,
        .jpeg_quality = CAM_CALIDAD,

        // >>> 2. dos buffers en la PSRAM: mientras uno se envia, la
        // >>> camara llena el otro. Siempre se entrega la foto mas nueva
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    // >>> 3. arranca: detecta el sensor (OV2640 u OV3660) y lo configura
    ESP_ERROR_CHECK(esp_camera_init(&cam_cfg));
}

static void video_iniciar(void)
{
    // Tarea fijada al nucleo 1, para no quitarle CPU al WiFi (nucleo 0)
    xTaskCreatePinnedToCore(tarea_video, "video", 4096, NULL, 5,
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
        ESP_LOGI(TAG, "Usa esta IP en ver_video.py:  " IPSTR,
                 IP2STR(&e.ip_info.ip));
    }
}

// Recibe "R" (registrarse): desde ahi se le manda el video a esa PC
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

        // >>> UDP: se guarda quien escribio, para mandarle el video
        s_pc_direccion = origen;
        s_pc_conocida = true;
        ESP_LOGI(TAG, "Mandando video a %s", inet_ntoa(origen.sin_addr));
    }
}

// Toma una foto y la manda por UDP
static void tarea_video(void *arg)
{
    while (true)
    {
        // >>> Espera la foto mas nueva; queda prestada hasta devolverla.
        // >>> Mientras espera suelta el nucleo: no hace falta vTaskDelay
        camera_fb_t *foto = esp_camera_fb_get();
        if (foto == NULL)
        {
            continue; // la camara no entrego foto: se intenta otra vez
        }

        // Si nadie ha escrito todavia, no hay a quien mandarlo
        if (s_pc_conocida)
        {
            // >>> UDP: manda la foto completa en un solo datagrama. No cabe
            // >>> en un paquete de WiFi: la pila de red la parte sola y la
            // >>> PC la rearma. Si se pierde un pedazo, se pierde la foto
            sendto(s_socket, foto->buf, foto->len, 0,
                   (struct sockaddr *)&s_pc_direccion, sizeof(s_pc_direccion));
        }

        // >>> Devuelve el buffer a la camara para que lo vuelva a llenar
        esp_camera_fb_return(foto);
    }
}
