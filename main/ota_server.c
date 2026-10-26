/* =============================================================================
 * main/ota_server.c — Servidor HTTPS local para OTA (modelo "push")
 * =============================================================================
 *
 * Este arquivo sobe um servidor HTTPS na placa que aceita:
 *
 *   POST /update
 *     - Header opcional: X-SHA256: <64 chars hex>
 *     - Corpo: binário completo do firmware (.bin gerado por idf.py build)
 *
 *   GET /status
 *     - Retorna 200 + JSON com estado atual, partição em execução e alvo
 *
 * O certificado é auto-assinado e vem do header gerado "certs.h".
 *
 * Concorrência: um mutex global (g_upload_mutex) garante que apenas uma
 * gravação OTA ocorra por vez. Tentativas concorrentes recebem HTTP 409.
 *
 * Este arquivo é dividido em 3 partes:
 *   PARTE 1/3 — Includes, contexto, mutex, handler POST /update (esta)
 *   PARTE 2/3 — Handler GET /status, config do servidor, start/stop
 *   PARTE 3/3 — API pública e montagem final
 * ========================================================================== */

#include "cJSON.h"
 #include "ota.h"
#include "ota_internal.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_https_server.h"
#include "esp_netif.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "psa/crypto.h"

/* Certificado gerado pelo build (ver tools/gen_cert.sh + main/CMakeLists.txt). */
#include "certs.h"

/* -----------------------------------------------------------------------------
 * Constantes
 * -------------------------------------------------------------------------- */

/* Buffer de leitura do corpo HTTP. 4 KB = página típica de flash. */
#define OTA_SERVER_BUF_SIZE             4096

/* Stack da task do httpd. O handler roda nesta stack; 8 KB é confortável. */
#define OTA_SERVER_TASK_STACK           (8 * 1024)

/* Prioridade do httpd. Igual à task de cliente OTA (5). */
#define OTA_SERVER_TASK_PRIO            5

/* Timeout (ms) do socket de recepção. httpd_req_recv() retorna timeout
 * se o cliente demorar mais que isso entre pacotes. Aceitamos alguns
 * timeouts consecutivos antes de abortar (rede instável). */
#define OTA_SERVER_RECV_TIMEOUT_MS      (10 * 1000)

/* Número de timeouts consecutivos que toleramos antes de abortar upload. */
#define OTA_SERVER_MAX_RECV_TIMEOUTS    3

/* Delay antes do reboot, dando tempo do cliente receber o HTTP 200. */
#define OTA_SERVER_REBOOT_DELAY_MS      2000

/* Tamanho máximo do valor de X-SHA256 (hex + '\0'). */
#define OTA_SERVER_SHA_HDR_MAX          80

/* -----------------------------------------------------------------------------
 * Contexto global do servidor
 * -------------------------------------------------------------------------- */

/*
 * Handle do httpd. NULL quando o servidor está parado.
 */
static httpd_handle_t g_httpd = NULL;

/*
 * Mutex que serializa uploads. Duas requisições POST /update concorrentes
 * corromperiam a partição OTA se ambas chamassem esp_ota_begin().
 */
static SemaphoreHandle_t g_upload_mutex = NULL;

/*
 * Porta em que o servidor está rodando, para diagnóstico.
 */
static uint16_t g_server_port = 0;

static const char *TAG_SRV = "OTA_SRV";

/* Buffer de upload. Estático — apenas um POST /update roda por vez,
 * garantido pelo g_upload_mutex. */
static uint8_t s_upload_buf[OTA_SERVER_BUF_SIZE];

// Mutex para o buffer de upload
static StaticSemaphore_t s_upload_buf_mutex_buf;
static SemaphoreHandle_t s_upload_buf_mutex = NULL;

// Protótipos
/* Forward decl — precisa existir antes de ser usada em httpd_uri_t. */
static esp_err_t update_post_handler(httpd_req_t *req);
static esp_err_t pull_ota_post_handler(httpd_req_t *req);
static esp_err_t status_get_handler(httpd_req_t *req);

/* -----------------------------------------------------------------------------
 * Helpers de resposta HTTP
 * -------------------------------------------------------------------------- */

/*
 * Envia uma resposta JSON simples. Sempre fecha o socket após (Connection: close)
 * para evitar reuso com estado inconsistente.
 */
