/* ============================================================
 * Fluidez - FreeRTOS
 * Experimento "la pelota a tirones": dos pelotas rebotan, una por
 * core. Un acaparador satura el core 0 con espera activa; su pelota
 * se congela durante la rafaga, la del core 1 sigue fluida.
 *
 * Conexiones:
 *   OLED SSD1306/SH1106 (I2C)   SDA -> GPIO7   SCL -> GPIO44
 *
 * Fuentes: https://github.com/mkfrey/u8g2-hal-esp-idf
 *          https://www.youtube.com/watch?v=-6BBtYKekRw
 *
 * Organizacion: declaraciones, app_main, definiciones.
 * ============================================================ */

#include "2_Fluidez.h"
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
// Tarea: dibuja las dos pelotas y la franja de acaparamiento.
static void oled_mostrar(void *pvParameters);

/* ----- Pelota a tirones (tareas FreeRTOS) ----- */
#define STACK_SIZE 1024 * 2
#define X_MIN 24
#define X_MAX 124
#define PASO 3             // pixeles que avanza la pelota por paso
#define PELOTA_DELAY 20    // ms entre avances (~50 fps)
#define RAFAGA_US 3000000  // duracion de la espera activa del acaparador
#define TREGUA_MS 3000     // pausa del acaparador entre rafagas
#define DISPLAY_DELAY 50   // ms entre refrescos del oled

// posicion de cada pelota; volatile: se escriben en un core y se
// leen en otro
volatile int bx1 = 24;
volatile int bx2 = 24;
volatile bool acaparando = false; // true mientras el acaparador satura el core 0

// Crea las 2 pelotas, el acaparador y la tarea que dibuja el OLED.
static esp_err_t tareas_crear(void);
// Pelota del core 0: la afecta el acaparador.
static void vTask1(void *pvParameters);
// Pelota del core 1: nunca comparte core con el acaparador.
static void vTask2(void *pvParameters);
// Satura el core 0 con espera activa (no cede el CPU a proposito).
static void vTask3(void *pvParameters);

/* ========== LLAMADO ========== */

void fluidez_app_main(void)
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
    while (1)
    {
        u8g2_ClearBuffer(&mi_oled);
        u8g2_SetDrawColor(&mi_oled, 1);
        u8g2_SetFont(&mi_oled, u8g2_font_helvR08_tr);
        // franja superior encendida = acaparador activo en el core 0
        if (acaparando)
            u8g2_DrawBox(&mi_oled, 0, 0, 128, 5);
        u8g2_DrawStr(&mi_oled, 0, 38, "C0");
        u8g2_DrawDisc(&mi_oled, bx1, 34, 4, U8G2_DRAW_ALL);
        u8g2_DrawStr(&mi_oled, 0, 60, "C1");
        u8g2_DrawDisc(&mi_oled, bx2, 56, 4, U8G2_DRAW_ALL);
        u8g2_SendBuffer(&mi_oled);
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_DELAY));
    }
}

/* ----- Pelota a tirones (tareas FreeRTOS) ----- */

static esp_err_t tareas_crear(void)
{
    // vTask1 y vTask2: pelotas, una por core, misma prioridad
    // vTask3: acaparador con prioridad MAYOR, fijado al core 0
    // oled_mostrar en el core 1 para que la pantalla nunca se congele
    xTaskCreatePinnedToCore(vTask1, "pelota0", STACK_SIZE, NULL, 1, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTask2, "pelota1", STACK_SIZE, NULL, 1, NULL, tskNO_AFFINITY);
    xTaskCreatePinnedToCore(vTask3, "acapara", STACK_SIZE, NULL, 2, NULL, 0);

    xTaskCreatePinnedToCore(oled_mostrar, "display", STACK_SIZE, NULL, 1,
                             NULL, 1);
    return ESP_OK;
}

static void vTask1(void *pvParameters)
{
    int dir = PASO;
    while (1)
    {
        bx1 += dir;
        if (bx1 >= X_MAX || bx1 <= X_MIN)
            dir = -dir;
        vTaskDelay(pdMS_TO_TICKS(PELOTA_DELAY));
    }
}

static void vTask2(void *pvParameters)
{
    int dir = PASO;
    while (1)
    {
        bx2 += dir;
        if (bx2 >= X_MAX || bx2 <= X_MIN)
            dir = -dir;
        vTaskDelay(pdMS_TO_TICKS(PELOTA_DELAY));
    }
}

static void vTask3(void *pvParameters)
{
    while (1)
    {
        acaparando = true;
        int64_t fin = esp_timer_get_time() + RAFAGA_US;
        while (esp_timer_get_time() < fin)
        {
            // espera activa: no cede el CPU a proposito (por eso
            // "acapara"); el watchdog de tareas esta desactivado en
            // este proyecto, si no esto lo dispararia
        }
        acaparando = false;
        vTaskDelay(pdMS_TO_TICKS(TREGUA_MS));
    }
}
