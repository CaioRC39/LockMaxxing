/* =============================================================================
 * main/ota_common.c — Núcleo compartilhado do subsistema OTA
 * =============================================================================
 *
 * Este arquivo implementa a "base" que o cliente HTTPS (pull) e o servidor
 * HTTPS local (push) compartilham:
 *
 *   - Mutex global e estado de OTA
 *   - Ciclo de vida (init)
 *   - Confirmação de boot e rollback
 *   - Consulta de partições
 *   - Helpers de SHA-256
 *   - Cálculo de progresso
 * ========================================================================== */

#include "ota_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

/* Tag de log centralizada para todo o subsistema OTA. */
static const char *TAG = "OTA_COMMON";

/* -----------------------------------------------------------------------------
 * Variáveis globais do subsistema
 * -------------------------------------------------------------------------- */

/*
 * Mutex criado uma única vez em ota_init(). Nunca destruído — o firmware
 * vive até reboot, e destruir o mutex introduziria um estado inválido
 * se outro módulo ainda o estivesse usando.
 */
SemaphoreHandle_t g_ota_mutex = NULL;

/*
 * Estado atual do subsistema. Acessado SOMENTE via ota_set_state() e
 * ota_get_state(), que pegam g_ota_mutex. Nunca acesse diretamente de
 * fora deste arquivo.
 */
static ota_state_t g_ota_state = OTA_STATE_IDLE;

volatile bool g_ota_busy = false;

/* -----------------------------------------------------------------------------
 * Estado (get/set com mutex)
 * -------------------------------------------------------------------------- */

