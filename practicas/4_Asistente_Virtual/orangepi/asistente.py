#!/usr/bin/env python3
"""Pruebas y asistente por texto. Python 3.9+, sin escritorio ni bocina.

La entrada viene del microfono de la ESP, nunca del microfono de la Orange Pi.
Whisper y llama.cpp se ejecutan en procesos separados y por turnos para liberar
su memoria al terminar. Los comandos diagnostico/pantalla/grabar no requieren IA.
"""
from __future__ import annotations

import argparse
import array
from collections import deque
from contextlib import suppress
import json
import math
from pathlib import Path
import queue
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import urllib.error
import urllib.request
import wave

BASE = Path(__file__).resolve().parent
FRECUENCIA = 16000
MUESTRAS = 512
BLOQUE_BYTES = MUESTRAS * 2
DURACION_BLOQUE = MUESTRAS / FRECUENCIA
CABECERA = struct.Struct("!4sIIHH")
SILENCIO = bytes(BLOQUE_BYTES)


def texto_oled(texto: str) -> str:
    """La consola conserva Unicode; la fuente de la OLED recibe ASCII imprimible."""
    normal = unicodedata.normalize("NFKD", texto).encode("ascii", "ignore").decode()
    return " ".join(normal.replace("|", "/").split())[:384]


def nivel_rms(pcm: bytes) -> float:
    muestras = array.array("h")
    muestras.frombytes(pcm)
    if sys.byteorder != "little":
        muestras.byteswap()
    if not muestras:
        return 0.0
    centro = sum(muestras) / len(muestras)
    return math.sqrt(sum((n - centro) ** 2 for n in muestras) / len(muestras))


def paquete_leer(datos: bytes) -> tuple[int, int, bytes]:
    if len(datos) != CABECERA.size + BLOQUE_BYTES:
        raise ValueError("Tamano de datagrama incorrecto")
    marca, sesion, secuencia, muestras, frecuencia = CABECERA.unpack_from(datos)
    if marca != b"AUDP" or muestras != MUESTRAS or frecuencia != FRECUENCIA:
        raise ValueError("Formato de audio incompatible")
    return sesion, secuencia, datos[CABECERA.size:]


class EnsambladorAudio:
    """Rechaza sesiones antiguas; conserva el tiempo cuando faltan pocos bloques.

    Los paquetes que llegan tarde se descartan. Un salto grande invalida la
    captura: no se intenta inventar varios segundos de audio perdido.
    """
    def __init__(self, sesion: int):
        self.sesion = sesion
        self.siguiente = 0
        self.recibidos = 0
        self.perdidos = 0
        self.tardios = 0

    def aceptar(self, datos: bytes) -> list[bytes]:
        sesion, secuencia, pcm = paquete_leer(datos)
        if sesion != self.sesion:
            return []
        diferencia = (secuencia - self.siguiente) & 0xFFFFFFFF
        if diferencia >= 0x80000000:
            self.tardios += 1
            return []
        if diferencia > 8:
            raise RuntimeError("Faltan mas de 8 bloques seguidos. Revisa el WiFi y repite.")
        self.perdidos += diferencia
        self.recibidos += 1
        self.siguiente = (secuencia + 1) & 0xFFFFFFFF
        return [SILENCIO] * diferencia + [pcm]


