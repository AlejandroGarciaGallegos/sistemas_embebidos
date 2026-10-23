#!/usr/bin/env python3
"""Recibe el aleatorio del ESP32 (ver 3_Aleatorio_UDP.c). Solo escucha: para
mandar un valor y prender el LED, usar enviar.py en OTRA terminal -- asi los
dos nunca se interrumpen entre si.

Muestra cada cuanto llega un numero nuevo, en milisegundos, para ver la
velocidad real -- sirve para comparar distintos valores de vTaskDelay()
en tarea_aleatorio (3_Aleatorio_UDP.c).

Uso:
    python3 escuchar.py 192.168.x.x
(la IP es la que el monitor serie imprime como "Usa esta IP en escuchar.py
y enviar.py")
"""

import socket
import sys
import time

PUERTO = 3333  # el mismo puerto que UDP_PUERTO en 3_Aleatorio_UDP.c
REGISTRO = b"R"  # le dice al ESP32 "mandame el aleatorio a mi"; no mueve el LED


def main():
    if len(sys.argv) != 2:
        print("Uso: python3 escuchar.py <ip_del_esp32>")
        sys.exit(1)

    esp32 = (sys.argv[1], PUERTO)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)  # si no llega nada en 2s, nos volvemos a registrar

    print(f"Escuchando a {esp32[0]}:{esp32[1]} -- Ctrl+C para salir")
    sock.sendto(REGISTRO, esp32)

    anterior = None
    recibidos = 0
    inicio = time.perf_counter()
    try:
        while True:
            try:
                datos, _ = sock.recvfrom(16)
            except socket.timeout:
                # nada en 2s: seguramente enviar.py escribio desde otra
                # terminal y el ESP32 le empezo a mandar el aleatorio a el
                sock.sendto(REGISTRO, esp32)
                continue

            # perf_counter() en vez de time.time(): pensado para medir
            # intervalos cortos, no la hora del dia
            ahora = time.perf_counter()
            if anterior is None:
                intervalo = "    --    "
            else:
                # en milisegundos y con decimales: en segundos con 2
                # decimales, todo lo mas rapido que 10 ms se ve como 0.00s
                intervalo = f"{(ahora - anterior) * 1000:7.2f} ms"
            anterior = ahora
            recibidos += 1

            # recibidos y cuantos por segundo, para comparar contra la
            # pagina del programa 2 (misma cuenta, del lado del navegador)
            segundos = ahora - inicio
            print(f"Aleatorio: {datos.decode():<3}  (cada {intervalo})"
                  f"  -- recibidos: {recibidos} ({recibidos / segundos:.1f}/s)")
    except KeyboardInterrupt:
        print("\nListo.")


if __name__ == "__main__":
    main()
