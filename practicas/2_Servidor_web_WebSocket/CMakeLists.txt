idf_component_register(SRCS "2_Servidor_web_WebSocket.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_http_server esp_driver_gpio
                                esp_timer web_comun)
