/* ============================================================
 * Bytes - Comunicacion UART
 *
 * La ESP32 manda, cada cierto tiempo, un valor de una onda
 * senoidal entre 0 y 255. Al mismo tiempo, escucha si le llega
 * algun dato: si recibe el valor 128 prende el LED de la
 * tarjeta, y con cualquier otro valor lo apaga.
 *
 * Conexion: TX (GPIO43) y RX (GPIO44) de la XIAO ESP32-S3 hacia
 * el convertidor TTL-USB (o, mas adelante, hacia el HC-12).
 *
 * Organizacion: declaraciones, app_main, definiciones.
 * ============================================================ */
#include "1_Bytes.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"

/* ========== DECLARACIONES ========== */
/* ----- UART ----- */
#define UART_PUERTO UART_NUM_1 // UART aparte del de programacion
#define UART_BAUD 9600
#define PIN_TX 43 // TX/RX marcados en la XIAO
#define PIN_RX 44
/* ----- LED ----- */
#define PIN_LED 21 // LED integrado de la XIAO
#define LED_PRENDIDO 0 // logica invertida: 0=prendido
#define LED_APAGADO 1
/* ----- Protocolo ----- */
#define VALOR_PRENDE 128 // valor que prende el LED
#define DOS_PI 6.2832
// Configura el pin del LED como salida y lo deja apagado.
void pines_configurar(void);
// Configura el puerto UART (baudrate, pines, driver).
void uart_configurar(void);

/* ========== LLAMADO ========== */
void bytes_app_main(void)
{
    pines_configurar();
    uart_configurar();
    float angulo = 0;
    uint8_t contador = 0;
    while (1) {
        // ejemplo 1: onda senoidal, un byte (0-255)
        uint8_t valor_enviado = (uint8_t)(127 + 127 * sin(angulo));
        uart_write_bytes(UART_PUERTO, &valor_enviado, 1);
        angulo += 0.2;
        if (angulo > DOS_PI) {
            angulo = 0;
        }

        // ejemplo 2: texto fijo
        // char mensaje_fijo[] = "Hola\r\n";
        // uart_write_bytes(UART_PUERTO, mensaje_fijo, strlen(mensaje_fijo));

        // ejemplo 3: "Hola" + digito (0-9)
        // contador = (contador + 1) % 10;
        // char contador_char = contador + '0';
        // char mensaje[8] = "Hola";
        // mensaje[4] = contador_char;
        // mensaje[5] = '\r';
        // mensaje[6] = '\n';
        // uart_write_bytes(UART_PUERTO, mensaje, strlen(mensaje));

        // recibir: prender o apagar el LED
        uint8_t valor_recibido;
        int bytes_leidos =
            uart_read_bytes(UART_PUERTO, &valor_recibido, 1, 0);
        if (bytes_leidos > 0) {
            if (valor_recibido == VALOR_PRENDE) {
                gpio_set_level(PIN_LED, LED_PRENDIDO);
            } else {
                gpio_set_level(PIN_LED, LED_APAGADO);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // periodo: 500 ms
    }
}

/* ========== DEFINICIONES ========== */
void pines_configurar(void)
{
    gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LED, LED_APAGADO);
}
void uart_configurar(void)
{
    uart_config_t configuracion = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_param_config(UART_PUERTO, &configuracion);
    uart_set_pin(UART_PUERTO, PIN_TX, PIN_RX,
        UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PUERTO, 256, 256, 0, NULL, 0);
}
