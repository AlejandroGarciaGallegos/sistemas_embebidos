"""Pruebas sin hardware ni modelos: python -m unittest -v.

El simulador verifica el protocolo real HTTP/WS/UDP del cliente Python.
No demuestra funcionamiento electrico, transcripcion ni calidad del LLM.
"""
import base64
from contextlib import redirect_stdout
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
from pathlib import Path
import socket
import struct
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import wave

import asistente as app


TONO = struct.pack("<512h", *(int(1500 * math.sin(i * 0.19)) for i in range(512)))


def datagrama(sesion=23, secuencia=0, pcm=TONO):
    return app.CABECERA.pack(b"AUDP", sesion, secuencia, 512, 16000) + pcm


class PruebasAudio(unittest.TestCase):
    def test_cabecera_sesion_y_pcm(self):
        self.assertEqual(app.paquete_leer(datagrama()), (23, 0, TONO))
        with self.assertRaises(ValueError):
            app.paquete_leer(datagrama()[:-1])
        with self.assertRaises(ValueError):
            app.paquete_leer(b"????" + datagrama()[4:])

    def test_perdidas_duplicados_y_sesion_antigua(self):
        receptor = app.EnsambladorAudio(23)
        self.assertEqual(receptor.aceptar(datagrama()), [TONO])
        self.assertEqual(receptor.aceptar(datagrama(secuencia=2)), [app.SILENCIO, TONO])
        self.assertEqual(receptor.aceptar(datagrama(secuencia=1)), [])
        self.assertEqual(receptor.aceptar(datagrama(sesion=24, secuencia=3)), [])
        self.assertEqual((receptor.recibidos, receptor.perdidos, receptor.tardios), (2, 1, 1))

    def test_salto_grande_y_desbordamiento_u32(self):
        receptor = app.EnsambladorAudio(23)
        with self.assertRaises(RuntimeError):
            receptor.aceptar(datagrama(secuencia=20))
        receptor.siguiente = 0xFFFFFFFF
        self.assertEqual(receptor.aceptar(datagrama(secuencia=0xFFFFFFFF)), [TONO])
        self.assertEqual(receptor.aceptar(datagrama(secuencia=0)), [TONO])

    def test_rms_elimina_offset(self):
        self.assertEqual(app.nivel_rms(struct.pack("<512h", *([1000] * 512))), 0)
        self.assertGreater(app.nivel_rms(TONO), 900)

    def test_frase_conserva_preaudio_y_pausa_interna(self):
        detector = app.DetectorFrase(100, 0.128, 5)
        for _ in range(12):
            self.assertFalse(detector.agregar(app.SILENCIO))
        for _ in range(6):
            self.assertFalse(detector.agregar(TONO))
        self.assertFalse(detector.agregar(app.SILENCIO))
        self.assertFalse(detector.agregar(TONO))
        for _ in range(3):
            self.assertFalse(detector.agregar(app.SILENCIO))
        self.assertTrue(detector.agregar(app.SILENCIO))
        self.assertEqual(len(detector.bloques), 10 + 6 + 1 + 1 + 4)

    def test_golpe_breve_no_es_frase(self):
        detector = app.DetectorFrase(100, 0.064, 5)
        self.assertFalse(detector.agregar(TONO))
        self.assertFalse(detector.agregar(app.SILENCIO))
        self.assertFalse(detector.agregar(app.SILENCIO))
        self.assertFalse(detector.iniciada)
        self.assertEqual(detector.bloques, [])

    def test_wav_y_texto(self):
        with tempfile.TemporaryDirectory() as temporal:
            ruta = Path(temporal) / "audio.wav"
            app.wav_guardar(ruta, TONO * 4)
            app.wav_comprobar(ruta)
            with wave.open(str(ruta)) as archivo:
                self.assertEqual(archivo.getnframes(), 2048)
                self.assertEqual(archivo.readframes(512), TONO)
        self.assertEqual(app.texto_oled("¡Hola, piñón!\n¿Sí? | 😀"), "Hola, pinon! Si? /")


