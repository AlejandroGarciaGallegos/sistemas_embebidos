/* ============================================================
 * Barras - FreeRTOS
 * Experimento "la carrera": tres tareas compiten por CPU y cada
 * una mueve una barra en el OLED. La velocidad de la barra es el
 * rendimiento real del core que le toco.
 *
 * Conexiones:
 *   OLED SSD1306/SH1106 (I2C)   SDA -> GPIO7   SCL -> GPIO44
 *
 * Fuentes: https://github.com/mkfrey/u8g2-hal-esp-idf
 *          https://www.youtube.com/watch?v=-6BBtYKekRw
 *
 * Organizacion: declaraciones, app_main, definiciones.
 * ============================================================ */

#include "1_Barras.h"
#include "esp_log.h"
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
// Tarea: dibuja las tres barras segun pos1/pos2/pos3.
static void oled_mostrar(void *pvParameters);

/* ----- Carrera (tareas FreeRTOS) ----- */
#define STACK_SIZE 1024 * 2
#define TRABAJO 1000000 // iteraciones de computo por pixel de avance
#define DISPLAY_DELAY 100 // ms entre refrescos del oled

// posiciones de cada barra; volatile: se escriben en un core y se
// leen en otro
volatile int pos1 = 0;
volatile int pos2 = 0;
volatile int pos3 = 0;

// Crea las 3 tareas de carga y la tarea que dibuja el OLED.
static esp_err_t tareas_crear(void);
// Bucle de carga real; consume CPU para simular computo pesado.
static void trabajar(void);
// Avanza pos1 un paso por cada bloque de computo completado.
static void vTask1(void *pvParameters);
// Avanza pos2 un paso por cada bloque de computo completado.
static void vTask2(void *pvParameters);
// Avanza pos3 un paso por cada bloque de computo completado.
static void vTask3(void *pvParameters);

/* ========== LLAMADO ========== */

void barras_app_main(void)
{
    i2c_iniciar();
    oled_iniciar();
    tareas_crear();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Number of Cores = %i", portNUM_PROCESSORS);
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
    const int y[3] = {22, 36, 50};
    char etiqueta[2] = {0, 0};

    while (1)
    {
        const int p[3] = {pos1, pos2, pos3};
        u8g2_ClearBuffer(&mi_oled);
        u8g2_SetDrawColor(&mi_oled, 1);
        u8g2_SetFont(&mi_oled, u8g2_font_helvR08_tr);
        u8g2_DrawButtonUTF8(&mi_oled, 64, 10,
                             U8G2_BTN_HCENTER | U8G2_BTN_BW1, 100, 1, 1,
                             "Dual core");
        for (int i = 0; i < 3; i++)
        {
            etiqueta[0] = '1' + i;
            u8g2_DrawStr(&mi_oled, 0, y[i] + 9, etiqueta);
            u8g2_DrawFrame(&mi_oled, 10, y[i], 118, 11);
            u8g2_DrawBox(&mi_oled, 10, y[i], (p[i] * 118) / 128, 11);
        }
        u8g2_SendBuffer(&mi_oled);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_DELAY));
    }
}

/* ----- Carrera (tareas FreeRTOS) ----- */

static esp_err_t tareas_crear(void)
{
    // <<< aqui se juega con el experimento: cambiar el ultimo parametro >>>
    //     0              -> fijada al core 0 (PRO_CPU)
    //     1              -> fijada al core 1 (APP_CPU)
    //     tskNO_AFFINITY -> el scheduler decide
    xTaskCreatePinnedToCore(vTask1, "vTask1", STACK_SIZE, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(vTask2, "vTask2", STACK_SIZE, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(vTask3, "vTask3", STACK_SIZE, NULL, 1, NULL, 1);

    xTaskCreatePinnedToCore(oled_mostrar, "display", STACK_SIZE, NULL, 1,
                             NULL, tskNO_AFFINITY);
    return ESP_OK;
}

static void trabajar(void)
{
    // volatile: impide que el optimizador borre el bucle
    volatile uint32_t x = 0;
    for (uint32_t i = 0; i < TRABAJO; i++)
    {
        x += i;
    }
}

static void vTask1(void *pvParameters)
{
    while (1)
    {
        trabajar();
        pos1 = (pos1 + 1) % 128;
    }
}

static void vTask2(void *pvParameters)
{
    while (1)
    {
        trabajar();
        pos2 = (pos2 + 1) % 128;
    }
}

static void vTask3(void *pvParameters)
{
    while (1)
    {
        trabajar();
        pos3 = (pos3 + 1) % 128;
    }
}
