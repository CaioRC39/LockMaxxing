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

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Primeiro boot: NVS vazia. Preenche defaults. */
        ESP_LOGI(TAG, "NVS vazia — primeiro boot");
        strlcpy(out->mqtt_uri, CONFIG_LOCKMAXXING_FACTORY_BROKER_URI,
                sizeof(out->mqtt_uri));
        out->provisioned = false;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open falhou: %s", esp_err_to_name(err));
        return err;
    }

    size_t len;

    len = sizeof(out->wifi_ssid);
    err = nvs_get_str(handle, KEY_SSID, out->wifi_ssid, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Erro ao ler SSID: %s", esp_err_to_name(err));
    }

    len = sizeof(out->wifi_password);
    err = nvs_get_str(handle, KEY_PASS, out->wifi_password, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Erro ao ler senha: %s", esp_err_to_name(err));
    }

    len = sizeof(out->mqtt_uri);
    err = nvs_get_str(handle, KEY_URI, out->mqtt_uri, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Erro ao ler URI: %s", esp_err_to_name(err));
    }
    /* Se a URI não estava salva (NVS de versão anterior), usa de fábrica */
    if (out->mqtt_uri[0] == '\0') {
        strlcpy(out->mqtt_uri, CONFIG_LOCKMAXXING_FACTORY_BROKER_URI,
                sizeof(out->mqtt_uri));
    }

    uint8_t prov_flag = 0;
    len = sizeof(prov_flag);
    nvs_get_u8(handle, KEY_PROVISIONED, &prov_flag);
    out->provisioned = (prov_flag != 0) && (out->wifi_ssid[0] != '\0');

    nvs_close(handle);

    ESP_LOGI(TAG, "Config carregada: ssid='%s', uri='%s', provisioned=%d",
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