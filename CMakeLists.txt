# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# practicas/ trae un componente por cada practica.
# Se registra aqui, antes de project(), para que ESP-IDF los encuentre.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/practicas/1_Barras"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/1_Bytes"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/2_Fluidez"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/3_Cores"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/web_comun"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/1_Servidor_web_STA"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/1_Microfono_UDP"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/1_STA_Audio_Ahorro"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/2_Servidor_web_AP"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/2_Servidor_web_WebSocket"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/2_Camara_UDP"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/2_STA_Video_Ahorro"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/3_Servidor_web_Router"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/3_Aleatorio_UDP"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/3_Audio_Video_UDP"
    "${CMAKE_CURRENT_LIST_DIR}/practicas/4_Asistente_Virtual")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Freertos_demos)
