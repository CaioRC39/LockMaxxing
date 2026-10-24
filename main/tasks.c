/* =============================================================================
 *
 * Implementa as 4 tasks FreeRTOS e a orquestração do ciclo:
 *
 *   wifi_mqtt_task → monitora rede, emite NETWORK_READY/NETWORK_DOWN
 *   camera_task    → atende comandos de captura, entrega frames
 *   comm_task      → máquina de estados, envia imagem, espera resultado
 *   actuator_task  → executa as sequências APROVADO/REPROVADO
 *
 * PARTE 1/3: includes, tipos internos, filas/tasks estáticas, callback MQTT,
 *            contexto da comm_task, forward declarations.
 * PARTE 2/3: wifi_mqtt_task, camera_task, actuator_task.
 * PARTE 3/3: comm_task, do_send_chunked, on_enter_state, process_comm_msg,
 *            tasks_start_all, tasks_trigger_capture, tasks_get_queue_stats.
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_camera.h" 

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "config.h"
#include "tasks.h"
#include "state_machine.h"
#include "camera.h"
#include "wifi.h"
#include "mqtt.h"
#include "actuators.h"

/* Timeouts específicos deste módulo (não em config.h para não poluir) */
#define TASKS_APPLY_TIMEOUT_MS      8000    /* actuator_task: 3s típ. + margem */
#define TASKS_ERROR_RECOVERY_MS     3000    /* delay antes de ERROR → IDLE    */

static const char *TAG = CONFIG_LOG_TAG_TASKS;

/* =============================================================================
 * Tipos internos
 * ============================================================================= */

/*
 * Comando para camera_task. Por enquanto sem campos — o comando é apenas
 * "capture agora". Mantemos struct (e não um simple int) para permitir
 * adicionar parâmetros no futuro (ex: resolução por comando) sem quebrar
 * a fila.
 */
typedef struct {
    uint32_t reserved;
} camera_cmd_t;

/*
 * Tipos de mensagem que fluem para a comm_task.
 */
typedef enum {
    COMM_MSG_NETWORK_READY,   /* wifi e mqtt conectados                   */
    COMM_MSG_NETWORK_DOWN,    /* wifi ou mqtt caiu                        */
    COMM_MSG_CAMERA_FRAME,    /* data.frame = frame JPEG capturado        */
    COMM_MSG_CAMERA_FAILED,   /* camera_task não conseguiu capturar       */
    COMM_MSG_RESULT,          /* data.result = resultado vindo do broker  */
    COMM_MSG_APPLY_DONE,      /* actuator_task terminou a sequência       */
    COMM_MSG_TRIGGER_CAPTURE, /* trigger externo (tasks_trigger_capture)  */
} comm_msg_type_t;

/*
 * Mensagem unificada da fila s_comm_msg_queue.
 *
 * Todos os produtores (wifi_mqtt_task, camera_task, actuator_task, o
 * callback do MQTT, e tasks_trigger_capture) escrevem aqui. A comm_task
 * é o único consumidor. O union carrega apenas os campos relevantes
 * ao type — memória do union = memória do maior membro (o ponteiro
 * de frame, 4 bytes + padding + os campos de result).
 */
typedef struct {
    comm_msg_type_t type;
    union {
        struct {
            uint32_t image_id;
            bool     approved;
        } result;
        camera_frame_t *frame;
    } data;
} comm_msg_t;

/*
 * Comando para actuator_task.
 */
typedef struct {
    actuator_result_t result;
} actuator_cmd_t;

/*
 * Estado interno da comm_task.
 *
 * Cada campo tem propósito específico:
 *   last_capture_tick : quando entramos em IDLE (para contar o intervalo)
 *   wait_start_tick   : quando entramos em WAITING (para o timeout)
 *   error_start_tick  : quando entramos em ERROR  (para a recuperação)
 *   last_state        : para detectar transições no início de cada iteração
 *   pending_frame     : frame capturado, aguardando envio (SENDING)
 *   pending_image_id  : ID dessa imagem (para correlacionar o resultado)
 */
typedef struct {
    TickType_t       state_start_tick;
    sm_state_t       last_state;
    camera_frame_t  *pending_frame;
    uint32_t         pending_image_id;
    bool             pending_apply_approved;
} comm_task_ctx_t;

