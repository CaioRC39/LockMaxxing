#!/usr/bin/env python3
# =============================================================================
# tools/mqtt_test_server.py — Servidor MQTT de teste (análise simulada)
# =============================================================================
#
# Faz o papel do "sistema de análise" do projeto LockMaxxing:
#   - Assina os tópicos REQUEST (metadados) e IMAGE (chunks binários)
#   - Reconstitui o JPEG a partir dos chunks
#   - Salva em disco para inspeção
#   - Publica {"id":N,"resultado":"aprovado"|"reprovado"} no tópico RESULT
#
# Pré-requisitos:
#   pip install paho-mqtt
#   Broker MQTT local (Mosquitto): veja README para instalação
#
# Uso básico:
#   python3 tools/mqtt_test_server.py --broker localhost
#
# Modos de decisão (--mode):
#   always-approve  (default) Sempre aprova
#   always-reject            Sempre rejeita
#   alternate                Aprova IDs pares, rejeita ímpares
#   random                   Sorteia
#
# Protocolo de chunk (definido em main/config.h):
#   [0..3]  image_id     uint32 big-endian
#   [4..5]  chunk_index  uint16 big-endian
#   [6..7]  total_chunks uint16 big-endian
#   [8..N]  payload      bytes do JPEG
#
# Compatível com paho-mqtt 1.x e 2.x.
# =============================================================================

import argparse
import json
import os
import random
import signal
import struct
import sys
import threading
import time
from dataclasses import dataclass, field
from datetime import datetime

try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("ERRO: paho-mqtt não instalado.", file=sys.stderr)
    print("Instale com: pip install paho-mqtt", file=sys.stderr)
    sys.exit(1)


# =============================================================================
# Constantes do protocolo — devem espelhar main/config.h
# =============================================================================

CHUNK_HEADER_SIZE = 8
MAX_IMAGE_SIZE    = 200 * 1024
IMAGE_TIMEOUT_S   = 30.0
MAX_CHUNKS        = 1000


# =============================================================================
# Estado de uma imagem em recebimento
# =============================================================================

@dataclass
class PendingImage:
    image_id:     int
    total_chunks: int
    total_size:   int
    received_at:  float
    chunks:       dict = field(default_factory=dict)

    def add_chunk(self, idx: int, data: bytes) -> None:
        self.chunks[idx] = data

    def is_complete(self) -> bool:
        if len(self.chunks) != self.total_chunks:
            return False
        return all(i in self.chunks for i in range(self.total_chunks))

    def reconstruct(self) -> bytes:
        return b"".join(self.chunks[i] for i in range(self.total_chunks))


# =============================================================================
# Compatibilidade paho-mqtt 1.x / 2.x
# =============================================================================

def _make_mqtt_client(client_id: str) -> "mqtt.Client":
    """Cria um Client que funciona tanto em paho-mqtt 1.x quanto 2.x."""
    try:
        from paho.mqtt.client import CallbackAPIVersion
        return mqtt.Client(
            callback_api_version=CallbackAPIVersion.VERSION1,
            client_id=client_id,
            clean_session=True,
        )
    except (ImportError, TypeError):
        return mqtt.Client(client_id=client_id, clean_session=True)


# =============================================================================
# Servidor
# =============================================================================