class ControlESP:
    """Un cliente WS, un RPC a la vez y latidos independientes de la inferencia."""
    def __init__(self, ip: str, puerto: int = 80):
        self.ip = socket.gethostbyname(ip)
        self.url = f"ws://{self.ip}:{puerto}/ws"
        self.ws = None
        self.cerrojo = threading.Lock()
        self.detener = threading.Event()
        self.fallo = threading.Event()
        self.hilo = None
        self.arranque = None

    def __enter__(self):
        try:
            import websocket
        except ImportError as exc:
            raise RuntimeError("Instala dependencias: python -m pip install -r requirements.txt") from exc
        # Evita que un proxy de escritorio intercepte la direccion privada de la ESP.
        self.ws = websocket.create_connection(
            self.url, timeout=3, enable_multithread=True, http_no_proxy=[self.ip])
        try:
            estado = self.enviar("PING")
            if estado.get("version") != 1:
                raise RuntimeError("Version de firmware incompatible")
            self.arranque = estado["arranque"]
            self.hilo = threading.Thread(target=self._latidos, daemon=True)
            self.hilo.start()
        except BaseException:
            self.ws.close()
            raise
        return self

    def enviar(self, mensaje: str) -> dict:
        if self.fallo.is_set():
            raise RuntimeError("Se perdio el control de la ESP. Vuelve a ejecutar la prueba.")
        with self.cerrojo:
            try:
                self.ws.send(mensaje)
                respuesta = json.loads(self.ws.recv())
            except Exception as exc:
                self.fallo.set()
                raise RuntimeError(f"Fallo WebSocket: {exc}") from exc
            if not respuesta.get("ok"):
                raise RuntimeError(f"La ESP rechazo el comando: {respuesta.get('error')}")
            if self.arranque is not None and respuesta.get("arranque") != self.arranque:
                self.fallo.set()
                raise RuntimeError("La ESP se reinicio. Repite la captura.")
            return respuesta

    def estado(self, nombre: str, texto: str):
        self.enviar(f"ESTADO|{nombre}|{texto_oled(texto)}")

    def resultado(self, texto: str):
        self.enviar(f"TEXTO|{texto_oled(texto)}")

    def _latidos(self):
        while not self.detener.wait(2):
            try:
                self.enviar("PING")
            except Exception:
                self.fallo.set()
                return

    def __exit__(self, *args):
        self.detener.set()
        if not self.fallo.is_set():
            with suppress(Exception):
                self.enviar("DETENER")
        if self.hilo:
            self.hilo.join(timeout=4)
        if self.ws:
            self.ws.close()


class ReceptorAudio:
    """Recibe continuamente en un hilo, incluso si el consumidor esta ocupado."""
    def __init__(self, control: ControlESP, puerto: int):
        self.control = control
        self.puerto = puerto
        self.sesion = secrets.randbelow(0xFFFFFFFF) + 1
        self.ensamblador = EnsambladorAudio(self.sesion)
        self.cola = queue.Queue(maxsize=128)  # ~4 segundos; no crece indefinidamente
        self.detener = threading.Event()
        self.registrado = threading.Event()
        self.error = None
        self.hilo = None
        self.sock = None

    def __enter__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 256 * 1024)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.settimeout(0.2)
        self.hilo = threading.Thread(target=self._recibir, daemon=True)
        self.hilo.start()
        try:
            if not self.registrado.wait(4):
                if self.error:
                    raise RuntimeError(self.error)
                raise RuntimeError("No hubo registro UDP. Revisa IP, red y puerto 3333.")
            self.control.enviar(f"INICIAR|{self.sesion}")
        except BaseException:
            self.__exit__()
            raise
        return self

    def _recibir(self):
        proximo_registro = 0.0
        destino = (self.control.ip, self.puerto)
        while not self.detener.is_set():
            try:
                ahora = time.monotonic()
                if ahora >= proximo_registro:
                    self.sock.sendto(f"R|{self.sesion}".encode("ascii"), destino)
                    proximo_registro = ahora + 1
                try:
                    datos, origen = self.sock.recvfrom(2048)
                except socket.timeout:
                    continue
                if origen != destino:
                    continue
                if datos == f"OK|{self.sesion}".encode("ascii"):
                    self.registrado.set()
                    continue
                try:
                    bloques = self.ensamblador.aceptar(datos)
                except ValueError:
                    continue
                for bloque in bloques:
                    self.cola.put_nowait(bloque)
            except queue.Full:
                self.error = "La cola de audio se lleno. La Orange Pi no esta procesando a tiempo."
                return
            except Exception as exc:
                self.error = str(exc)
                return

    def leer(self) -> bytes:
        limite = time.monotonic() + 3
        while time.monotonic() < limite:
            if self.error:
                raise RuntimeError(self.error)
            if self.control.fallo.is_set():
                raise RuntimeError("Se perdio el control durante la captura")
            try:
                return self.cola.get(timeout=0.2)
            except queue.Empty:
                continue
        raise RuntimeError("No llegan bloques de audio desde la ESP (3 segundos sin datos).")

    def comprobar_calidad(self):
        total = self.ensamblador.recibidos + self.ensamblador.perdidos
        if total and self.ensamblador.perdidos / total > 0.1:
            raise RuntimeError("Se perdio mas del 10% del audio. Repite la frase cerca del router.")

    def resumen(self) -> str:
        e = self.ensamblador
        return f"UDP: {e.recibidos} recibidos, {e.perdidos} perdidos, {e.tardios} tardios/duplicados"

    def __exit__(self, *args):
        # Se detiene primero en la ESP para no inundar el socket al cerrar.
        with suppress(Exception):
            self.control.enviar("DETENER")
        self.detener.set()
        if self.hilo:
            self.hilo.join(timeout=2)
        if self.sock:
            self.sock.close()


