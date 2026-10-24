/* =============================================================================
 * main/wifi.c — Conexão Wi-Fi (station mode) com auto-reconexão
 * =============================================================================
 *
 * Responsabilidades:
 *   - Inicializar netif, event loop e driver Wi-Fi
 *   - Conectar com as credenciais de config.h
 *   - Reconectar com backoff exponencial (1s, 2s, 4s... até 30s)
 *   - Expor estado via event group para outras tasks bloquearem
 *   - Reportar IP e estatísticas
 *
 * PONTOS CRÍTICOS:
 *   - O event handler NÃO bloqueia. Backoff é agendado via esp_timer.
 *   - WIFI_REASON_ASSOC_LEAVE não reconecta (desconexão intencional).
 *   - WIFI_GOT_IP_BIT é o bit que indica "rede utilizável".
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "config.h"
#include "wifi.h"

#include "sdkconfig.h"

static const char *TAG = CONFIG_LOG_TAG_WIFI;

/* -----------------------------------------------------------------------------
 * Constantes internas
 * -------------------------------------------------------------------------- */

/* Backoff: 1s, 2s, 4s, 8s, 16s, 30s, 30s, ... */
#define WIFI_BACKOFF_BASE_MS        1000
#define WIFI_BACKOFF_MAX_MS         30000

/* -----------------------------------------------------------------------------
 * Estado interno
 * -------------------------------------------------------------------------- */

static bool                  s_initialized        = false;
static EventGroupHandle_t    s_event_group        = NULL;
static esp_netif_t          *s_netif              = NULL;
static esp_timer_handle_t    s_reconnect_timer    = NULL;

static esp_event_handler_instance_t s_wifi_handler = NULL;
static esp_event_handler_instance_t s_ip_handler   = NULL;

/* Contadores protegidos por spinlock (event handler e timer callback
 * rodam em tasks diferentes). */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t     s_retry_count   = 0;
static uint32_t     s_failure_count = 0;

/* -----------------------------------------------------------------------------
 * Forward declarations
 * -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data);

static void reconnect_timer_cb(void *arg);

static void schedule_reconnect(void);

/* -----------------------------------------------------------------------------
 * Reconexão com backoff
 * -------------------------------------------------------------------------- */

/*
 * Agenda uma tentativa de reconexão após o delay calculado.
 *
 * Fórmula: delay = min(1000 * 2^(retry_count-1), 30000)
 *   retry 1 → 1000 ms
 *   retry 2 → 2000 ms
 *   retry 3 → 4000 ms
 *   retry 4 → 8000 ms
 *   retry 5 → 16000 ms
 *   retry 6+ → 30000 ms
 *
 * O contador é resetado quando IP_EVENT_STA_GOT_IP chega.
 */
static void schedule_reconnect(void)
{
    portENTER_CRITICAL(&s_lock);
    s_retry_count++;

    uint32_t delay = WIFI_BACKOFF_BASE_MS;
    for (uint32_t i = 1; i < s_retry_count && delay < WIFI_BACKOFF_MAX_MS; i++) {
        delay *= 2;
    }
    if (delay > WIFI_BACKOFF_MAX_MS) {
        delay = WIFI_BACKOFF_MAX_MS;
    }
    uint32_t attempt = s_retry_count;
    portEXIT_CRITICAL(&s_lock);

    ESP_LOGW(TAG, "Reconectando em %u ms (tentativa %u)",
             (unsigned)delay, (unsigned)attempt);

    /*
     * esp_timer_start_once falha se o timer já estiver ativo. Paramos antes
     * e ignoramos ESP_ERR_INVALID_STATE (timer não estava rodando).
     */
    esp_timer_stop(s_reconnect_timer);

    esp_err_t err = esp_timer_start_once(s_reconnect_timer,
                                         (uint64_t)delay * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao agendar reconexão: %s", esp_err_to_name(err));
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Tentando reconectar...");

    /*
     * esp_wifi_connect() é thread-safe e pode ser chamado da task do
     * esp_timer. Se o Wi-Fi ainda estiver em processo de conexão,
     * retorna ESP_ERR_WIFI_CONN — tratamos como aviso.
     */
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect() retornou %s", esp_err_to_name(err));
    }
}

/* -----------------------------------------------------------------------------
 * Event handler
 * -------------------------------------------------------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {

        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA iniciado — conectando a '%s'",
                     CONFIG_WIFI_SSID);
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Associado ao AP");
            xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
            xEventGroupClearBits(s_event_group, WIFI_DISCONNECTED_BIT);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *evt =
                (wifi_event_sta_disconnected_t *)event_data;

            ESP_LOGW(TAG, "Desconectado (reason=%d)", evt->reason);

            xEventGroupClearBits(s_event_group,
                                 WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
            xEventGroupSetBits(s_event_group, WIFI_DISCONNECTED_BIT);

            portENTER_CRITICAL(&s_lock);
            s_failure_count++;
            portEXIT_CRITICAL(&s_lock);

            /*
             * WIFI_REASON_ASSOC_LEAVE (8) indica que fomos NÓS que
             * chamamos esp_wifi_disconnect(). Não devemos reconectar
             * automaticamente nesse caso — senão wifi_disconnect()
             * entraria em loop.
             */
            if (evt->reason == WIFI_REASON_ASSOC_LEAVE) {
                ESP_LOGI(TAG, "Desconexão intencional — não reconectando");
                break;
            }

            schedule_reconnect();
            break;
        }

        default:
            /* Outros eventos (SCAN_DONE, etc.) são ignorados. */
            break;
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "IP obtido: " IPSTR, IP2STR(&evt->ip_info.ip));

        /* Reseta backoff: próxima queda recomeça em 1s. */
        portENTER_CRITICAL(&s_lock);
        s_retry_count = 0;
        portEXIT_CRITICAL(&s_lock);

        /* Cancela reconexão pendente, se houver. */
        esp_timer_stop(s_reconnect_timer);

        xEventGroupSetBits(s_event_group, WIFI_GOT_IP_BIT);
        xEventGroupClearBits(s_event_group, WIFI_DISCONNECTED_BIT);
    }
}

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