static esp_err_t send_json_error(httpd_req_t *req, int status, const char *msg)
{
    char body[256];
    /* json escaping mínimo: só aspas. Mensagens internas não têm outros chars. */
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status == 400 ? "400 Bad Request" :
                               status == 409 ? "409 Conflict" :
                               status == 500 ? "500 Internal Server Error" :
                               status == 413 ? "413 Payload Too Large" :
                                               "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json_ok(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* -----------------------------------------------------------------------------
 * Handler POST /update
 * -------------------------------------------------------------------------- */

 static const httpd_uri_t uri_post_update = {
    .uri      = "/update",
    .method   = HTTP_POST,
    .handler  = update_post_handler,
    .user_ctx = NULL,
};

static esp_err_t update_post_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG_SRV, "POST /update de %s (content_length=%d)",
             req->uri, req->content_len);

    /* -------------------------------------------------------------------------
     * Grace period: rejeita OTA nos primeiros N segundos após boot.
     *
     * Colocado ANTES do mutex para:
     *   - Não segurar recurso desnecessariamente
     *   - Não pausar a comm_task (g_ota_busy)
     *   - Não sujar o estado da máquina (DOWNLOADING → ERROR → IDLE)
     *
     * Se CONFIG_LOCKMAXXING_OTA_WARMUP_S == 0, checagem é pulada.
     * ---------------------------------------------------------------------- */
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    if (uptime_s < CONFIG_LOCKMAXXING_OTA_WARMUP_S) {
        int remaining = CONFIG_LOCKMAXXING_OTA_WARMUP_S - (int)uptime_s;
        ESP_LOGW(TAG_SRV, "OTA rejeitada: warmup (uptime=%llds, faltam %ds)",
                 (long long)uptime_s, remaining);

        char body[128];
        snprintf(body, sizeof(body),
                 "{\"error\":\"warmup\",\"uptime_s\":%lld,\"retry_in_s\":%d}",
                 (long long)uptime_s, remaining);

        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "60");
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }

    /* 1) Exclusividade: uma OTA por vez */
    if (g_upload_mutex == NULL) {
        ESP_LOGE(TAG_SRV, "Mutex de upload não inicializado");
        return send_json_error(req, 500, "server not initialized");
    }

    if (xSemaphoreTake(g_upload_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG_SRV, "Upload já em andamento — rejeitando");
        return send_json_error(req, 409, "update already in progress");
    }

    ota_set_state(OTA_STATE_DOWNLOADING);
    g_ota_busy = true;

    /* A partir daqui, precisamos liberar o mutex em QUALQUER caminho. */
    esp_err_t err = ESP_OK;
    esp_ota_handle_t ota_handle = 0;
    psa_hash_operation_t sha_ctx = PSA_HASH_OPERATION_INIT;
    bool sha_initialized = false;
    size_t total_received = 0;
    bool reboot_pending = false;

