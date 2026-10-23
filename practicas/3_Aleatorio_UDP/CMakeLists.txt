idf_component_register(SRCS "3_Aleatorio_UDP.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_driver_gpio esp_timer lwip)
