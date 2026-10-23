idf_component_register(SRCS "1_STA_Audio_Ahorro.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_driver_i2s esp_driver_gpio esp_pm
                                esp_timer lwip)