class DetectorFrase:
    """Primer endpoint por energia. No es un clasificador neuronal de voz.

    Conserva preaudio, bloques de silencio y pausas internas. El umbral RMS se
    calibra con 'grabar'. Una rafaga demasiado corta no cuenta como una frase.
    """
    def __init__(self, umbral: float, silencio: float, maximo: float):
        self.umbral = umbral
        self.silencio = max(1, math.ceil(silencio / DURACION_BLOQUE))
        self.maximo = max(1, math.ceil(maximo / DURACION_BLOQUE))
        self.previo = deque(maxlen=10)  # 320 ms antes del inicio
        self.bloques = []
        self.iniciada = False
        self.con_voz = 0
        self.callados = 0
        self.total = 0

    def agregar(self, bloque: bytes) -> bool:
        fuerte = nivel_rms(bloque) >= self.umbral
        if not self.iniciada:
            if fuerte:
                self.iniciada = True
                self.bloques = list(self.previo)
            else:
                self.previo.append(bloque)
                return False
        self.bloques.append(bloque)
        self.total += 1
        self.con_voz += int(fuerte)
        self.callados = 0 if fuerte else self.callados + 1
        if self.callados >= self.silencio and self.con_voz < 6:
            # Golpe breve: se vuelve a esperar habla, sin conservar ese ruido.
            self.bloques.clear()
            self.previo.clear()
            self.iniciada = False
            self.con_voz = self.callados = self.total = 0
            return False
        return self.callados >= self.silencio or self.total >= self.maximo


def wav_guardar(ruta: Path, pcm: bytes):
    ruta.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(ruta), "wb") as archivo:
        archivo.setnchannels(1)
        archivo.setsampwidth(2)
        archivo.setframerate(FRECUENCIA)
        archivo.writeframes(pcm)


def wav_comprobar(ruta: Path):
    with wave.open(str(ruta), "rb") as archivo:
        if (archivo.getnchannels(), archivo.getsampwidth(), archivo.getframerate(),
                archivo.getcomptype()) != (1, 2, FRECUENCIA, "NONE"):
            raise RuntimeError("Se necesita WAV PCM de 16 kHz, mono, 16 bits sin compresion.")
        if archivo.getnframes() == 0:
            raise RuntimeError("El archivo WAV esta vacio")


def ejecutable(ruta: Path) -> str:
    ruta = ruta.expanduser().resolve()
    if not ruta.is_file():
        raise RuntimeError(f"No existe el motor: {ruta}. Consulta README.md para compilarlo.")
    return str(ruta)


def transcribir(args, archivo: Path) -> str:
    wav_comprobar(archivo)
    modelo = args.modelo_voz.expanduser().resolve()
    if not modelo.is_file():
        raise RuntimeError(f"Falta el modelo de voz: {modelo}")
    with tempfile.TemporaryDirectory(prefix="asistente_stt_") as temporal:
        salida = Path(temporal) / "transcripcion"
        comando = [ejecutable(args.whisper), "-m", str(modelo), "-f", str(archivo.resolve()),
                   "-l", "es", "-t", str(args.hilos), "-ng", "-nt", "-otxt", "-of", str(salida)]
        try:
            proceso = subprocess.run(comando, capture_output=True, text=True, encoding="utf-8",
                                     errors="replace", timeout=args.timeout, check=False)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("Whisper excedio el tiempo limite; su proceso fue detenido.") from exc
        if proceso.returncode:
            raise RuntimeError(f"Whisper termino con codigo {proceso.returncode}:\n{proceso.stderr[-2500:]}")
        txt = salida.with_suffix(".txt")
        if not txt.is_file():
            raise RuntimeError("Whisper no genero la transcripcion. Revisa la version del motor.")
        texto = " ".join(txt.read_text(encoding="utf-8").split())
        return "" if texto.strip("[]() .").lower() in {
            "blank_audio", "silencio", "sin voz", "music", "musica"} else texto


def http_json(url: str, datos=None, timeout=3):
    contenido = None if datos is None else json.dumps(datos).encode("utf-8")
    solicitud = urllib.request.Request(url, data=contenido,
                                     headers={"Content-Type": "application/json"})
    # Todas las conexiones de esta practica van directamente a la LAN/localhost.
    directo = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with directo.open(solicitud, timeout=timeout) as respuesta:
        return json.load(respuesta)


