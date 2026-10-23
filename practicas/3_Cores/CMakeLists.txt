idf_component_register(SRCS "3_Cores.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer
                                u8g2
                                u8g2-hal-esp-idf)