#define SRV_FAIL(code, msg)                                                    \
    do {                                                                       \
        err = (code);                                                          \
        ESP_LOGE(TAG_SRV, "%s", (msg));                                        \
        send_json_error(req, 500, (msg));                                      \
        goto cleanup;                                                          \
    } while (0)

    /* -------------------------------------------------------------------------
     * 2) Identifica partição alvo e valida Content-Length
     * ---------------------------------------------------------------------- */

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        SRV_FAIL(ESP_ERR_NOT_FOUND, "no OTA partition available");
    }

    if (req->content_len <= 0) {
        SRV_FAIL(ESP_ERR_INVALID_ARG, "empty body");
    }
    if ((size_t)req->content_len > update_part->size) {
        SRV_FAIL(ESP_ERR_INVALID_SIZE, "image larger than partition");
    }

    ESP_LOGI(TAG_SRV, "Gravando em '%s' (offset=0x%08lx, max=%lu B)",
             update_part->label,
             (unsigned long)update_part->address,
             (unsigned long)update_part->size);

    /* -------------------------------------------------------------------------
     * 3) Inicia escrita
     * ---------------------------------------------------------------------- */

    err = esp_ota_begin(update_part, (size_t)req->content_len, &ota_handle);
    if (err != ESP_OK) {
        SRV_FAIL(err, "esp_ota_begin failed");
    }

    /* -------------------------------------------------------------------------
     * 4) Buffer e SHA-256
     * ---------------------------------------------------------------------- */

    err = ota_sha256_stream_init(&sha_ctx);
    if (err != ESP_OK) {
        SRV_FAIL(err, "sha256 init failed");
    }
    sha_initialized = true;

    /* -------------------------------------------------------------------------
     * 5) Loop de recepção
     * ---------------------------------------------------------------------- */

    // Adquire o mutex do buffer antes de começar a usá-lo
    if (xSemaphoreTake(s_upload_buf_mutex, portMAX_DELAY) != pdTRUE) {
        SRV_FAIL(ESP_ERR_TIMEOUT, "timeout no mutex do buffer");
    }
    
     int consecutive_timeouts = 0;

    while (total_received < (size_t)req->content_len) {
        int n = httpd_req_recv(req, (char *)s_upload_buf, OTA_SERVER_BUF_SIZE);

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            consecutive_timeouts++;
            ESP_LOGW(TAG_SRV, "Timeout %d/%d em httpd_req_recv",
                     consecutive_timeouts, OTA_SERVER_MAX_RECV_TIMEOUTS);
            if (consecutive_timeouts >= OTA_SERVER_MAX_RECV_TIMEOUTS) {
                SRV_FAIL(ESP_ERR_TIMEOUT, "socket timeout");
            }
            continue;
        }

        if (n < 0) {
            SRV_FAIL(ESP_FAIL, "httpd_req_recv failed");
        }
        if (n == 0) {
            SRV_FAIL(ESP_ERR_INVALID_SIZE,
                     "client closed before sending all bytes");
        }

        consecutive_timeouts = 0;

        err = esp_ota_write(ota_handle, s_upload_buf, (size_t)n);
        if (err != ESP_OK) {
            SRV_FAIL(err, "esp_ota_write failed");
        }

        err = ota_sha256_stream_update(&sha_ctx, s_upload_buf, (size_t)n);
        if (err != ESP_OK) {
            SRV_FAIL(err, "sha256 update failed");
        }

        total_received += (size_t)n;
    }

    ESP_LOGI(TAG_SRV, "Recebidos %zu bytes", total_received);

    /* -------------------------------------------------------------------------
     * 6) Valida SHA-256 (se o cliente enviou o header)
     * ---------------------------------------------------------------------- */

    char computed_hex[OTA_SHA256_HEX_LEN + 1];
    err = ota_sha256_stream_finish(&sha_ctx, computed_hex);
    sha_initialized = false;   /* finish() já liberou o contexto */
    if (err != ESP_OK) {
        SRV_FAIL(err, "sha256 finish failed");
    }
    ESP_LOGI(TAG_SRV, "SHA-256 calculado: %s", computed_hex);

    char expected_hdr[OTA_SERVER_SHA_HDR_MAX];
    size_t hdr_len = sizeof(expected_hdr);
    if (httpd_req_get_hdr_value_str(req, "X-SHA256",
                                    expected_hdr, hdr_len) == ESP_OK) {
        if (!ota_sha256_hex_equals(computed_hex, expected_hdr)) {
            SRV_FAIL(ESP_ERR_INVALID_CRC, "sha256 mismatch");
        }
        ESP_LOGI(TAG_SRV, "SHA-256 confere com o header");
    } else {
        ESP_LOGW(TAG_SRV, "Header X-SHA256 ausente — integridade não verificada");
    }

    /* -------------------------------------------------------------------------
     * 7) Finaliza e marca partição
     * ---------------------------------------------------------------------- */

    err = esp_ota_end(ota_handle);
    ota_handle = 0;
    if (err != ESP_OK) {
        SRV_FAIL(err, "esp_ota_end rejected the image");
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        SRV_FAIL(err, "esp_ota_set_boot_partition failed");
    }

    /* -------------------------------------------------------------------------
     * 8) Resposta de sucesso
     * ---------------------------------------------------------------------- */

    char ok_body[192];
    snprintf(ok_body, sizeof(ok_body),
             "{\"status\":\"ok\",\"bytes\":%zu,\"partition\":\"%s\","
             "\"sha256\":\"%s\",\"reboot_in_ms\":%d}",
             total_received, update_part->label, computed_hex,
             OTA_SERVER_REBOOT_DELAY_MS);
    send_json_ok(req, ok_body);
    ota_set_state(OTA_STATE_SUCCESS);
    reboot_pending = true;
    ESP_LOGI(TAG_SRV, "OTA aplicada — reboot em %d ms", OTA_SERVER_REBOOT_DELAY_MS);

