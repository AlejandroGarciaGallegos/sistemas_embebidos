idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES 1_Barras
                                1_Bytes
                                2_Fluidez
                                3_Cores
                                1_Servidor_web_STA
                                1_Microfono_UDP
                                1_STA_Audio_Ahorro
                                2_Servidor_web_AP
                                2_Servidor_web_WebSocket
                                2_Camara_UDP
                                2_STA_Video_Ahorro
                                3_Servidor_web_Router
                                3_Aleatorio_UDP
                                3_Audio_Video_UDP
                                4_Asistente_Virtual)
