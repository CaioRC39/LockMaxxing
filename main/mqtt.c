/* =============================================================================
 * main/mqtt.c — Cliente MQTT (publish de imagem + subscribe de resultado)
 * =============================================================================
 *
 * Encapsula o esp-mqtt. O resto do firmware NUNCA chama esp_mqtt_client_*.
 *
 * Tópicos (config.h):
 *   CONFIG_MQTT_TOPIC_REQUEST  → JSON {"id":N,"size":S,"chunks":C}
 *   CONFIG_MQTT_TOPIC_IMAGE    → binário: [id 4B][idx 2B][total 2B][jpeg]
 *   CONFIG_MQTT_TOPIC_RESULT   → JSON do servidor
 *
 * Compatível com ESP-IDF 6.1.0 (esp-mqtt 5.0+ API aninhada).
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>       /* strcasecmp */
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "mqtt_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "cJSON.h"

#include "config.h"
#include "mqtt.h"

static const char *TAG = CONFIG_LOG_TAG_MQTT;

/* -----------------------------------------------------------------------------
 * Estado interno
 * -------------------------------------------------------------------------- */

static bool                     s_initialized = false;
static esp_mqtt_client_handle_t s_client      = NULL;
static EventGroupHandle_t       s_event_group = NULL;
static mqtt_result_cb_t         s_result_cb   = NULL;

/* Contadores protegidos por spinlock (handler do esp-mqtt e tasks do
 * firmware podem consultar ao mesmo tempo). */
static portMUX_TYPE s_lock       = portMUX_INITIALIZER_UNLOCKED;

/* Buffer estático para o chunk MQTT. Válido porque publish_chunk() só é
 * chamado sequencialmente pela comm_task — nunca concorrente. Não é
 * thread-safe; se um dia houver múltiplos publishers, proteja com mutex. */
static uint8_t s_chunk_buf[CONFIG_MQTT_CHUNK_HEADER_SIZE + CONFIG_MQTT_CHUNK_SIZE];

static uint32_t     s_published  = 0;
static uint32_t     s_received   = 0;
static uint32_t     s_dropped    = 0;

// Buffer para o mutex (evita malloc interno do FreeRTOS)
static StaticSemaphore_t s_chunk_mutex_buf;
static SemaphoreHandle_t s_chunk_mutex = NULL;

/* -----------------------------------------------------------------------------
 * Helpers — big-endian packing (protocolo binário)
 * -------------------------------------------------------------------------- */

static inline void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)((v >> 24) & 0xFF);
    p[1] = (uint8_t)((v >> 16) & 0xFF);
    p[2] = (uint8_t)((v >>  8) & 0xFF);
    p[3] = (uint8_t)( v        & 0xFF);
}

static inline void put_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)((v >> 8) & 0xFF);
    p[1] = (uint8_t)( v       & 0xFF);
}

/* -----------------------------------------------------------------------------
 * Parser de resultado (cJSON)
 * --------------------------------------------------------------------------
 *
 * Formato esperado:
 *   {"id": <uint32 ou "uint32">, "resultado": "aprovado"|"reprovado"}
 *
 * Aceita campos extras, qualquer ordem, "id" como número ou string
 * numérica. Rejeita JSON truncado, tipo errado ou valor desconhecido.
 *
 * cJSON aloca uma árvore temporária (~200-500 B para esta mensagem),
 * liberada com cJSON_Delete no final. Alocação transacional, sem
 * fragmentação persistente.
 *
 * Chamado na task interna do esp-mqtt. NÃO BLOQUEAR aqui.
 * -------------------------------------------------------------------------- */

