# Grafica y graba el audio que manda el ESP32, y lo transcribe con Whisper.
# Uso: poner en IP_ESP32 la IP que imprime el monitor serie y correr
#     python voz_a_texto.py
# Para salir: Ctrl+C en la terminal. El audio queda en grabacion.wav
# (se reemplaza en cada corrida).

import socket
import wave
import numpy as np
from faster_whisper import WhisperModel

# ---------- 1) Datos ----------
IP_ESP32 = "192.168.43.72"  # la IP que imprime el monitor serie
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32

FRECUENCIA = 16000          # muestras por segundo, igual que en el ESP32
SEGUNDOS = 20               # cuanto tiempo se ve en la grafica
LIMITE_Y = 8000             # alto de la grafica
SEGUNDOS_AUDIO = 3          # segundos que se juntan antes de transcribir
BYTES_POR_MUESTRA = 2       # enteros de 16 bits = 2 bytes por muestra
BYTES_PARA_TRANSCRIBIR = SEGUNDOS_AUDIO * FRECUENCIA * BYTES_POR_MUESTRA

# ---------- 2) Whisper: cargar el modelo una sola vez ----------
# Tarda unos segundos y la primera vez lo descarga de internet.
# Tamano: "tiny", "base" o "small" (mas grande: mejor, pero mas lento)
print("Cargando modelo Whisper...")
modelo = WhisperModel("tiny", device="cpu", compute_type="int8")

# En Windows, Qt debe importarse despues de cargar el modelo.
import pyqtgraph as pg

# ---------- 3) UDP: pedir el audio ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP

# ---------- 4) Archivo WAV: donde se graba el audio ----------
archivo = wave.open("grabacion.wav", "wb")
archivo.setnchannels(1)                # mono
archivo.setsampwidth(2)                # 16 bits = 2 bytes por muestra
archivo.setframerate(FRECUENCIA)

# ---------- 5) Grafica vacia ----------
tiempo = np.arange(SEGUNDOS * FRECUENCIA) / FRECUENCIA  # eje x, en s
onda = np.zeros(SEGUNDOS * FRECUENCIA)                   # eje y, en 0

ventana = pg.plot(title="Microfono del ESP32")
ventana.setYRange(-LIMITE_Y, LIMITE_Y)
ventana.setLabel("bottom", "tiempo (s)")
curva = ventana.plot(tiempo, onda)
curva.setDownsampling(auto=True, method="peak")  # dibuja solo los picos

pg.QtWidgets.QApplication.processEvents()        # abre la ventana ya

sock.sendto(b"R", (IP_ESP32, PUERTO))  # "R": mandame el audio a mi
print("Esperando audio de", IP_ESP32, "... (Ctrl+C para salir)")

# ---------- 6) Ciclo: recibir, grabar, graficar y transcribir ----------
pendiente = b""
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

    pendiente = pendiente + datos

    if len(pendiente) >= BYTES_PARA_TRANSCRIBIR:
        # El ESP32 manda enteros de 16 bits; Whisper pide decimales de -1 a 1.
        audio = np.frombuffer(pendiente, dtype=np.int16).astype(np.float32)
        audio = audio / 32768

        # Transcribir en espanol; vad_filter=True salta el silencio.
        segmentos, info = modelo.transcribe(
            audio,
            language="es",
            vad_filter=True,
        )

        for segmento in segmentos:
            print(segmento.text)

        pendiente = b""  # vaciar para juntar los siguientes segundos
