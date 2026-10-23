#ifndef ASISTENTE_CONFIG_H
#define ASISTENTE_CONFIG_H

// Editar estos dos datos antes de compilar, igual que en las otras practicas.
#define ASISTENTE_WIFI_SSID "HUAWEInova14"
#define ASISTENTE_WIFI_PASS "12345678"

// Mismo OLED y mismos pines que 1_Barras / 3_Cores.
// Poner OLED_HABILITADA en 0 permite probar la red sin conectar la pantalla.
#define OLED_HABILITADA 1
#define OLED_ES_SH1106 0
#define OLED_PIN_SDA 7
#define OLED_PIN_SCL 44
#define OLED_DIRECCION 0x3C // direccion de 7 bits; el codigo hace el desplazamiento

// Microfono PDM integrado en la XIAO ESP32S3 Sense.
#define MIC_PIN_CLK GPIO_NUM_42
#define MIC_PIN_DATA GPIO_NUM_41
#define AUDIO_FRECUENCIA 16000
#define AUDIO_MUESTRAS 512
#define AUDIO_PUERTO_UDP 3333
#define CONTROL_PUERTO_HTTP 80
#define CLIENTE_ESPERA_MS 8000

#endif