/* Converte cJSON string numérica para uint32, com validação estrita. */
static bool json_id_from_string(const char *s, uint32_t *out)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *endp = NULL;
    unsigned long v = strtoul(s, &endp, 10);
    if (endp == s || *endp != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static void handle_result_message(const char *data, int len)
{
    if (data == NULL || len <= 0) {
        ESP_LOGW(TAG, "Resultado vazio — descartando");
        portENTER_CRITICAL(&s_lock); s_dropped++; portEXIT_CRITICAL(&s_lock);
        return;
    }

    /* ParseWithLength aceita buffer sem null-terminator (é o que
     * event->data entrega). cJSON_Parse exigiria '\0' e leria lixo. */
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "JSON inválido no resultado (len=%d)", len);
        portENTER_CRITICAL(&s_lock); s_dropped++; portEXIT_CRITICAL(&s_lock);
        return;
    }

    /* Rejeita qualquer coisa que não seja um objeto no topo. */
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "Resultado não é um objeto JSON");
        goto drop;
    }

    /* ----- Campo "id" ----- */
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    uint32_t image_id = 0;
    bool id_ok = false;

    if (id_item == NULL) {
        ESP_LOGW(TAG, "Campo 'id' ausente");
    } else if (cJSON_IsNumber(id_item)) {
        double v = id_item->valuedouble;
        if (v < 0.0 || v > (double)UINT32_MAX) {
            ESP_LOGW(TAG, "Campo 'id' fora do range uint32: %f", v);
        } else {
            image_id = (uint32_t)v;
            id_ok = true;
        }
    } else if (cJSON_IsString(id_item)) {
        if (json_id_from_string(id_item->valuestring, &image_id)) {
            id_ok = true;
        } else {
            ESP_LOGW(TAG, "Campo 'id' string não numérico: '%s'",
                     id_item->valuestring ? id_item->valuestring : "");
        }
    } else {
        ESP_LOGW(TAG, "Campo 'id' com tipo inesperado");
    }

    if (!id_ok) {
        goto drop;
    }

    /* ----- Campo "resultado" ----- */
    cJSON *res_item = cJSON_GetObjectItemCaseSensitive(root, "resultado");
    if (res_item == NULL || !cJSON_IsString(res_item) ||
        res_item->valuestring == NULL) {
        ESP_LOGW(TAG, "Campo 'resultado' ausente ou não é string");
        goto drop;
    }

    mqtt_result_t result;
    if (strcasecmp(res_item->valuestring, "aprovado") == 0) {
        result = MQTT_RESULT_APPROVED;
    } else if (strcasecmp(res_item->valuestring, "reprovado") == 0) {
        result = MQTT_RESULT_REJECTED;
    } else {
        ESP_LOGW(TAG, "Valor 'resultado' desconhecido: '%s'",
                 res_item->valuestring);
        goto drop;
    }

    ESP_LOGI(TAG, "Resultado recebido: id=%u, %s",
             (unsigned)image_id,
             result == MQTT_RESULT_APPROVED ? "APROVADO" : "REPROVADO");

    portENTER_CRITICAL(&s_lock); s_received++; portEXIT_CRITICAL(&s_lock);

    /* Callback roda AQUI, na task do esp-mqtt. Quem registrou deve
     * apenas enfileirar o resultado para a actuator_task processar. */
    if (s_result_cb != NULL) {
        s_result_cb(image_id, result);
    }

    cJSON_Delete(root);
    return;

drop:
    portENTER_CRITICAL(&s_lock); s_dropped++; portEXIT_CRITICAL(&s_lock);
    cJSON_Delete(root);
}

