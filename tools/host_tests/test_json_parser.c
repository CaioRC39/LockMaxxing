/* =============================================================================
 * tools/host_tests/test_json_parser.c — Testes do parser de resultado MQTT
 * =============================================================================
 *
 * Este arquivo replica a lógica de handle_result_message() de main/mqtt.c
 * usando cJSON, para rodar no PC. Se você alterar o parser no firmware,
 * altere aqui também.
 *
 * Dependência: cJSON (mesma versão do espressif/cjson: 1.7.x).
 * O run_all.sh/ps1 baixa automaticamente para external/cJSON/ na primeira
 * execução.
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>

#include "cJSON.h"

/* ------------------------- Lógica equivalente a mqtt.c --------------------- */

static bool json_id_from_string(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') return false;
    char *endp = NULL;
    unsigned long v = strtoul(s, &endp, 10);
    if (endp == s || *endp != '\0' || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

/* Retorna true se o parse foi válido. Preenche id/approved. */
static bool parse_result(const char *data, int len,
                         uint32_t *id_out, bool *approved_out)
{
    if (data == NULL || len <= 0) return false;

    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) return false;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    /* ----- Campo "id" ----- */
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    uint32_t image_id = 0;
    bool id_ok = false;

    if (id_item == NULL) {
        /* id_ok permanece false */
    } else if (cJSON_IsNumber(id_item)) {
        double v = id_item->valuedouble;
        if (v >= 0.0 && v <= (double)UINT32_MAX) {
            image_id = (uint32_t)v;
            id_ok = true;
        }
    } else if (cJSON_IsString(id_item)) {
        id_ok = json_id_from_string(id_item->valuestring, &image_id);
    }

    if (!id_ok) {
        cJSON_Delete(root);
        return false;
    }

    /* ----- Campo "resultado" ----- */
    cJSON *res_item = cJSON_GetObjectItemCaseSensitive(root, "resultado");
    if (res_item == NULL || !cJSON_IsString(res_item) ||
        res_item->valuestring == NULL) {
        cJSON_Delete(root);
        return false;
    }

    bool approved;
    if (strcasecmp(res_item->valuestring, "aprovado") == 0) {
        approved = true;
    } else if (strcasecmp(res_item->valuestring, "reprovado") == 0) {
        approved = false;
    } else {
        cJSON_Delete(root);
        return false;
    }

    if (id_out)       *id_out       = image_id;
    if (approved_out) *approved_out = approved;

    cJSON_Delete(root);
    return true;
}

/* ------------------------- Framework de teste ------------------------------ */

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond, msg) do {                                             \
    if (cond) { printf("  [PASS] %s\n", msg); g_pass++; }                       \
    else      { printf("  [FAIL] %s\n", msg); g_fail++; }                       \
} while (0)

#define ASSERT_FALSE(cond, msg) ASSERT_TRUE(!(cond), msg)

#define CHECK_PARSE(str, exp_ok, exp_id, exp_approved, desc) do {               \
    uint32_t _id = 0xDEAD;                                                      \
    bool _appr = false;                                                         \
    bool _ok = parse_result((str), (int)strlen(str), &_id, &_appr);             \
    if (!(exp_ok)) {                                                            \
        ASSERT_FALSE(_ok, desc " → rejeitado");                                 \
    } else {                                                                    \
        ASSERT_TRUE(_ok && _id == (exp_id) && _appr == (exp_approved),          \
                    desc " → aceito, id e resultado corretos");                 \
    }                                                                           \
} while (0)

/* ------------------------- Casos de teste --------------------------------- */

static void test_valid(void)
{
    printf("[CASE] JSON válido\n");
    CHECK_PARSE("{\"id\":123,\"resultado\":\"aprovado\"}",
                true, 123, true, "aprovado padrão");
    CHECK_PARSE("{\"id\":456,\"resultado\":\"reprovado\"}",
                true, 456, false, "reprovado padrão");
    CHECK_PARSE("{ \"id\" : 789 , \"resultado\" : \"APROVADO\" }",
                true, 789, true, "espaços e uppercase");
    CHECK_PARSE("{\"resultado\":\"reprovado\",\"id\":42}",
                true, 42, false, "ordem trocada");
    CHECK_PARSE("{\"id\":\"1234\",\"resultado\":\"aprovado\"}",
                true, 1234, true, "id como string");
    CHECK_PARSE("{\"id\":1,\"resultado\":\"aprovado\",\"extra\":99}",
                true, 1, true, "campo extra ignorado");
    CHECK_PARSE("{\"outro\":true,\"id\":7,\"resultado\":\"reprovado\"}",
                true, 7, false, "campo extra antes");
}

static void test_invalid(void)
{
    printf("[CASE] JSON inválido\n");
    CHECK_PARSE("", false, 0, false, "string vazia");
    CHECK_PARSE("not json at all", false, 0, false, "texto livre");
    CHECK_PARSE("{\"id\":123}", false, 0, false, "sem campo resultado");
    CHECK_PARSE("{\"resultado\":\"aprovado\"}", false, 0, false, "sem campo id");
    CHECK_PARSE("{\"id\":123,\"resultado\":\"talvez\"}",
                false, 0, false, "resultado desconhecido");
    CHECK_PARSE("{\"id\":-1,\"resultado\":\"aprovado\"}",
                false, 0, false, "id negativo");
    CHECK_PARSE("{\"id\":12345678901,\"resultado\":\"aprovado\"}",
                false, 0, false, "id acima de uint32");
    CHECK_PARSE("{\"id\":\"abc\",\"resultado\":\"aprovado\"}",
                false, 0, false, "id string não numérica");
    CHECK_PARSE("{\"id\":123,\"resultado\":123}",
                false, 0, false, "resultado numérico (não string)");
    CHECK_PARSE("{\"id\":123,\"resultado\":\"aprovado\"",
                false, 0, false, "faltando }");
}

static void test_edge_cases(void)
{
    printf("[CASE] casos limite\n");
    CHECK_PARSE("{\"id\":0,\"resultado\":\"aprovado\"}",
                true, 0, true, "id=0 aceito");
    CHECK_PARSE("{\"id\":4294967295,\"resultado\":\"reprovado\"}",
                true, 4294967295u, false, "id=UINT32_MAX");
}

int main(void)
{
    printf("=== Testes de json_parser (cJSON) ===\n");
    test_valid();
    test_invalid();
    test_edge_cases();
    printf("\n=== Resultado: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}