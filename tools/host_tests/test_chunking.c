/* =============================================================================
 * tools/host_tests/test_chunking.c — Testes da matemática de chunking
 * =============================================================================
 * A fórmula (len + CHUNK - 1) / CHUNK é usada em tasks.c e o servidor
 * Python replica a mesma conta. Testar isoladamente garante que não há
 * off-by-one nos limites.
 * ========================================================================== */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#define CHUNK_SIZE 2048

static int g_pass = 0;
static int g_fail = 0;

static uint16_t calc_total_chunks(size_t len)
{
    return (uint16_t)((len + CHUNK_SIZE - 1) / CHUNK_SIZE);
}

#define ASSERT_EQ(actual, expected, msg) do {                                   \
    long _a = (long)(actual);                                                   \
    long _e = (long)(expected);                                                 \
    if (_a == _e) { printf("  [PASS] %s\n", msg); g_pass++; }                   \
    else { printf("  [FAIL] %s (got %ld, expected %ld)\n", msg, _a, _e);        \
           g_fail++; }                                                          \
} while (0)

int main(void)
{
    printf("=== Testes de chunking ===\n");

    printf("[CASE] fronteiras exatas\n");
    ASSERT_EQ(calc_total_chunks(1),                    1, "1 byte → 1 chunk");
    ASSERT_EQ(calc_total_chunks(CHUNK_SIZE - 1),       1, "2047 → 1 chunk");
    ASSERT_EQ(calc_total_chunks(CHUNK_SIZE),           1, "2048 → 1 chunk");
    ASSERT_EQ(calc_total_chunks(CHUNK_SIZE + 1),       2, "2049 → 2 chunks");
    ASSERT_EQ(calc_total_chunks(2 * CHUNK_SIZE - 1),   2, "4095 → 2 chunks");
    ASSERT_EQ(calc_total_chunks(2 * CHUNK_SIZE),       2, "4096 → 2 chunks");
    ASSERT_EQ(calc_total_chunks(2 * CHUNK_SIZE + 1),   3, "4097 → 3 chunks");

    printf("[CASE] tamanhos típicos de imagem\n");
    ASSERT_EQ(calc_total_chunks(10240),  5, "10 KB → 5 chunks");
    ASSERT_EQ(calc_total_chunks(38914), 20, "38 KB típico → 20 chunks");
    ASSERT_EQ(calc_total_chunks(51200), 25, "50 KB → 25 chunks");

    printf("[CASE] limite máximo do protocolo\n");
    /* 65535 chunks * 2048 = 134,215,680 bytes. Acima disso, uint16 overflow.
     * O firmware limita em 200 KB, então nunca chega perto. */
    ASSERT_EQ(calc_total_chunks(100 * 1024), 50, "100 KB → 50 chunks");
    ASSERT_EQ(calc_total_chunks(200 * 1024), 100, "200 KB → 100 chunks");

    printf("[CASE] zero bytes (edge)\n");
    /* A fórmula dá 0. Em prática, nunca chamamos com len=0 (câmera falha antes). */
    ASSERT_EQ(calc_total_chunks(0), 0, "0 bytes → 0 chunks (sem divisão por zero)");

    printf("\n=== Resultado: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}