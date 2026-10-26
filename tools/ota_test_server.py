#!/usr/bin/env python3
# =============================================================================
# tools/ota_test_server.py — Servidor/cliente HTTPS para testar OTA
# =============================================================================
#
# Suporta dois modos de teste de OTA:
#
#   SERVE (pull):
#     O PC hospeda um servidor HTTPS local que serve o .bin. O ESP32 baixa
#     via ota_https_update_start(). Simula "atualização pela internet".
#
#       python tools/ota_test_server.py serve build\LockMaxxing.bin --port 8443
#
#     Saída:
#       [SERVE] HTTPS em https://192.168.1.100:8443/LockMaxxing.bin
#       [SERVE] SHA-256: abc123...
#       [SERVE] Use no firmware: ota_https_update_start("https://...", "abc123...")
#
#   PUSH (local server no ESP32):
#     O ESP32 hospeda o servidor HTTPS em :8443. O PC envia o .bin via POST
#     /update. Simula "atualização via rede local".
#
#       python tools/ota_test_server.py push build\LockMaxxing.bin --device 192.168.1.42
#
#     Saída:
#       [PUSH] Enviando 1024 KB para https://192.168.1.42:8443/update
#       [PUSH] SHA-256: abc123...
#       [PUSH] 200 OK — OTA aplicada. A placa vai reiniciar em ~2s
#
#   SHA256 (utilitário):
#     Calcula o SHA-256 de um arquivo, no formato que o firmware espera
#     (64 chars hex lowercase). Útil para passar ao ota_https_update_start().
#
#       python tools/ota_test_server.py sha256 build\LockMaxxing.bin
#
# Pré-requisitos:
#   - Python 3.8+
#   - certificado auto-assinado em main/certs.h (gerado por tools/gen_cert.sh
#     durante o build do firmware) para o modo SERVE
#   - ESP32 no modo push precisa de ota_local_server_start() chamado (já é
#     feito em app_main)
#
# Compatível com Windows, Linux e macOS.
# =============================================================================

import argparse
import hashlib
import http.server
import json
import os
import ssl
import sys
import urllib.error
import urllib.request
from pathlib import Path

# =============================================================================
# Utilitários
# =============================================================================

def file_sha256(path: Path) -> str:
    """Calcula SHA-256 do arquivo. Retorna 64 chars hex lowercase."""
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def file_size_human(n: int) -> str:
    for unit in ("B", "KB", "MB"):
        if n < 1024:
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} {unit}"
        n /= 1024
    return f"{n:.1f} GB"


def validate_bin(path: Path) -> None:
    """Verifica que o arquivo existe e parece um binário ESP-IDF válido."""
    if not path.exists():
        raise FileNotFoundError(f"Arquivo não encontrado: {path}")
    if not path.is_file():
        raise ValueError(f"Não é um arquivo: {path}")

    size = path.stat().st_size
    if size < 4096:
        raise ValueError(f"Arquivo muito pequeno ({size} B) — não parece um .bin de firmware")

    # ESP-IDF app binaries começam com 0xE9 (magic byte do ESP-IDF app header)
    with open(path, "rb") as fh:
        magic = fh.read(1)
    if magic != b"\xE9":
        print(f"[AVISO] Primeiro byte é 0x{magic[0]:02X}, esperado 0xE9 (ESP-IDF app).", file=sys.stderr)
        print("[AVISO] O arquivo pode não ser um .bin de app ESP-IDF válido.", file=sys.stderr)
        print("[AVISO] Verifique se apontou para build/LockMaxxing.bin (não bootloader.bin).", file=sys.stderr)


def find_certs_h() -> Path | None:
    """Procura main/certs.h a partir da raiz do projeto."""
    here = Path(__file__).resolve().parent.parent  # tools/../
    candidate = here / "main" / "certs.h"
    return candidate if candidate.exists() else None


def extract_certs_from_header(header: Path) -> tuple[Path, Path]:
    """
    Extrai o certificado e a chave privada do main/certs.h para arquivos
    temporários (server.pem e key.pem) que o Python pode usar via ssl.

    Retorna (cert_path, key_path).
    """
    import re

    text = header.read_text(encoding="utf-8", errors="replace")

    def extract(name: str) -> str:
        # Procura: static const char NAME[] = "linha1\n" "linha2\n" ... ;
        m = re.search(rf'{name}\[\]\s*=\s*((?:\s*"[^"]*"\s*)+)', text)
        if not m:
            raise ValueError(f"Bloco '{name}' não encontrado em {header}")
        raw = m.group(1)
        # Concatena todas as strings literais C
        parts = re.findall(r'"([^"]*)"', raw)
        joined = "".join(parts)
        # Desfaz escapes \n e \" do C
        joined = joined.replace("\\n", "\n").replace('\\"', '"').replace("\\\\", "\\")
        return joined

    cert_pem = extract("SERVER_CERT_PEM")
    key_pem = extract("SERVER_KEY_PEM")

    tmp_dir = Path(__file__).resolve().parent / ".tmp_certs"
    tmp_dir.mkdir(exist_ok=True)
    cert_path = tmp_dir / "server.pem"
    key_path = tmp_dir / "key.pem"

    cert_path.write_text(cert_pem, encoding="utf-8")
    key_path.write_text(key_pem, encoding="utf-8")
    return cert_path, key_path