cleanup:
    /* Libera recursos internos do handler (sha, ota_handle, s_upload_buf ) */
    if (sha_initialized) psa_hash_abort(&sha_ctx);
    if (ota_handle != 0) esp_ota_abort(ota_handle);
    if (err != ESP_OK) ota_set_state(OTA_STATE_ERROR);
    if (err != ESP_OK) g_ota_busy = false;

    /* Se reboot_pending, deixa true — vai reiniciar. */
    if (reboot_pending) {
        /* MANTÉM o mutex durante o delay: bloqueia novos POSTs (409)
         * até o reboot acontecer. Nenhuma segunda requisição pode
         * começar um upload concorrente ao s_upload_buf. */
        ESP_LOGI(TAG_SRV, "Reboot em %d ms (mutex mantido — uploads "
                          "bloqueados com 409 até lá)",
                 OTA_SERVER_REBOOT_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(OTA_SERVER_REBOOT_DELAY_MS));
        esp_restart();

        /* Só executado se esp_restart() falhar. Log de erro explícito. */
        ESP_LOGE(TAG_SRV, "esp_restart() retornou inesperadamente!");
    }

    /* Só chegamos aqui se NÃO houve reboot (falha no upload) ou se
     * esp_restart() falhou. */
    xSemaphoreGive(g_upload_mutex);
    return ESP_OK;

#undef SRV_FAIL
}


/* =============================================================================
 * Conteúdo: GET /status + httpd_ssl_config_t pronto para start
 * ========================================================================== */

/* -----------------------------------------------------------------------------
 * Handler GET /status
 * -------------------------------------------------------------------------- */

static const httpd_uri_t uri_get_status = {
    .uri      = "/status",
    .method   = HTTP_GET,
    .handler  = status_get_handler,
    .user_ctx = NULL,
};

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const esp_partition_t *running = NULL;
    const esp_partition_t *next    = NULL;

    /*
     * Consulta via esp_ota_get_*_partition (não as funções de ota_common,
     * para manter esta peça autocontida e independente do estado global
     * de OTA — /status deve funcionar mesmo se ninguém chamou ota_init
     * além do start do servidor).
     */
    running = esp_ota_get_running_partition();
    next    = esp_ota_get_next_update_partition(NULL);

    int64_t uptime_ms = esp_timer_get_time() / 1000;

    /* 384 bytes é folgado para 3 labels + state + uint64 em decimal. */
    char body[384];
    int n = snprintf(body, sizeof(body),
        "{\"state\":%d,"
        "\"running\":\"%s\","
        "\"next\":\"%s\","
        "\"pending_verify\":%s,"
        "\"port\":%u,"
        "\"uptime_ms\":%lld}",
        (int)ota_get_state(),
        running != NULL ? running->label : "unknown",
        next    != NULL ? next->label    : "none",
        ota_is_pending_verify() ? "true" : "false",
        (unsigned)g_server_port,
        (long long)uptime_ms);

    if (n < 0 || (size_t)n >= sizeof(body)) {
        /* snprintf truncou — improvável, mas devolve erro explícito. */
        return send_json_error(req, 500, "status serialization overflow");
    }

    return send_json_ok(req, body);
}

/* -----------------------------------------------------------------------------
 * Configuração TLS do servidor
 * -------------------------------------------------------------------------- */

/*
 * Constrói a struct de configuração do servidor HTTPS. Retorna por ponteiro
 * para que a chamada viva no escopo de ota_local_server_start() (Parte 3/3);
 * o httpd_ssl_start() copia os campos que precisa.
 *
 * A configuração padrão (HTTPD_SSL_CONFIG_DEFAULT) preenche:
 *   - transport_mode  = HTTPD_SSL_TRANSPORT_SECURE
 *   - cert_select_cb  = NULL (usa httpd_ssl_accept sem seleção de cert)
 *   - session tickets desabilitados
 *   - httpd.stack_size = 10 KB (o nosso ajusta para 8 KB)
 *   - httpd.max_open_sockets = 7
 *
 * Nós só sobrescrevemos o necessário.
 */
