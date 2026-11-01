#!/usr/bin/env python3
# =============================================================================
# tools/status_poll.py — Poller do endpoint /status do ESP32
# =============================================================================
#
# Consulta https://<device>:<port>/status periodicamente e imprime todas as
# métricas formatadas por categoria.
#
# Uso:
#   python tools/status_poll.py
#   python tools/status_poll.py --device <IP>
#   python tools/status_poll.py --device <IP> --interval 2
#   python tools/status_poll.py --once        # imprime uma vez e sai
#   python tools/status_poll.py --json        # saída crua (para pipe/jq)
#
# Pré-requisitos:
#   - Python 3.8+
#   - Sem dependências externas (só stdlib)
# =============================================================================

import argparse
import json
import ssl
import sys
import urllib.error
import urllib.request
from datetime import datetime


# =============================================================================
# Formatação
# =============================================================================

# Ordem dos campos por categoria. Cada tupla: (chave, rótulo, sufixo).
CATEGORIES = [
    ("Geral", [
        ("state",       "Estado",       ""),
        ("uptime_ms",   "Uptime",       "ms"),
        ("running",     "Partição",     ""),
        ("next",        "Próxima OTA",  ""),
        ("pending_verify", "Verify pendente", ""),
    ]),
    ("Wi-Fi", [
        ("wifi_connected", "Conectado", ""),
        ("wifi_rssi",      "RSSI",      "dBm"),
    ]),
    ("MQTT", [
        ("mqtt_connected",  "Conectado",   ""),
        ("mqtt_published",  "Publicadas",  ""),
        ("mqtt_received",   "Recebidas",   ""),
        ("mqtt_dropped",    "Descartadas", ""),
    ]),
    ("Filas", [
        ("queue_comm_msg",     "comm_msg",     ""),
        ("queue_camera_cmd",   "camera_cmd",   ""),
        ("queue_actuator_cmd", "actuator_cmd", ""),
    ]),
    ("Imagens", [
        ("images_captured",           "Capturadas",     ""),
        ("images_sent",               "Enviadas",       ""),
        ("images_rejected_quality",   "Rej. qualidade", ""),
        ("images_rejected_network",   "Rej. rede",      ""),
    ]),
    ("Qualidade", [
        ("avg_brightness", "Brilho médio",    ""),
        ("avg_sharpness",  "Nitidez média",   ""),
    ]),
    ("Resultado", [
        ("last_result", "Último",     ""),
        ("last_error",  "Último erro",""),
    ]),
    ("Features", [
        ("hmac_enabled",           "HMAC",           ""),
        ("quality_filter_enabled", "Filtro qualidade", ""),
    ]),
]


def _format_value(v, suffix: str) -> str:
    """Formata um valor para exibição."""
    if isinstance(v, bool):
        return "sim" if v else "não"
    if v is None:
        return "-"
    if isinstance(v, int):
        s = f"{v}"
    elif isinstance(v, float):
        s = f"{v:.1f}"
    else:
        s = str(v)
    if suffix and s != "-":
        s += f" {suffix}"
    return s


def print_status(data: dict) -> None:
    """Imprime o JSON do /status formatado por categoria."""
    ts = datetime.now().strftime("%H:%M:%S")

    print()
    print("=" * 60)
    print(f"  /status  —  {ts}")
    print("=" * 60)

    for category, fields in CATEGORIES:
        print(f"\n┌─ {category} " + "─" * (55 - len(category)))
        for key, label, suffix in fields:
            if key in data:
                val = _format_value(data[key], suffix)
                print(f"│ {label:22s} {val}")
        print(f"└" + "─" * 58)


# =============================================================================
# Cliente HTTP
# =============================================================================

def fetch_status(url: str, timeout: float) -> dict:
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    with urllib.request.urlopen(url, context=ctx, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


# =============================================================================
# CLI
# =============================================================================

def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Poller do endpoint /status do ESP32",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--device", default="10.198.76.23",
                   help="IP do ESP32")
    p.add_argument("--port", type=int, default=8443,
                   help="Porta HTTPS do servidor de OTA")
    p.add_argument("--interval", type=float, default=5.0,
                   help="Intervalo entre polls em segundos")
    p.add_argument("--timeout", type=float, default=5.0,
                   help="Timeout da conexão HTTP")
    p.add_argument("--once", action="store_true",
                   help="Executa uma vez e sai")
    p.add_argument("--json", action="store_true",
                   help="Imprime JSON cru em vez de formatar")
    p.add_argument("--compact", action="store_true",
                   help="Formato compacto (uma linha por poll)")
    return p


def main() -> int:
    args = build_argparser().parse_args()
    url = f"https://{args.device}:{args.port}/status"

    import time

    while True:
        try:
            data = fetch_status(url, args.timeout)

            if args.json:
                print(json.dumps(data, ensure_ascii=False))
            elif args.compact:
                ts = datetime.now().strftime("%H:%M:%S")
                print(f"[{ts}] state={data.get('state','?'):10s} "
                      f"sent={data.get('images_sent',0):4d} "
                      f"rej_q={data.get('images_rejected_quality',0):3d} "
                      f"rej_n={data.get('images_rejected_network',0):3d} "
                      f"rssi={data.get('wifi_rssi',-127):4d}dBm "
                      f"bright={data.get('avg_brightness',0):3d} "
                      f"sharp={data.get('avg_sharpness',0):4d}")
            else:
                print_status(data)

        except urllib.error.HTTPError as exc:
            print(f"[{datetime.now():%H:%M:%S}] HTTP {exc.code} {exc.reason}",
                  file=sys.stderr)
        except urllib.error.URLError as exc:
            print(f"[{datetime.now():%H:%M:%S}] Erro de conexão: {exc.reason}",
                  file=sys.stderr)
        except TimeoutError:
            print(f"[{datetime.now():%H:%M:%S}] Timeout após {args.timeout}s",
                  file=sys.stderr)
        except KeyboardInterrupt:
            print("\nEncerrando.")
            return 0
        except Exception as exc:  # noqa: BLE001
            print(f"[{datetime.now():%H:%M:%S}] Erro: {exc}", file=sys.stderr)

        if args.once:
            return 0

        try:
            time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\nEncerrando.")
            return 0


if __name__ == "__main__":
    sys.exit(main())