void ota_set_state(ota_state_t state)
{
    if (g_ota_mutex != NULL) {
        if (xSemaphoreTake(g_ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /*
             * Não bloqueia indefinidamente: se o mutex estiver travado por
             * mais de 1s, algo está muito errado. Loga e segue — melhor um
             * estado divergente do que uma task travada para sempre.
             */
            ESP_LOGW(TAG, "Timeout ao adquirir mutex (set_state=%d)", (int)state);
            return;
        }
    } else {
        ESP_LOGW(TAG, "Mutex não inicializado — chamar ota_init() primeiro");
    }

    g_ota_state = state;

    if (g_ota_mutex != NULL) {
        xSemaphoreGive(g_ota_mutex);
    }
}

ota_state_t ota_get_state(void)
{
    ota_state_t snapshot;

    if (g_ota_mutex != NULL) {
        if (xSemaphoreTake(g_ota_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            /* Devolve o valor atual como "best effort". */
            return g_ota_state;
        }
    }

    snapshot = g_ota_state;

    if (g_ota_mutex != NULL) {
        xSemaphoreGive(g_ota_mutex);
    }

    return snapshot;
}

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

esp_err_t ota_init(void)
{
    /* Cria o mutex apenas uma vez. Chamadas subsequentes são idempotentes. */
    if (g_ota_mutex == NULL) {
        g_ota_mutex = xSemaphoreCreateMutex();
        if (g_ota_mutex == NULL) {
            ESP_LOGE(TAG, "Falha ao criar mutex de OTA");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Mutex de OTA criado");
    }

    ota_set_state(OTA_STATE_IDLE);

    /* Log de partições — útil para confirmar que a tabela está correta. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        ESP_LOGI(TAG, "Rodando da partição '%s' (offset=0x%08lx, size=0x%lx, "
                      "subtype=0x%02x)",
                 running->label,
                 (unsigned long)running->address,
                 (unsigned long)running->size,
                 (unsigned int)running->subtype);
    } else {
        ESP_LOGE(TAG, "esp_ota_get_running_partition() retornou NULL");
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next != NULL) {
        ESP_LOGI(TAG, "Próximo alvo de OTA: '%s' (offset=0x%08lx)",
                 next->label, (unsigned long)next->address);
    } else {
        ESP_LOGW(TAG, "Sem partição OTA disponível para atualização");
    }

    /* Estado de rollback pendente, se houver. */
    if (ota_is_pending_verify()) {
        ESP_LOGW(TAG, "Firmware atual em PENDING_VERIFY — aguardando confirmação");
        ESP_LOGW(TAG, "Chame ota_mark_app_valid() após o self-test passar");
    } else {
        ESP_LOGI(TAG, "Firmware atual confirmado (sem rollback pendente)");
    }

    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Rollback / confirmação
 * -------------------------------------------------------------------------- */

bool ota_is_pending_verify(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return false;
    }

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        /*
         * ESP_ERR_NOT_SUPPORTED significa "não é partição OTA" (ex: factory).
         * Qualquer outro erro também não indica pendência.
         */
        return false;
    }

    return (state == ESP_OTA_IMG_PENDING_VERIFY);
}

esp_err_t ota_mark_app_valid(void)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Firmware confirmado como válido — rollback cancelado");
    } else {
        ESP_LOGE(TAG, "Falha ao confirmar firmware: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t ota_mark_app_invalid_and_reboot(void)
{
    ESP_LOGW(TAG, "Marcando firmware atual como INVÁLIDO e reiniciando...");
    ESP_LOGW(TAG, "O bootloader reverterá para a partição anterior");

    /*
     * Esta função do IDF, em caso de sucesso, chama esp_restart() e NÃO
     * retorna. O código abaixo só é alcançado se houver falha.
     */
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();

    ESP_LOGE(TAG, "Falha ao disparar rollback: %s", esp_err_to_name(err));
    return err;
}

/* -----------------------------------------------------------------------------
 * Consulta de partições
 * -------------------------------------------------------------------------- */

esp_err_t ota_get_running_partition_info(const esp_partition_t **part_out)
{
    if (part_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *part_out = p;
    return ESP_OK;
}

esp_err_t ota_get_next_update_partition_info(const esp_partition_t **part_out)
{
    if (part_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    if (p == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    *part_out = p;
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Progresso
 * -------------------------------------------------------------------------- */

void ota_fill_progress(ota_progress_t *p, size_t received, size_t total)
{
    if (p == NULL) {
        return;
    }

    p->bytes_received = received;
    p->bytes_total    = total;

    if (total > 0 && received <= total) {
        /* uint64_t evita overflow em imagens > 42 MB (improvável, mas seguro). */
        p->percent = (uint8_t)(((uint64_t)received * 100ULL) / (uint64_t)total);
    } else {
        p->percent = 0;
    }
}

/* -----------------------------------------------------------------------------
 * SHA-256 — helpers
 * -------------------------------------------------------------------------- */

/* Tabela de hex lowercase, evitando sprintf(). */
static const char k_hexchars[] = "0123456789abcdef";

esp_err_t ota_sha256_hex(const uint8_t *data, size_t len,
                         char out_hex[OTA_SHA256_HEX_LEN + 1])
{
    if (out_hex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (data == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[32];
    size_t digest_len = 0;

    psa_status_t ret = psa_hash_compute(
        PSA_ALG_SHA_256,
        data,
        len,
        digest,
        sizeof(digest),
        &digest_len
    );

    if (ret != PSA_SUCCESS || digest_len != sizeof(digest)) {
        ESP_LOGE(TAG, "psa_hash_compute() falhou: %d", (int)ret);
        return ESP_FAIL;
    }

    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = k_hexchars[(digest[i] >> 4) & 0x0F];
        out_hex[i * 2 + 1] = k_hexchars[digest[i] & 0x0F];
    }
    out_hex[OTA_SHA256_HEX_LEN] = '\0';

    return ESP_OK;
}

esp_err_t ota_sha256_stream_init(psa_hash_operation_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *ctx = psa_hash_operation_init();

    psa_status_t ret = psa_hash_setup(ctx, PSA_ALG_SHA_256);
    if (ret != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup() falhou: %d", (int)ret);
        (void)psa_hash_abort(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Compara duas strings hex de SHA-256 (case-insensitive).
 * Retorna true se ambas forem não-NULL, tiverem exatamente 64 chars
 * e forem iguais. Não usa mbedtls/PSA — só comparação de caracteres. */
bool ota_sha256_hex_equals(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    for (int i = 0; i < OTA_SHA256_HEX_LEN; i++) {
        char ca = a[i];
        char cb = b[i];

        if (ca == '\0' || cb == '\0') {
            return false;
        }

        /* Normaliza hex para lowercase. */
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');

        if (ca != cb) {
            return false;
        }
    }

    return (a[OTA_SHA256_HEX_LEN] == '\0' && b[OTA_SHA256_HEX_LEN] == '\0');
}

esp_err_t ota_sha256_stream_update(psa_hash_operation_t *ctx,
                                   const uint8_t *data,
                                   size_t len)
{
    if (ctx == NULL || (data == NULL && len != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    psa_status_t ret = psa_hash_update(ctx, data, len);
    if (ret != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_update() falhou: %d", (int)ret);
        (void)psa_hash_abort(ctx);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ota_sha256_stream_finish(psa_hash_operation_t *ctx,
                                   char hex_out[65])
{
    if (ctx == NULL || hex_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[32];
    size_t digest_len = 0;

    psa_status_t ret = psa_hash_finish(
        ctx,
        digest,
        sizeof(digest),
        &digest_len
    );

    if (ret != PSA_SUCCESS || digest_len != sizeof(digest)) {
        ESP_LOGE(TAG, "psa_hash_finish() falhou: %d", (int)ret);
        (void)psa_hash_abort(ctx);
        return ESP_FAIL;
    }

    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < sizeof(digest); i++) {
        hex_out[i * 2]     = hex[digest[i] >> 4];
        hex_out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }

    hex_out[64] = '\0';
    return ESP_OK;
}