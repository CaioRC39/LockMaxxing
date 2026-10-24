#!/usr/bin/env bash
# =============================================================================
# tools/gen_cert.sh — Gerador de certificado TLS auto-assinado para o servidor
#                     HTTPS local de OTA (main/ota_server.c).
# =============================================================================
#
# Uso:
#   ./tools/gen_cert.sh            # gera main/certs.h se não existir
#   ./tools/gen_cert.sh --force    # regenera main/certs.h mesmo se existir
#
# Pré-requisitos:
#   - bash 4+
#   - openssl 1.1.1+ (presente por padrão em Linux/macOS; no Windows, use
#     Git Bash, WSL ou instale via "winget install ShiningLight.OpenSSL")
#
# O arquivo gerado, main/certs.h, contém:
#   static const char     SERVER_CERT_PEM[];
#   static const unsigned SERVER_CERT_PEM_LEN;
#   static const char     SERVER_KEY_PEM[];
#   static const unsigned SERVER_KEY_PEM_LEN;
#
# O certificado é auto-assinado, válido por 10 anos, com CN configurável
# abaixo. Clientes precisam aceitar auto-assinado:
#   curl  -k  https://<ip>:8443/status
#   python com ssl._create_unverified_context() ou carregando o cert
#
# NÃO VERSIONE main/certs.h no Git. Adicione ao .gitignore.
# =============================================================================

set -euo pipefail

# -----------------------------------------------------------------------------
# Configurações — edite aqui se quiser outro CN/SAN/validade
# -----------------------------------------------------------------------------

# Common Name do certificado. Não precisa ser resolvível em DNS — é
# apenas um identificador. O servidor escuta por IP.
CN_NAME="esp32-camera.local"

# Subject Alternative Names. Inclua localhost e o IP que a placa costuma
# receber na sua rede local (opcional, mas evita warnings em clientes modernos).
# Para descobrir o IP da placa após conectar, veja o log do monitor serial.
SAN_LIST="DNS:esp32-camera.local,DNS:localhost,IP:127.0.0.1"

# Validade em dias. 3650 = 10 anos. Ajuste se o firmware ficar em produção
# por mais tempo (improvável para bancada).
VALIDITY_DAYS=3650

# Tamanho da chave RSA. 2048 é o mínimo aceitável hoje e o mais rápido no
# handshake; o ESP32-S3 tem aceleração de RSA, então não é gargalo.
RSA_BITS=2048

# -----------------------------------------------------------------------------
# Resolve caminhos
# -----------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
OUTPUT_FILE="$PROJECT_ROOT/main/certs.h"

# -----------------------------------------------------------------------------
# Parse de argumentos
# -----------------------------------------------------------------------------

FORCE=false
for arg in "$@"; do
    case "$arg" in
        --force|-f) FORCE=true ;;
        --help|-h)
            echo "Uso: $0 [--force]"
            echo "  --force, -f   Regenera main/certs.h mesmo se já existir"
            exit 0
            ;;
        *)
            echo "Argumento desconhecido: $arg" >&2
            echo "Use --help para ver as opções." >&2
            exit 1
            ;;
    esac
done

# -----------------------------------------------------------------------------
# Idempotência: pula se certs.h já existe (a menos que --force)
# -----------------------------------------------------------------------------

if [ -f "$OUTPUT_FILE" ] && [ "$FORCE" = false ]; then
    echo "[gen_cert] $OUTPUT_FILE já existe — pulando (use --force para regenerar)"
    exit 0
fi

# -----------------------------------------------------------------------------
# Verificações de ambiente
# -----------------------------------------------------------------------------

if ! command -v openssl >/dev/null 2>&1; then
    echo "[gen_cert] ERRO: openssl não encontrado no PATH." >&2
    echo "[gen_cert] Instale com:" >&2
    echo "[gen_cert]   Debian/Ubuntu:  sudo apt install openssl" >&2
    echo "[gen_cert]   Fedora:         sudo dnf install openssl" >&2
    echo "[gen_cert]   macOS:          brew install openssl" >&2
    echo "[gen_cert]   Windows:        winget install ShiningLight.OpenSSL" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT_FILE")"

# -----------------------------------------------------------------------------
# Gera certificado e chave em diretório temporário
# -----------------------------------------------------------------------------

TMPDIR="$(mktemp -d)"
# Garante limpeza mesmo em caso de erro (set -e aborta no primeiro erro).
trap 'rm -rf "$TMPDIR"' EXIT

CERT_PEM="$TMPDIR/cert.pem"
KEY_PEM="$TMPDIR/key.pem"

echo "[gen_cert] Gerando certificado auto-assinado (RSA-$RSA_BITS, $VALIDITY_DAYS dias)"
echo "[gen_cert] CN=$CN_NAME"
echo "[gen_cert] SAN=$SAN_LIST"