/* =============================================================================
 * Filas estáticas
 * =============================================================================
 *
 * Cada fila usa xQueueCreateStatic: um StaticQueue_t + um buffer de
 * armazenamento. Tamanho exato: N itens × sizeof(item). Zero malloc.
 * ========================================================================== */

static StaticQueue_t s_camera_cmd_queue_static;
static uint8_t       s_camera_cmd_storage[TASKS_CAMERA_CMD_QUEUE_LEN * sizeof(camera_cmd_t)];
static QueueHandle_t s_camera_cmd_queue = NULL;

static StaticQueue_t s_comm_msg_queue_static;
static uint8_t       s_comm_msg_storage[TASKS_COMM_MSG_QUEUE_LEN * sizeof(comm_msg_t)];
static QueueHandle_t s_comm_msg_queue = NULL;

static StaticQueue_t s_actuator_cmd_queue_static;
static uint8_t       s_actuator_cmd_storage[TASKS_ACTUATOR_CMD_QUEUE_LEN * sizeof(actuator_cmd_t)];
static QueueHandle_t s_actuator_cmd_queue = NULL;

/* =============================================================================
 * Tasks estáticas
 * =============================================================================
 *
 * Cada task usa xTaskCreateStaticPinnedToCore: um StaticTask_t + um array
 * de StackType_t (que é uint8_t na prática). Tamanho em bytes dividido
 * por sizeof(StackType_t) para o número de "palavras" da pilha.
 * ========================================================================== */

static StaticTask_t s_wifi_mqtt_task_static;
static StackType_t  s_wifi_mqtt_stack[CONFIG_TASK_WIFI_MQTT_STACK / sizeof(StackType_t)];

static StaticTask_t s_camera_task_static;
static StackType_t  s_camera_stack[CONFIG_TASK_CAMERA_STACK / sizeof(StackType_t)];

static StaticTask_t s_comm_task_static;
static StackType_t  s_comm_stack[CONFIG_TASK_COMM_STACK / sizeof(StackType_t)];

static StaticTask_t s_actuator_task_static;
static StackType_t  s_actuator_stack[CONFIG_TASK_ACTUATOR_STACK / sizeof(StackType_t)];

/* =============================================================================
 * Estado compartilhado do módulo
 * ========================================================================== */

/* Flag de inicialização (tasks_start_all é idempotente). */
static bool s_started = false;

/* Contador monotônico de IDs de imagem (1, 2, 3, …). Reset no reboot. */
static uint32_t s_next_image_id = 1;

/* Contexto persistente da comm_task. Só comm_task lê/escreve. */
static comm_task_ctx_t s_comm_ctx = {
    .state_start_tick       = 0,
    .last_state             = SM_STATE_INIT,
    .pending_frame          = NULL,
    .pending_image_id       = 0,
    .pending_apply_approved = false,
};

/* =============================================================================
 * Callback MQTT (roda na task interna do esp-mqtt)
 * =============================================================================
 *
 * REGRA DE OURO: este callback NÃO bloqueia. Só enfileira e retorna.
 * Se bloquear, trava a recepção de mensagens MQTT inteira (incluindo
 * PUBACK, PINGRESP, etc), causando desconexão por keepalive.
 *
 * O uso de xQueueSend(..., 0) é intencional: timeout zero. Se a fila
 * estiver cheia (improvável com TASKS_COMM_MSG_QUEUE_LEN=10), o resultado
 * é perdido — e a comm_task eventualmente vai dar timeout em WAITING.
 * Isso é preferível a bloquear o esp-mqtt.
 * ========================================================================== */

static void on_mqtt_result(uint32_t image_id, mqtt_result_t result)
{
    if (s_comm_msg_queue == NULL) {
        ESP_LOGW(TAG, "MQTT callback sem fila inicializada (bug?)");
        return;
    }

    comm_msg_t msg = {
        .type = COMM_MSG_RESULT,
        .data.result = {
            .image_id = image_id,
            .approved = (result == MQTT_RESULT_APPROVED),
        },
    };

    if (xQueueSend(s_comm_msg_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Fila comm cheia — resultado descartado "
                      "(image_id=%u, %s)",
                 (unsigned)image_id,
                 msg.data.result.approved ? "APROVADO" : "REPROVADO");
    }
}

