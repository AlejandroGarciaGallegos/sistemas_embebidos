# Recibe el audio y el video que manda el ESP32 (ver 3_Audio_Video_UDP.c):
# muestra el video y graba los dos. Cada datagrama trae al inicio un byte
# que dice que es: b"A" audio o b"V" video.
# Uso: poner en IP_ESP32 la IP que imprime el monitor serie y correr
#     python3 recibir_audio_video.py
# Para salir: Ctrl+C en la terminal. Quedan grabacion.wav y
# grabacion.mjpeg; para juntarlos en un MP4: python3 unir_audio_video.py

import socket
import wave
import numpy as np
import cv2

# ---------- 1) Datos ----------
IP_ESP32 = "192.168.0.247"  # la IP que imprime el monitor serie
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32
FRECUENCIA = 16000          # muestras por segundo, igual que en el ESP32
ESCALA = 2                  # 2: al doble (320 x 240); 1 desde VGA

# ---------- 2) UDP: pedir audio y video ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP
sock.sendto(b"R", (IP_ESP32, PUERTO))  # "R": mandame todo a mi

# ---------- 3) Archivos: uno por flujo, como en los programas 1 y 2 ----
audio = wave.open("grabacion.wav", "wb")
audio.setnchannels(1)                  # mono
audio.setsampwidth(2)                  # 16 bits = 2 bytes por muestra
audio.setframerate(FRECUENCIA)

video = open("grabacion.mjpeg", "wb")

print("Esperando audio y video de", IP_ESP32, "... (Ctrl+C para salir)")

# ---------- 4) Ciclo: recibir, separar, grabar y mostrar ----------
while True:
    datos, origen = sock.recvfrom(65535)  # espera un datagrama
    tipo = datos[0:1]                     # el primer byte: que es
    contenido = datos[1:]                 # el resto: el dato en si

    if tipo == b"A":
        audio.writeframes(contenido)      # 512 muestras al WAV

    elif tipo == b"V":
        # JPEG -> pixeles. Si llego danada, se salta
        jpeg = np.frombuffer(contenido, dtype=np.uint8)
        foto = cv2.imdecode(jpeg, cv2.IMREAD_COLOR)
        if foto is None:
            continue

        video.write(contenido)            # la foto al MJPEG
        foto = cv2.resize(foto, None, fx=ESCALA, fy=ESCALA)
        cv2.imshow("Camara del ESP32", foto)
        cv2.waitKey(1)                    # deja que la ventana se dibuje
