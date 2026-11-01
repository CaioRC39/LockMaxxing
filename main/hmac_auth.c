/* =============================================================================
 * main/hmac_auth.c — Assinatura HMAC-SHA256 via PSA Crypto
 * ========================================================================== */

#include <string.h>
#include "esp_log.h"
#include "psa/crypto.h"

#include "sdkconfig.h"
#include "app_config.h"
#include "hmac_auth.h"

static const char *TAG = "HMAC";

/* Segredo em bytes, carregado uma vez em hmac_auth_init().
 * Tamanho fixo: 32 bytes (HMAC-SHA256 key size). */
static uint8_t s_secret[32];
static size_t  s_secret_len = 0;
static bool    s_enabled    = false;

/* -----------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Converte 64 chars hex em 32 bytes. Retorna ESP_OK ou erro. */
static esp_err_t hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return ESP_ERR_INVALID_ARG;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char k[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = k[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = k[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* -----------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */

esp_err_t hmac_auth_init(void)
{
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init falhou: %d", (int)st);
        return ESP_ERR_INVALID_STATE;
    }

    app_config_t cfg;
    esp_err_t err = app_config_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app_config_load falhou: %s", esp_err_to_name(err));
        return err;
    }

    if (cfg.hmac_secret[0] == '\0') {
        ESP_LOGW(TAG, "HMAC desabilitado (segredo vazio)");
        s_enabled = false;
        return ESP_OK;
    }

    err = hex_to_bytes(cfg.hmac_secret, s_secret, sizeof(s_secret));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Segredo HMAC inválido: %s "
                      "(esperado 64 chars hex, obtido len=%zu)",
                 esp_err_to_name(err), strlen(cfg.hmac_secret));
        s_enabled = false;
        return err;
    }

    s_secret_len = sizeof(s_secret);
    s_enabled    = true;
    ESP_LOGI(TAG, "HMAC-SHA256 ativo (chave de %zu bytes)", s_secret_len);
    return ESP_OK;
}

bool hmac_auth_is_enabled(void)
{
    return s_enabled;
}

/* -----------------------------------------------------------------------------
 * Sign
 * -------------------------------------------------------------------------- */

esp_err_t hmac_sign_image(uint32_t image_id,
                          uint32_t total_size,
                          uint16_t total_chunks,
                          const uint8_t *image_data,
                          size_t image_len,
                          char out_hex[65])
{
    if (!s_enabled) return ESP_ERR_INVALID_STATE;
    if (image_data == NULL || out_hex == NULL) return ESP_ERR_INVALID_ARG;

    /* Monta o cabeçalho: id(4) || size(4) || chunks(2) — big-endian */
    uint8_t hdr[10];
    hdr[0] = (uint8_t)(image_id >> 24);
    hdr[1] = (uint8_t)(image_id >> 16);
    hdr[2] = (uint8_t)(image_id >>  8);
    hdr[3] = (uint8_t)(image_id      );
    hdr[4] = (uint8_t)(total_size >> 24);
    hdr[5] = (uint8_t)(total_size >> 16);
    hdr[6] = (uint8_t)(total_size >>  8);
    hdr[7] = (uint8_t)(total_size      );
    hdr[8] = (uint8_t)(total_chunks >> 8);
    hdr[9] = (uint8_t)(total_chunks     );

    /* Importa a chave no PSA. */
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    psa_key_id_t key_id = 0;
    psa_status_t st = psa_import_key(&attr, s_secret, s_secret_len, &key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_import_key falhou: %d", (int)st);
        return ESP_FAIL;
    }

    /* Streaming HMAC: metadata + image sem concatenar em buffer grande. */
    psa_mac_operation_t op = PSA_MAC_OPERATION_INIT;
    st = psa_mac_sign_setup(&op, key_id, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) goto fail_abort;

    st = psa_mac_update(&op, hdr, sizeof(hdr));
    if (st != PSA_SUCCESS) goto fail_abort;

    st = psa_mac_update(&op, image_data, image_len);
    if (st != PSA_SUCCESS) goto fail_abort;

    uint8_t mac[32];
    size_t  mac_len = 0;
    st = psa_mac_sign_finish(&op, mac, sizeof(mac), &mac_len);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_mac_sign_finish falhou: %d", (int)st);
        psa_destroy_key(key_id);
        return ESP_FAIL;
    }

    bytes_to_hex(mac, mac_len, out_hex);
    psa_destroy_key(key_id);

    ESP_LOGD(TAG, "HMAC id=%u: %s", (unsigned)image_id, out_hex);
    return ESP_OK;

fail_abort:
    ESP_LOGE(TAG, "psa_mac_update falhou: %d", (int)st);
    psa_mac_abort(&op);
    psa_destroy_key(key_id);
    return ESP_FAIL;
}