# =============================================================================
# Modo SERVE — hospeda o .bin via HTTPS
# =============================================================================

class BinHandler(http.server.SimpleHTTPRequestHandler):
    """Servidor HTTP que serve o .bin configurado."""

    bin_path: Path = None  # type: ignore
    sha256_hex: str = ""
    serve_url_path: str = ""

    def do_GET(self):  # noqa: N802 — nome exigido pela stdlib
        if self.path != f"/{self.serve_url_path}" and self.path != "/":
            self.send_error(404, "Not found")
            return

        if self.path == "/":
            # Lista simples
            body = (
                f"OTA Test Server\n"
                f"File: {self.serve_url_path}\n"
                f"SHA-256: {self.sha256_hex}\n"
                f"Size: {self.bin_path.stat().st_size} bytes\n"
                f"\nGET /{self.serve_url_path} to download\n"
            ).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        # Download do .bin
        size = self.bin_path.stat().st_size
        print(f"[SERVE] Cliente conectou: {self.client_address[0]}")
        print(f"[SERVE] Enviando {file_size_human(size)} ({self.serve_url_path})")

        try:
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("X-SHA256", self.sha256_hex)
            self.end_headers()

            with open(self.bin_path, "rb") as fh:
                while True:
                    chunk = fh.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    self.wfile.flush()

            print(f"[SERVE] Envio concluído")
        except (BrokenPipeError, ConnectionResetError) as exc:
            print(f"[SERVE] Conexão interrompida: {exc}")

    def log_message(self, fmt, *args):
        # Silencia o log padrão (spam de cada request)
        pass


def cmd_serve(args) -> int:
    bin_path = Path(args.file).resolve()
    validate_bin(bin_path)

    sha256_hex = file_sha256(bin_path)
    size = bin_path.stat().st_size

    # Prepara certificado TLS a partir de main/certs.h
    certs_h = find_certs_h()
    if certs_h is None:
        print(f"[SERVE] ERRO: main/certs.h não encontrado.", file=sys.stderr)
        print(f"[SERVE] Rode o build do firmware primeiro para gerar certs.h.", file=sys.stderr)
        print(f"[SERVE] Alternativa: python tools/ota_test_server.py push ...", file=sys.stderr)
        return 1

    try:
        cert_pem, key_pem = extract_certs_from_header(certs_h)
    except Exception as exc:  # noqa: BLE001
        print(f"[SERVE] ERRO ao extrair certificado de {certs_h}: {exc}", file=sys.stderr)
        return 1

    # Configura o handler
    BinHandler.bin_path = bin_path
    BinHandler.sha256_hex = sha256_hex
    BinHandler.serve_url_path = bin_path.name

    httpd = http.server.HTTPServer((args.bind, args.port), BinHandler)

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=str(cert_pem), keyfile=str(key_pem))
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    # Descobre IP local para a URL
    import socket as _socket
    try:
        probe = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        local_ip = probe.getsockname()[0]
        probe.close()
    except OSError:
        local_ip = "127.0.0.1"

    url = f"https://{local_ip}:{args.port}/{bin_path.name}"

    print(f"[SERVE] Arquivo:   {bin_path}")
    print(f"[SERVE] Tamanho:   {file_size_human(size)} ({size} bytes)")
    print(f"[SERVE] SHA-256:   {sha256_hex}")
    print(f"[SERVE] URL:       {url}")
    print(f"[SERVE] Cert:      auto-assinado (use -k ou --insecure no cliente)")
    print()
    print(f"Para testar via pull no firmware:")
    print(f"  ota_https_update_start(")
    print(f'    "{url}",')
    print(f'    "{sha256_hex}",')
    print(f"    progress_cb, result_cb);")
    print()
    print(f"Ou teste via curl de qualquer máquina na rede:")
    print(f"  curl -k -o baixado.bin {url}")
    print()
    print(f"[SERVE] Escutando... (Ctrl+C para parar)")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[SERVE] Encerrando")
    finally:
        httpd.server_close()

    return 0


# =============================================================================
# Modo PUSH — envia o .bin para o servidor HTTPS local do ESP32
# =============================================================================