static esp_err_t build_ssl_config(httpd_ssl_config_t *cfg, uint16_t port)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *cfg = (httpd_ssl_config_t)HTTPD_SSL_CONFIG_DEFAULT();

    /* Certificado e chave (ponteiros para certs.h gerado no build).
     *
     * mbedtls exige que o comprimento INCLUA o '\0' terminador para
     * strings PEM. Usamos strlen() + 1 para garantir isso, independente
     * de SERVER_CERT_PEM_LEN estar com ou sem o '\0' (o gerador pode
     * ter mudado entre versões).
     */
    const char *cert_str = (const char *)SERVER_CERT_PEM;
    const char *key_str  = (const char *)SERVER_KEY_PEM;

    cfg->servercert     = (const uint8_t *)cert_str;
    cfg->servercert_len = strlen(cert_str) + 1;
    cfg->prvtkey_pem    = (const uint8_t *)key_str;
    cfg->prvtkey_len    = strlen(key_str) + 1;

    /* Porta pública. */
    cfg->port_secure = port;

    /* Configurações do servidor HTTP interno. */
    cfg->httpd.server_port        = port;
    cfg->httpd.ctrl_port          = 0;
    cfg->httpd.stack_size         = OTA_SERVER_TASK_STACK;
    cfg->httpd.max_uri_handlers   = 8;
    cfg->httpd.max_open_sockets   = 3;
    cfg->httpd.lru_purge_enable   = true;
    cfg->httpd.recv_wait_timeout  = OTA_SERVER_RECV_TIMEOUT_MS / 1000;
    cfg->httpd.send_wait_timeout  = 5;
    cfg->httpd.uri_match_fn       = httpd_uri_match_wildcard;
    cfg->httpd.keep_alive_enable  = false;

    /* Validação defensiva: certificado e chave são obrigatórios. */
    if (cfg->servercert_len < 30 || cfg->prvtkey_len < 30) {
        ESP_LOGE(TAG_SRV, "Certificado ou chave com comprimento suspeito "
                          "(cert=%u, key=%u) — certs.h vazio?",
                 (unsigned)cfg->servercert_len,
                 (unsigned)cfg->prvtkey_len);
        return ESP_ERR_INVALID_STATE;
    }

    /* Verifica marcadores BEGIN/END. Usa strstr(), que é robusto ao
     * posicionamento exato do newline final e não depende do comprimento
     * passado. O array subjacente é null-terminated (static const char[]). */
    if (strstr(cert_str, "-----BEGIN CERTIFICATE-----") == NULL ||
        strstr(cert_str, "-----END CERTIFICATE-----")   == NULL) {
        ESP_LOGE(TAG_SRV, "Certificado não parece ser PEM "
                          "(faltam marcadores BEGIN ou END)");
        return ESP_ERR_INVALID_STATE;
    }

    if (strstr(key_str, "-----BEGIN") == NULL ||
        strstr(key_str, "-----END")   == NULL) {
        ESP_LOGE(TAG_SRV, "Chave privada não parece ser PEM "
                          "(faltam marcadores BEGIN ou END)");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


/* =============================================================================
 * Conteúdo: ota_local_server_start / stop / is_running + montagem final
 * ========================================================================== */

/* -----------------------------------------------------------------------------
 * Helpers para cleanup e log de IP
 * -------------------------------------------------------------------------- */

/*
 * Registra um handler e faz log. Encapsula o padrão "if (httpd_register_uri_handler
 * != ESP_OK) { log; return err; }" que aparece duas vezes.
 */
static esp_err_t register_handler(httpd_handle_t server,
                                  const httpd_uri_t *uri)
{
    esp_err_t err = httpd_register_uri_handler(server, uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SRV, "Falha ao registrar %s: %s",
                 uri->uri, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG_SRV, "Handler registrado: %s %s",
             uri->method == HTTP_GET ? "GET" : "POST", uri->uri);
    return ESP_OK;
}

/*
 * Descobre o IP atual da interface Wi-Fi Station e loga o endereço
 * de escuta do servidor. Se a interface não estiver pronta, loga
 * 0.0.0.0 e continua — o servidor sobe mesmo sem IP.
 */
static void log_listen_address(uint16_t port)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == NULL) {
        ESP_LOGW(TAG_SRV, "Wi-Fi ainda não inicializado — servidor ouvindo em "
                          "0.0.0.0:%u (acessível após Wi-Fi conectar)",
                 (unsigned)port);
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        ESP_LOGW(TAG_SRV, "Wi-Fi sem IP ainda — servidor ouvindo em "
                          "0.0.0.0:%u (acessível após DHCP)",
                 (unsigned)port);
        return;
    }

    ESP_LOGI(TAG_SRV, "Servidor HTTPS ativo em https://" IPSTR ":%u",
             IP2STR(&ip_info.ip), (unsigned)port);
    ESP_LOGI(TAG_SRV, "  POST /update  (corpo = firmware.bin)");
    ESP_LOGI(TAG_SRV, "  GET  /status  (diagnóstico)");
    ESP_LOGW(TAG_SRV, "Certificado é auto-assinado — use -k no curl ou "
                      "--insecure no cliente Python");
}

