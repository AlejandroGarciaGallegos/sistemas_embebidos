# Junta grabacion.wav y grabacion.mjpeg en un solo grabacion.mp4.
# Se corre despues de recibir_audio_video.py, en la misma carpeta:
#     python3 unir_audio_video.py
# Necesita ffmpeg (Linux: sudo apt install ffmpeg).

import wave
import subprocess

# ---------- 1) Cuanto dura: lo dice el audio ----------
# El audio llega a ritmo fijo (16000 muestras/s): su duracion es exacta
audio = wave.open("grabacion.wav", "rb")
segundos = audio.getnframes() / audio.getframerate()

# ---------- 2) Cuantas fotos hay ----------
# Cada JPEG empieza con los bytes FF D8 FF: se cuentan en el archivo
video = open("grabacion.mjpeg", "rb").read()
fotos = video.count(b"\xff\xd8\xff")

# ---------- 3) Cuadros por segundo reales ----------
# Si hubo 300 fotos en 20 s de audio, el video fue de 15 cuadros/s
cuadros_por_segundo = fotos / segundos
print(f"{segundos:.1f} s de audio, {fotos} fotos: "
      f"{cuadros_por_segundo:.1f} cuadros/s")

# ---------- 4) ffmpeg los junta ----------
subprocess.run([
    "ffmpeg", "-y",
    "-framerate", str(cuadros_por_segundo), "-i", "grabacion.mjpeg",
    "-i", "grabacion.wav",
    "-c:v", "libx264", "-pix_fmt", "yuv420p",  # video MP4 comun
    "-c:a", "aac",                             # audio MP4 comun
    "grabacion.mp4",
])
