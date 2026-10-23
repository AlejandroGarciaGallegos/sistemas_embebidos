// PROGRAMA 2: vigia de video con light sleep. Como el programa 1, pero
// con la camara en lugar del microfono. Cada MIRA_CADA_MS despierta,
// enciende la camara, compara dos fotos y mide el movimiento; si hay
// movimiento MOVIMIENTO_SEGUIDAS veces seguidas, manda video a la PC hasta
// que pasan ESPERA_QUIETO_MS sin movimiento. Las fotos van en gris,
// 96 x 96, sin comprimir. El cliente es
// ver_vigia.py. Partes nuevas: "// >>>".

// ---------- 1) Importaciones ----------
#include "2_STA_Video_Ahorro.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

// ---------- 2) Declaraciones ----------

#define WIFI_SSID "fr2.4g"
#define WIFI_PASS "GrayArrow123"

// IP fija: cada equipo usa la suya (201 a 211)
#define IP_ESP32 ESP_IP4TOADDR(192, 168, 8, 201)
#define IP_ROUTER ESP_IP4TOADDR(192, 168, 8, 1)
#define IP_MASCARA ESP_IP4TOADDR(255, 255, 255, 0)

// >>> Camara de la Sense (pines internos, igual que en la clase 14)
#define CAM_PIN_XCLK 10
#define CAM_PIN_SDA 40
#define CAM_PIN_SCL 39
#define CAM_PIN_D7 48
#define CAM_PIN_D6 11
#define CAM_PIN_D5 12
#define CAM_PIN_D4 14
#define CAM_PIN_D3 16
#define CAM_PIN_D2 18
#define CAM_PIN_D1 17
#define CAM_PIN_D0 15
#define CAM_PIN_VSYNC 38
#define CAM_PIN_HREF 47
#define CAM_PIN_PCLK 13
#define CAM_XCLK_HZ 20000000 // reloj del sensor; 20 MHz es lo probado

// LED de la XIAO (activo en bajo). Encendido = el vigia no duerme:
// busca el WiFi, mira (camara encendida) o esta enviando
#define LED_GPIO GPIO_NUM_21
#define LED_ENCENDIDO 0
#define LED_APAGADO 1

// >>> Foto: 96 x 96 en gris, sin comprimir: 1 byte por pixel (0 a 255)
#define FOTO_TAMANO FRAMESIZE_96X96
#define FOTO_ANCHO 96
#define FOTO_ALTO 96
#define FOTO_BYTES (FOTO_ANCHO * FOTO_ALTO) // 9216: cabe en un datagrama

// >>> El vigia: cada cuanto mira, y que es "movimiento"
#define MIRA_CADA_MS 2000     // tiempo dormido entre miradas
#define FOTOS_DESCARTE 3      // al encender, la camara ajusta la luz
#define PIXEL_CAMBIO 25       // un pixel "cambio" si vario mas de esto
#define MOVIMIENTO_UMBRAL 3   // % de pixeles que cambiaron (calibrar)
#define MOVIMIENTO_SEGUIDAS 1 // miradas con movimiento para despertar
#define ESPERA_QUIETO_MS 4000 // sigue enviando tras el ultimo movimiento
#define COMPARA_CADA_MS 500   // al enviar: compara con la foto de hace
                              // 0.5 s; con la de hace 0.05 s no se nota
#define SIGUE_UMBRAL 2        // al enviar: % para seguir (mas tolerante)

// Energia: la CPU baja a 40 MHz cuando no hay trabajo, y si todas las
// tareas esperan, el chip entra solo a light sleep
#define CPU_MAX_MHZ 160
#define CPU_MIN_MHZ 40

#define UDP_PUERTO 3333 // el mismo puerto en ver_vigia.py

// La tarea del vigia corre en el nucleo 1: el 0 es del WiFi
#define NUCLEO_ENVIO 1

// >>> La foto anterior, para comparar contra la nueva
static uint8_t s_anterior[FOTO_BYTES];

