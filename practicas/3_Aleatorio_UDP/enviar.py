#!/usr/bin/env python3
"""Manda un valor al ESP32 (ver 3_Aleatorio_UDP.c): si coincide con 42,
enciende el LED. Se corre en una terminal aparte de escuchar.py, para no
interrumpirlo mientras escribes.

Uso:
    python3 enviar.py 192.168.x.x
(la IP es la que el monitor serie imprime como "Usa esta IP en escuchar.py
y enviar.py")
"""

import socket
import sys

PUERTO = 3333  # el mismo puerto que UDP_PUERTO en 3_Aleatorio_UDP.c


def main():
    if len(sys.argv) != 2:
        print("Uso: python3 enviar.py <ip_del_esp32>")
        sys.exit(1)

    esp32 = (sys.argv[1], PUERTO)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    print(f"Mandando valores a {esp32[0]}:{esp32[1]} -- Ctrl+C para salir")
    try:
        while True:
            valor = input("Valor a enviar (42 enciende el LED): ")
            sock.sendto(valor.encode(), esp32)
    except (KeyboardInterrupt, EOFError):
        print("\nListo.")


if __name__ == "__main__":
    main()
