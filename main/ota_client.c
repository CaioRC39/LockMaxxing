/* =============================================================================
 * main/ota_client.c — Cliente HTTPS para OTA (modelo "pull")
 * =============================================================================
 *
 * Este arquivo implementa o download de firmware via HTTPS usando
 * esp_http_client + esp_ota_write diretamente.
 *
 * Dependências em main/CMakeLists.txt (REQUIRES):
 *     esp_http_client, esp_crt_bundle, mbedtls, esp_timer, freertos
 *
 * Fluxo:
 *   1. Configura esp_http_client com CRT bundle (valida cadeia TLS)
 *   2. Abre GET, lê cabeçalhos, verifica status HTTP 200
 *   3. Para cada chunk de 4 KB:
 *        - grava via esp_ota_write() na partição OTA inativa
 *        - atualiza SHA-256 em streaming
 *        - chama progress_cb (throttled a cada 500 ms)
 *   4. Confirma que o download terminou (is_complete_data_received)
 *   5. Compara SHA-256 calculado com o esperado (se fornecido)
 *   6. Finaliza (esp_ota_end) e marca a nova partição como bootável
 *
 * O código está dividido em 3 partes neste documento:
 *   PARTE 1/3 — Includes, constantes, contexto, forward decls (esta)
 *   PARTE 2/3 — ota_client_perform_download() (o núcleo do download)
 *   PARTE 3/3 — ota_client_task() + ota_https_update_start() (público)
 * ========================================================================== */

#include "certs.h"
 #include "ota_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/task.h"

#include "sdkconfig.h"

/* -----------------------------------------------------------------------------
 * Constantes
 * -------------------------------------------------------------------------- */

/*
 * Stack da task de download. 8 KB é folgado: ~2 KB para o contexto interno
 * do HTTP client (alocado em heap, não na stack, mas há ponteiros), ~1 KB
 * para o estado do mbedtls (na stack), e o resto para variáveis locais.
 */
#define OTA_CLIENT_TASK_STACK           (8 * 1024)

/*
 * Prioridade igual à task de Wi-Fi/MQTT (5), para que o download não
 * fique esfomeado quando o broker estiver ativo. Mais alta que câmera (4)
 * e atuadores (2), porque OTA é uma operação rara e de curta duração.
 */
#define OTA_CLIENT_TASK_PRIO            5

/*
 * Buffer de leitura HTTP. 4 KB = tamanho típico de uma página de flash;
 * escrever nesse tamanho gera o mínimo de operações de erase/write.
 */
#define OTA_CLIENT_BUF_SIZE             4096

/* Timeout total da conexão HTTP (incluindo handshake TLS e cabeçalhos). */
#define OTA_CLIENT_HTTP_TIMEOUT_MS      (30 * 1000)

/*
 * Intervalo mínimo entre callbacks de progresso. 500 ms evita inundar o
 * consumidor (ex: um app exibindo barra de progresso) com centenas de
 * eventos por segundo.
 */
#define OTA_CLIENT_PROGRESS_INTERVAL_MS 500

/* Número máximo de tentativas (primeira + 1 retry). */
#define OTA_CLIENT_MAX_ATTEMPTS         2

/* Delay entre tentativas, permite que o AP Wi-Fi se recupere se cair. */
#define OTA_CLIENT_RETRY_DELAY_MS       2000

/* Tag centralizada para logs do módulo. */
static const char *TAG = "OTA_CLIENT";

#define OTA_CLIENT_URL_MAX      512

#define OTA_CLIENT_SHA256_MAX   (OTA_SHA256_HEX_LEN + 1)

/* -----------------------------------------------------------------------------
 * Contexto interno da task
 * -------------------------------------------------------------------------- */

/* Passado como argumento para a task. A task é DONA de todos os ponteiros. */
typedef struct {
    char              url[OTA_CLIENT_URL_MAX];
    char              sha256[OTA_CLIENT_SHA256_MAX];  /* "" = sem verificação */
    ota_progress_cb_t progress_cb;
    ota_result_cb_t   result_cb;
} ota_client_ctx_t;

/* Contexto estático. Protegido por s_ctx_lock, porque _start pode ser
 * chamada de qualquer task (main, uma task de controle, etc.). */
static ota_client_ctx_t s_ctx;
static bool             s_ctx_in_use = false;
static portMUX_TYPE     s_ctx_lock   = portMUX_INITIALIZER_UNLOCKED;

