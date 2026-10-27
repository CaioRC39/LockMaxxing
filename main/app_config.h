/* =============================================================================
 * main/app_config.h — Persistência de configuração em NVS
 * =============================================================================
 *
 * Armazena as credenciais Wi-Fi e a URI do broker em NVS, para que possam
 * ser alteradas em runtime (via portal de provisionamento) sem recompilar.
 *
 * Estrutura:
 *   - wifi_ssid, wifi_password: configurados pelo USUÁRIO via portal
 *   - mqtt_uri: configurado de FÁBRICA (CONFIG_LOCKMAXXING_FACTORY_BROKER_URI)
 *   - provisioned: flag indicando se o primeiro boot já rodou o portal
 *
 * Fluxo:
 *   1º boot: NVS vazia → provisioned=false → main.c entra em modo portal
 *   Após portal: NVS gravada → provisioned=true → main.c conecta normal
 * ========================================================================== */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdbool.h>
#include "esp_err.h"

#define APP_CONFIG_SSID_MAX     33
#define APP_CONFIG_PASS_MAX     65
#define APP_CONFIG_URI_MAX      129

typedef struct {
    char wifi_ssid[APP_CONFIG_SSID_MAX];
    char wifi_password[APP_CONFIG_PASS_MAX];
    char mqtt_uri[APP_CONFIG_URI_MAX];
    bool provisioned;
} app_config_t;

/**
 * @brief Carrega a configuração da NVS.
 *
 * Se a NVS estiver vazia (primeiro boot):
 *   - preenche wifi_ssid/wifi_password com string vazia
 *   - preenche mqtt_uri com CONFIG_LOCKMAXXING_FACTORY_BROKER_URI
 *   - provisioned = false
 *   - NÃO salva ainda (só salva quando o portal for submetido)
 *
 * Se a NVS tiver dados:
 *   - carrega tudo
 *   - provisioned = true
 *
 * @param out  Struct de destino (não NULL).
 * @return ESP_OK em sucesso.
 */
esp_err_t app_config_load(app_config_t *out);

/**
 * @brief Salva a configuração na NVS.
 *
 * Chamada pelo portal de provisionamento após o usuário submeter o formulário.
 * Grava todos os campos, incluindo a URI do broker (que vem de fábrica).
 */
esp_err_t app_config_save(const app_config_t *cfg);

/**
 * @brief Apaga toda a configuração da NVS.
 *
 * Próximo boot entra em modo provisionamento. Usado em reset de fábrica.
 */
esp_err_t app_config_erase(void);

/**
 * @brief Retorna true se a placa já foi provisionada (Wi-Fi configurado).
 */
bool app_config_is_provisioned(void);

/**
 * @brief Retorna a URI de fábrica do broker (do Kconfig).
 *
 * Útil para log e para exibir em algum diagnóstico.
 */
const char *app_config_factory_broker(void);

#endif /* APP_CONFIG_H */