def _preguntar_servidor(url: str, pregunta: str, historial: list, args) -> str:
    mensajes = [{"role": "system", "content": (
        "Eres un asistente de un proyecto de sistemas embebidos. Responde en espanol, "
        "con claridad y en un maximo de tres frases breves. Si no sabes algo, dilo.")}]
    # Historial deliberadamente corto para un contexto y una RAM limitados.
    mensajes.extend(historial[-2:])
    mensajes.append({"role": "user", "content": pregunta[:600]})
    datos = http_json(url.rstrip("/") + "/v1/chat/completions", {
        "messages": mensajes, "max_tokens": args.tokens, "temperature": 0.4,
        "stream": False,
    }, timeout=args.timeout)
    try:
        respuesta = datos["choices"][0]["message"]["content"].strip()
    except (KeyError, IndexError, TypeError, AttributeError) as exc:
        raise RuntimeError("El servidor del modelo devolvio una respuesta incompatible") from exc
    if not respuesta:
        raise RuntimeError("El modelo devolvio una respuesta vacia")
    return respuesta


def responder(args, pregunta: str, historial: list) -> str:
    if args.motor_ia == "prueba":
        return f"[PRUEBA SIN IA] Recibi tu texto: {pregunta}"
    if args.llm_url:
        return _preguntar_servidor(args.llm_url, pregunta, historial, args)
    modelo = args.modelo_ia.expanduser().resolve()
    if not modelo.is_file():
        raise RuntimeError(f"Falta el modelo conversacional: {modelo}")
    motor = ejecutable(args.llama_server)
    # Comprueba el puerto antes de cargar un modelo. No conecta por accidente a otro servicio.
    with socket.socket() as comprobacion:
        comprobacion.settimeout(0.3)
        if comprobacion.connect_ex(("127.0.0.1", args.puerto_modelo)) == 0:
            raise RuntimeError("El puerto del modelo esta ocupado. Usa --puerto-modelo u --llm-url.")
        # Una conexion de comprobacion no confunde TIME_WAIT de un turno previo
        # con un servidor activo, como puede ocurrir al intentar bind() sin REUSE.
    url = f"http://127.0.0.1:{args.puerto_modelo}"
    comando = [motor, "-m", str(modelo), "-c", "1024", "-t", str(args.hilos),
               "-b", "128", "-ub", "64", "-np", "1", "-ngl", "0", "--host", "127.0.0.1",
               "--port", str(args.puerto_modelo)]
    # El archivo de log evita llenar memoria con la salida del motor.
    with tempfile.TemporaryFile(mode="w+b") as log:
        proceso = subprocess.Popen(comando, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
        try:
            limite = time.monotonic() + args.timeout
            while time.monotonic() < limite:
                if proceso.poll() is not None:
                    log.seek(0)
                    detalle = log.read()[-2500:].decode("utf-8", "replace")
                    raise RuntimeError(f"llama-server termino con codigo {proceso.returncode}:\n{detalle}")
                try:
                    if http_json(url + "/health", timeout=1).get("status") == "ok":
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.2)
            else:
                raise RuntimeError("El modelo no termino de cargar dentro del tiempo limite")
            return _preguntar_servidor(url, pregunta, historial, args)
        finally:
            # Tambien se ejecuta con Ctrl+C o errores de red/inferencia.
            if proceso.poll() is None:
                proceso.terminate()
                try:
                    proceso.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proceso.kill()
                    proceso.wait()


def capturar_frase(control, args) -> bytes:
    detector = DetectorFrase(args.umbral, args.silencio, args.max_frase)
    inicio = time.monotonic()
    anunciada = False
    with ReceptorAudio(control, args.puerto_udp) as audio:
        print("Habla ahora. La captura termina despues del silencio.", flush=True)
        while True:
            bloque = audio.leer()
            terminada = detector.agregar(bloque)
            if detector.iniciada and not anunciada:
                print("Se detecto sonido; grabando la frase...", flush=True)
                anunciada = True
            if terminada:
                audio.comprobar_calidad()
                print(audio.resumen())
                return b"".join(detector.bloques)
            if not detector.iniciada and time.monotonic() - inicio > args.espera_voz:
                raise RuntimeError("No se detecto una frase. Ajusta --umbral con la prueba grabar.")
            if time.monotonic() - inicio > args.espera_voz + args.max_frase + 5:
                raise RuntimeError("Captura demasiado larga; repite la pregunta")


