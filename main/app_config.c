/* =============================================================================
 * main/app_config.c — Implementação da persistência em NVS
 * ========================================================================== */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "app_config.h"

static const char *TAG = "APP_CFG";

/* Namespace NVS — diferente de "nvs.net80211" (usado pelo driver Wi-Fi) */
#define NVS_NAMESPACE   "app_cfg"

/* Chaves dentro do namespace */
#define KEY_SSID        "ssid"
#define KEY_PASS        "pass"
#define KEY_URI         "uri"
#define KEY_PROVISIONED "prov"

esp_err_t app_config_load(app_config_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

    bool have_nvs = (err == ESP_OK);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "nvs_open falhou: %s", esp_err_to_name(err));
        return err;
    }

    if (have_nvs) {
        size_t len;

        len = sizeof(out->wifi_ssid);
        nvs_get_str(handle, KEY_SSID, out->wifi_ssid, &len);

        len = sizeof(out->wifi_password);
        nvs_get_str(handle, KEY_PASS, out->wifi_password, &len);

        len = sizeof(out->mqtt_uri);
        nvs_get_str(handle, KEY_URI, out->mqtt_uri, &len);

        len = sizeof(out->hmac_secret);
        err = nvs_get_str(handle, "hmac", out->hmac_secret, &len);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "Erro ao ler HMAC: %s", esp_err_to_name(err));
        }
        /* Fallback: se NVS não tem segredo, usa o de fábrica do Kconfig */
        if (out->hmac_secret[0] == '\0') {
            strlcpy(out->hmac_secret, CONFIG_LOCKMAXXING_FACTORY_HMAC_SECRET,
                    sizeof(out->hmac_secret));
        }

        uint8_t prov_flag = 0;
        len = sizeof(prov_flag);
        nvs_get_u8(handle, KEY_PROVISIONED, &prov_flag);

        out->provisioned = (prov_flag != 0) && (out->wifi_ssid[0] != '\0');
        nvs_close(handle);

        ESP_LOGI(TAG, "NVS lida: ssid='%s' (len=%zu), prov_flag=%d",
                 out->wifi_ssid, strlen(out->wifi_ssid), prov_flag);
    } else {
        ESP_LOGI(TAG, "NVS vazia (primeiro boot)");
    }

    /* ── Fallback Kconfig (DEV) ──
     * Aplicado sempre que a NVS não proveu credenciais válidas,
     * independente do motivo (vazia OU chaves vazias). */
    if (!out->provisioned) {
        if (CONFIG_LOCKMAXXING_WIFI_SSID[0] != '\0' &&
            strcmp(CONFIG_LOCKMAXXING_WIFI_SSID, "myssid") != 0) {
            strlcpy(out->wifi_ssid, CONFIG_LOCKMAXXING_WIFI_SSID,
                    sizeof(out->wifi_ssid));
            strlcpy(out->wifi_password, CONFIG_LOCKMAXXING_WIFI_PASSWORD,
                    sizeof(out->wifi_password));
            out->provisioned = true;
            ESP_LOGW(TAG, "FALLBACK Kconfig: SSID='%s' (NVS não tinha credenciais válidas)",
                     out->wifi_ssid);
        } else {
            ESP_LOGW(TAG, "Kconfig também está com default — portal vai abrir");
        }
    }

    if (out->mqtt_uri[0] == '\0') {
        strlcpy(out->mqtt_uri, CONFIG_LOCKMAXXING_FACTORY_BROKER_URI,
                sizeof(out->mqtt_uri));
    }

    ESP_LOGI(TAG, "Config final: ssid='%s', uri='%s', provisioned=%d",
             out->wifi_ssid, out->mqtt_uri, out->provisioned);
    return ESP_OK;
}

esp_err_t app_config_save(const app_config_t *cfg)
{
    if (cfg == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open (RW) falhou: %s", esp_err_to_name(err));
        return err;
    }

    if ((err = nvs_set_str(handle, KEY_SSID, cfg->wifi_ssid)) != ESP_OK) goto fail;
    if ((err = nvs_set_str(handle, KEY_PASS, cfg->wifi_password)) != ESP_OK) goto fail;
    if ((err = nvs_set_str(handle, KEY_URI,  cfg->mqtt_uri)) != ESP_OK) goto fail;
    if ((err = nvs_set_str(handle, "hmac", cfg->hmac_secret)) != ESP_OK) goto fail;
    if ((err = nvs_set_u8 (handle, KEY_PROVISIONED,
                           cfg->provisioned ? 1 : 0)) != ESP_OK) goto fail;
    if ((err = nvs_commit(handle)) != ESP_OK) goto fail;

    ESP_LOGI(TAG, "Config salva na NVS");
    nvs_close(handle);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "Falha ao salvar na NVS: %s", esp_err_to_name(err));
    nvs_close(handle);
    return err;
}

esp_err_t app_config_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  /* já está vazia */
    }
    if (err != ESP_OK) return err;

    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "NVS de configuração apagada");
    return ESP_OK;
}

bool app_config_is_provisioned(void)
{
    app_config_t cfg;
    if (app_config_load(&cfg) != ESP_OK) {
        return false;
    }
    return cfg.provisioned;
}

const char *app_config_factory_broker(void)
{
    return CONFIG_LOCKMAXXING_FACTORY_BROKER_URI;
}