/* =============================================================================
 * wifi_mqtt_task — monitora rede, emite NETWORK_READY / NETWORK_DOWN
 * =============================================================================
 *
 * Polling a cada 500 ms:
 *   - Lê wifi_is_connected() e mqtt_is_connected() (retorno rápido, só
 *     consulta event group).
 *   - Se o estado combinado (wifi && mqtt) mudou desde a última iteração,
 *     enfileira NETWORK_READY ou NETWORK_DOWN.
 *
 * Não reconecta nada — a reconexão é feita automaticamente por wifi.c
 * (backoff exponencial) e mqtt.c (reconnect automático do esp-mqtt).
 * Este task é apenas um "sensor de estado" para a comm_task.
 * ========================================================================== */

static void wifi_mqtt_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "wifi_mqtt_task iniciada (core %d)", (int)xPortGetCoreID());

    bool was_up = false;

    while (true) {
        bool wifi_ok = wifi_is_connected();
        bool mqtt_ok = mqtt_is_connected();
        bool now_up  = wifi_ok && mqtt_ok;

        if (now_up != was_up) {
            comm_msg_t msg = {
                .type = now_up ? COMM_MSG_NETWORK_READY : COMM_MSG_NETWORK_DOWN,
            };

            /* Timeout zero: nunca bloqueia o monitor. Se a fila estiver
             * cheia, perdemos o evento — mas o próximo poll (500ms) vai
             * reenviar se o estado continuar divergente. */
            if (xQueueSend(s_comm_msg_queue, &msg, 0) != pdTRUE) {
                ESP_LOGW(TAG, "Fila comm cheia — evento de rede perdido");
            } else if (now_up) {
                ESP_LOGI(TAG, "Rede UP (wifi=%d, mqtt=%d)",
                         (int)wifi_ok, (int)mqtt_ok);
            } else {
                ESP_LOGW(TAG, "Rede DOWN (wifi=%d, mqtt=%d)",
                         (int)wifi_ok, (int)mqtt_ok);
            }

            was_up = now_up;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* =============================================================================
 * camera_task — atende comandos de captura, entrega frames
 * =============================================================================
 *
 * Bloqueia em s_camera_cmd_queue. Para cada comando recebido:
 *   1. Chama camera_capture() (bloqueante; ~50-200ms típico em SVGA).
 *   2. Em sucesso, enfileira COMM_MSG_CAMERA_FRAME com o ponteiro.
 *   3. Em falha, enfileira COMM_MSG_CAMERA_FAILED.
 *
 * OWNERSHIP DO FRAME:
 *   - Em sucesso E xQueueSend OK    → ownership passa para comm_task.
 *   - Em sucesso E xQueueSend FALHA → camera_task DEVE liberar o frame
 *     com camera_release() (senão vaza ~40KB de PSRAM por falha).
 *   - Em falha da captura           → não há frame para liberar.
 * ========================================================================== */

static void camera_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "camera_task iniciada (core %d)", (int)xPortGetCoreID());

    while (true) {
        camera_cmd_t cmd;
        if (xQueueReceive(s_camera_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            /* Timeout impossível com portMAX_DELAY, mas defensivo. */
            continue;
        }

        ESP_LOGI(TAG, "Capturando frame...");

        camera_frame_t *frame = NULL;
        esp_err_t err = camera_capture(&frame);

        if (err != ESP_OK || frame == NULL) {
            ESP_LOGE(TAG, "Captura falhou: %s", esp_err_to_name(err));

            comm_msg_t msg = { .type = COMM_MSG_CAMERA_FAILED };
            if (xQueueSend(s_comm_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "Fila comm cheia — falha de captura perdida");
                /* comm_task vai detectar timeout em CAPTURING e ir a ERROR */
            }
            continue;
        }

        ESP_LOGI(TAG, "Frame capturado: %u×%u, %u bytes",
                 (unsigned)frame->width, (unsigned)frame->height,
                 (unsigned)frame->len);

        comm_msg_t msg = {
            .type = COMM_MSG_CAMERA_FRAME,
            .data.frame = frame,
        };
        if (xQueueSend(s_comm_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* Fila cheia: ninguém vai consumir este frame. Libera AGORA
             * para não vazar PSRAM. */
            ESP_LOGW(TAG, "Fila comm cheia — frame descartado");
            camera_release(frame);
        }
        /* Se xQueueSend OK, ownership passou para comm_task. */
    }
}

/* =============================================================================
 * actuator_task — executa sequências APROVADO/REPROVADO
 * =============================================================================
 *
 * Bloqueia em s_actuator_cmd_queue. Para cada comando:
 *   1. Chama actuators_apply_result() — bloqueia ~3 s.
 *   2. Notifica comm_task com COMM_MSG_APPLY_DONE.
 *
 * O bloqueio de 3 s é intencional: enquanto a sequência roda, a comm_task
 * está em APPLYING aguardando APPLY_DONE, e nenhuma outra task depende
 * desta. O vTaskDelay interno de actuators_apply_result libera a CPU.
 *
 * Sobre falha em actuators_apply_result:
 *   Mesmo em falha, enviamos APPLY_DONE. Isso força a máquina de estados
 *   a sair de APPLYING e voltar a IDLE — melhor do que travar aguardando
 *   um timeout que não existe no estado APPLYING. O erro fica registrado
 *   no log e no estado interno do módulo actuators.
 * ========================================================================== */

static void actuator_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "actuator_task iniciada (core %d)", (int)xPortGetCoreID());

    while (true) {
        actuator_cmd_t cmd;
        if (xQueueReceive(s_actuator_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "Aplicando resultado: %s",
                 cmd.result == ACTUATOR_RESULT_APPROVED ? "APROVADO" : "REPROVADO");

        esp_err_t err = actuators_apply_result(cmd.result);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "actuators_apply_result falhou: %s",
                     esp_err_to_name(err));
            /* Segue para enviar APPLY_DONE — ver comentário acima. */
        }

        comm_msg_t msg = { .type = COMM_MSG_APPLY_DONE };
        if (xQueueSend(s_comm_msg_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Fila comm cheia — APPLY_DONE perdido");
            /* comm_task vai detectar timeout em APPLYING (se houver)
             * ou continuar aguardando — a próxima ação só acontece
             * após APPLY_DONE, então este caso não deve ocorrer na
             * prática: a fila tem 10 slots e o único produtor nesse
             * momento é esta task. */
        }
    }
}