def cmd_push(args) -> int:
    bin_path = Path(args.file).resolve()
    validate_bin(bin_path)

    sha256_hex = file_sha256(bin_path)
    size = bin_path.stat().st_size
    url = f"https://{args.device}:{args.port}/update"

    print(f"[PUSH] Arquivo:  {bin_path}")
    print(f"[PUSH] Tamanho:  {file_size_human(size)} ({size} bytes)")
    print(f"[PUSH] SHA-256:  {sha256_hex}")
    print(f"[PUSH] Destino:  {url}")
    print(f"[PUSH] Enviando...")

    if args.dry_run:
        print(f"[PUSH] --dry-run: nenhum byte enviado")
        return 0

    # Contexto SSL que aceita auto-assinado (como curl -k)
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    # Prepara a request
    with open(bin_path, "rb") as fh:
        data = fh.read()

    req = urllib.request.Request(
        url,
        data=data,
        method="POST",
        headers={
            "Content-Type": "application/octet-stream",
            "Content-Length": str(len(data)),
            "X-SHA256": sha256_hex,
            "User-Agent": "esp32-ota-test/1.0",
        },
    )

    try:
        with urllib.request.urlopen(req, context=ctx, timeout=args.timeout) as resp:
            body = resp.read().decode("utf-8", errors="replace")
            print(f"[PUSH] HTTP {resp.status} {resp.reason}")
            print(f"[PUSH] Resposta: {body}")
            if resp.status == 200:
                print(f"[PUSH] OTA aplicada — a placa vai reiniciar em ~2s")
                print(f"[PUSH] Acompanhe o reboot no monitor serial.")
                return 0
            print(f"[PUSH] Status inesperado: {resp.status}", file=sys.stderr)
            return 1

    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")

        # Tratamento especial para warmup (503 com corpo JSON)
        if exc.code == 503:
            try:
                info = json.loads(body)
                if info.get("error") == "warmup":
                    retry = info.get("retry_in_s", 60)
                    print(f"[PUSH] Servidor em warmup.")
                    print(f"[PUSH] Uptime: {info.get('uptime_s', '?')}s")
                    print(f"[PUSH] Tente novamente em ~{retry}s.")
                    return 1
            except (json.JSONDecodeError, KeyError):
                pass  # cai no tratamento genérico abaixo

        print(f"[PUSH] HTTP {exc.code} {exc.reason}", file=sys.stderr)
        print(f"[PUSH] Resposta: {body}", file=sys.stderr)
        return 1
    except urllib.error.URLError as exc:
        print(f"[PUSH] Erro de conexão: {exc.reason}", file=sys.stderr)
        print(f"[PUSH] Verifique: IP do ESP32, Wi-Fi conectado, servidor HTTPS ativo", file=sys.stderr)
        return 1
    except TimeoutError:
        print(f"[PUSH] Timeout após {args.timeout}s", file=sys.stderr)
        print(f"[PUSH] Verifique se o servidor HTTPS local está rodando (log: OTA_SRV)", file=sys.stderr)
        return 1


# =============================================================================
# Modo SHA256
# =============================================================================

def cmd_sha256(args) -> int:
    bin_path = Path(args.file).resolve()
    validate_bin(bin_path)
    sha256_hex = file_sha256(bin_path)
    print(sha256_hex)
    return 0


# =============================================================================
# CLI
# =============================================================================

def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Servidor/cliente HTTPS para testar OTA no LockMaxxing",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    # --- serve ---
    s = sub.add_parser("serve", help="Hospeda o .bin via HTTPS (pull)")
    s.add_argument("file", help="Caminho para o .bin do firmware")
    s.add_argument("--bind", default="0.0.0.0",
                   help="IP de bind (0.0.0.0 = todas as interfaces)")
    s.add_argument("--port", type=int, default=8443,
                   help="Porta HTTPS")
    s.set_defaults(func=cmd_serve)

    # --- push ---
    pu = sub.add_parser("push", help="Envia o .bin via POST para o ESP32 (push)")
    pu.add_argument("file", help="Caminho para o .bin do firmware")
    pu.add_argument("--device", required=True,
                    help="IP do ESP32 (ex: 192.168.1.42)")
    pu.add_argument("--port", type=int, default=8443,
                    help="Porta HTTPS do ESP32")
    pu.add_argument("--timeout", type=int, default=120,
                    help="Timeout de conexão em segundos")
    pu.add_argument("--dry-run", action="store_true",
                    help="Não envia, apenas mostra o que faria")
    pu.set_defaults(func=cmd_push)

    # --- sha256 ---
    sh = sub.add_parser("sha256", help="Calcula SHA-256 de um arquivo")
    sh.add_argument("file", help="Caminho para o arquivo")
    sh.set_defaults(func=cmd_sha256)

    return p


def main() -> int:
    args = build_argparser().parse_args()
    try:
        return args.func(args)
    except FileNotFoundError as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        return 2
    except ValueError as exc:
        print(f"ERRO: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())