class ImageAnalyzer:

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.pending_images: "dict[int, PendingImage]" = {}
        self.lock    = threading.Lock()
        self.running = True

        self.stats = {
            "requests":        0,
            "images_complete": 0,
            "images_timeout":  0,
            "images_duplicate":0,
            "chunks_total":    0,
            "bytes_received":  0,
            "approved":        0,
            "rejected":        0,
        }

        self.client = _make_mqtt_client(client_id="mqtt-test-server")
        if args.username:
            self.client.username_pw_set(args.username, args.password or "")
        self.client.on_connect    = self.on_connect
        self.client.on_disconnect = self.on_disconnect
        self.client.on_message    = self.on_message

        os.makedirs(args.output_dir, exist_ok=True)

    # -------------------------------------------------------------------------
    # Callbacks MQTT
    # -------------------------------------------------------------------------

    def on_connect(self, client, userdata, flags, rc) -> None:
        if rc == 0:
            print(f"[MQTT] Conectado a {self.args.broker}:{self.args.port}")
            client.subscribe(self.args.topic_request, qos=1)
            client.subscribe(self.args.topic_image,   qos=1)
            print(f"[MQTT] Assinado: {self.args.topic_request}")
            print(f"[MQTT] Assinado: {self.args.topic_image}")
        else:
            print(f"[MQTT] Falha na conexão, rc={rc}")

    def on_disconnect(self, client, userdata, rc) -> None:
        if rc != 0 and self.running:
            print(f"[MQTT] Desconectado inesperadamente, rc={rc}")

    def on_message(self, client, userdata, msg) -> None:
        try:
            if msg.topic == self.args.topic_request:
                self.handle_request(msg.payload)
            elif msg.topic == self.args.topic_image:
                self.handle_chunk(msg.payload)
        except Exception as exc:  # noqa: BLE001 — não deixar exceção matar o loop
            print(f"[ERRO] on_message: {exc}")

    # -------------------------------------------------------------------------
    # Handlers
    # -------------------------------------------------------------------------

    def handle_request(self, payload: bytes) -> None:
        try:
            data = json.loads(payload.decode("utf-8"))
        except (json.JSONDecodeError, UnicodeDecodeError) as exc:
            print(f"[REQUEST] JSON inválido: {exc}")
            return

        image_id     = data.get("id")
        total_size   = data.get("size", 0)
        total_chunks = data.get("chunks", 0)

        if not isinstance(image_id, int) or image_id <= 0:
            print(f"[REQUEST] id inválido: {image_id!r}")
            return
        if not isinstance(total_size, int) or total_size <= 0 \
           or total_size > MAX_IMAGE_SIZE:
            print(f"[REQUEST] size inválido: {total_size!r}")
            return
        if not isinstance(total_chunks, int) or total_chunks <= 0 \
           or total_chunks > MAX_CHUNKS:
            print(f"[REQUEST] chunks inválido: {total_chunks!r}")
            return

        with self.lock:
            if image_id in self.pending_images:
                print(f"[REQUEST] id={image_id} já pendente — sobrescrevendo")
                self.stats["images_duplicate"] += 1

            self.pending_images[image_id] = PendingImage(
                image_id=image_id,
                total_chunks=total_chunks,
                total_size=total_size,
                received_at=time.time(),
            )
            self.stats["requests"] += 1

        print(f"[REQUEST] id={image_id}, size={total_size}B, "
              f"chunks={total_chunks}")

    def handle_chunk(self, payload: bytes) -> None:
        if len(payload) < CHUNK_HEADER_SIZE:
            print(f"[CHUNK] payload curto demais ({len(payload)}B)")
            return

        image_id     = struct.unpack(">I", payload[0:4])[0]
        chunk_idx    = struct.unpack(">H", payload[4:6])[0]
        total_chunks = struct.unpack(">H", payload[6:8])[0]
        data         = payload[CHUNK_HEADER_SIZE:]

        with self.lock:
            img = self.pending_images.get(image_id)
            if img is None:
                # Chega ocasionalmente porque o broker pode reordenar
                # mensagens entre tópicos diferentes (request vs image).
                print(f"[CHUNK] id={image_id} sem request prévio — descarta")
                return

            if img.total_chunks != total_chunks:
                print(f"[CHUNK] id={image_id}: total_chunks inconsistente "
                      f"(request={img.total_chunks}, chunk={total_chunks})")
                return

            if chunk_idx >= total_chunks:
                print(f"[CHUNK] id={image_id}: idx={chunk_idx} >= "
                      f"total={total_chunks}")
                return

            img.add_chunk(chunk_idx, data)
            self.stats["chunks_total"]   += 1
            self.stats["bytes_received"] += len(data)

            if img.is_complete():
                # Remove do dicionário ANTES de finalizar, para evitar
                # reentrância se um chunk duplicado chegar durante o save.
                del self.pending_images[image_id]
                self.finalize_image(img)

    # -------------------------------------------------------------------------
    # Finalização de uma imagem
    # -------------------------------------------------------------------------

    def finalize_image(self, img: PendingImage) -> None:
        try:
            data = img.reconstruct()
        except (KeyError, ValueError) as exc:
            print(f"[FINALIZE] id={img.image_id}: erro na reconstrução: {exc}")
            return

        self.stats["images_complete"] += 1

        if len(data) != img.total_size:
            print(f"[FINALIZE] id={img.image_id}: tamanho divergente "
                  f"(esperado {img.total_size}B, obtido {len(data)}B)")

        # Verificação rápida de magic JPEG (SOI + EOI)
        if not (data[:2] == b"\xff\xd8" and data[-2:] == b"\xff\xd9"):
            print(f"[FINALIZE] id={img.image_id}: header/footer JPEG inválido")

        ts    = datetime.now().strftime("%Y%m%d_%H%M%S")
        fname = f"img_{ts}_id{img.image_id:08d}.jpg"
        fpath = os.path.join(self.args.output_dir, fname)
        try:
            with open(fpath, "wb") as fh:
                fh.write(data)
        except IOError as exc:
            print(f"[FINALIZE] id={img.image_id}: erro ao salvar: {exc}")
            return

        print(f"[FINALIZE] id={img.image_id}: {len(data)}B → {fpath}")

        resultado = self.decide_result(img.image_id)
        self.publish_result_async(img.image_id, resultado)

    def decide_result(self, image_id: int) -> str:
        if self.args.mode == "always-approve":
            return "aprovado"
        if self.args.mode == "always-reject":
            return "reprovado"
        if self.args.mode == "alternate":
            return "aprovado" if image_id % 2 == 0 else "reprovado"
        if self.args.mode == "random":
            return random.choice(["aprovado", "reprovado"])
        return "aprovado"

    # -------------------------------------------------------------------------
    # Publicação (async para não bloquear o message loop)
    # -------------------------------------------------------------------------

    def publish_result_async(self, image_id: int, resultado: str) -> None:
        delay = self.args.analysis_delay

        def _worker() -> None:
            if delay > 0:
                time.sleep(delay)
            self.publish_result(image_id, resultado)

        if delay > 0:
            threading.Thread(target=_worker, daemon=True).start()
        else:
            self.publish_result(image_id, resultado)

    def publish_result(self, image_id: int, resultado: str) -> None:
        payload = json.dumps({"id": image_id, "resultado": resultado})
        info = self.client.publish(self.args.topic_result, payload, qos=1)

        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            print(f"[RESULT] erro ao publicar id={image_id}: rc={info.rc}")
            return

        if resultado == "aprovado":
            self.stats["approved"] += 1
        else:
            self.stats["rejected"] += 1

        print(f"[RESULT] id={image_id} → {resultado}")

    # -------------------------------------------------------------------------
    # Timeouts
    # -------------------------------------------------------------------------

    def check_timeouts(self) -> None:
        now     = time.time()
        expired = []
        with self.lock:
            for image_id, img in list(self.pending_images.items()):
                if now - img.received_at > IMAGE_TIMEOUT_S:
                    expired.append(img)
                    del self.pending_images[image_id]

        for img in expired:
            missing = [i for i in range(img.total_chunks) if i not in img.chunks]
            preview = missing[:5]
            suffix  = "..." if len(missing) > 5 else ""
            print(f"[TIMEOUT] id={img.image_id}: faltam {len(missing)} "
                  f"chunks {preview}{suffix}")
            self.stats["images_timeout"] += 1

    def cleanup_loop(self) -> None:
        while self.running:
            time.sleep(2.0)
            if self.running:
                self.check_timeouts()

    # -------------------------------------------------------------------------
    # Loop principal
    # -------------------------------------------------------------------------

    def print_stats(self) -> None:
        print()
        print("=" * 60)
        print(" ESTATÍSTICAS")
        print("=" * 60)
        for key, val in self.stats.items():
            print(f"  {key:20s}: {val}")
        print("=" * 60)
        print()

    def run(self) -> int:
        print(f"[INIT] Modo: {self.args.mode}")
        print(f"[INIT] Output: {os.path.abspath(self.args.output_dir)}")
        print(f"[INIT] Timeout de imagem: {IMAGE_TIMEOUT_S}s")
        if self.args.analysis_delay > 0:
            print(f"[INIT] Delay de análise: {self.args.analysis_delay}s")

        try:
            self.client.connect(self.args.broker,
                                self.args.port,
                                keepalive=60)
        except Exception as exc:  # noqa: BLE001
            print(f"[ERRO] Falha ao conectar em "
                  f"{self.args.broker}:{self.args.port}: {exc}")
            return 1

        cleanup_thread = threading.Thread(target=self.cleanup_loop,
                                          daemon=True)
        cleanup_thread.start()

        def _sig_handler(signum, frame):  # noqa: ARG001
            print("\n[SIGNAL] Encerrando...")
            self.running = False
            self.client.disconnect()

        signal.signal(signal.SIGINT,  _sig_handler)
        signal.signal(signal.SIGTERM, _sig_handler)

        try:
            self.client.loop_forever()
        except KeyboardInterrupt:
            pass
        finally:
            self.running = False

        self.print_stats()
        return 0