esp_err_t wifi_init(void)
{
    if (CONFIG_WIFI_SSID[0] == '\0' ||
        strcmp(CONFIG_WIFI_SSID, "SUA_REDE_AQUI") == 0) {

        ESP_LOGE("WIFI", "Configure o SSID da rede em main/config.h");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (s_initialized) {
        ESP_LOGW(TAG, "wifi_init() chamada novamente — ignorando");
        return ESP_OK;
    }

    esp_err_t err;

    /* 1) Netif e event loop. Ambos podem já existir (outro componente
     *    inicializou). ESP_ERR_INVALID_STATE significa "já existe" — OK. */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init() falhou: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default() falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* 2) Event group. */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Falha ao criar event group");
        return ESP_ERR_NO_MEM;
    }

    /* 3) Interface de rede station. */
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta() retornou NULL");
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        return ESP_FAIL;
    }

    /* 4) Driver Wi-Fi. */
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() falhou: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 5) Handlers. */
    err = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_wifi_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar handler WIFI_EVENT: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    err = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler,
                                              NULL,
                                              &s_ip_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar handler IP_EVENT: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    /* 6) Configuração da estação. */
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));

    /*
     * ssid e password são uint8_t[] no driver, mas strings C no config.h.
     * snprintf garante null-terminator e trunca se necessário.
     */
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid),
             "%s", CONFIG_WIFI_SSID);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password),
             "%s", CONFIG_WIFI_PASSWORD);

    /*
     * WPA2-PSK cobre a maioria das redes domésticas. Se a sua rede for
     * WPA3 pura, troque para WIFI_AUTH_WPA3_PSK. Se for aberta, use
     * WIFI_AUTH_OPEN.
     */
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode() falhou: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config() falhou: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    /* 7) Timer de reconexão. */
    esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .arg      = NULL,
        .name     = "wifi_reconnect",
    };
    err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create() falhou: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 8) Start. Dispara WIFI_EVENT_STA_START → handler chama connect. */
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start() falhou: %s", esp_err_to_name(err));
        goto fail;
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Wi-Fi inicializado — SSID='%s'", CONFIG_WIFI_SSID);
    return ESP_OK;

fail:
    /* Cleanup parcial. Como s_initialized ainda é false, wifi_teardown()
     * não pode ser chamada; fazemos manualmente. */
    if (s_reconnect_timer != NULL) {
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }
    if (s_ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        s_wifi_handler = NULL;
    }
    esp_wifi_deinit();

    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    return err;
}

esp_err_t wifi_teardown(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_delete(s_reconnect_timer);
        s_reconnect_timer = NULL;
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_ip_handler != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler);
        s_ip_handler = NULL;
    }
    if (s_wifi_handler != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        s_wifi_handler = NULL;
    }

    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Wi-Fi finalizado");
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Estado e espera
 * -------------------------------------------------------------------------- */

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized || s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Timeout zero: apenas consulta, sem bloquear. */
    if (timeout_ms == 0) {
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        return (bits & WIFI_GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    TickType_t ticks;
    if (timeout_ms == portMAX_DELAY) {
        ticks = portMAX_DELAY;
    } else {
        ticks = pdMS_TO_TICKS(timeout_ms);
    }

    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_GOT_IP_BIT,
                                           pdFALSE,   /* não limpa ao sair */
                                           pdTRUE,    /* espera todos os bits */
                                           ticks);
    return (bits & WIFI_GOT_IP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool wifi_is_connected(void)
{
    if (!s_initialized || s_event_group == NULL) {
        return false;
    }

    EventBits_t bits = xEventGroupGetBits(s_event_group);
    return (bits & WIFI_GOT_IP_BIT) != 0;
}

esp_err_t wifi_get_ip(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_initialized || s_netif == NULL) {
        snprintf(buf, buf_len, "0.0.0.0");
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(s_netif, &ip_info);
    if (err != ESP_OK) {
        snprintf(buf, buf_len, "0.0.0.0");
        return err;
    }

    if (ip_info.ip.addr == 0) {
        snprintf(buf, buf_len, "0.0.0.0");
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(buf, buf_len, IPSTR, IP2STR(&ip_info.ip));
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Event group
 * -------------------------------------------------------------------------- */

EventGroupHandle_t wifi_get_event_group(void)
{
    return s_event_group;
}

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

void wifi_get_stats(uint32_t *attempts_out, uint32_t *failures_out)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t attempts = s_retry_count;
    uint32_t failures = s_failure_count;
    portEXIT_CRITICAL(&s_lock);

    if (attempts_out != NULL) {
        *attempts_out = attempts;
    }
    if (failures_out != NULL) {
        *failures_out = failures;
    }
}

esp_err_t wifi_disconnect(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Gera WIFI_EVENT_STA_DISCONNECTED com reason WIFI_REASON_ASSOC_LEAVE.
     * O handler reconhece esse reason e NÃO agenda reconexão.
     */
    return esp_wifi_disconnect();
}