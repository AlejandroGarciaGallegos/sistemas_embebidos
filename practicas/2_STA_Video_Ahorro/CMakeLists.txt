idf_component_register(SRCS "2_STA_Video_Ahorro.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_driver_gpio esp_pm
                                esp_timer lwip)
