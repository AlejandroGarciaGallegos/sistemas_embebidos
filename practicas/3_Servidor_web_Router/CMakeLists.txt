idf_component_register(SRCS "3_Servidor_web_Router.c"
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi esp_http_server esp_driver_gpio
                                lwip web_comun)

# El filtro de lwIP (aislar_lan_hook.h) usa estas 4 variables globales, pero
# nada en el flujo normal las referencia cuando otra practica es la activa:
# --gc-sections las descartaria. "-u" fuerza al enlazador a tratarlas como
# necesarias (mismo mecanismo que usa esp_app_format o cxx en el ESP-IDF).
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u g_aislar_activo"
    "-u g_ip_sta"
    "-u g_red_sta"
    "-u g_mascara_sta")
