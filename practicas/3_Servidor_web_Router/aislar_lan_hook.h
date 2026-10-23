// Filtro de lwIP: descarta, ANTES de cualquier NAT o reenvio, los paquetes
// que entran por el AP y van dirigidos a la subred de la STA (la red de
// casa). Todo lo demas (la subred propia del AP, e internet real) sigue
// su camino normal.
//
// Este archivo NO se compila como parte de esta practica: el CMakeLists.txt
// raiz lo inyecta directamente en el componente "lwip" (ver EXTRA_COMPONENT_
// DIRS y el bloque de "idf_component_get_property(lwip ...)"), por eso las
// funciones son "static inline": terminan copiadas en varios archivos .c de
// lwIP, y sin "static" el enlazador vería el mismo nombre repetido.
//
// Como se reconoce "entro por el AP": NO comparando el puntero de netif por
// indice (netif_get_by_index dio, en la practica, un puntero que nunca
// coincidia con el netif real -- verificado con logs de depuracion). En vez
// de eso se compara la IP PROPIA del netif de entrada contra la IP de la
// STA: si no es la STA, y solo hay dos interfaces activas, es el AP.
#ifndef AISLAR_LAN_HOOK_H
#define AISLAR_LAN_HOOK_H

#include "lwip/netif.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/udp.h"
#include "aislar_lan_estado.h"

static inline int aislar_lan_filtro(struct pbuf *p, struct netif *input_netif)
{
    const ip4_addr_t *ip_entrada = netif_ip4_addr(input_netif);
    bool entro_por_sta = ip_entrada && ip_entrada->addr == g_ip_sta;

    if (!g_aislar_activo || entro_por_sta)
    {
        return 0; // no hay nada que filtrar todavia, o no entro por el AP
    }

    const struct ip_hdr *iphdr = (const struct ip_hdr *)p->payload;
    uint32_t destino = iphdr->dest.addr;

    if ((destino & g_mascara_sta) == g_red_sta)
    {
        // Excepcion: el DNS que usan los dispositivos del AP es el mismo de
        // la STA (ver ap_compartir_internet_aislado), y ese DNS normalmente
        // vive DENTRO de la red de casa (el router). Bloquear la subred
        // entera bloquearia tambien el DNS, y sin DNS no se puede navegar
        // aunque el NAT funcione perfecto. Por eso se deja pasar UDP al
        // puerto 53 (DNS), y se bloquea todo lo demas de esa subred.
        if (IPH_PROTO(iphdr) == IP_PROTO_UDP)
        {
            uint16_t hlen = IPH_HL_BYTES(iphdr);
            const struct udp_hdr *udphdr =
                (const struct udp_hdr *)((const uint8_t *)iphdr + hlen);
            if (lwip_ntohs(udphdr->dest) == 53)
            {
                return 0; // es una consulta DNS: se deja pasar
            }
        }

        pbuf_free(p); // iba hacia la red de casa: se descarta aqui mismo
        return 1;     // "consumido": lwIP no lo procesa mas
    }

    return 0; // cualquier otro destino (por ejemplo, internet real) sigue igual
}

#endif
