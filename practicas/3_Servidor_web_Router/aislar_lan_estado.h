// Estado compartido entre esta practica y el filtro de lwIP
// (aislar_lan_hook.h). Vive en un header aparte porque lo incluyen dos
// mundos distintos: el .c de esta practica (que lo ESCRIBE) y el hook de
// lwIP, compilado dentro del propio componente "lwip" (que lo LEE). No
// puede ser "static": el enlazador tiene que ver la misma variable desde
// los dos lados.
#ifndef AISLAR_LAN_ESTADO_H
#define AISLAR_LAN_ESTADO_H

#include <stdint.h>
#include <stdbool.h>

extern bool g_aislar_activo;   // false hasta que la STA obtiene IP
extern uint32_t g_ip_sta;      // IP propia de la STA (para reconocer su netif)
extern uint32_t g_red_sta;     // direccion de red de la STA, con mascara
extern uint32_t g_mascara_sta; // mascara de la STA

#endif
