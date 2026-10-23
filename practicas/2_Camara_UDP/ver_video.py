# Muestra el video que manda el ESP32 (ver 2_Camara_UDP.c).
# Uso: poner en IP_ESP32 la IP que imprime el monitor serie y correr
#     python3 ver_video.py
# Para salir: Ctrl+C en la terminal. El video queda en grabacion.mjpeg
# (se reemplaza en cada corrida). Para verlo: ffplay grabacion.mjpeg

import socket
import time
from pathlib import Path

import numpy as np
import cv2

# ---------- 1) Dato ----------
IP_ESP32 = "192.168.0.247"  # la IP que imprime el monitor serie
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32
ESCALA = 1                  # 2: al doble (320 x 240); 1 desde VGA
TIMEOUT_SOCKET = 0.25       # permite atender Ctrl+C aunque no llegue video
REINTENTO_REGISTRO = 2.0    # vuelve a pedir video si se pierde el primer "R"
AVISO_SIN_VIDEO = 5.0

# ---------- 2) UDP: pedir el video ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP
sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
sock.settimeout(TIMEOUT_SOCKET)

# ---------- 3) Archivo MJPEG: donde se graba el video ----------
# Un MJPEG son solo las fotos JPEG una tras otra: no tiene encabezado
# que se pueda danar si el programa se corta
ruta_grabacion = Path(__file__).with_name("grabacion.mjpeg")
archivo = ruta_grabacion.open("wb")

print("Esperando video de", IP_ESP32, "... (Ctrl+C para salir)", flush=True)

# ---------- 4) Ciclo: recibir, grabar y mostrar ----------
cuadros = 0
inicio = time.monotonic()
ultimo_registro = 0.0
ultimo_cuadro = inicio
ultimo_aviso = inicio

try:
    while True:
        ahora = time.monotonic()

        # El primer registro puede perderse si el ESP32 aun esta arrancando.
        if ahora - ultimo_registro >= REINTENTO_REGISTRO:
            sock.sendto(b"R", (IP_ESP32, PUERTO))
            ultimo_registro = ahora

        try:
            datos, origen = sock.recvfrom(65535)
        except socket.timeout:
            ahora = time.monotonic()
            if (ahora - ultimo_cuadro >= AVISO_SIN_VIDEO and
                    ahora - ultimo_aviso >= AVISO_SIN_VIDEO):
                print("Sin cuadros: verifica la IP, el monitor serie y el "
                      "Firewall de Windows.", flush=True)
                ultimo_aviso = ahora
            continue

        ultimo_cuadro = time.monotonic()

        # JPEG -> pixeles. Si llego danada, se salta: ya viene la siguiente
        jpeg = np.frombuffer(datos, dtype=np.uint8)
        foto = cv2.imdecode(jpeg, cv2.IMREAD_COLOR)
        if foto is None:
            print("Se recibio un JPEG danado de", len(datos), "bytes")
            continue

        archivo.write(datos)              # graba la foto tal cual llega

        # Cada segundo: cuantas fotos llegaron y cuanto pesa cada una
        cuadros += 1
        if time.monotonic() - inicio >= 1:
            print(cuadros, "cuadros/s |", foto.shape[1], "x", foto.shape[0],
                  "|", len(datos) // 1024, "KB por foto", flush=True)
            archivo.flush()
            cuadros = 0
            inicio = time.monotonic()

        foto = cv2.resize(foto, None, fx=ESCALA, fy=ESCALA)
        cv2.imshow("Camara del ESP32", foto)
        tecla = cv2.waitKey(1) & 0xFF
        if tecla in (27, ord("q")):        # Esc o Q cierran la ventana
            break
except KeyboardInterrupt:
    print("\nCierre solicitado con Ctrl+C.")
finally:
    archivo.close()
    sock.close()
    cv2.destroyAllWindows()
    print("Grabacion guardada en", ruta_grabacion)
