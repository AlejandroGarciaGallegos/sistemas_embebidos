# Muestra el video que manda el ESP32 (ver 2_Camara_UDP.c).
# Uso: poner en IP_ESP32 la IP que imprime el monitor serie y correr
#     python3 senas_mano
# Para salir: Ctrl+C en la terminal. El video queda en grabacion.mjpeg
# (se reemplaza en cada corrida). Para verlo: ffplay grabacion.mjpeg

import socket
import time
from pathlib import Path

import numpy as np
import cv2
import mediapipe as mp
from mediapipe.tasks.python import vision, BaseOptions

# ---------- 1) Datos ----------
IP_ESP32 = "192.168.43.72"  # la IP que imprime el monitor serie
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32
ESCALA = 1                  # 2: al doble (320 x 240); 1 desde VGA

# ---------- 2) Detector de manos: cargar el modelo una sola vez ----------
ruta = Path(__file__).parent / "hand_landmarker.task"
opciones = vision.HandLandmarkerOptions(
    base_options=BaseOptions(model_asset_path=str(ruta)),
    num_hands=1,
)
detector = vision.HandLandmarker.create_from_options(opciones)

# ---------- 3) UDP: pedir el video ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP
sock.sendto(b"R", (IP_ESP32, PUERTO))  # "R": mandame el video a mi

# ---------- 4) Archivo MJPEG: donde se graba el video ----------
# Un MJPEG son solo las fotos JPEG una tras otra: no tiene encabezado
# que se pueda danar si el programa se corta
archivo = open("grabacion.mjpeg", "wb")

print("Esperando video de", IP_ESP32, "... (Ctrl+C para salir)")

# ---------- 5) Ciclo: recibir, grabar, detectar y mostrar ----------
cuadros = 0
inicio = time.time()
while True:
    datos, origen = sock.recvfrom(65535)  # espera una foto JPEG completa

    # JPEG -> pixeles. Si llego danada, se salta: ya viene la siguiente
    jpeg = np.frombuffer(datos, dtype=np.uint8)
    foto = cv2.imdecode(jpeg, cv2.IMREAD_COLOR)
    if foto is None:
        continue

    archivo.write(datos)                  # graba la foto tal cual llega

    # Cada segundo: cuantas fotos llegaron y cuanto pesa cada una
    cuadros = cuadros + 1
    if time.time() - inicio >= 1:
        print(cuadros, "cuadros/s |", foto.shape[1], "x", foto.shape[0],
              "|", len(datos) // 1024, "KB por foto")
        cuadros = 0
        inicio = time.time()

    foto = cv2.resize(foto, None, fx=ESCALA, fy=ESCALA)

    rgb = cv2.cvtColor(foto, cv2.COLOR_BGR2RGB)
    imagen = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)
    resultado = detector.detect(imagen)

    alto, ancho, _ = foto.shape
    for mano in resultado.hand_landmarks:
        pulgar = mano[4]  # punta del pulgar
        indice = mano[8]  # punta del indice

        punto_pulgar = (int(pulgar.x * ancho), int(pulgar.y * alto))
        punto_indice = (int(indice.x * ancho), int(indice.y * alto))

        vision.drawing_utils.draw_landmarks(
            foto, mano, vision.HandLandmarksConnections.HAND_CONNECTIONS
        )

        cv2.line(foto, punto_pulgar, punto_indice, (0, 255, 255), 2)
        distancia = np.hypot(
            punto_indice[0] - punto_pulgar[0],
            punto_indice[1] - punto_pulgar[1],
        )
        texto = f"{distancia:.0f} px"
        tamano, _ = cv2.getTextSize(texto, cv2.FONT_HERSHEY_SIMPLEX, 0.5, 1)
        x_texto = max(0, min(ancho - tamano[0], (punto_pulgar[0] + punto_indice[0]) // 2))
        y_texto = max(tamano[1], min(alto - 2, (punto_pulgar[1] + punto_indice[1]) // 2 - 8))
        cv2.putText(foto, texto, (x_texto, y_texto), cv2.FONT_HERSHEY_SIMPLEX,
                    0.5, (0, 255, 255), 1, cv2.LINE_AA)

    cv2.imshow("Camara del ESP32", foto)
    cv2.waitKey(1)                        # deja que la ventana se dibuje