# =============================================================================
# CLI
# =============================================================================

def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Servidor MQTT de teste para o projeto LockMaxxing",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    # Broker
    p.add_argument("--broker",   default="localhost",
                   help="Endereço do broker MQTT")
    p.add_argument("--port",     type=int, default=1883,
                   help="Porta do broker MQTT")
    p.add_argument("--username", default=None,
                   help="Usuário MQTT (opcional)")
    p.add_argument("--password", default=None,
                   help="Senha MQTT (opcional)")

    # Tópicos
    p.add_argument("--topic-request", default="lockmaxxing/camera/request",
                   help="Tópico de metadados (JSON)")
    p.add_argument("--topic-image",   default="lockmaxxing/camera/image",
                   help="Tópico de chunks binários")
    p.add_argument("--topic-result",  default="lockmaxxing/camera/result",
                   help="Tópico de resultado (JSON)")

    # Comportamento
    p.add_argument("--mode", default="always-approve",
                   choices=["always-approve", "always-reject",
                            "alternate", "random"],
                   help="Modo de decisão de resultado")
    p.add_argument("--analysis-delay", type=float, default=0.0,
                   help="Delay (s) antes de publicar o resultado")

    # Output
    p.add_argument("--output-dir", default="received_images",
                   help="Diretório onde as imagens recebidas são salvas")

    return p


def main() -> int:
    args = build_argparser().parse_args()
    return ImageAnalyzer(args).run()


if __name__ == "__main__":
    sys.exit(main())