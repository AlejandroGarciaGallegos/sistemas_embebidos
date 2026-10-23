idf_component_register(SRCS "3_Audio_Video_UDP.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_driver_i2s lwip)
