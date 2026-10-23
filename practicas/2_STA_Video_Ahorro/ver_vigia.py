# Muestra las fotos que manda el vigia de video (ver 2_STA_Video_Ahorro.c). Solo
# llega video cuando el vigia detecta movimiento; el resto del tiempo la
# ventana espera quieta. Cada evento de movimiento se graba en su propio
# archivo evento_...mjpeg. Al salir con Ctrl+C, todos los eventos se unen,
# en orden, en un solo vigia_AAAAMMDD_HHMMSS.mp4 con la velocidad real, y
# los .mjpeg se borran (necesita ffmpeg).
# Uso: poner en IP_ESP32 la IP fija de tu equipo y correr
#     python3 ver_vigia.py

import os
import socket
import subprocess
import time
import numpy as np
import cv2

# ---------- 1) Datos ----------
IP_ESP32 = "192.168.8.201"  # la IP fija de tu equipo (201 a 211)
PUERTO = 3333               # el mismo que UDP_PUERTO en el ESP32
ANCHO = 96                  # igual que FOTO_ANCHO en el ESP32
ALTO = 96                   # igual que FOTO_ALTO
ESCALA = 5                  # la foto se muestra 5 veces mas grande

# ---------- 2) UDP: pedir el video ----------
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)  # socket UDP
sock.sendto(b"R", (IP_ESP32, PUERTO))  # "R": mandame el video a mi
sock.settimeout(0.2)                   # no espera mas de 0.2 s seguidos

# ---------- 3) Ventana vacia ----------
negro = np.zeros((ALTO * ESCALA, ANCHO * ESCALA), dtype=np.uint8)
cv2.imshow("Vigia de video", negro)
cv2.waitKey(1)

sesion = time.strftime("vigia_%Y%m%d_%H%M%S.mp4")  # el video final
eventos = []           # nombre de cada .mjpeg, en el orden en que llegaron
total_fotos = 0        # fotos de todos los eventos
total_segundos = 0.0   # tiempo grabado (sin contar cuando el vigia duerme)

print("Esperando video de", IP_ESP32, "... (Ctrl+C para salir)")


def cerrar_evento():
    """Cierra el .mjpeg del evento en curso y suma sus fotos y segundos."""
    global archivo, total_fotos, total_segundos
    archivo.close()
    archivo = None
    duracion = ultima - inicio
    total_fotos = total_fotos + fotos
    total_segundos = total_segundos + duracion
    print(f"  evento terminado: {fotos} fotos en {duracion:.1f} s")


# ---------- 4) Ciclo: recibir, grabar y mostrar ----------
esperas = 0                            # veces seguidas sin recibir nada
archivo = None                         # el archivo del evento en curso
try:
    while True:
        # El vigia casi siempre duerme: si en 0.2 s no llega nada, solo se
        # atiende la ventana (si no, el sistema la marca como "no responde")
        try:
            datos, origen = sock.recvfrom(65535)  # una foto (9216 B)
        except socket.timeout:
            cv2.waitKey(1)
            esperas = esperas + 1
            if esperas == 10 and archivo is not None:  # 2 s sin fotos: el
                cerrar_evento()                        # vigia se durmio
            if esperas % 10 == 0:      # cada 2 s sin datos: otra "R", por
                sock.sendto(b"R", (IP_ESP32, PUERTO))  # si se reinicio
            continue
        esperas = 0

        # Bytes -> numeros de 0 a 255 -> una matriz de 96 x 96: la foto
        foto = np.frombuffer(datos, dtype=np.uint8).reshape(ALTO, ANCHO)

        # Primera foto de un evento: se abre un .mjpeg nuevo
        if archivo is None:
            nombre = time.strftime("evento_%Y%m%d_%H%M%S.mjpeg")
            archivo = open(nombre, "wb")
            eventos.append(nombre)
            print("Movimiento: grabando", nombre)
            fotos = 0
            inicio = time.time()
        fotos = fotos + 1
        ultima = time.time()

        # La foto llega sin comprimir: se guarda como JPEG, una tras otra
        # (MJPEG, igual que en la clase 14). Si el programa se corta, no se
        # dana
        correcto, jpeg = cv2.imencode(".jpg", foto)
        archivo.write(jpeg.tobytes())

        foto = cv2.resize(foto, None, fx=ESCALA, fy=ESCALA,
                          interpolation=cv2.INTER_NEAREST)
        cv2.imshow("Vigia de video", foto)
        cv2.waitKey(1)                 # deja que la ventana se dibuje

# ---------- 5) Al salir: unir todo en un MP4 y borrar los .mjpeg ----------
except KeyboardInterrupt:
    if archivo is not None:            # se corto a mitad de un evento
        cerrar_evento()

    if total_fotos == 0:
        print("\nNo hubo movimiento: no se grabo nada.")
    else:
        # Un MJPEG son solo JPEG pegados: unir los eventos es pegar sus bytes
        todo = b""
        for nombre in eventos:
            todo = todo + open(nombre, "rb").read()

        # El MJPEG no guarda la velocidad: se le da a ffmpeg la real
        por_segundo = total_fotos / max(total_segundos, 0.1)
        print(f"\nUniendo {len(eventos)} eventos: {total_fotos} fotos,"
              f" {total_segundos:.1f} s ({por_segundo:.1f} fotos/s)...")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error",
                        "-f", "mjpeg", "-framerate", f"{por_segundo:.2f}",
                        "-i", "-",     # "-": lee de lo que le pasa Python
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", sesion],
                       input=todo)

        # Si el MP4 quedo bien, ya no hacen falta los .mjpeg
        if os.path.exists(sesion) and os.path.getsize(sesion) > 0:
            for nombre in eventos:
                os.remove(nombre)
            print("Video listo:", sesion)
        else:
            print("No se pudo crear el MP4: los .mjpeg se conservan.")
