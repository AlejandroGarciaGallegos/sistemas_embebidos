idf_component_register(SRCS "4_Asistente_Virtual.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_http_server esp_driver_i2s
                                esp_timer lwip u8g2 u8g2-hal-esp-idf)