#if CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE

/* Callback do pull OTA. Roda na task do ota_client (prioridade 5). */
static void pull_ota_result_cb(esp_err_t result, const char *msg)
{
    if (result == ESP_OK) {
        ESP_LOGI(TAG_SRV, "Pull OTA aplicada — reboot em 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    } else {
        ESP_LOGE(TAG_SRV, "Pull OTA falhou: %s", msg);
    }
}

static esp_err_t pull_ota_post_handler(httpd_req_t *req)
{
    /* Warmup */
    int64_t uptime_s = esp_timer_get_time() / 1000000;
    if (uptime_s < CONFIG_LOCKMAXXING_OTA_WARMUP_S) {
        int remaining = CONFIG_LOCKMAXXING_OTA_WARMUP_S - (int)uptime_s;
        ESP_LOGW(TAG_SRV, "pull-ota rejeitada: warmup (uptime=%llds, faltam %ds)",
                 (long long)uptime_s, remaining);
        char body[128];
        snprintf(body, sizeof(body),
                 "{\"error\":\"warmup\",\"uptime_s\":%lld,\"retry_in_s\":%d}",
                 (long long)uptime_s, remaining);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "60");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }

    /* Lê o corpo em loop (garante receber tudo, mesmo se vier em pedaços) */
    char body[512];
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, body + total, req->content_len - total);
        if (n <= 0) {
            ESP_LOGE(TAG_SRV, "pull-ota: recv falhou (n=%d)", n);
            return send_json_error(req, 500, "recv failed");
        }
        total += n;
    }
    if (total >= (int)sizeof(body)) {
        return send_json_error(req, 413, "body too large");
    }
    body[total] = '\0';

    /* Parse JSON */
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_json_error(req, 400, "invalid json");
    }
    cJSON *url_item = cJSON_GetObjectItem(root, "url");
    cJSON *sha_item = cJSON_GetObjectItem(root, "sha256");
    if (!cJSON_IsString(url_item) || !cJSON_IsString(sha_item)) {
        cJSON_Delete(root);
        return send_json_error(req, 400, "missing url or sha256");
    }

    /* Copia strings antes de liberar cJSON */
    char url[256];
    char sha[80];
    snprintf(url, sizeof(url), "%s", url_item->valuestring);
    snprintf(sha, sizeof(sha), "%s", sha_item->valuestring);
    cJSON_Delete(root);

    ESP_LOGI(TAG_SRV, "Pull OTA: url=%s", url);

    /* Verifica se já há OTA em andamento */
    if (g_ota_busy) {
        return send_json_error(req, 409, "ota already in progress");
    }

    /* Dispara */
    esp_err_t err = ota_https_update_start(url, sha, NULL, pull_ota_result_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SRV, "ota_https_update_start falhou: %s",
                 esp_err_to_name(err));
        return send_json_error(req, 500, "failed to start pull");
    }

    return send_json_ok(req, "{\"status\":\"started\"}");
}

static const httpd_uri_t uri_post_pull_ota = {
    .uri      = "/pull-ota",
    .method   = HTTP_POST,
    .handler  = pull_ota_post_handler,
    .user_ctx = NULL,
};

#endif /* CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE */

/* -----------------------------------------------------------------------------
 * API pública — start
 * -------------------------------------------------------------------------- */