// >>> Candado de energia: mientras la camara esta encendida, el chip no
// >>> debe dormir (el driver del microfono lo hacia solo; la camara no)
static esp_pm_lock_handle_t s_candado;

// UDP: un solo socket para todo, y la direccion de la ultima PC que
// escribio (se aprende con recvfrom; sin ella no hay a quien mandar)
static int s_socket = -1;
static struct sockaddr_in s_pc_direccion;
static volatile bool s_pc_conocida = false;

// Configuracion
static void energia_iniciar(void);
static void led_iniciar(void);
static void vigia_iniciar(void);
static void wifi_iniciar(void);
static void udp_iniciar(void);

// Procesos
static void eventos_wifi(void *arg, esp_event_base_t base,
                         int32_t id, void *data);
static void tarea_escuchar(void *arg);
static void tarea_vigia(void *arg);
static void camara_encender(void);
static void camara_apagar(void);
static int mirar_un_momento(void);
static void transmitir_hasta_quieto(void);
static int movimiento_nivel(const uint8_t *foto);

// ---------- 3) Funcion principal ----------

void sta_video_ahorro_app_main(void)
{
    // Si la NVS quedo de otro programa, se borra
    if (nvs_flash_init() != ESP_OK)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    energia_iniciar();
    led_iniciar();
    vigia_iniciar();
    wifi_iniciar();
    udp_iniciar();
}

// ---------- 4) Funciones ----------

// -- 4.1 Configuracion --

static void energia_iniciar(void)
{
    // Administrador de energia: frecuencia variable y light sleep
    // automatico cuando ninguna tarea tiene nada que hacer
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = CPU_MAX_MHZ,
        .min_freq_mhz = CPU_MIN_MHZ,
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));

    // >>> El candado: mientras esta tomado, el chip no duerme ni baja la
    // >>> frecuencia (la camara necesita su reloj estable)
    ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "camara",
                                       &s_candado));
}

static void led_iniciar(void)
{
    // Encendido desde el arranque: se apaga al conectarse al WiFi
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_ENCENDIDO);
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

    // Modem sleep: el radio se apaga entre los beacons del router y
    // se enciende a tiempo para escucharlos: sigue conectado
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

// Conecta y reconecta. El LED muestra si hay WiFi
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

// Recibe "R" (registrarse): desde ahi se le manda el video a esa PC
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

        // UDP: se guarda quien escribio, para mandarle el video
        s_pc_direccion = origen;
        s_pc_conocida = true;
    }
}

// El vigia: duerme, mira un momento y decide si transmite
static void tarea_vigia(void *arg)
{
    int seguidas = 0; // miradas seguidas con movimiento

    while (true)
    {
        // 1. duerme: con todas las tareas esperando, el chip entra
        // solo a light sleep (y el WiFi sigue conectado)
        vTaskDelay(pdMS_TO_TICKS(MIRA_CADA_MS));

        // 2. mira un momento y cuenta si hubo movimiento
        if (mirar_un_momento() > MOVIMIENTO_UMBRAL)
        {
            seguidas = seguidas + 1;
        }
        else
        {
            seguidas = 0; // la quietud rompe la racha
        }

        // 3. movimiento sostenido: transmite hasta que todo quede quieto
        if (seguidas >= MOVIMIENTO_SEGUIDAS)
        {
            transmitir_hasta_quieto();
            seguidas = 0;
        }
    }
}

// >>> Toma el candado y enciende la camara. Encenderla cuesta: el driver
// >>> vuelve a configurar el sensor (unos cientos de ms)
static void camara_encender(void)
{
    esp_pm_lock_acquire(s_candado); // >>> desde aqui el chip no duerme

    // 1. pines, reloj y formato: gris, 160 x 120, sin comprimir
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

        .pixel_format = PIXFORMAT_GRAYSCALE, // 1 byte por pixel
        .frame_size = FOTO_TAMANO,           // 96 x 96

        // 2. dos buffers en la RAM interna (2 x 9 KB, sin PSRAM): mientras
        // uno se envia, la camara llena el otro. Siempre la foto mas nueva
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_DRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    ESP_ERROR_CHECK(esp_camera_init(&cam_cfg));
}