# -x509           emite certificado auto-assinado (não é CSR)
# -newkey rsa:N   gera chave RSA de N bits
# -nodes          NÃO criptografa a chave com passphrase (o firmware não
#                 tem como digitar passphrase no boot)
# -keyout         arquivo da chave privada (PEM)
# -out            arquivo do certificado (PEM)
# -days           validade em dias
# -subj           subject (CN etc.) sem prompt interativo
# -addext         SAN (necessário em clientes modernos, mesmo com -k)
MSYS2_ARG_CONV_EXCL='/C=' openssl req -x509 \
    -newkey "rsa:$RSA_BITS" \
    -nodes \
    -keyout "$KEY_PEM" \
    -out "$CERT_PEM" \
    -days "$VALIDITY_DAYS" \
    -subj "/C=BR/O=ESP32-Camera-MQTT/OU=OTA/CN=$CN_NAME" \
    -addext "subjectAltName=$SAN_LIST" \
    2>/dev/null

# -----------------------------------------------------------------------------
# Sanidade: verifica que o PEM é válido antes de embutir
# -----------------------------------------------------------------------------

if ! openssl x509 -in "$CERT_PEM" -noout -subject >/dev/null 2>&1; then
    echo "[gen_cert] ERRO: certificado gerado é inválido (openssl x509 falhou)" >&2
    exit 1
fi

if ! openssl rsa -in "$KEY_PEM" -check -noout >/dev/null 2>&1; then
    echo "[gen_cert] ERRO: chave privada gerada é inválida" >&2
    exit 1
fi

echo "[gen_cert] Verificação OK"

# -----------------------------------------------------------------------------
# Converte PEM em string literal C (com \n entre linhas)
# -----------------------------------------------------------------------------
#
# Estratégia: lê linha a linha e emite:
#     "conteúdo-da-linha\n"
# Isso evita problemas com xxd -i (que gera arrays de bytes hexadecimais
# enormes e ilegíveis) e mantém o header inspecionável.

emit_pem_literal() {
    local file="$1"
    # Escapa \, " e embrulha em "" com \n no fim.
    # PEM padrão não tem \ nem " (só base64 e traços), mas o escape
    # garante robustez caso o formato mude.
    sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/    "/' -e 's/$/\\n"/' "$file"
}

echo "[gen_cert] Escrevendo $OUTPUT_FILE"

{
    cat <<'HEADER_EOF'
/* =============================================================================
 * main/certs.h — GERADO AUTOMATICAMENTE por tools/gen_cert.sh
 * =============================================================================
 *
 * NÃO EDITE ESTE ARQUIVO À MÃO. Ele é sobrescrito quando você roda
 *   ./tools/gen_cert.sh --force
 *
 * Para alterar CN, SAN, validade ou tamanho da chave, edite as variáveis
 * no topo de tools/gen_cert.sh e regenere com --force.
 *
 * As strings abaixo são embutidas no firmware e servidas pelo servidor
 * HTTPS local (main/ota_server.c).
 *
 * Declaradas como "static" para evitar múltiplas definições se este
 * header for incluído em mais de uma translation unit. Como só o
 * ota_server.c inclui, o custo é uma cópia por arquivo que incluir.
 *
 * NÃO VERSIONE ESTE ARQUIVO. Adicione ao .gitignore.
 * ========================================================================== */

#ifndef CERTS_H
#define CERTS_H

HEADER_EOF

    # --- Certificado ---
    echo "static const char SERVER_CERT_PEM[] ="
    emit_pem_literal "$CERT_PEM"
    echo "    ;"
    echo ""
    echo "static const unsigned int SERVER_CERT_PEM_LEN ="
    echo "    sizeof(SERVER_CERT_PEM) - 1;   /* sem o '\\0' final */"
    echo ""

    # --- Chave privada ---
    echo "static const char SERVER_KEY_PEM[] ="
    emit_pem_literal "$KEY_PEM"
    echo "    ;"
    echo ""
    echo "static const unsigned int SERVER_KEY_PEM_LEN ="
    echo "    sizeof(SERVER_KEY_PEM) - 1;    /* sem o '\\0' final */"
    echo ""

    cat <<'FOOTER_EOF'
#endif /* CERTS_H */
FOOTER_EOF
} > "$OUTPUT_FILE"

# -----------------------------------------------------------------------------
# Relatório final
# -----------------------------------------------------------------------------

CERT_BYTES=$(wc -c < "$CERT_PEM")
KEY_BYTES=$(wc -c < "$KEY_PEM")
HEADER_BYTES=$(wc -c < "$OUTPUT_FILE")

echo "[gen_cert] OK"
echo "[gen_cert]   certificado: $CERT_BYTES bytes"
echo "[gen_cert]   chave:       $KEY_BYTES bytes"
echo "[gen_cert]   header C:    $HEADER_BYTES bytes ($OUTPUT_FILE)"
echo "[gen_cert] Fingerprint SHA-256 do certificado (para pinning manual):"
openssl x509 -in "$CERT_PEM" -noout -fingerprint -sha256 2>/dev/null \
    | sed 's/^SHA256 Fingerprint=/[gen_cert]   /'