/* =============================================================================
 * do_send_chunked — publica a imagem inteira
 * =============================================================================
 *
 * OWNERSHIP: pending_frame é liberado em TODOS os caminhos antes do
 * sm_process_event() final. Se o publish falhar no meio, os chunks já
 * enviados ficam órfãos no servidor — mas o próximo ciclo envia um novo
 * image_id e o servidor descarta o incompleto por timeout.
 * ========================================================================== */

static void do_send_chunked(comm_task_ctx_t *ctx)
{
    if (ctx->pending_frame == NULL) {
        ESP_LOGE(TAG, "do_send_chunked() sem pending_frame — abortando");
        sm_process_event(SM_EVENT_SEND_FAILED);
        return;
    }

    camera_frame_t *frame    = ctx->pending_frame;
    uint32_t        image_id = s_next_image_id++;
    uint16_t total_chunks    = (uint16_t)((frame->len + CONFIG_MQTT_CHUNK_SIZE - 1)
                                          / CONFIG_MQTT_CHUNK_SIZE);

    ctx->pending_image_id = image_id;

    ESP_LOGI(TAG, "Enviando imagem id=%u: %u bytes, %u chunks",
             (unsigned)image_id, (unsigned)frame->len,
             (unsigned)total_chunks);

    /* 1) Request com metadados */
    esp_err_t err = mqtt_publish_request(image_id, frame->len, total_chunks);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_publish_request falhou: %s", esp_err_to_name(err));
        goto fail;
    }

    /* 2) Chunks */
    for (uint16_t i = 0; i < total_chunks; i++) {
        size_t offset    = (size_t)i * CONFIG_MQTT_CHUNK_SIZE;
        size_t remaining = frame->len - offset;
        size_t chunk_len = (remaining > CONFIG_MQTT_CHUNK_SIZE)
                           ? CONFIG_MQTT_CHUNK_SIZE : remaining;

        err = mqtt_publish_chunk(image_id, i, total_chunks,
                                 frame->buf + offset, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mqtt_publish_chunk %u/%u falhou: %s",
                     (unsigned)(i + 1), (unsigned)total_chunks,
                     esp_err_to_name(err));
            goto fail;
        }

        /* A cada 5 chunks, cede CPU — evita monopolizar o Core 0 por
         * muito tempo, o que atrasaria wifi_mqtt_task e o próprio
         * esp-mqtt de processar seus eventos. */
        if ((i % 5) == 4) {
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "Imagem id=%u enviada com sucesso", (unsigned)image_id);
    camera_release(frame);
    ctx->pending_frame = NULL;
    sm_process_event(SM_EVENT_SEND_OK);
    return;

