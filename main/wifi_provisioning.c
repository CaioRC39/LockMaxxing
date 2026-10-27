/* =============================================================================
 * main/wifi_provisioning.c — Portal de provisionamento via SoftAP
 * ========================================================================== */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "sdkconfig.h"
#include "app_config.h"
#include "wifi_provisioning.h"

static const char *TAG = "WIFI_PROV";

static httpd_handle_t s_httpd = NULL;
static esp_timer_handle_t s_timeout_timer = NULL;

/* ── Event group para sincronização do boot do AP ── */
static EventGroupHandle_t s_prov_events = NULL;
#define PROV_AP_STARTED_BIT   BIT0

/* Handler que só sinaliza AP_START */
static void prov_wifi_event_handler(void *arg, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "AP_START recebido — SoftAP ativo");
        xEventGroupSetBits(s_prov_events, PROV_AP_STARTED_BIT);
    }
}

/* ---------- HTML do formulário ----------------------------------------- */
/* Sem JS redirect: o captive handler (302) faz o redirecionamento correto.
 * JS extra só causava loop no mobile quando o browser mostra :80 no host. */

static const char HTML_FORM[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>LockMaxxing Setup</title>"
"<style>"
"body{font-family:system-ui,sans-serif;max-width:420px;margin:40px auto;padding:0 20px;"
"background:#f5f5f5;color:#333}"
"h1{font-size:22px;margin-bottom:6px}"
"p{color:#666;font-size:14px;margin-top:0}"
"label{display:block;margin:16px 0 4px;font-weight:600;font-size:14px}"
"input{width:100%;padding:10px;font-size:16px;border:1px solid #ccc;border-radius:6px;box-sizing:border-box}"
"button{width:100%;padding:12px;font-size:16px;background:#0066cc;color:white;"
"border:none;border-radius:6px;margin-top:24px;cursor:pointer}"
"button:active{background:#004499}"
".info{background:#e7f3ff;border-left:4px solid #0066cc;padding:10px;margin-top:20px;"
"font-size:13px;color:#004085}"
"</style></head><body>"
"<h1>LockMaxxing</h1>"
"<p>Configure a rede Wi-Fi para o dispositivo.</p>"
"<form action='/save' method='POST'>"
"<label>Nome da rede (SSID)</label>"
"<input name='ssid' required maxlength='32' autofocus placeholder='MinhaRedeWiFi'>"
"<label>Senha da rede</label>"
"<input name='password' type='password' maxlength='64' placeholder='Deixe em branco se aberta'>"
"<button type='submit'>Salvar e Conectar</button>"
"</form>"
"<div class='info'>"
"O endereço do servidor j&aacute; est&aacute; configurado de f&aacute;brica. "
"Voc&ecirc; s&oacute; precisa informar a rede Wi-Fi."
"</div>"
"</body></html>";

/* ---------- HTML de sucesso -------------------------------------------- */

static const char HTML_SUCCESS[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>Configurado</title>"
"<style>body{font-family:system-ui;max-width:420px;margin:60px auto;padding:20px;"
"text-align:center;background:#f5f5f5}"
"h1{color:#00a651}.spinner{display:inline-block;width:40px;height:40px;"
"border:4px solid #ddd;border-top-color:#0066cc;border-radius:50%;"
"animation:spin 1s linear infinite;margin:20px auto}"
"@keyframes spin{to{transform:rotate(360deg)}}</style></head>"
"<body><h1>Configurado com sucesso</h1>"
"<div class='spinner'></div>"
"<p>Reiniciando o dispositivo...</p>"
"<p style='color:#888;font-size:13px'>Voc&ecirc; pode fechar esta p&aacute;gina.</p>"
"</body></html>";

/* ---------- HTML de erro ----------------------------------------------- */

static const char HTML_ERROR[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<title>Erro</title></head><body>"
"<h2>Erro ao salvar configuração</h2>"
"<p>Tente novamente. <a href='/'>Voltar</a></p>"
"</body></html>";

/* ---------- Utilitário: parse de form-urlencoded ----------------------- */

static esp_err_t form_get(const char *body, const char *key,
                          char *out, size_t out_len)
{
    size_t klen = strlen(key);
    const char *p = body;

    while (p != NULL && *p != '\0') {
        const char *eq = strchr(p, '=');
        if (eq == NULL) break;

        const char *amp = strchr(p, '&');
        if (amp == NULL) amp = p + strlen(p);

        size_t this_key_len = (size_t)(eq - p);
        if (this_key_len == klen && strncmp(p, key, klen) == 0) {
            const char *val_start = eq + 1;
            size_t val_len = (size_t)(amp - val_start);
            if (val_len >= out_len) return ESP_ERR_INVALID_SIZE;

            size_t j = 0;
            for (size_t i = 0; i < val_len && j < out_len - 1; i++) {
                char c = val_start[i];
                if (c == '+') {
                    out[j++] = ' ';
                } else if (c == '%' && i + 2 < val_len) {
                    char hex[3] = { val_start[i+1], val_start[i+2], 0 };
                    out[j++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    out[j++] = c;
                }
            }
            out[j] = '\0';
            return ESP_OK;
        }

        p = (*amp == '&') ? (amp + 1) : NULL;
    }
    return ESP_ERR_NOT_FOUND;
}

/* ============================================================================
 * HANDLERS (definidos ANTES dos httpd_uri_t para evitar forward declarations)
 * ========================================================================== */

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_FORM, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handle_save(httpd_req_t *req)
{
    /* 1) Lê o corpo do POST */
    char body[512];
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        ESP_LOGE(TAG, "httpd_req_recv falhou");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* 2) Extrai os campos */
    char ssid[APP_CONFIG_SSID_MAX]   = {0};
    char pass[APP_CONFIG_PASS_MAX]   = {0};

    if (form_get(body, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        ESP_LOGW(TAG, "SSID ausente ou vazio");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, HTML_ERROR, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    if (form_get(body, "password", pass, sizeof(pass)) != ESP_OK) {
        pass[0] = '\0';
    }

    ESP_LOGI(TAG, "Configuração recebida: SSID='%s'", ssid);

    /* 3) Salva na NVS */
    app_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy(cfg.wifi_ssid,     ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_password, pass, sizeof(cfg.wifi_password));
    strlcpy(cfg.mqtt_uri, app_config_factory_broker(), sizeof(cfg.mqtt_uri));
    cfg.provisioned = true;

    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao salvar na NVS: %s", esp_err_to_name(err));
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    /* 4) Responde e reinicia */
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, HTML_SUCCESS, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Configuração salva — reboot em 1500 ms");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

/* Handler genérico de captive portal: força redirecionamento com 302.
 * Usado por Android, iOS, Windows e catch-all. Retornar 302 (em vez de
 * "204" ou "Success") é o que cada SO interpreta como "há portal aqui". */
static esp_err_t handle_captive_generic(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive redirect: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "", 0);
}

/* Android: espera 204 vazio; 302 força abrir o portal. */
static esp_err_t handle_captive_android(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive Android: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "", 0);
}

/* iOS: espera HTML com "Success"; qualquer outra coisa força o portal.
 * Retornar 302 é o jeito mais confiável (não depende do formato do HTML). */
static esp_err_t handle_captive_ios(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Captive iOS: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, "", 0);
}

/* Favicon: 204 No Content evita 404 e poluição no log. */
static esp_err_t handle_favicon(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* ============================================================================
 * URI STRUCTS (todos os handlers já estão definidos acima)
 * ========================================================================== */

static const httpd_uri_t s_uri_root = {
    .uri = "/", .method = HTTP_GET, .handler = handle_root,
};
static const httpd_uri_t s_uri_save = {
    .uri = "/save", .method = HTTP_POST, .handler = handle_save,
};
/* Android */
static const httpd_uri_t s_uri_android = {
    .uri = "/generate_204", .method = HTTP_GET, .handler = handle_captive_android,
};
static const httpd_uri_t s_uri_android2 = {
    .uri = "/gen_204", .method = HTTP_GET, .handler = handle_captive_android,
};
/* iOS */
static const httpd_uri_t s_uri_ios = {
    .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = handle_captive_ios,
};
static const httpd_uri_t s_uri_ios2 = {
    .uri = "/library/test/success.html", .method = HTTP_GET, .handler = handle_captive_ios,
};
/* Windows */
static const httpd_uri_t s_uri_win = {
    .uri = "/connecttest.txt", .method = HTTP_GET, .handler = handle_captive_generic,
};
static const httpd_uri_t s_uri_win2 = {
    .uri = "/ncsi.txt", .method = HTTP_GET, .handler = handle_captive_generic,
};
static const httpd_uri_t s_uri_win3 = {
    .uri = "/redirect", .method = HTTP_GET, .handler = handle_captive_generic,
};
/* Favicon */
static const httpd_uri_t s_uri_favicon = {
    .uri = "/favicon.ico", .method = HTTP_GET, .handler = handle_favicon,
};
/* Catch-all — DEVE ser registrado por último */
static const httpd_uri_t s_uri_catchall = {
    .uri = "/*", .method = HTTP_GET, .handler = handle_captive_generic,
};

/* ---------- DNS captive portal ---------------------------------------- */

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket falhou");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind falhou");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive portal rodando na porta 53");

    uint8_t buf[512];
    while (1) {
        struct sockaddr_in client;
        socklen_t client_len = sizeof(client);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &client_len);
        if (len < 12) continue;

        uint8_t resp[512];
        memcpy(resp, buf, len);

        resp[2] = 0x81;
        resp[3] = 0x80;
        resp[6] = 0x00; resp[7] = 0x01;

        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;
        resp[pos++] = 0x00; resp[pos++] = 0x01;
        resp[pos++] = 0x00; resp[pos++] = 0x01;
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3C;
        resp[pos++] = 0x00; resp[pos++] = 0x04;
        resp[pos++] = 192; resp[pos++] = 168;
        resp[pos++] = 4;   resp[pos++] = 1;

        sendto(sock, resp, pos, 0,
               (struct sockaddr *)&client, client_len);
    }
}

/* ---------- Timeout do portal ----------------------------------------- */

static void on_timeout(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Timeout do portal — reboot sem configuração");
    esp_restart();
}

/* ---------- Helper de registro (detecta erros como o do max_uri_handlers) */

static void register_uri(httpd_handle_t h, const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(h, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar %s: %s",
                 uri->uri, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Handler registrado: %s", uri->uri);
    }
}

/* ---------- Função principal ------------------------------------------ */

esp_err_t wifi_provisioning_run(void)
{
    ESP_LOGI(TAG, "=== Modo provisionamento ===");

    esp_err_t err;

    /* 1) Netif e event loop (idempotente) */
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    /* 2) Event group para sincronizar boot do AP */
    s_prov_events = xEventGroupCreate();
    if (s_prov_events == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate falhou");
        return ESP_ERR_NO_MEM;
    }

    /* 3) Netif AP com IP fixo e DHCP */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap falhou");
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw,      192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    /* 4) Handler do AP_START registrado ANTES do start */
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                        &prov_wifi_event_handler, NULL, NULL);

    /* 5) Wi-Fi config + start */
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wcfg);
    if (err != ESP_OK) return err;

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = "LockMaxxing-Setup",
            .channel        = 1,
            .max_connection = 4,
            .authmode       = (CONFIG_LOCKMAXXING_PROVISIONING_AP_PASSWORD[0] != '\0')
                              ? WIFI_AUTH_WPA2_PSK
                              : WIFI_AUTH_OPEN,
        },
    };
    if (CONFIG_LOCKMAXXING_PROVISIONING_AP_PASSWORD[0] != '\0') {
        strlcpy((char *)ap_cfg.ap.password,
                CONFIG_LOCKMAXXING_PROVISIONING_AP_PASSWORD,
                sizeof(ap_cfg.ap.password));
    }

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    /* 6) Espera AP_START (evento real, não delay) */
    ESP_LOGI(TAG, "Aguardando WIFI_EVENT_AP_START...");
    EventBits_t bits = xEventGroupWaitBits(s_prov_events, PROV_AP_STARTED_BIT,
                                            pdTRUE, pdTRUE,
                                            pdMS_TO_TICKS(10000));
    if ((bits & PROV_AP_STARTED_BIT) == 0) {
        ESP_LOGE(TAG, "Timeout aguardando AP_START — abortando");
        return ESP_ERR_TIMEOUT;
    }

    esp_netif_ip_info_t actual_ip;
    esp_netif_get_ip_info(ap_netif, &actual_ip);
    ESP_LOGI(TAG, "AP IP: " IPSTR, IP2STR(&actual_ip.ip));

    /* 7) HTTP server — max_uri_handlers aumentado para 16
     *    (o default é 8, e temos 10+ handlers: o catch-all seria cortado) */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port       = 80;
    hcfg.lru_purge_enable  = true;
    hcfg.uri_match_fn      = httpd_uri_match_wildcard;
    hcfg.max_uri_handlers  = 16;    /* ← CORREÇÃO CRÍTICA */

    err = httpd_start(&s_httpd, &hcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou: %s", esp_err_to_name(err));
        return err;
    }

    /* 8) Registro — catch-all DEVE ser o último */
    register_uri(s_httpd, &s_uri_root);
    register_uri(s_httpd, &s_uri_save);
    register_uri(s_httpd, &s_uri_android);
    register_uri(s_httpd, &s_uri_android2);
    register_uri(s_httpd, &s_uri_ios);
    register_uri(s_httpd, &s_uri_ios2);
    register_uri(s_httpd, &s_uri_win);
    register_uri(s_httpd, &s_uri_win2);
    register_uri(s_httpd, &s_uri_win3);
    register_uri(s_httpd, &s_uri_favicon);
    register_uri(s_httpd, &s_uri_catchall);   /* por último */

    ESP_LOGI(TAG, "SoftAP '%s' ativo — portal em http://192.168.4.1/",
             ap_cfg.ap.ssid);
    ESP_LOGI(TAG, "Timeout: %d s", CONFIG_LOCKMAXXING_PROVISIONING_TIMEOUT_S);

    /* 9) DNS captive */
    if (xTaskCreate(dns_server_task, "dns_prov", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar task DNS");
    }

    /* 10) Timer de timeout */
    esp_timer_create_args_t targs = {
        .callback = on_timeout,
        .name     = "prov_timeout",
    };
    esp_timer_create(&targs, &s_timeout_timer);
    esp_timer_start_once(s_timeout_timer,
                         (uint64_t)CONFIG_LOCKMAXXING_PROVISIONING_TIMEOUT_S * 1000000ULL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Aguardando configuração...");
    }
    return ESP_OK;
}