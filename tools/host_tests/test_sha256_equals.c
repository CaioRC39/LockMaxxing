/* =============================================================================
 * tools/host_tests/test_sha256_equals.c — Testes de ota_sha256_hex_equals
 * =============================================================================
 *
 * ATENÇÃO: CÓPIA de main/ota_common.c. Se você alterar a função real,
 * atualize aqui também.
 *
 * A função é puramente comparação de caracteres, sem dependência de PSA
 * ou mbedtls — por isso é trivial copiá-la sem mocks.
 * ========================================================================== */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define OTA_SHA256_HEX_LEN 64

/* ---------------- Cópia de ota_sha256_hex_equals -------------------------- */
static bool ota_sha256_hex_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) return false;

    for (int i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        char ca = a[i];
        char cb = b[i];
        if (ca == '\0' || cb == '\0') return false;
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return (a[OTA_SHA256_HEX_LEN] == '\0' && b[OTA_SHA256_HEX_LEN] == '\0');
}

/* ------------------------- Framework ------------------------------------- */

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_EQ(actual, expected, msg) do {                                   \
    long _a = (long)(actual);                                                   \
    long _e = (long)(expected);                                                 \
    if (_a == _e) { printf("  [PASS] %s\n", msg); g_pass++; }                   \
    else { printf("  [FAIL] %s (got %ld, expected %ld)\n", msg, _a, _e);        \
           g_fail++; }                                                          \
} while (0)

int main(void)
{
    printf("=== Testes de ota_sha256_hex_equals ===\n");

    /* Hash de exemplo (32 bytes em hex = 64 chars) */
    const char *h_lower = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *h_upper = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
    const char *h_dif   = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdee";
    const char *h_short = "0123456789abcdef";
    const char *h_long  = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefZZZ";

    printf("[CASE] comparações válidas\n");
    ASSERT_EQ(ota_sha256_hex_equals(h_lower, h_lower), true,  "iguais lowercase");
    ASSERT_EQ(ota_sha256_hex_equals(h_upper, h_upper), true,  "iguais uppercase");
    ASSERT_EQ(ota_sha256_hex_equals(h_lower, h_upper), true,  "case-insensitive");

    printf("[CASE] diferenças\n");
    ASSERT_EQ(ota_sha256_hex_equals(h_lower, h_dif),   false, "1 char diferente");
    ASSERT_EQ(ota_sha256_hex_equals(h_short, h_lower), false, "string curta");
    ASSERT_EQ(ota_sha256_hex_equals(h_long,  h_lower), false, "lixo após 64 chars");
    ASSERT_EQ(ota_sha256_hex_equals(h_lower, h_short), false, "curta do outro lado");

    printf("[CASE] NULL\n");
    ASSERT_EQ(ota_sha256_hex_equals(NULL, h_lower), false, "primeiro NULL");
    ASSERT_EQ(ota_sha256_hex_equals(h_lower, NULL), false, "segundo NULL");
    ASSERT_EQ(ota_sha256_hex_equals(NULL, NULL),    false, "ambos NULL");

    printf("\n=== Resultado: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}