class ESPsimulada:
    def __init__(self):
        self.detener = threading.Event()
        self.cerrojo = threading.Lock()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("127.0.0.1", 0))
        self.udp.settimeout(0.008)
        self.destino = None
        self.sesion = 0
        self.secuencia = 0
        self.audio = False
        self.comandos = []
        self.preguntas = []
        dueno = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                if self.path == "/estado":
                    datos = json.dumps(dueno.estado()).encode()
                    self.send_response(200)
                    self.send_header("Content-Length", str(len(datos)))
                    self.end_headers()
                    self.wfile.write(datos)
                    return
                if self.path != "/ws":
                    self.send_error(404)
                    return
                clave = self.headers["Sec-WebSocket-Key"]
                acepta = base64.b64encode(hashlib.sha1(
                    (clave + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()
                self.send_response(101)
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", acepta)
                self.end_headers()
                self.connection.settimeout(5)
                while True:
                    cabecera = self.rfile.read(2)
                    if len(cabecera) != 2:
                        return
                    opcode, largo = cabecera[0] & 15, cabecera[1] & 127
                    if largo == 126:
                        largo = struct.unpack("!H", self.rfile.read(2))[0]
                    elif largo == 127:
                        largo = struct.unpack("!Q", self.rfile.read(8))[0]
                    mascara = self.rfile.read(4) if cabecera[1] & 128 else b""
                    contenido = self.rfile.read(largo)
                    if mascara:
                        contenido = bytes(n ^ mascara[i % 4] for i, n in enumerate(contenido))
                    if opcode == 8:
                        self.connection.sendall(b"\x88\x00")
                        return
                    mensaje = contenido.decode()
                    with dueno.cerrojo:
                        dueno.comandos.append(mensaje)
                        if mensaje.startswith("INICIAR|"):
                            dueno.audio = True
                            dueno.secuencia = 0
                        elif mensaje == "DETENER":
                            dueno.audio = False
                    datos = json.dumps(dueno.estado()).encode()
                    cabecera = b"\x81" + (bytes([len(datos)]) if len(datos) < 126
                                             else b"\x7e" + struct.pack("!H", len(datos)))
                    self.connection.sendall(cabecera + datos)

            def do_POST(self):
                datos = self.rfile.read(int(self.headers["Content-Length"]))
                dueno.preguntas.append(json.loads(datos))
                respuesta = json.dumps({"choices": [{"message": {
                    "content": "Respuesta de prueba del servidor simulado"}}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(respuesta)))
                self.end_headers()
                self.wfile.write(respuesta)

        self.http = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.http.daemon_threads = True
        self.hilos = [threading.Thread(target=self.http.serve_forever, daemon=True),
                      threading.Thread(target=self.enviar_audio, daemon=True)]

    def estado(self):
        with self.cerrojo:
            return {"ok": True, "version": 1, "arranque": 456, "microfono": True,
                    "sesion": self.sesion, "audio": self.audio}

    def enviar_audio(self):
        import time
        proximo = 0
        while not self.detener.is_set():
            try:
                datos, origen = self.udp.recvfrom(256)
                if datos.startswith(b"R|"):
                    with self.cerrojo:
                        self.sesion = int(datos[2:])
                        self.destino = origen
                    self.udp.sendto(b"OK|" + datos[2:], origen)
            except socket.timeout:
                pass
            with self.cerrojo:
                if self.audio and self.destino and time.monotonic() >= proximo:
                    self.udp.sendto(datagrama(self.sesion, self.secuencia), self.destino)
                    self.secuencia += 1
                    proximo = time.monotonic() + app.DURACION_BLOQUE

    def __enter__(self):
        for hilo in self.hilos:
            hilo.start()
        return self

    def argumentos(self):
        return ["--esp", "127.0.0.1", "--puerto-http", str(self.http.server_port),
                "--puerto-udp", str(self.udp.getsockname()[1])]

    def __exit__(self, *args):
        self.detener.set()
        self.http.shutdown()
        for hilo in self.hilos:
            hilo.join(timeout=2)
        self.http.server_close()
        self.udp.close()


@unittest.skipUnless(__import__("importlib.util").util.find_spec("websocket"), "Instalar requirements.txt")
class PruebasIntegracion(unittest.TestCase):
    def test_diagnostico_real_por_loopback(self):
        with ESPsimulada() as esp, redirect_stdout(io.StringIO()) as salida:
            self.assertEqual(app.main(esp.argumentos() + ["diagnostico"]), 0)
            self.assertIn("OK: HTTP", salida.getvalue())
            self.assertFalse(esp.audio)

    def test_grabacion_udp_genera_wav_valido(self):
        with ESPsimulada() as esp, tempfile.TemporaryDirectory() as temporal:
            ruta = Path(temporal) / "prueba.wav"
            with redirect_stdout(io.StringIO()):
                codigo = app.main(esp.argumentos() + ["grabar", "--segundos", "0.128", "--salida", str(ruta)])
            self.assertEqual(codigo, 0)
            with wave.open(str(ruta)) as archivo:
                self.assertEqual(archivo.getnframes(), 2048)
                self.assertEqual(archivo.readframes(512), TONO)

    def test_pregunta_http_y_resultado_por_ws(self):
        with ESPsimulada() as esp, redirect_stdout(io.StringIO()):
            url = f"http://127.0.0.1:{esp.http.server_port}"
            with patch("builtins.input", return_value=""):
                self.assertEqual(app.main(esp.argumentos() + ["--llm-url", url, "texto", "Hola"]), 0)
            self.assertEqual(esp.preguntas[0]["messages"][-1]["content"], "Hola")
            self.assertTrue(any(c.startswith("TEXTO|Respuesta de prueba") for c in esp.comandos))

    def test_turno_captura_transcripcion_y_respuesta(self):
        with ESPsimulada() as esp, tempfile.TemporaryDirectory() as temporal:
            with patch.object(app, "BASE", Path(temporal)), patch.object(
                    app, "transcribir", return_value="Hola desde el microfono") as voz:
                with patch("builtins.input", return_value=""), redirect_stdout(io.StringIO()):
                    codigo = app.main(esp.argumentos() + ["--motor-ia", "prueba", "conversar",
                                      "--umbral", "100", "--max-frase", "0.3", "--una-vez"])
                self.assertEqual(codigo, 0)
                voz.assert_called_once()
                app.wav_comprobar(Path(temporal) / "grabaciones/ultima_frase.wav")
                registro = json.loads((Path(temporal) / "grabaciones/ultimo_turno.json").read_text("utf-8"))
                self.assertIn("[PRUEBA SIN IA]", registro["respuesta"])


class PruebasMotores(unittest.TestCase):
    def test_puerto_ocupado_no_lanza_ni_consulta_otro_modelo(self):
        with tempfile.TemporaryDirectory() as temporal, socket.socket() as ocupado:
            ocupado.bind(("127.0.0.1", 0))
            ocupado.listen()
            modelo = Path(temporal) / "modelo.gguf"
            modelo.touch()
            args = app.argumentos(["--modelo-ia", str(modelo), "--puerto-modelo",
                                   str(ocupado.getsockname()[1]), "texto", "Hola"])
            with patch.object(app, "ejecutable", return_value="motor-simulado"), patch.object(
                    app.subprocess, "Popen") as motor:
                with self.assertRaisesRegex(RuntimeError, "puerto del modelo esta ocupado"):
                    app.responder(args, "Hola", [])
                motor.assert_not_called()

    def test_motor_se_detiene_si_la_peticion_falla(self):
        with tempfile.TemporaryDirectory() as temporal:
            modelo = Path(temporal) / "modelo.gguf"
            modelo.touch()
            args = app.argumentos(["--modelo-ia", str(modelo), "texto", "Hola"])
            proceso = Mock()
            proceso.poll.return_value = None
            with patch.object(app, "ejecutable", return_value="motor-simulado"), patch.object(
                    app.subprocess, "Popen", return_value=proceso), patch.object(
                    app, "http_json", side_effect=[{"status": "ok"}, RuntimeError("Fallo de inferencia")]):
                with self.assertRaisesRegex(RuntimeError, "Fallo de inferencia"):
                    app.responder(args, "Hola", [])
            proceso.terminate.assert_called_once()
            proceso.wait.assert_called_once_with(timeout=5)

    def test_modo_prueba_se_identifica_y_no_carga_motor(self):
        args = app.argumentos(["--motor-ia", "prueba", "texto", "Hola"])
        with patch.object(app.subprocess, "Popen") as motor:
            self.assertIn("[PRUEBA SIN IA]", app.responder(args, "Hola", []))
            motor.assert_not_called()


if __name__ == "__main__":
    unittest.main()
