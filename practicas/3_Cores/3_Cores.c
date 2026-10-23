/* ============================================================
 * Cores - FreeRTOS
 * Experimento de saturacion: 5 tareas piden entre todas mas CPU de
 * la que un solo core puede dar, asi que el planificador se ve
 * obligado a repartirlas entre los dos cores. Dos estan fijadas (una
 * por core, de referencia visual); las otras tres no tienen afinidad
 * y migran donde haya hueco.
 *
 * Conexiones:
 *   OLED SSD1306/SH1106 (I2C)   SDA -> GPIO7   SCL -> GPIO44
 *
 * Fuentes: https://github.com/mkfrey/u8g2-hal-esp-idf
 *
 * Organizacion: declaraciones, app_main, definiciones.
 * ============================================================ */

#include "3_Cores.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "stdio.h"
#include "string.h"
#include "sdkconfig.h"

#include "u8g2.h"
#include "u8g2_esp32_hal.h"

/* ========== DECLARACIONES ========== */

static const char *TAG = "sh1106";

/* ----- I2C + OLED (SSD1306/SH1106, u8g2) ----- */
#define PIN_SDA 7
#define PIN_SCL 44

static u8g2_t mi_oled; // handler: display OLED via u8g2

// Inicializa el HAL de I2C que usa u8g2 (pines definidos arriba).
static esp_err_t i2c_iniciar(void);
// Configura el SSD1306, envia la secuencia de arranque y lo despierta.
static esp_err_t oled_iniciar(void);
// Tarea: dibuja las 5 cajas (banda = core, relleno = recibiendo cpu).
static void oled_mostrar(void *pvParameters);

/* ----- Saturacion de cores (tareas FreeRTOS) ----- */
#define STACK_SIZE 1024 * 2
#define N_TASKS 5

// estado compartido; volatile: se escribe en un core y se lee en otro
volatile int core_de[N_TASKS] = {0};
volatile bool trabajando[2] = {0, 0};      // solo tasks fijadas 1 y 2
volatile uint32_t visto_ms[N_TASKS] = {0}; // sello de vida de tasks 3 a 5

// Carga de las tasks 3 a 5: trabajan TRABAJO_MS y descansan
// DESCANSO_MS. Demanda = 3 * TRABAJO/(TRABAJO+DESCANSO) cores; con
// 20/80 son 0.6 cores. El resto de la saturacion la ponen las fases
// de las tasks 1 y 2 (ver vTask1/vTask2 abajo): durante la fase en
// que ambas trabajan, ningun core queda libre para las tasks 3 a 5.
#define TRABAJO_MS 20
#define DESCANSO_MS 80
#define FASE_MS 800     // duracion de cada fase de las tasks 1 y 2
#define TROZO_MS 50      // con que granularidad las fijadas revisan la fase
#define UMBRAL_MS 400    // sin sello mas reciente que esto, la task no tiene cpu
#define DISPLAY_DELAY 50

// desfases iniciales de las tasks 3 a 5, primos entre si para que no
// se sincronicen
#define D3 13
#define D4 29
#define D5 41

// geometria de las cajas (se evita la franja superior de la pantalla)
#define Y_BANDA0 18
#define Y_LINEA 38
#define Y_BANDA1 44
#define CAJA_W 20
#define CAJA_H 16
#define CAJA_X0 20 // deja lugar a la etiqueta "C0"/"C1" (~14px de ancho)
#define CAJA_DX 21

// fuente de las etiquetas "C0"/"C1" y de los numeros dentro de las
// cajas; monoespaciada, para poder centrar con aritmetica simple
#define CAJA_FONT u8g2_font_7x13B_tr
#define CAJA_FONT_W 7  // ancho de un caracter de CAJA_FONT
#define CAJA_TXT_DX ((CAJA_W - CAJA_FONT_W) / 2) // centra 1 digito en x
#define CAJA_TXT_DY 13 // linea base en y, centrada en CAJA_H