/* -----------------------------------------------------------------------------
 * Event handler do esp-mqtt
 * -------------------------------------------------------------------------- */

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "Conectado ao broker");
        xEventGroupSetBits(s_event_group, MQTT_CONNECTED_BIT);
        xEventGroupClearBits(s_event_group, MQTT_DISCONNECTED_BIT);

        /* Re-assina a cada conexão (clean_session=false ainda exige
         * re-subscribe após reconnect no esp-mqtt). */
        int sub_id = esp_mqtt_client_subscribe(s_client,
                                               CONFIG_MQTT_TOPIC_RESULT,
                                               CONFIG_MQTT_QOS);
        if (sub_id < 0) {
            ESP_LOGE(TAG, "Falha ao assinar '%s'", CONFIG_MQTT_TOPIC_RESULT);
        } else {
            ESP_LOGI(TAG, "Assinado '%s' (msg_id=%d)",
                     CONFIG_MQTT_TOPIC_RESULT, sub_id);
        }
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Desconectado do broker");
        xEventGroupClearBits(s_event_group, MQTT_CONNECTED_BIT);
        xEventGroupSetBits(s_event_group, MQTT_DISCONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA: {
        /* Só tratamos mensagens do tópico de resultado. */
        size_t expected_len = strlen(CONFIG_MQTT_TOPIC_RESULT);
        if (event->topic != NULL && (size_t)event->topic_len == expected_len &&
            strncmp(event->topic, CONFIG_MQTT_TOPIC_RESULT, expected_len) == 0) {

            /* Mensagens fragmentadas (total_data_len != data_len) não
             * deveriam ocorrer em JSONs pequenos. Se ocorrerem, o JSON
             * não vai parsear — o que é o comportamento correto (drop). */
            if (event->total_data_len != event->data_len) {
                ESP_LOGW(TAG, "Resultado fragmentado (data_len=%d, total=%d) "
                              "— descartando",
                         event->data_len, event->total_data_len);
                portENTER_CRITICAL(&s_lock); s_dropped++;
                portEXIT_CRITICAL(&s_lock);
            } else {
                handle_result_message(event->data, event->data_len);
            }
        } else {
            ESP_LOGD(TAG, "Mensagem em tópico não tratado: %.*s",
                     event->topic_len, event->topic ? event->topic : "");
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Erro MQTT (tipo=%d)", (int)event->error_handle->error_type);
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGE(TAG, "  errno=%d",
                     event->error_handle->esp_transport_sock_errno);
            ESP_LOGE(TAG, "  esp_tls_last_esp_err=0x%x",
                     event->error_handle->esp_tls_last_esp_err);
        }
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribe confirmado (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "Unsubscribe confirmado (msg_id=%d)", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        /* Incrementa a cada mensagem publicada. Não loga (spam — um
         * JPEG grande gera 20+ chunks). */
        portENTER_CRITICAL(&s_lock); s_published++; portEXIT_CRITICAL(&s_lock);
        break;

    case MQTT_EVENT_BEFORE_CONNECT:
        ESP_LOGD(TAG, "Pré-conexão (DNS/TCP in progress)");
        break;

    default:
        ESP_LOGD(TAG, "Evento MQTT não tratado: %d", (int)event_id);
        break;
    }
}

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

esp_err_t mqtt_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "mqtt_init() chamada novamente — ignorando");
        return ESP_OK;
    }

    /* Validação mínima da URI: esquema + host */
    if (CONFIG_MQTT_BROKER_URI[0] == '\0') {
        ESP_LOGE(TAG, "CONFIG_MQTT_BROKER_URI vazia em config.h");
        return ESP_ERR_INVALID_ARG;
    }

    /* ------------------------------------------------------------------
     * 1) Mutex do buffer de chunks — criado PRIMEIRO, antes de qualquer
     *    outro recurso. Se falhar aqui, não há nada para limpar: só
     *    retornar erro.
     *
     *    Por que primeiro: evita que tenhamos que desfazer event_group
     *    e client em caso de falha do mutex. Quanto mais cedo criamos
     *    recursos, mais cedo podemos falhar sem custo.
     * ------------------------------------------------------------------ */
    s_chunk_mutex = xSemaphoreCreateMutexStatic(&s_chunk_mutex_buf);
    if (s_chunk_mutex == NULL) {
        ESP_LOGE(TAG, "Falha ao criar mutex do buffer de chunk");
        return ESP_ERR_NO_MEM;
    }

    /* ------------------------------------------------------------------
     * 2) Event group
     * ------------------------------------------------------------------ */
    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        ESP_LOGE(TAG, "Falha ao criar event group");
        /* Mutex já foi criado — precisa ser limpo antes de retornar. */
        vSemaphoreDelete(s_chunk_mutex);
        s_chunk_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* ------------------------------------------------------------------
     * 3) Cliente MQTT
     * ------------------------------------------------------------------ */
    esp_mqtt_client_config_t cfg = {
        .broker.address.uri                  = CONFIG_MQTT_BROKER_URI,
        .credentials.client_id               = CONFIG_MQTT_CLIENT_ID,
        .credentials.username                = CONFIG_MQTT_USERNAME,
        .credentials.authentication.password = CONFIG_MQTT_PASSWORD,
        .session.keepalive                   = CONFIG_MQTT_KEEPALIVE_S,
        .session.disable_clean_session       = false,
        .network.reconnect_timeout_ms        = CONFIG_MQTT_RECONNECT_MS,
        .network.timeout_ms                  = CONFIG_MQTT_CONNECT_TIMEOUT_MS,
        .network.disable_auto_reconnect      = false,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init() retornou NULL (URI inválida?)");
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
        vSemaphoreDelete(s_chunk_mutex);
        s_chunk_mutex = NULL;
        return ESP_FAIL;
    }

    /* ------------------------------------------------------------------
     * 4) Handlers e start — erros aqui caem no label 'fail'
     * ------------------------------------------------------------------ */
    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                    mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar handler: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start() falhou: %s",
                 esp_err_to_name(err));
        goto fail;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Cliente MQTT iniciado: broker='%s', client_id='%s'",
             CONFIG_MQTT_BROKER_URI, CONFIG_MQTT_CLIENT_ID);
    return ESP_OK;

fail:
    /* Libera tudo que foi criado até este ponto, na ordem inversa. */
    if (s_client != NULL) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    if (s_chunk_mutex != NULL) {
        vSemaphoreDelete(s_chunk_mutex);
        s_chunk_mutex = NULL;
    }
    return err;
}

esp_err_t mqtt_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_event_group != NULL) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }

    s_result_cb   = NULL;
    s_initialized = false;
    ESP_LOGI(TAG, "Cliente MQTT finalizado");
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Estado e espera
 * -------------------------------------------------------------------------- */