esp_err_t ota_local_server_start(uint16_t port)
{
    s_upload_buf_mutex = xSemaphoreCreateMutexStatic(&s_upload_buf_mutex_buf);
    if (s_upload_buf_mutex == NULL) {
        ESP_LOGE(TAG_SRV, "Falha ao criar mutex de buffer de upload");
        return ESP_ERR_NO_MEM;
    }
    
    /* 1) Idempotência defensiva. */
    if (g_httpd != NULL) {
        ESP_LOGW(TAG_SRV, "Servidor já está rodando (porta %u)",
                 (unsigned)g_server_port);
        return ESP_ERR_INVALID_STATE;
    }

    if (port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    /* 2) Mutex de upload — criado antes do httpd para que o primeiro POST
     *    que chegar já o encontre pronto. */
    if (g_upload_mutex == NULL) {
        g_upload_mutex = xSemaphoreCreateMutex();
        if (g_upload_mutex == NULL) {
            ESP_LOGE(TAG_SRV, "Falha ao criar mutex de upload");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 3) Configuração TLS. */
    httpd_ssl_config_t ssl_cfg;
    err = build_ssl_config(&ssl_cfg, port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SRV, "build_ssl_config() falhou: %s",
                 esp_err_to_name(err));
        goto fail_mutex;
    }

    /* 4) Sobe o servidor. */
    httpd_handle_t server = NULL;
    err = httpd_ssl_start(&server, &ssl_cfg);
    if (err != ESP_OK || server == NULL) {
        ESP_LOGE(TAG_SRV, "httpd_ssl_start() falhou: %s",
                 esp_err_to_name(err));
        /* Se httpd_ssl_start retornou erro, 'server' pode estar indefinido;
         * garante NULL para o cleanup não tentar parar algo inválido. */
        server = NULL;
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto fail_mutex;
    }

    ESP_LOGI(TAG_SRV, "httpd iniciado (handle=%p)", (void *)server);

    /* 5) Registra /update. Em falha, desfaz o httpd. */
    err = register_handler(server, &uri_post_update);
    if (err != ESP_OK) {
        goto fail_httpd;
    }

    /* 6) Registra /status. Em falha, desfaz o httpd inteiro (não desfazemos
     *    apenas o handler de update porque httpd_ssl_stop cuida de tudo). */
    err = register_handler(server, &uri_get_status);
    if (err != ESP_OK) {
        goto fail_httpd;
    }

#if CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE
    err = register_handler(server, &uri_post_pull_ota);
    if (err != ESP_OK) {
        goto fail_httpd;
    }
#endif

    /* 7) Publica globais apenas após sucesso total. */
    g_httpd      = server;
    g_server_port = port;

    log_listen_address(port);
    
#if CONFIG_LOCKMAXXING_OTA_PULL_ENDPOINT_ENABLE
    ESP_LOGI(TAG_SRV, "  POST /pull-ota (JSON url+sha256)");
#endif

    return ESP_OK;

fail_httpd:
    /* Desfaz o httpd. O stop é bloqueante mas rápido — não há conexão
     * em andamento porque acabamos de subir. */
    httpd_ssl_stop(server);
    /* cai para fail_mutex */

fail_mutex:
    /* Destrói o mutex apenas se ele foi criado nesta chamada (se já existia,
     * significa que alguém antes criou — improvável, mas preserva estado). */
    if (g_upload_mutex != NULL) {
        vSemaphoreDelete(g_upload_mutex);
        g_upload_mutex = NULL;
    }
    return err;
}

/* -----------------------------------------------------------------------------
 * API pública — stop
 * -------------------------------------------------------------------------- */

esp_err_t ota_local_server_stop(void)
{
    if (g_httpd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG_SRV, "Parando servidor HTTPS...");

    /*
     * httpd_ssl_stop() é bloqueante: sinaliza as tasks do servidor e espera
     * que terminem. Se um POST /update estiver em andamento, ele termina
     * naturalmente (ou faz esp_restart, caso em que não voltamos daqui).
     */
    esp_err_t err = httpd_ssl_stop(g_httpd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SRV, "httpd_ssl_stop() falhou: %s", esp_err_to_name(err));
        /* Continua o cleanup mesmo assim — deixar g_httpd != NULL após
         * uma stop falha permitiria double-stop, o que é pior. */
    }

    /* Neste ponto não há mais handlers rodando. Destruir o mutex é seguro. */
    if (g_upload_mutex != NULL) {
        vSemaphoreDelete(g_upload_mutex);
        g_upload_mutex = NULL;
    }

    g_httpd       = NULL;
    g_server_port = 0;

    ESP_LOGI(TAG_SRV, "Servidor HTTPS parado");
    return err;
}

/* -----------------------------------------------------------------------------
 * API pública — is_running
 * -------------------------------------------------------------------------- */

bool ota_local_server_is_running(void)
{
    return (g_httpd != NULL);
}