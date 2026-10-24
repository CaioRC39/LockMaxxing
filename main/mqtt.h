/* =============================================================================
 * main/mqtt.h — Cliente MQTT (publish de imagem + subscribe de resultado)
 * =============================================================================
 *
 * Este módulo encapsula esp-mqtt. O resto do firmware NUNCA chama
 * esp_mqtt_client_* — apenas as funções abaixo.
 *
 * Tópicos (definidos em config.h):
 *   CONFIG_MQTT_TOPIC_REQUEST  → JSON com metadados da captura
 *   CONFIG_MQTT_TOPIC_IMAGE    → binário: [id 4B][idx 2B][total 2B][jpeg]
 *   CONFIG_MQTT_TOPIC_RESULT   → JSON do servidor: {"id":N,"resultado":"..."}
 *
 * PROTOCOLO DE RESULTADO (JSON esperado no tópico RESULT):
 *   {"id": 12345, "resultado": "aprovado"}
 *   {"id": 12345, "resultado": "reprovado"}
 *
 *   - "id" pode ser número ou string (aceitamos os dois por robustez)
 *   - "resultado" aceita apenas "aprovado" / "reprovado" (case-insensitive)
 *   - Mensagens inválidas são descartadas com log de warning; NÃO chamam
 *     o callback.
 *
 * THREADING — IMPORTANTE:
 *   O callback de resultado é chamado na TASK INTERNA DO ESP-MQTT.
 *   Ele NÃO deve bloquear. Faça apenas enfileiramento (xQueueSend) e
 *   retorne imediatamente. Se você chamar algo como actuators_apply_result()
 *   direto do callback, vai travar a recepção MQTT por vários segundos.
 *
 *   A arquitetura correta é:
 *     mqtt callback → xQueueSend(actuator_cmd_queue, ...)
 *     actuator_task → xQueueReceive(...) → actuators_apply_result()
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef MQTT_H
#define MQTT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Bits do event group
 * -------------------------------------------------------------------------- */

/* Conectado ao broker (CONNACK recebido). */
#define MQTT_CONNECTED_BIT      BIT0

/* Desconectado (evento DISCONNECTED recebido). */
#define MQTT_DISCONNECTED_BIT   BIT1

/* -----------------------------------------------------------------------------
 * Tipos públicos
 * -------------------------------------------------------------------------- */

/*
 * Resultado interpretado pelo módulo. Se o JSON é válido mas o valor
 * de "resultado" não é um destes, o módulo descarta e NÃO chama o callback.
 */
typedef enum {
    MQTT_RESULT_APPROVED = 0,   /* "aprovado" */
    MQTT_RESULT_REJECTED = 1,   /* "reprovado" */
} mqtt_result_t;

/*
 * Callback de resultado.
 *
 * CHAMADO NA TASK INTERNA DO ESP-MQTT. Não bloqueie.
 *
 * @param image_id  ID da imagem (uint32) extraído do JSON "id".
 * @param result    Resultado tipado (APPROVED / REJECTED).
 */
typedef void (*mqtt_result_cb_t)(uint32_t image_id, mqtt_result_t result);

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa o cliente MQTT, conecta ao broker e assina o tópico
 *        de resultado.
 *
 * Não bloqueia esperando CONNACK — retorna após o cliente ser criado.
 * Para esperar a conexão, use mqtt_wait_connected().
 *
 * Idempotente: chamar duas vezes retorna ESP_OK na segunda.
 *
 * Requer Wi-Fi já inicializado (chame wifi_init() antes). Se você chamar
 * mqtt_init() antes do Wi-Fi estar inicializado, o esp-mqtt vai falhar ao
 * resolver o endereço do broker.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_ARG se a URI em config.h for inválida.
 *         ESP_ERR_NO_MEM em falha de alocação.
 */
esp_err_t mqtt_init(void);

/**
 * @brief Para o cliente MQTT e libera recursos.
 *
 * Normalmente não é chamada. Útil em testes.
 */
esp_err_t mqtt_deinit(void);

/* -----------------------------------------------------------------------------
 * Estado e espera
 * -------------------------------------------------------------------------- */

/**
 * @brief Espera até que o MQTT esteja conectado ao broker, ou até o timeout.
 *
 * Bloqueia a task chamadora no event group. Se o MQTT cair durante a
 * espera, continua esperando até o timeout.
 *
 * @param timeout_ms  Tempo máximo em ms. 0 = checa sem esperar.
 *                    portMAX_DELAY = espera para sempre.
 *
 * @return ESP_OK se conectado antes do timeout.
 *         ESP_ERR_TIMEOUT se o timeout expirou.
 *         ESP_ERR_INVALID_STATE se mqtt_init() não foi chamada.
 */
