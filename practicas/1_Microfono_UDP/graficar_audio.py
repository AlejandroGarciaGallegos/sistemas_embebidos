# Grafica el audio que manda el ESP32 (ver 1_Microfono_UDP.c).
# Uso: poner en IP_ESP32 la IP que imprime el monitor serie y correr
#     python3 graficar_audio.py
# Para salir: Ctrl+C en la terminal. El audio queda en grabacion.wav
# (se reemplaza en cada corrida).

import socket
import wave
import numpy as np
import pyqtgraph as pg

# ---------- 1) Datos ----------
IP_ESP32 = "192.168.0.247"  # la IP que imprime el monitor serie
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32

FRECUENCIA = 16000          # muestras por segundo, igual que en el ESP32
SEGUNDOS = 20               # cuanto tiempo se ve en la grafica
LIMITE_Y = 8000             # alto de la grafica

# ---------- 2) UDP: pedir el audio ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP
sock.sendto(b"R", (IP_ESP32, PUERTO))  # "R": mandame el audio a mi

# ---------- 3) Archivo WAV: donde se graba el audio ----------
archivo = wave.open("grabacion.wav", "wb")
archivo.setnchannels(1)                # mono
archivo.setsampwidth(2)                # 16 bits = 2 bytes por muestra
archivo.setframerate(FRECUENCIA)

# ---------- 4) Grafica vacia ----------
tiempo = np.arange(SEGUNDOS * FRECUENCIA) / FRECUENCIA  # eje x, en s
onda = np.zeros(SEGUNDOS * FRECUENCIA)                   # eje y, en 0

ventana = pg.plot(title="Microfono del ESP32")
ventana.setYRange(-LIMITE_Y, LIMITE_Y)
ventana.setLabel("bottom", "tiempo (s)")
curva = ventana.plot(tiempo, onda)
curva.setDownsampling(auto=True, method="peak")  # dibuja solo los picos
pg.QtWidgets.QApplication.processEvents()        # abre la ventana ya

print("Esperando audio de", IP_ESP32, "... (Ctrl+C para salir)")

# ---------- 5) Ciclo: recibir, grabar y graficar ----------
bloques = 0
while True:
    datos, origen = sock.recvfrom(2048)   # espera un datagrama (1024 B)
    archivo.writeframes(datos)            # graba el bloque tal cual llega
    muestras = np.frombuffer(datos, dtype=np.int16)  # bytes -> 512 nums

    # Recorre la onda: salen las mas viejas, entran las nuevas al final
    onda = np.append(onda[len(muestras):], muestras)

    # Redibuja cada 2 bloques (~15 veces por segundo)
    bloques = bloques + 1
    if bloques % 2 == 0:
        curva.setData(tiempo, onda)
        pg.QtWidgets.QApplication.processEvents()  # actualiza la ventana