fail:
    camera_release(frame);
    ctx->pending_frame = NULL;
    sm_process_event(SM_EVENT_SEND_FAILED);
}

/* =============================================================================
 * on_enter_state — ações de entrada em cada estado
 * =============================================================================
 *
 * Chamado por handle_transitions() APÓS state_start_tick já ter sido
 * atualizado. Pode emitir eventos de SM (transições encadeadas) — o
 * chamador (handle_transitions) lida com isso em loop.
 * ========================================================================== */

static void on_enter_state(comm_task_ctx_t *ctx, sm_state_t new_state)
{
    switch (new_state) {

    case SM_STATE_INIT:
        /* Nada a fazer — aguarda NETWORK_READY */
        break;

    case SM_STATE_IDLE:
        /* Nada a fazer — o timer de trigger é derivado de state_start_tick
         * no compute_state_timeout/check_state_timeouts. */
        break;

    case SM_STATE_CAPTURING: {
        /* Enfileira comando de captura para a camera_task */
        camera_cmd_t cmd = { .reserved = 0 };
        if (xQueueSend(s_camera_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "Fila da câmera cheia — não foi possível capturar");
            sm_process_event(SM_EVENT_CAPTURE_FAILED);
        }
        break;
    }

    case SM_STATE_SENDING:
        /* Síncrono — do_send_chunked() transiciona para WAITING ou ERROR
         * antes de retornar. handle_transitions lida com a nova transição. */
        do_send_chunked(ctx);
        break;

    case SM_STATE_WAITING:
        /* Nada — apenas aguarda resultado. Timeout via state_start_tick. */
        break;

    case SM_STATE_APPLYING: {
        /* Enfileira sequência para a actuator_task. A decisão de APROVADO
         * vs REPROVADO vem do resultado que causou esta transição — está
         * em ctx->pending_apply_approved (setado em process_comm_msg). */
        actuator_cmd_t cmd = {
            .result = ctx->pending_apply_approved
                      ? ACTUATOR_RESULT_APPROVED
                      : ACTUATOR_RESULT_REJECTED,
        };
        if (xQueueSend(s_actuator_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGE(TAG, "Fila do atuador cheia — abortando");
            sm_process_event(SM_EVENT_ABORT);
        }
        break;
    }

    case SM_STATE_ERROR:
        /* Defesa: se por algum motivo ainda temos frame pendente, libera. */
        if (ctx->pending_frame != NULL) {
            ESP_LOGW(TAG, "Frame pendente em ERROR — liberando");
            camera_release(ctx->pending_frame);
            ctx->pending_frame = NULL;
        }
        break;

    default:
        break;
    }
}

/* =============================================================================
 * process_comm_msg — traduz mensagens em eventos da máquina de estados
 * =============================================================================
 *
 * Pode emitir transições. handle_transitions() as processa em seguida.
 * ========================================================================== */

static void process_comm_msg(comm_task_ctx_t *ctx, const comm_msg_t *msg)
{
    sm_state_t state = sm_get_state();

    switch (msg->type) {

    case COMM_MSG_NETWORK_READY:
        sm_process_event(SM_EVENT_NETWORK_READY);
        break;

    case COMM_MSG_NETWORK_DOWN:
        sm_process_event(SM_EVENT_NETWORK_DOWN);
        break;

    case COMM_MSG_CAMERA_FRAME:
        /* Só aceita frame se estamos realmente em CAPTURING e sem frame
         * pendente. Caso contrário, libera — o frame é do contexto que
         * o produziu, mas não tem consumidor agora. */
        if (state == SM_STATE_CAPTURING && ctx->pending_frame == NULL) {
            ctx->pending_frame = msg->data.frame;
            sm_process_event(SM_EVENT_CAPTURE_OK);
        } else {
            ESP_LOGW(TAG, "Frame inesperado (estado=%s, pending=%d) — descarta",
                     sm_get_state_name(state),
                     ctx->pending_frame != NULL);
            camera_release(msg->data.frame);
        }
        break;

    case COMM_MSG_CAMERA_FAILED:
        sm_process_event(SM_EVENT_CAPTURE_FAILED);
        break;

    case COMM_MSG_RESULT:
        if (state != SM_STATE_WAITING) {
            /* Fora de WAITING: descarta. Pode acontecer se o servidor
             * respondeu atrasado (após timeout) — comportamento correto. */
            ESP_LOGW(TAG, "Resultado fora de WAITING (estado=%s) — descarta",
                     sm_get_state_name(state));
            break;
        }

        if (msg->data.result.image_id != ctx->pending_image_id) {
            ESP_LOGW(TAG, "Resultado para id=%u, esperando id=%u — inválido",
                     (unsigned)msg->data.result.image_id,
                     (unsigned)ctx->pending_image_id);
            sm_process_event(SM_EVENT_RESULT_INVALID);
            break;
        }

        /* Guarda decisão antes de emitir o evento (on_enter_state(APPLYING)
         * precisa saber se é approved ou rejected). */
        ctx->pending_apply_approved = msg->data.result.approved;
        sm_process_event(msg->data.result.approved
                         ? SM_EVENT_RESULT_APPROVED
                         : SM_EVENT_RESULT_REJECTED);
        break;

    case COMM_MSG_APPLY_DONE:
        sm_process_event(SM_EVENT_APPLY_DONE);
        break;

    case COMM_MSG_TRIGGER_CAPTURE:
        sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
        break;

    default:
        ESP_LOGW(TAG, "Mensagem desconhecida: %d", (int)msg->type);
        break;
    }
}

/* =============================================================================
 * handle_transitions — consome transições pendentes
 * =============================================================================
 *
 * Executa on_enter_state() para cada transição. Se on_enter_state()
 * emitir mais transições (ex: SENDING → WAITING via do_send_chunked),
 * o loop continua, atualizando state_start_tick antes de cada
 * on_enter_state. safety=5 protege contra loop infinito.
 * ========================================================================== */

static void handle_transitions(comm_task_ctx_t *ctx)
{
    int safety = 5;

    while (sm_get_state() != ctx->last_state) {
        if (safety-- <= 0) {
            ESP_LOGE(TAG, "Loop de transições não convergiu — forçando ERROR");
            sm_process_event(SM_EVENT_ABORT);
            ctx->last_state = sm_get_state();
            ctx->state_start_tick = xTaskGetTickCount();
            return;
        }

        sm_state_t new_state = sm_get_state();
        ctx->last_state = new_state;
        ctx->state_start_tick = xTaskGetTickCount();
        on_enter_state(ctx, new_state);
    }
}

/* =============================================================================
 * compute_state_timeout / check_state_timeouts
 * ========================================================================== */

static TickType_t compute_state_timeout(comm_task_ctx_t *ctx)
{
    TickType_t now     = xTaskGetTickCount();
    TickType_t elapsed = now - ctx->state_start_tick;
    TickType_t timeout_ticks;

    switch (sm_get_state()) {
    case SM_STATE_IDLE:
        timeout_ticks = pdMS_TO_TICKS(CONFIG_CAPTURE_INTERVAL_MS);
        break;
    case SM_STATE_CAPTURING:
        timeout_ticks = pdMS_TO_TICKS(CONFIG_CAMERA_CAPTURE_TIMEOUT_MS);
        break;
    case SM_STATE_WAITING:
        timeout_ticks = pdMS_TO_TICKS(CONFIG_MQTT_RESULT_TIMEOUT_MS);
        break;
    case SM_STATE_APPLYING:
        timeout_ticks = pdMS_TO_TICKS(TASKS_APPLY_TIMEOUT_MS);
        break;
    case SM_STATE_ERROR:
        timeout_ticks = pdMS_TO_TICKS(TASKS_ERROR_RECOVERY_MS);
        break;
    default:
        return portMAX_DELAY;
    }

    return (elapsed >= timeout_ticks) ? 0 : (timeout_ticks - elapsed);
}

static void check_state_timeouts(comm_task_ctx_t *ctx)
{
    TickType_t now     = xTaskGetTickCount();
    TickType_t elapsed = now - ctx->state_start_tick;

    switch (sm_get_state()) {
    case SM_STATE_IDLE:
        if (elapsed >= pdMS_TO_TICKS(CONFIG_CAPTURE_INTERVAL_MS)) {
            if (mqtt_is_connected()) {
                ESP_LOGI(TAG, "Timer de captura expirou — disparando ciclo");
                sm_process_event(SM_EVENT_TRIGGER_CAPTURE);
            } else {
                /* Sem rede: não faz sentido capturar. Reinicia o timer. */
                ESP_LOGD(TAG, "Timer expirou mas MQTT offline — adiando");
                ctx->state_start_tick = now;
            }
        }
        break;

    case SM_STATE_CAPTURING:
        if (elapsed >= pdMS_TO_TICKS(CONFIG_CAMERA_CAPTURE_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Timeout aguardando frame da câmera");
            sm_process_event(SM_EVENT_CAPTURE_TIMEOUT);
        }
        break;

    case SM_STATE_WAITING:
        if (elapsed >= pdMS_TO_TICKS(CONFIG_MQTT_RESULT_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Timeout aguardando resultado do servidor");
            sm_process_event(SM_EVENT_RESULT_TIMEOUT);
        }
        break;

    case SM_STATE_APPLYING:
        if (elapsed >= pdMS_TO_TICKS(TASKS_APPLY_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "Timeout aguardando actuator_task");
            sm_process_event(SM_EVENT_ABORT);
        }
        break;

    case SM_STATE_ERROR:
        if (elapsed >= pdMS_TO_TICKS(TASKS_ERROR_RECOVERY_MS)) {
            ESP_LOGI(TAG, "Recuperação concluída: ERROR → IDLE");
            sm_process_event(SM_EVENT_RESET);
        }
        break;

    default:
        break;
    }
}

/* =============================================================================
 * comm_task — o orquestrador
 * ========================================================================== */

static void comm_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "comm_task iniciada (core %d)", (int)xPortGetCoreID());

    sm_init();
    s_comm_ctx.last_state       = SM_STATE_INIT;
    s_comm_ctx.state_start_tick = xTaskGetTickCount();

    while (true) {
        /* 1) Bloqueia pela duração do timeout do estado atual */
        TickType_t wait = compute_state_timeout(&s_comm_ctx);

        comm_msg_t msg;
        if (xQueueReceive(s_comm_msg_queue, &msg, wait) == pdTRUE) {
            process_comm_msg(&s_comm_ctx, &msg);
        }

        /* 2) Checa timeouts do estado (pode emitir eventos) */
        check_state_timeouts(&s_comm_ctx);

        /* 3) Consome todas as transições pendentes */
        handle_transitions(&s_comm_ctx);
    }
}

/* =============================================================================
 * API pública
 * ========================================================================== */

esp_err_t tasks_start_all(void)
{
    if (s_started) {
        ESP_LOGW(TAG, "tasks_start_all() chamada novamente — ignorando");
        return ESP_OK;
    }

    /* 1) Filas estáticas */
    s_camera_cmd_queue = xQueueCreateStatic(
        TASKS_CAMERA_CMD_QUEUE_LEN, sizeof(camera_cmd_t),
        s_camera_cmd_storage, &s_camera_cmd_queue_static);
    if (s_camera_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar s_camera_cmd_queue");
        return ESP_ERR_NO_MEM;
    }

    s_comm_msg_queue = xQueueCreateStatic(
        TASKS_COMM_MSG_QUEUE_LEN, sizeof(comm_msg_t),
        s_comm_msg_storage, &s_comm_msg_queue_static);
    if (s_comm_msg_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar s_comm_msg_queue");
        return ESP_ERR_NO_MEM;
    }

    s_actuator_cmd_queue = xQueueCreateStatic(
        TASKS_ACTUATOR_CMD_QUEUE_LEN, sizeof(actuator_cmd_t),
        s_actuator_cmd_storage, &s_actuator_cmd_queue_static);
    if (s_actuator_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Falha ao criar s_actuator_cmd_queue");
        return ESP_ERR_NO_MEM;
    }

    /* 2) Callback de resultado MQTT */
    esp_err_t err = mqtt_set_result_callback(on_mqtt_result);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_set_result_callback() falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* 3) Tasks estáticas (pinadas nos cores definidos em config.h) */
    TaskHandle_t h;

    h = xTaskCreateStaticPinnedToCore(
        wifi_mqtt_task, "wifi_mqtt",
        CONFIG_TASK_WIFI_MQTT_STACK / sizeof(StackType_t),
        NULL, CONFIG_TASK_WIFI_MQTT_PRIO,
        s_wifi_mqtt_stack, &s_wifi_mqtt_task_static,
        CONFIG_TASK_WIFI_MQTT_CORE);
    if (h == NULL) {
        ESP_LOGE(TAG, "Falha ao criar wifi_mqtt_task");
        return ESP_ERR_NO_MEM;
    }

    h = xTaskCreateStaticPinnedToCore(
        camera_task, "camera",
        CONFIG_TASK_CAMERA_STACK / sizeof(StackType_t),
        NULL, CONFIG_TASK_CAMERA_PRIO,
        s_camera_stack, &s_camera_task_static,
        CONFIG_TASK_CAMERA_CORE);
    if (h == NULL) {
        ESP_LOGE(TAG, "Falha ao criar camera_task");
        return ESP_ERR_NO_MEM;
    }

    h = xTaskCreateStaticPinnedToCore(
        comm_task, "comm",
        CONFIG_TASK_COMM_STACK / sizeof(StackType_t),
        NULL, CONFIG_TASK_COMM_PRIO,
        s_comm_stack, &s_comm_task_static,
        CONFIG_TASK_COMM_CORE);
    if (h == NULL) {
        ESP_LOGE(TAG, "Falha ao criar comm_task");
        return ESP_ERR_NO_MEM;
    }

    h = xTaskCreateStaticPinnedToCore(
        actuator_task, "actuator",
        CONFIG_TASK_ACTUATOR_STACK / sizeof(StackType_t),
        NULL, CONFIG_TASK_ACTUATOR_PRIO,
        s_actuator_stack, &s_actuator_task_static,
        CONFIG_TASK_ACTUATOR_CORE);
    if (h == NULL) {
        ESP_LOGE(TAG, "Falha ao criar actuator_task");
        return ESP_ERR_NO_MEM;
    }

    s_started = true;
    ESP_LOGI(TAG, "Todas as 4 tasks iniciadas");
    return ESP_OK;
}

esp_err_t tasks_trigger_capture(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    comm_msg_t msg = { .type = COMM_MSG_TRIGGER_CAPTURE };
    return (xQueueSend(s_comm_msg_queue, &msg, 0) == pdTRUE)
           ? ESP_OK
           : ESP_ERR_TIMEOUT;
}

void tasks_get_queue_stats(tasks_queue_stats_t *out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));

    if (s_comm_msg_queue != NULL) {
        out->comm_msg_depth    = (uint32_t)uxQueueMessagesWaiting(s_comm_msg_queue);
        out->comm_msg_capacity = (uint32_t)TASKS_COMM_MSG_QUEUE_LEN;
    }
    if (s_camera_cmd_queue != NULL) {
        out->camera_cmd_depth = (uint32_t)uxQueueMessagesWaiting(s_camera_cmd_queue);
    }
    if (s_actuator_cmd_queue != NULL) {
        out->actuator_cmd_depth = (uint32_t)uxQueueMessagesWaiting(s_actuator_cmd_queue);
    }
}