def grabar(control, args):
    bloques = []
    total = math.ceil(args.segundos / DURACION_BLOQUE)
    with ReceptorAudio(control, args.puerto_udp) as audio:
        for i in range(total):
            bloque = audio.leer()
            bloques.append(bloque)
            if i % 31 == 0:
                muestras = array.array("h")
                muestras.frombytes(bloque)
                if sys.byteorder != "little":
                    muestras.byteswap()
                pico = max(abs(n) for n in muestras)
                print(f"{i * DURACION_BLOQUE:5.1f} s | RMS={nivel_rms(bloque):7.1f} | pico={pico:5d}")
        print(audio.resumen())
        audio.comprobar_calidad()
    cantidad = round(args.segundos * FRECUENCIA) * 2
    wav_guardar(args.salida, b"".join(bloques)[:cantidad])
    control.estado("esperando", "Grabacion terminada")
    print(f"WAV guardado: {args.salida.resolve()}")


def conversar(control, args):
    historial = []
    while True:
        control.estado("esperando", "Pulsa Enter en la Orange Pi")
        orden = input("\nEnter para hablar; q para salir; limpiar para borrar contexto: ").strip().lower()
        if orden == "q":
            return
        if orden == "limpiar":
            historial.clear()
            continue
        try:
            pcm = capturar_frase(control, args)
            archivo = BASE / "grabaciones" / "ultima_frase.wav"
            wav_guardar(archivo, pcm)
            control.estado("transcribiendo", "Reconociendo tu pregunta...")
            inicio = time.monotonic()
            pregunta = transcribir(args, archivo)
            tiempo_voz = time.monotonic() - inicio
            if not pregunta:
                print("No se obtuvo texto. Repite la pregunta.")
                control.estado("error", "No entendi. Repite la pregunta")
                continue
            print(f"\nTu: {pregunta}\nTranscripcion: {tiempo_voz:.1f} s", flush=True)
            control.estado("pensando", "Generando respuesta...")
            inicio = time.monotonic()
            respuesta = responder(args, pregunta, historial)
            tiempo_ia = time.monotonic() - inicio
            print(f"\nAsistente: {respuesta}\nGeneracion: {tiempo_ia:.1f} s", flush=True)
            control.resultado(respuesta)
            # Este archivo se reemplaza en cada turno; no acumula toda la conversacion.
            (BASE / "grabaciones" / "ultimo_turno.json").write_text(json.dumps({
                "pregunta": pregunta, "respuesta": respuesta,
                "segundos_transcripcion": tiempo_voz, "segundos_generacion": tiempo_ia,
            }, ensure_ascii=False, indent=2), encoding="utf-8")
            historial = [{"role": "user", "content": pregunta[:300]},
                         {"role": "assistant", "content": respuesta[:500]}]
            if args.una_vez:
                return
            # La respuesta permanece en la OLED mientras el usuario la lee.
            input("\nEnter cuando termines de leer la respuesta...")
        except (RuntimeError, OSError, ValueError, wave.Error) as exc:
            print(f"\nError de turno: {exc}", file=sys.stderr)
            if control.fallo.is_set():
                raise
            control.estado("error", "Prueba fallida. Revisa consola")
            if args.una_vez:
                raise