esp_err_t mqtt_wait_connected(uint32_t timeout_ms);

/**
 * @brief Retorna true se o MQTT está conectado ao broker. Não bloqueia.
 */
bool mqtt_is_connected(void);

/**
 * @brief Retorna o event group do MQTT. Válido após mqtt_init().
 *
 * Uso em tasks que dependem de MQTT:
 *
 *   EventGroupHandle_t eg = mqtt_get_event_group();
 *   if (eg != NULL) {
 *       xEventGroupWaitBits(eg, MQTT_CONNECTED_BIT,
 *                           pdFALSE, pdTRUE, portMAX_DELAY);
 *   }
 */
EventGroupHandle_t mqtt_get_event_group(void);

/* -----------------------------------------------------------------------------
 * Callback de resultado
 * -------------------------------------------------------------------------- */

/**
 * @brief Registra o callback que recebe os resultados do servidor.
 *
 * O callback é invocado na task interna do esp-mqtt sempre que uma
 * mensagem válida chega em CONFIG_MQTT_TOPIC_RESULT.
 *
 * Substitui o callback anterior. Se NULL, nenhum callback é chamado
 * (mas as mensagens continuam sendo logadas).
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_STATE se mqtt_init() não foi chamada.
 */
esp_err_t mqtt_set_result_callback(mqtt_result_cb_t cb);

/* -----------------------------------------------------------------------------
 * Publicação
 * --------------------------------------------------------------------------
 *
 * ATENÇÃO: estas funções PODEM BLOQUEAR se a fila de saída do esp-mqtt
 * estiver cheia (broker lento). Isso é intencional — é backpressure.
 * Chame de tasks que toleram esperar (a comm_task tolera).
 *
 * Todas retornam:
 *   ESP_OK              mensagem enfileirada com sucesso
 *   ESP_ERR_INVALID_STATE  MQTT não está conectado
 *   ESP_ERR_INVALID_ARG    parâmetro inválido
 *   ESP_FAIL               erro do driver (ex: msg id indisponível)
 * -------------------------------------------------------------------------- */

/**
 * @brief Publica metadados JSON no tópico REQUEST.
 *
 * Payload gerado:
 *   {"id": <image_id>, "size": <total_size>, "chunks": <total_chunks>}
 *
 * O servidor usa esses dados para pré-alocar buffer e validar integridade
 * antes de receber os chunks. Deve ser publicado UMA vez, antes do
 * primeiro chunk.
 *
 * @param image_id       ID da imagem (uint32).
 * @param total_size     Tamanho total do JPEG em bytes.
 * @param total_chunks   Número total de chunks que serão enviados.
 *
 * @return ESP_OK em sucesso (ver bloco acima para outros códigos).
 */
esp_err_t mqtt_publish_request(uint32_t image_id,
                               uint32_t total_size,
                               uint16_t total_chunks);

/**
 * @brief Publica um chunk de imagem no tópico IMAGE.
 *
 * Formato binário (big-endian):
 *   [0..3]  image_id      uint32
 *   [4..5]  chunk_index   uint16
 *   [6..7]  total_chunks  uint16
 *   [8..N]  payload       bytes do JPEG
 *
 * Tamanho total: CONFIG_MQTT_CHUNK_HEADER_SIZE (8) + len.
 *
 * O buffer `data` é COPIADO internamente; o chamador pode liberá-lo
 * imediatamente após o retorno (em caso de ESP_OK ou de erro).
 *
 * @param image_id       ID da imagem (deve ser o mesmo do request).
 * @param chunk_index    Índice deste chunk (0-based).
 * @param total_chunks   Número total de chunks.
 * @param data           Bytes do JPEG (não NULL).
 * @param len            Tamanho de `data` (0 < len <= CONFIG_MQTT_CHUNK_SIZE).
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_ARG se len=0, len>CONFIG_MQTT_CHUNK_SIZE,
 *         data=NULL, ou chunk_index >= total_chunks.
 */
esp_err_t mqtt_publish_chunk(uint32_t image_id,
                             uint16_t chunk_index,
                             uint16_t total_chunks,
                             const uint8_t *data,
                             size_t len);

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

/**
 * @brief Contadores desde o boot. Úteis em logs.
 *
 * @param published_out       Mensagens publicadas com sucesso (chunks + requests).
 * @param received_out        Mensagens válidas de resultado recebidas.
 * @param dropped_out         Mensagens de resultado DESCARTADAS por
 *                            validação (JSON inválido, campo faltando,
 *                            valor desconhecido).
 *
 * Todos podem ser NULL.
 */
void mqtt_get_stats(uint32_t *published_out,
                    uint32_t *received_out,
                    uint32_t *dropped_out);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_H */