esp_err_t mqtt_wait_connected(uint32_t timeout_ms)
{
    if (!s_initialized || s_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (timeout_ms == 0) {
        EventBits_t bits = xEventGroupGetBits(s_event_group);
        return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
    }

    TickType_t ticks = (timeout_ms == portMAX_DELAY)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_event_group, MQTT_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, ticks);
    return (bits & MQTT_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool mqtt_is_connected(void)
{
    if (!s_initialized || s_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_event_group);
    return (bits & MQTT_CONNECTED_BIT) != 0;
}

EventGroupHandle_t mqtt_get_event_group(void)
{
    return s_event_group;
}

/* -----------------------------------------------------------------------------
 * Callback de resultado
 * -------------------------------------------------------------------------- */

esp_err_t mqtt_set_result_callback(mqtt_result_cb_t cb)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_result_cb = cb;
    ESP_LOGI(TAG, "Callback de resultado %s",
             cb != NULL ? "registrado" : "removido");
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Publicação
 * -------------------------------------------------------------------------- */

esp_err_t mqtt_publish_request(uint32_t image_id,
                               uint32_t total_size,
                               uint16_t total_chunks)
{
    if (!s_initialized || !mqtt_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[128];
    int n = snprintf(payload, sizeof(payload),
                     "{\"id\":%u,\"size\":%u,\"chunks\":%u}",
                     (unsigned)image_id,
                     (unsigned)total_size,
                     (unsigned)total_chunks);
    if (n < 0 || (size_t)n >= sizeof(payload)) {
        ESP_LOGE(TAG, "snprintf truncou payload de request");
        return ESP_FAIL;
    }

    int msg_id = esp_mqtt_client_publish(s_client,
                                         CONFIG_MQTT_TOPIC_REQUEST,
                                         payload, n,
                                         CONFIG_MQTT_QOS,
                                         0 /* retain=false */);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Falha ao publicar request (id=%u)", (unsigned)image_id);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Request publicado: id=%u, size=%u, chunks=%u (msg_id=%d)",
             (unsigned)image_id, (unsigned)total_size,
             (unsigned)total_chunks, msg_id);
    return ESP_OK;
}

esp_err_t mqtt_publish_chunk(uint32_t image_id,
                             uint16_t chunk_index,
                             uint16_t total_chunks,
                             const uint8_t *data, size_t len)
{
    /* 1) VALIDAÇÕES — sem lock, todos os returns antecipados aqui */
    if (!s_initialized || !mqtt_is_connected()) return ESP_ERR_INVALID_STATE;
    if (s_chunk_mutex == NULL)                  return ESP_ERR_INVALID_STATE;
    if (data == NULL || len == 0 || len > CONFIG_MQTT_CHUNK_SIZE)
                                                return ESP_ERR_INVALID_ARG;
    if (chunk_index >= total_chunks)            return ESP_ERR_INVALID_ARG;

    size_t buf_len = CONFIG_MQTT_CHUNK_HEADER_SIZE + len;

    /* 2) ACQUIRE — só agora, depois de tudo validado */
    if (xSemaphoreTake(s_chunk_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout ao adquirir mutex do buffer de chunk");
        return ESP_FAIL;
    }

    /* 3) SEÇÃO CRÍTICA — só o uso do s_chunk_buf */
    put_u32_be(s_chunk_buf + 0, image_id);
    put_u16_be(s_chunk_buf + 4, chunk_index);
    put_u16_be(s_chunk_buf + 6, total_chunks);
    memcpy(s_chunk_buf + CONFIG_MQTT_CHUNK_HEADER_SIZE, data, len);

    int msg_id = esp_mqtt_client_publish(s_client,
                                         CONFIG_MQTT_TOPIC_IMAGE,
                                         (const char *)s_chunk_buf,
                                         (int)buf_len,
                                         CONFIG_MQTT_QOS, 0);

    /* 4) RELEASE — imediatamente após o último uso do buffer.
     *    esp_mqtt_client_publish() já copiou os dados, então não
     *    precisamos mais do s_chunk_buf. */
    xSemaphoreGive(s_chunk_mutex);

    /* 5) PÓS-PROCESSAMENTO — sem lock */
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Falha ao publicar chunk %u/%u (id=%u)",
                 (unsigned)(chunk_index + 1), (unsigned)total_chunks,
                 (unsigned)image_id);
        return ESP_FAIL;
    }

    if (chunk_index % 5 == 0 || chunk_index == (uint16_t)(total_chunks - 1)) {
        ESP_LOGI(TAG, "Chunk %u/%u enviado (id=%u, %u bytes)",
                 (unsigned)(chunk_index + 1), (unsigned)total_chunks,
                 (unsigned)image_id, (unsigned)len);
    }

    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

void mqtt_get_stats(uint32_t *published_out,
                    uint32_t *received_out,
                    uint32_t *dropped_out)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t p = s_published;
    uint32_t r = s_received;
    uint32_t d = s_dropped;
    portEXIT_CRITICAL(&s_lock);

    if (published_out != NULL) *published_out = p;
    if (received_out  != NULL) *received_out  = r;
    if (dropped_out   != NULL) *dropped_out   = d;
}