def argumentos(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--esp", help="IP o nombre de la ESP; obligatoria para pruebas de red")
    parser.add_argument("--puerto-http", type=int, default=80)
    parser.add_argument("--puerto-udp", type=int, default=3333)
    parser.add_argument("--hilos", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=300, help="limite de cada operacion de inferencia, segundos")
    parser.add_argument("--whisper", type=Path, default=BASE / "motores/whisper.cpp/build/bin/whisper-cli")
    parser.add_argument("--modelo-voz", type=Path, default=BASE / "modelos/ggml-tiny.bin")
    parser.add_argument("--llama-server", type=Path, default=BASE / "motores/llama.cpp/build/bin/llama-server")
    parser.add_argument("--modelo-ia", type=Path, default=BASE / "modelos/qwen2.5-0.5b-instruct-q4_k_m.gguf")
    parser.add_argument("--puerto-modelo", type=int, default=8081)
    parser.add_argument("--llm-url", help="opcional: servidor llama.cpp ya iniciado, por ejemplo http://127.0.0.1:8081")
    parser.add_argument("--motor-ia", choices=("local", "prueba"), default="local")
    parser.add_argument("--tokens", type=int, default=96)
    sub = parser.add_subparsers(dest="modo", required=True)
    sub.add_parser("diagnostico", help="comprueba HTTP, WebSocket, registro UDP y microfono")
    sub.add_parser("pantalla", help="recorre las expresiones sin cargar IA")
    grabacion = sub.add_parser("grabar", help="recibe audio de la ESP y guarda WAV")
    grabacion.add_argument("--segundos", type=float, default=6)
    grabacion.add_argument("--salida", type=Path, default=BASE / "grabaciones/prueba.wav")
    voz = sub.add_parser("transcribir", help="convierte un WAV en texto; no requiere ESP")
    voz.add_argument("archivo", type=Path)
    texto = sub.add_parser("texto", help="pregunta escrita al modelo; --esp es opcional")
    texto.add_argument("pregunta")
    charla = sub.add_parser("conversar", help="microfono ESP -> transcripcion -> respuesta escrita")
    charla.add_argument("--umbral", type=float, default=300, help="umbral RMS, calibrar con grabar")
    charla.add_argument("--silencio", type=float, default=1.0)
    charla.add_argument("--max-frase", type=float, default=15)
    charla.add_argument("--espera-voz", type=float, default=10)
    charla.add_argument("--una-vez", action="store_true")
    args = parser.parse_args(argv)
    if args.modo not in {"texto", "transcribir"} and not args.esp:
        parser.error("Este modo requiere --esp IP antes del nombre del modo")
    for nombre in ("hilos", "timeout", "tokens", "segundos", "umbral", "silencio", "max_frase", "espera_voz"):
        valor = getattr(args, nombre, 1)
        if not math.isfinite(valor) or valor <= 0:
            parser.error(f"--{nombre.replace('_', '-')} debe ser un numero positivo finito")
    for nombre in ("puerto_http", "puerto_udp", "puerto_modelo"):
        if not 1 <= getattr(args, nombre) <= 65535:
            parser.error(f"Puerto invalido: {nombre}")
    if args.tokens > 256:
        parser.error("Usa como maximo 256 tokens para el contexto de esta practica")
    if getattr(args, "segundos", 1) > 60 or getattr(args, "max_frase", 1) > 30:
        parser.error("Limite: 60 segundos de grabacion y 30 segundos por frase")
    return args


def main(argv=None) -> int:
    args = argumentos(argv)
    try:
        if args.modo == "transcribir":
            print(transcribir(args, args.archivo))
            return 0
        if args.modo == "texto" and not args.esp:
            print(responder(args, args.pregunta, []))
            return 0
        with ControlESP(args.esp, args.puerto_http) as control:
            if args.modo == "diagnostico":
                estado = http_json(f"http://{control.ip}:{args.puerto_http}/estado")
                print(json.dumps(estado, indent=2, ensure_ascii=False))
                if not estado.get("microfono"):
                    raise RuntimeError("HTTP y WS funcionan, pero fallo la inicializacion del microfono")
                with ReceptorAudio(control, args.puerto_udp) as audio:
                    bloques = [audio.leer() for _ in range(16)]
                    audio.comprobar_calidad()
                    print(audio.resumen())
                    print(f"RMS promedio: {sum(nivel_rms(b) for b in bloques) / len(bloques):.1f}")
                control.estado("esperando", "Diagnostico correcto")
                print("OK: HTTP, WebSocket, registro y recepcion de audio UDP.")
            elif args.modo == "pantalla":
                for nombre, texto in [
                    ("esperando", "Lista para conversar"), ("escuchando", "Te escucho..."),
                    ("transcribiendo", "Reconociendo palabras..."), ("pensando", "Pensando..."),
                    ("respuesta", "Hola, soy tu asistente virtual."), ("error", "Ejemplo de error"),
                ]:
                    print(nombre)
                    control.estado(nombre, texto)
                    time.sleep(3)
                control.estado("esperando", "Prueba de pantalla terminada")
            elif args.modo == "grabar":
                grabar(control, args)
            elif args.modo == "texto":
                control.estado("pensando", "Generando respuesta...")
                respuesta = responder(args, args.pregunta, [])
                print(respuesta)
                control.resultado(respuesta)
                input("Enter cuando termines de leer la OLED...")
            elif args.modo == "conversar":
                conversar(control, args)
        return 0
    except KeyboardInterrupt:
        print("\nPrograma detenido. Captura y motores cerrados.")
        return 130
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