/* Buffer de download. Estático porque só uma OTA roda por vez (o estado
 * é serializado por ota_set_state e pelo próprio contexto estático). */
static uint8_t s_download_buf[OTA_CLIENT_BUF_SIZE];


/* =============================================================================
 * Conteúdo: ota_client_perform_download() — uma tentativa completa
 * ========================================================================== */

static esp_err_t ota_client_perform_download(const ota_client_ctx_t *ctx,
                                              char *out_message,
                                              size_t msg_size)
{   
    /* Pré-condições verificadas pelo chamador; aqui apenas sanity. */
    if (ctx == NULL || ctx->url[0] == '\0' || out_message == NULL || msg_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err           = ESP_OK;
    esp_http_client_handle_t http  = NULL;
    esp_ota_handle_t         ota   = 0;
    uint8_t                 *buf   = s_download_buf;
    psa_hash_operation_t     sha_ctx = psa_hash_operation_init();
    bool                     sha_initialized = false;

    size_t   total_written     = 0;
    int64_t  content_length    = -1;
    int64_t  last_progress_us  = 0;

    char sha_final_hex[OTA_SHA256_HEX_LEN + 1];
    memset(sha_final_hex, 0, sizeof(sha_final_hex));

    /* Escreve uma mensagem final e desvia para o cleanup. Macro local
     * para evitar repetição em cada caminho de erro. */
#define OTA_FAIL(err_code, fmt, ...)                                           \
    do {                                                                       \
        err = (err_code);                                                      \
        snprintf(out_message, msg_size, fmt, ##__VA_ARGS__);                   \
        ESP_LOGE(TAG, "%s", out_message);                                      \
        goto cleanup;                                                          \
    } while (0)

    /* -------------------------------------------------------------------------
     * 1) Configuração do cliente HTTP
     * ---------------------------------------------------------------------- */

    esp_http_client_config_t cfg = {
        .url               = ctx->url,
        .method            = HTTP_METHOD_GET,
#if CONFIG_LOCKMAXXING_OTA_PULL_USE_LOCAL_CERT
        /* Modo dev/test: verifica contra o próprio cert do serve (certs.h),
         * em vez de CA pública. Não é "skip" — é "pin" para o cert local.
         * Funciona com auto-assinado e previne MITM (atacante precisaria
         * da chave privada do certs.h). */
        .crt_bundle_attach            = NULL,
        .cert_pem                     = SERVER_CERT_PEM,
        .common_name                 = "esp32-camera.local",
        .skip_cert_common_name_check  = false,
#else
        /* Produção: verifica contra CA raiz pública (Let's Encrypt, etc.). */
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
        .timeout_ms        = OTA_CLIENT_HTTP_TIMEOUT_MS,
        .buffer_size       = OTA_CLIENT_BUF_SIZE,
        .buffer_size_tx    = 512,
        .keep_alive_enable = true,
        .disable_auto_redirect   = false,
        .max_redirection_count   = 5,
        .user_agent        = "esp32-camera-mqtt/1.0",
    };

    http = esp_http_client_init(&cfg);
    if (http == NULL) {
        OTA_FAIL(ESP_FAIL, "esp_http_client_init() falhou");
    }

    /* -------------------------------------------------------------------------
     * 2) Abre conexão (handshake TLS + envia request)
     * ---------------------------------------------------------------------- */

    err = esp_http_client_open(http, 0 /* content_length do request: 0 */);
    if (err != ESP_OK) {
        OTA_FAIL(err, "Falha ao abrir conexão HTTPS: %s", esp_err_to_name(err));
    }

    /* -------------------------------------------------------------------------
     * 3) Lê cabeçalhos de resposta e valida status
     * ---------------------------------------------------------------------- */

    /*
     * fetch_headers retorna o Content-Length, ou -1 se for chunked/unknown.
     * O status é obtido separadamente com get_status_code.
     */
    content_length = esp_http_client_fetch_headers(http);
    if (content_length < 0) {
        ESP_LOGW(TAG, "Content-Length desconhecido (chunked?) — "
                      "progresso será reportado em bytes, sem percentual");
    } else {
        ESP_LOGI(TAG, "Content-Length: %" PRId64 " bytes", content_length);
    }

    int status = esp_http_client_get_status_code(http);
    if (status != 200) {
        OTA_FAIL(ESP_ERR_INVALID_RESPONSE,
                 "HTTP status %d (esperado 200)", status);
    }

    /* -------------------------------------------------------------------------
     * 4) Identifica a partição alvo e valida tamanho antecipadamente
     * ---------------------------------------------------------------------- */

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        OTA_FAIL(ESP_ERR_NOT_FOUND, "Sem partição OTA disponível");
    }

    ESP_LOGI(TAG, "Gravando em '%s' (offset=0x%08lx, size=%lu)",
             update_part->label,
             (unsigned long)update_part->address,
             (unsigned long)update_part->size);

    if (content_length > 0 && (size_t)content_length > update_part->size) {
        OTA_FAIL(ESP_ERR_INVALID_SIZE,
                 "Imagem (%" PRId64 " B) maior que partição (%lu B)",
                 content_length, (unsigned long)update_part->size);
    }

    /* -------------------------------------------------------------------------
     * 5) Inicia escrita na partição
     * ---------------------------------------------------------------------- */

    /*
     * Se conhecemos o tamanho, passamos como hint para que o IDF pré-apague
     * apenas os setores necessários. Caso contrário, OTA_SIZE_UNKNOWN faz
     * o erase sob demanda.
     */
    size_t hint_size = (content_length > 0) ? (size_t)content_length : OTA_SIZE_UNKNOWN;

    err = esp_ota_begin(update_part, hint_size, &ota);
    if (err != ESP_OK) {
        OTA_FAIL(err, "esp_ota_begin() falhou: %s", esp_err_to_name(err));
    }

    /* -------------------------------------------------------------------------
     * 6) Aloca buffer e inicializa SHA-256
     * ---------------------------------------------------------------------- */

    err = ota_sha256_stream_init(&sha_ctx);
    if (err != ESP_OK) {
        /* ota_sha256_stream_init já chama mbedtls_sha256_free() em falha;
         * não precisamos limpar aqui. sha_initialized continua false. */
        OTA_FAIL(err, "Falha ao inicializar SHA-256");
    }
    sha_initialized = true;

    /* -------------------------------------------------------------------------
     * 7) Loop de leitura/escrita/hash
     * ---------------------------------------------------------------------- */

    last_progress_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Iniciando download de %s", ctx->url);

    while (true) {
        int n = esp_http_client_read(http, (char *)buf, OTA_CLIENT_BUF_SIZE);

        if (n < 0) {
            OTA_FAIL(ESP_FAIL, "esp_http_client_read() falhou (n=%d)", n);
        }
        if (n == 0) {
            /* 0 significa "fim dos dados" SE a transmissão foi completa. */
            if (esp_http_client_is_complete_data_received(http)) {
                break;
            }
            OTA_FAIL(ESP_ERR_INVALID_SIZE,
                     "Conexão encerrada antes do fim (recebido %zu B)",
                     total_written);
        }

        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            OTA_FAIL(err, "esp_ota_write() falhou: %s", esp_err_to_name(err));
        }

        err = ota_sha256_stream_update(&sha_ctx, buf, (size_t)n);
        if (err != ESP_OK) {
            OTA_FAIL(err, "Falha no SHA-256 update");
        }

        total_written += (size_t)n;

        /* Verificação antecipada contra overflow de partição (chunked). */
        if (total_written > update_part->size) {
            OTA_FAIL(ESP_ERR_INVALID_SIZE,
                     "Download (%zu B) excedeu partição (%lu B)",
                     total_written, (unsigned long)update_part->size);
        }

        /* Callback de progresso com throttle (evita spam). */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_progress_us) >=
            (OTA_CLIENT_PROGRESS_INTERVAL_MS * 1000LL)) {
            if (ctx->progress_cb != NULL) {
                ota_progress_t p;
                ota_fill_progress(&p, total_written,
                                  content_length > 0 ? (size_t)content_length : 0);
                ctx->progress_cb(&p);
            }
            last_progress_us = now_us;
        }
    }

    ESP_LOGI(TAG, "Download concluído: %zu bytes", total_written);

    /* -------------------------------------------------------------------------
     * 8) Progresso final 100%
     * ---------------------------------------------------------------------- */

    if (ctx->progress_cb != NULL) {
        ota_progress_t p;
        ota_fill_progress(&p, total_written, total_written);
        ctx->progress_cb(&p);
    }

    /* -------------------------------------------------------------------------
     * 9) Finaliza SHA-256 (libera o contexto internamente)
     * ---------------------------------------------------------------------- */

    err = ota_sha256_stream_finish(&sha_ctx, sha_final_hex);
    sha_initialized = false;   /* finish() libera o contexto, mesmo em falha */
    if (err != ESP_OK) {
        OTA_FAIL(err, "Falha ao finalizar SHA-256");
    }

    ESP_LOGI(TAG, "SHA-256 calculado: %s", sha_final_hex);

    /* -------------------------------------------------------------------------
     * 10) Verificação de integridade
     * ---------------------------------------------------------------------- */

    if (ctx->sha256[0] != '\0') {
        if (!ota_sha256_hex_equals(sha_final_hex, ctx->sha256)) {
            OTA_FAIL(ESP_ERR_INVALID_CRC,
                     "SHA-256 divergente: esperado %s",
                     ctx->sha256);
        }
        ESP_LOGI(TAG, "SHA-256 confere com o esperado");
    } else {
        ESP_LOGW(TAG, "SHA-256 NÃO verificado (nenhum esperado fornecido)");
        ESP_LOGW(TAG, "Guardando como referência: %s", sha_final_hex);
    }

    /* -------------------------------------------------------------------------
     * 11) Valida estrutura da imagem e encerra escrita
     * ---------------------------------------------------------------------- */

    err = esp_ota_end(ota);
    ota = 0;   /* end() libera o handle mesmo em falha; não chamar abort depois */
    if (err != ESP_OK) {
        OTA_FAIL(err,
                 "esp_ota_end() rejeitou a imagem: %s "
                 "(verifique se o arquivo é um .bin do ESP-IDF)",
                 esp_err_to_name(err));
    }

    /* -------------------------------------------------------------------------
     * 12) Marca a nova partição como alvo do próximo boot
     * ---------------------------------------------------------------------- */

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        OTA_FAIL(err, "esp_ota_set_boot_partition() falhou: %s",
                 esp_err_to_name(err));
    }

    snprintf(out_message, msg_size,
             "OTA aplicada: %zu B em '%s'",
             total_written, update_part->label);
    ESP_LOGI(TAG, "%s — reinicie para bootar o novo firmware", out_message);