// >>> Apaga la camara y suelta el candado: el chip ya puede dormir
static void camara_apagar(void)
{
    esp_camera_deinit();
    esp_pm_lock_release(s_candado);
}

// >>> Enciende la camara, compara dos fotos y devuelve el movimiento
static int mirar_un_momento(void)
{
    camera_fb_t *foto = NULL;

    gpio_set_level(LED_GPIO, LED_ENCENDIDO); // mirando
    camara_encender();

    // Las primeras fotos salen oscuras: la camara esta ajustando la luz
    for (int i = 0; i < FOTOS_DESCARTE; i++)
    {
        foto = esp_camera_fb_get();
        if (foto != NULL)
        {
            esp_camera_fb_return(foto);
        }
    }

    // La primera foto solo se guarda; la segunda se compara con ella
    int nivel = 0;
    for (int i = 0; i < 2; i++)
    {
        foto = esp_camera_fb_get();
        if (foto == NULL)
        {
            continue; // sin foto: cuenta como "sin movimiento"
        }
        nivel = movimiento_nivel(foto->buf);
        esp_camera_fb_return(foto);
    }

    camara_apagar(); // apagada: el chip ya puede dormir
    gpio_set_level(LED_GPIO, LED_APAGADO);

    return nivel;
}

// >>> Manda todas las fotos mientras haya movimiento. Termina cuando
// >>> pasan ESPERA_QUIETO_MS sin movimiento
static void transmitir_hasta_quieto(void)
{
    int64_t ultimo_movimiento_us = esp_timer_get_time();
    int64_t ultima_comparacion_us = 0; // cuando se midio el movimiento

    gpio_set_level(LED_GPIO, LED_ENCENDIDO); // fase de envio
    camara_encender();
    while (esp_timer_get_time() - ultimo_movimiento_us <
           ESPERA_QUIETO_MS * 1000LL)
    {
        camera_fb_t *foto = esp_camera_fb_get();
        if (foto == NULL)
        {
            continue; // la camara no entrego foto: se intenta otra vez
        }

        // >>> Cada COMPARA_CADA_MS se mide el movimiento (contra la foto de
        // >>> la vez anterior). Con movimiento se reinicia la espera. Todas
        // >>> las fotos se mandan: el video no salta
        int64_t ahora_us = esp_timer_get_time();
        if (ahora_us - ultima_comparacion_us >= COMPARA_CADA_MS * 1000LL)
        {
            ultima_comparacion_us = ahora_us;
            if (movimiento_nivel(foto->buf) > SIGUE_UMBRAL)
            {
                ultimo_movimiento_us = ahora_us;
            }
        }

        if (s_pc_conocida)
        {
            sendto(s_socket, foto->buf, foto->len, 0,
                   (struct sockaddr *)&s_pc_direccion, sizeof(s_pc_direccion));
        }
        esp_camera_fb_return(foto); // devuelve el buffer a la camara
    }
    camara_apagar();
    gpio_set_level(LED_GPIO, LED_APAGADO); // de vuelta a dormir
}

// >>> Movimiento: que porcentaje de los pixeles cambio de verdad (mas de
// >>> PIXEL_CAMBIO niveles de gris) contra la foto anterior (0 a 100). El
// >>> ruido y los ajustes de luz mueven cada pixel solo unos niveles: no
// >>> cuentan. Deja esta foto como la anterior
static int movimiento_nivel(const uint8_t *foto)
{
    int32_t cambiaron = 0;
    for (int i = 0; i < FOTO_BYTES; i++)
    {
        if (abs(foto[i] - s_anterior[i]) > PIXEL_CAMBIO)
        {
            cambiaron = cambiaron + 1;
        }
    }
    memcpy(s_anterior, foto, FOTO_BYTES);

    return (cambiaron * 100) / FOTO_BYTES;
}