// Crea las 5 tareas de carga y la tarea que dibuja el OLED.
static esp_err_t tareas_crear(void);
// Milisegundos desde el arranque (32 bits: acceso atomico en Xtensa).
static uint32_t ahora_ms(void);
// Fase actual del reloj comun: 0 = solo t1, 1 = ambas, 2 = solo t2.
static int fase_actual(void);
// Ocupa el core en espera activa durante los ms indicados.
static void trabajar_ms(int ms);
// Fijada al core 0: carga su core en las fases 0 y 1.
static void vTask1(void *pvParameters);
// Fijada al core 1: carga su core en las fases 1 y 2.
static void vTask2(void *pvParameters);
// Sin afinidad, carga continua.
static void vTask3(void *pvParameters);
// Sin afinidad, carga continua.
static void vTask4(void *pvParameters);
// Sin afinidad, carga continua.
static void vTask5(void *pvParameters);

/* ========== LLAMADO ========== */

void cores_app_main(void)
{
    i2c_iniciar();
    oled_iniciar();
    tareas_crear();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGI(TAG, "fase=%d  cores: %d %d %d %d %d",
                 fase_actual(),
                 core_de[0], core_de[1], core_de[2], core_de[3],
                 core_de[4]);
    }
}

/* ========== DEFINICIONES ========== */
/* ----- I2C + OLED (SSD1306/SH1106, u8g2) ----- */

static esp_err_t i2c_iniciar(void)
{
    // 1. argumentos I2C para el HAL de u8g2
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.i2c.sda = PIN_SDA;
    u8g2_esp32_hal.bus.i2c.scl = PIN_SCL;
    // 2. inicializa el HAL
    u8g2_esp32_hal_init(u8g2_esp32_hal);
    return ESP_OK;
}

static esp_err_t oled_iniciar(void)
{
    // 1. geometria y controlador (SSD1306, 128x64, buffer completo)
    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&mi_oled, U8G2_R0,
                                            u8g2_esp32_i2c_byte_cb,
                                            u8g2_esp32_gpio_and_delay_cb);
    // 2. direccion I2C (recorrida 1 bit a la izquierda por u8x8)
    u8x8_SetI2CAddress(&mi_oled.u8x8, 0x78);
    // 3. secuencia de arranque; el display queda dormido
    u8g2_InitDisplay(&mi_oled);
    // 4. despierta el display
    u8g2_SetPowerSave(&mi_oled, 0);
    u8g2_ClearBuffer(&mi_oled);
    return ESP_OK;
}

static void oled_mostrar(void *pvParameters)
{
    char etiqueta[2] = {0, 0};
    bool activo[N_TASKS];

    while (1)
    {
        uint32_t t = ahora_ms();
        // tasks fijadas: usan su propio flag de trabajo
        activo[0] = trabajando[0];
        activo[1] = trabajando[1];
        // tasks sin afinidad (3 a 5): activas solo si dejaron un
        // sello reciente (no pueden avisar que les falta cpu; la
        // ausencia de actualizacion es la señal, igual que un
        // watchdog)
        for (int i = 2; i < N_TASKS; i++)
            activo[i] = (t - visto_ms[i]) < UMBRAL_MS;
        u8g2_ClearBuffer(&mi_oled);
        u8g2_SetDrawColor(&mi_oled, 1);
        u8g2_SetFont(&mi_oled, CAJA_FONT);
        // C0 arriba, C1 abajo, con una linea de separacion
        u8g2_DrawStr(&mi_oled, 0, Y_BANDA0 + CAJA_TXT_DY, "C0");
        u8g2_DrawStr(&mi_oled, 0, Y_BANDA1 + CAJA_TXT_DY, "C1");
        u8g2_DrawHLine(&mi_oled, 0, Y_LINEA, 128);
        for (int i = 0; i < N_TASKS; i++)
        {
            int x = CAJA_X0 + i * CAJA_DX;
            int y = (core_de[i] == 0) ? Y_BANDA0 : Y_BANDA1;
            // caja rellena = recibiendo cpu; vacia = sin cpu
            u8g2_SetDrawColor(&mi_oled, 1);
            if (activo[i])
                u8g2_DrawRBox(&mi_oled, x, y, CAJA_W, CAJA_H, 3);
            else
                u8g2_DrawRFrame(&mi_oled, x, y, CAJA_W, CAJA_H, 3);
            // numero centrado en la caja, en color inverso si esta rellena
            etiqueta[0] = '1' + i;
            u8g2_SetDrawColor(&mi_oled, activo[i] ? 0 : 1);
            u8g2_DrawStr(&mi_oled, x + CAJA_TXT_DX, y + CAJA_TXT_DY, etiqueta);
        }
        u8g2_SetDrawColor(&mi_oled, 1);
        u8g2_SendBuffer(&mi_oled);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_DELAY));
    }
}

