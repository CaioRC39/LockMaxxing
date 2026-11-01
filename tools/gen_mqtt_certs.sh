#!/usr/bin/env bash
# =============================================================================
# tools/gen_mqtt_certs.sh — Gera main/mqtt_certs.h com certificados como arrays C
# =============================================================================
# Executado automaticamente pelo CMake a cada build que detectar mudanças
# em tools/certs/. Gera um header C com os PEMs prontos para embutir.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
CERTS_DIR="$PROJECT_ROOT/tools/certs"
OUTPUT="$PROJECT_ROOT/main/mqtt_certs.h"

if [ ! -f "$CERTS_DIR/ca.crt" ]; then
    echo "[gen_mqtt_certs] ERRO: $CERTS_DIR/ca.crt não encontrado." >&2
    exit 1
fi

echo "[gen_mqtt_certs] Gerando $OUTPUT"

emit_pem_literal() {
    local file="$1"
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$file"
}

{
    cat <<'HEADER'
/* =============================================================================
 * main/mqtt_certs.h — GERADO AUTOMATICAMENTE por tools/gen_mqtt_certs.sh
 * =============================================================================
 * NÃO EDITE À MÃO. Regenerado a cada build a partir de tools/certs/.
 * Contém chave privada — NÃO VERSIONE este arquivo.
 * ========================================================================== */

#ifndef MQTT_CERTS_H
#define MQTT_CERTS_H

HEADER

    if [ -f "$CERTS_DIR/ca.crt" ]; then
        echo "static const char MQTT_CA_PEM[] ="
        emit_pem_literal "$CERTS_DIR/ca.crt"
        echo "    ;"
        echo ""
        echo "static const unsigned int MQTT_CA_PEM_LEN = sizeof(MQTT_CA_PEM) - 1;"
        echo ""
    fi

    if [ -f "$CERTS_DIR/client.crt" ]; then
        echo "static const char MQTT_CLIENT_PEM[] ="
        emit_pem_literal "$CERTS_DIR/client.crt"
        echo "    ;"
        echo ""
        echo "static const unsigned int MQTT_CLIENT_PEM_LEN = sizeof(MQTT_CLIENT_PEM) - 1;"
        echo ""
    fi

    if [ -f "$CERTS_DIR/client.key" ]; then
        echo "static const char MQTT_CLIENT_KEY_PEM[] ="
        emit_pem_literal "$CERTS_DIR/client.key"
        echo "    ;"
        echo ""
        echo "static const unsigned int MQTT_CLIENT_KEY_PEM_LEN = sizeof(MQTT_CLIENT_KEY_PEM) - 1;"
        echo ""
    fi

    echo "#endif /* MQTT_CERTS_H */"
} > "$OUTPUT"

echo "[gen_mqtt_certs] OK: $OUTPUT ($(wc -c < "$OUTPUT") bytes)"