cleanup:
    /* Libera o contexto SHA-256 se ele foi inicializado mas não finalizado. */
    if (sha_initialized) {
        (void)psa_hash_abort(&sha_ctx);
    }
    if (ota != 0) {
        /* Chegamos aqui sem passar por esp_ota_end → aborta e descarta. */
        esp_ota_abort(ota);
    }
    if (http != NULL) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
    }
    
    return err;

#undef OTA_FAIL
}


/* =============================================================================
 * Conteúdo: ota_client_task() e ota_https_update_start()
 * ========================================================================== */

static void ota_client_task(void *arg)
{
    ota_client_ctx_t *ctx = (ota_client_ctx_t *)arg;
    /* ctx foi alocado por ota_https_update_start; nós liberamos tudo. */

    char message[OTA_RESULT_MSG_MAX];
    memset(message, 0, sizeof(message));

    esp_err_t final_err = ESP_FAIL;
    int attempt = 0;

    ota_set_state(OTA_STATE_DOWNLOADING);

    while (attempt < OTA_CLIENT_MAX_ATTEMPTS) {
        attempt++;
        ESP_LOGI(TAG, "Tentativa %d/%d", attempt, OTA_CLIENT_MAX_ATTEMPTS);

        final_err = ota_client_perform_download(ctx,
                                                message,
                                                sizeof(message));

        if (final_err == ESP_OK) {
            break;
        }

        ESP_LOGW(TAG, "Tentativa %d falhou: %s",
                 attempt, esp_err_to_name(final_err));

        /*
         * Erros de integridade NÃO se beneficiam de retry — o arquivo no
         * servidor está corrompido, baixar de novo vai dar o mesmo hash.
         */
        if (final_err == ESP_ERR_INVALID_CRC) {
            ESP_LOGE(TAG, "SHA-256 divergente: não vale a pena retentar");
            break;
        }

        if (attempt < OTA_CLIENT_MAX_ATTEMPTS) {
            ESP_LOGI(TAG, "Aguardando %d ms antes de retentar",
                     OTA_CLIENT_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(OTA_CLIENT_RETRY_DELAY_MS));
            /* Zera mensagem para o próximo try não herdar texto antigo. */
            memset(message, 0, sizeof(message));
        }
    }

    if (final_err == ESP_OK) {
        ota_set_state(OTA_STATE_SUCCESS);
    } else {
        ota_set_state(OTA_STATE_ERROR);
        if (message[0] == '\0') {
            /* Garante mensagem não vazia mesmo se algo escapou. */
            snprintf(message, sizeof(message),
                     "OTA falhou após %d tentativas: %s",
                     attempt, esp_err_to_name(final_err));
        }
    }

    ESP_LOGI(TAG, "Resultado final: %s — %s",
             esp_err_to_name(final_err), message);

    /* Avisa o chamador. Callback roda nesta task, com prioridade 5. */
    if (ctx->result_cb != NULL) {
        ctx->result_cb(final_err, message);
    }

    /* Libera o "lock" do contexto estático para permitir nova OTA. */
    portENTER_CRITICAL(&s_ctx_lock);
    s_ctx_in_use = false;
    portEXIT_CRITICAL(&s_ctx_lock);

    g_ota_busy = false;

    /* A task se auto-deleta. Nada mais a fazer. */
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------------
 * API pública
 * -------------------------------------------------------------------------- */

esp_err_t ota_https_update_start(const char *url,
                                 const char *expected_sha256_hex,
                                 ota_progress_cb_t progress_cb,
                                 ota_result_cb_t result_cb)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Valida formato do hash antes de aceitar (evita trabalho inútil). */
    if (expected_sha256_hex != NULL) {
        size_t hexlen = strlen(expected_sha256_hex);
        if (hexlen != OTA_SHA256_HEX_LEN) {
            ESP_LOGE(TAG, "SHA-256 esperado tem %zu chars (esperado %d)",
                     hexlen, OTA_SHA256_HEX_LEN);
            return ESP_ERR_INVALID_ARG;
        }
    }

    /* Rejeita se já há uma operação em andamento. */
    ota_state_t current = ota_get_state();
    if (current != OTA_STATE_IDLE &&
        current != OTA_STATE_SUCCESS &&
        current != OTA_STATE_ERROR) {
        ESP_LOGW(TAG, "OTA já em andamento (estado=%d)", (int)current);
        return ESP_ERR_INVALID_STATE;
    }

    /* Reserva o contexto estático. Se já está em uso, rejeita. */
    portENTER_CRITICAL(&s_ctx_lock);
    if (s_ctx_in_use) {
        portEXIT_CRITICAL(&s_ctx_lock);
        ESP_LOGW(TAG, "Outra OTA já reservou o contexto");
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx_in_use = true;
    portEXIT_CRITICAL(&s_ctx_lock);

    /* Copia a URL — falha se for mais longa que o buffer. */
    size_t url_len = strlen(url);
    if (url_len == 0 || url_len >= sizeof(s_ctx.url)) {
        ESP_LOGE(TAG, "URL muito longa (%zu chars, max %zu)",
                 url_len, sizeof(s_ctx.url) - 1);
        portENTER_CRITICAL(&s_ctx_lock);
        s_ctx_in_use = false;
        portEXIT_CRITICAL(&s_ctx_lock);
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_ctx.url, url, url_len + 1);

    /* Copia o SHA esperado, ou marca "sem verificação". */
    if (expected_sha256_hex != NULL) {
        size_t hexlen = strlen(expected_sha256_hex);
        if (hexlen != OTA_SHA256_HEX_LEN) {
            ESP_LOGE(TAG, "SHA-256 esperado tem %zu chars (esperado %d)",
                     hexlen, OTA_SHA256_HEX_LEN);
            portENTER_CRITICAL(&s_ctx_lock);
            s_ctx_in_use = false;
            portEXIT_CRITICAL(&s_ctx_lock);
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(s_ctx.sha256, expected_sha256_hex, hexlen + 1);
    } else {
        s_ctx.sha256[0] = '\0';
    }

    s_ctx.progress_cb = progress_cb;
    s_ctx.result_cb   = result_cb;

    ota_set_state(OTA_STATE_DOWNLOADING);
    g_ota_busy = true;

    /* Cria a task. Stack em PSRAM? Não — stack de task precisa ser RAM
     * interna. Por isso OTA_CLIENT_TASK_STACK é 8 KB em RAM interna,
     * alocado pelo próprio FreeRTOS. */
    BaseType_t ok = xTaskCreate(
        ota_client_task,
        "ota_client",
        OTA_CLIENT_TASK_STACK / sizeof(StackType_t),
        &s_ctx,
        OTA_CLIENT_TASK_PRIO,
        NULL);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate() falhou (memória insuficiente?)");
        portENTER_CRITICAL(&s_ctx_lock);
        s_ctx_in_use = false;
        portEXIT_CRITICAL(&s_ctx_lock);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Task de download iniciada — URL=%s", url);
    return ESP_OK;
}