/* ----- Saturacion de cores (tareas FreeRTOS) ----- */

static esp_err_t tareas_crear(void)
{
    // vTask1 y vTask2 fijadas con prioridad 2 (por encima de las 3
    // sin afinidad, que comparten prioridad 1): son la referencia
    // visual y no las desplaza la carga. Entre las 3 sin afinidad la
    // migracion la fuerza la saturacion total, no la prioridad.
    xTaskCreatePinnedToCore(vTask1, "vTask1", STACK_SIZE, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(vTask2, "vTask2", STACK_SIZE, NULL, 2, NULL, 1);

    xTaskCreatePinnedToCore(vTask3, "vTask3", STACK_SIZE, NULL, 1, NULL,
                             tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTask4, "vTask4", STACK_SIZE, NULL, 1, NULL,
                             tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTask5, "vTask5", STACK_SIZE, NULL, 1, NULL,
                             tskNO_AFFINITY);

    // prioridad 3: por encima de todo, la pantalla siempre refresca
    xTaskCreatePinnedToCore(oled_mostrar, "display", STACK_SIZE, NULL, 3,
                             NULL, tskNO_AFFINITY);
    return ESP_OK;
}

static uint32_t ahora_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int fase_actual(void)
{
    return (int)((ahora_ms() / FASE_MS) % 3);
}

static void trabajar_ms(int ms)
{
    int64_t fin = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < fin)
    {
        // trabajo simulado: espera activa, no cede el cpu
    }
}

static void vTask1(void *pvParameters)
{
    while (1)
    {
        core_de[0] = xPortGetCoreID();
        int f = fase_actual();

        if (f == 0 || f == 1)
        {
            trabajando[0] = true;
            trabajar_ms(TROZO_MS);
        }
        else
        {
            trabajando[0] = false;
            vTaskDelay(pdMS_TO_TICKS(TROZO_MS));
        }
    }
}

static void vTask2(void *pvParameters)
{
    while (1)
    {
        core_de[1] = xPortGetCoreID();
        int f = fase_actual();

        if (f == 1 || f == 2)
        {
            trabajando[1] = true;
            trabajar_ms(TROZO_MS);
        }
        else
        {
            trabajando[1] = false;
            vTaskDelay(pdMS_TO_TICKS(TROZO_MS));
        }
    }
}

static void vTask3(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(D3));

    while (1)
    {
        core_de[2] = xPortGetCoreID();
        visto_ms[2] = ahora_ms();
        trabajar_ms(TRABAJO_MS);
        vTaskDelay(pdMS_TO_TICKS(DESCANSO_MS));
    }
}

static void vTask4(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(D4));

    while (1)
    {
        core_de[3] = xPortGetCoreID();
        visto_ms[3] = ahora_ms();
        trabajar_ms(TRABAJO_MS);
        vTaskDelay(pdMS_TO_TICKS(DESCANSO_MS));
    }
}

static void vTask5(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(D5));

    while (1)
    {
        core_de[4] = xPortGetCoreID();
        visto_ms[4] = ahora_ms();
        trabajar_ms(TRABAJO_MS);
        vTaskDelay(pdMS_TO_TICKS(DESCANSO_MS));
    }
}
