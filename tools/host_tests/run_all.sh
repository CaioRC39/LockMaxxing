#!/usr/bin/env bash
# =============================================================================
# tools/host_tests/run_all.sh — Compila e roda todos os testes de host
# =============================================================================
# Uso:  ./run_all.sh
# =============================================================================

set -euo pipefail

cd "$(dirname "$0")"

CC="${CC:-gcc}"
CFLAGS=(-Wall -Wextra -std=c11 -O2 -I../../main -I mocks -I external/cJSON)

CJSON_TAG="v1.7.18"
CJSON_DIR="external/cJSON"

# Baixa o cJSON na primeira execução (idempotente).
if [ ! -f "${CJSON_DIR}/cJSON.c" ]; then
    echo "[setup] Baixando cJSON ${CJSON_TAG}..."
    mkdir -p external
    if command -v git >/dev/null 2>&1; then
        git clone --depth 1 --branch "${CJSON_TAG}" \
            https://github.com/DaveGamble/cJSON.git "${CJSON_DIR}"
    else
        mkdir -p "${CJSON_DIR}"
        curl -fsSL "https://raw.githubusercontent.com/DaveGamble/cJSON/${CJSON_TAG}/cJSON.c" -o "${CJSON_DIR}/cJSON.c"
        curl -fsSL "https://raw.githubusercontent.com/DaveGamble/cJSON/${CJSON_TAG}/cJSON.h" -o "${CJSON_DIR}/cJSON.h"
    fi
    echo "[setup] cJSON pronto em ${CJSON_DIR}/"
fi

mkdir -p build

pass=0
fail=0
failed_tests=()

for test_src in test_*.c; do
    test_name="${test_src%.c}"
    exe="build/${test_name}"

    echo ""
    echo "============================================================"
    echo " Compilando: ${test_src}"
    echo "============================================================"

    if ! "$CC" "${CFLAGS[@]}" "$test_src" \
            mocks/mock_esp_err.c "${CJSON_DIR}/cJSON.c" -o "$exe"; then
        echo "  [ERRO] Compilação falhou: $test_src"
        fail=$((fail+1))
        failed_tests+=("$test_name (compilação)")
        continue
    fi

    if "$exe"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        failed_tests+=("$test_name")
    fi
done

echo ""
echo "============================================================"
echo " RESUMO: $pass passed, $fail failed"
echo "============================================================"

if [ "$fail" -gt 0 ]; then
    for t in "${failed_tests[@]}"; do
        echo "  [FAIL] $t"
    done
    exit 1
fi

exit 0