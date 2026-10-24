/* =============================================================================
 * main/tasks.h — Criação e interface das 4 tasks FreeRTOS
 * =============================================================================
 *
 * As 4 tasks e suas responsabilidades:
 *
 *   wifi_mqtt_task  (prio 5, core 0)
 *       Monitora o estado de Wi-Fi e MQTT. Emite NETWORK_READY quando
 *       ambos estão conectados, e NETWORK_DOWN quando qualquer um cai.
 *       Não faz captura, envio ou atuação — apenas notifica.
 *
 *   camera_task     (prio 4, core 1)
 *       Aguarda comando em s_camera_cmd_queue. Ao receber, chama
 *       camera_capture() e envia o frame (ou falha) para s_comm_msg_queue.
 *       Nunca faz I/O de rede.
 *
 *   comm_task       (prio 3, core 0)
 *       Orquestrador. Roda a máquina de estados, envia imagens via
 *       mqtt_publish_chunk(), registra o callback de resultado MQTT,
 *       controla os timeouts e enfileira comandos para camera_task e
 *       actuator_task. É a ÚNICA task que chama sm_process_event().
 *
 *   actuator_task   (prio 2, core 1)
 *       Aguarda comando em s_actuator_cmd_queue. Ao receber, chama
 *       actuators_apply_result() (bloqueante ~3s) e envia APPLY_DONE
 *       de volta a s_comm_msg_queue.
 *
 * DISTRIBUIÇÃO POR CORE:
 *   Core 0: wifi_mqtt_task + comm_task (rede + orquestração)
 *   Core 1: camera_task  + actuator_task (periféricos)
 *
 *   Isso separa rede de periféricos no silício, evitando contenção
 *   de cache e mantendo ambas as filas de produção/consumo "quentes"
 *   em cores distintos.
 *
 * OWNERSHIP DE FRAMES (crítico — leia antes de modificar):
 *   - camera_task produz camera_frame_t* via camera_capture().
 *   - Se enfileirar com sucesso para comm_task, o frame passa a ser
 *     responsabilidade de comm_task.
 *   - Se falhar (fila cheia), camera_task DEVE chamar camera_release()
 *     antes de descartar.
 *   - comm_task DEVE chamar camera_release() em todos os caminhos
 *     (sucesso, erro, timeout) antes de transicionar de SENDING/ERROR
 *     para outro estado que não use o frame.
 *   - Nenhuma outra task toca em camera_frame_t*.
 *
 * FILAS (internas — não expostas neste header):
 *   s_camera_cmd_queue     → comm_task  → camera_task    (camera_cmd_t)
 *   s_comm_msg_queue       → todos      → comm_task      (comm_msg_t)
 *   s_actuator_cmd_queue   → comm_task  → actuator_task  (actuator_cmd_t)
 *
 * PRÉ-REQUISITOS:
 *   tasks_start_all() DEVE ser chamada APÓS:
 *     - nvs_flash_init()
 *     - ota_init()
 *     - actuators_init()
 *     - camera_init()
 *     - wifi_init()
 *     - mqtt_init()   ← necessário para registrar o callback de resultado
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef TASKS_H
#define TASKS_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Tamanhos de fila
 * --------------------------------------------------------------------------
 *
 * Escolhidos para absorver picos curtos sem bloquear o produtor:
 *
 *   CAMERA_CMD_QUEUE_LEN = 2
 *     O comm_task só envia UM comando por ciclo, e só volta a enviar
 *     quando recebe o frame (ou falha). Um buffer de 2 evita travar se
 *     chegar um trigger manual enquanto a câmera ainda está ocupada.
 *
 *   COMM_MSG_QUEUE_LEN = 10
 *     Produtores heterogêneos (wifi, camera, actuator, MQTT callback,
 *     trigger externo). 10 é folgado para não encher mesmo com eventos
 *     rápidos (ex: NETWORK_DOWN durante envio em andamento).
 *
 *   ACTUATOR_CMD_QUEUE_LEN = 2
 *     Um comando por ciclo. Buffer de 2 absorve reordenação ocasional
 *     sem travar comm_task.
 *
 * Se você aumentar drasticamente a taxa de triggers manuais (botão
 * apertado várias vezes por segundo), aumente COMM_MSG_QUEUE_LEN.
 * -------------------------------------------------------------------------- */

#define TASKS_CAMERA_CMD_QUEUE_LEN      2
#define TASKS_COMM_MSG_QUEUE_LEN        10
#define TASKS_ACTUATOR_CMD_QUEUE_LEN    2

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Cria as filas, registra o callback MQTT, e inicia as 4 tasks.
 *
 * Chamada UMA vez por app_main(), após todos os módulos estarem
 * inicializados. Retorna assim que as tasks foram criadas; elas vivem
 * até o reboot.
 *
 * Idempotente: chamar duas vezes retorna ESP_OK na segunda e loga um
 * warning (as tasks já estão rodando e não são recriadas).
 *
 * Efeitos colaterais:
 *   - Cria 3 filas FreeRTOS (estáticas? ver tasks.c — usamos estáticas)
 *   - Registra mqtt_set_result_callback() com o callback interno
 *   - Cria 4 tasks com prioridades e cores de config.h
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_NO_MEM em falha de criação de fila ou task.
 *         ESP_FAIL em falha do registro do callback MQTT.
 */
esp_err_t tasks_start_all(void);

/* -----------------------------------------------------------------------------
 * Trigger externo (botão físico, comando MQTT, teste)
 * -------------------------------------------------------------------------- */

/**
 * @brief Enfileira um SM_EVENT_TRIGGER_CAPTURE para a comm_task.
 *
 * Não bloqueia: usa xQueueSend(..., 0). Se a fila estiver cheia,
 * retorna ESP_ERR_TIMEOUT imediatamente.
 *
 * Seguro de qualquer task ou de um handler de evento. NÃO chame de
 * dentro de uma ISR — use xTaskNotifyFromISR ou similar para acionar
 * uma task intermediária que chame esta função.
 *
 * A máquina de estados só aceita TRIGGER em SM_STATE_IDLE. Se estiver
 * em outro estado (CAPTURING, SENDING, …), o evento é logado e
 * ignorado. Isso é intencional — evita injeção de ciclos durante um
 * envio em andamento.
 *
 * @return ESP_OK se enfileirado.
 *         ESP_ERR_INVALID_STATE se tasks_start_all() ainda não rodou.
 *         ESP_ERR_TIMEOUT se a fila está cheia.
 */
esp_err_t tasks_trigger_capture(void);

/* -----------------------------------------------------------------------------
 * Diagnóstico
 * -------------------------------------------------------------------------- */

/**
 * @brief Estatísticas das filas. Preenchida por tasks_get_queue_stats().
 */
typedef struct {
    /* Comm msg queue — o "hub" central */
    uint32_t comm_msg_depth;        /* itens aguardando processamento */
    uint32_t comm_msg_capacity;     /* tamanho máximo (TASKS_COMM_MSG_QUEUE_LEN) */

    /* Filas de comando */
    uint32_t camera_cmd_depth;      /* comandos pendentes para camera_task */
    uint32_t actuator_cmd_depth;    /* comandos pendentes para actuator_task */
} tasks_queue_stats_t;

/**
 * @brief Copia o estado atual das filas para *out.
 *
 * Útil em logs periódicos ou em um endpoint HTTP de status.
 * Não bloqueia. Se tasks_start_all() ainda não rodou, preenche *out
 * com zeros e retorna (não falha).
 *
 * @param out  Ponteiro para a struct de destino (não NULL).
 */
void tasks_get_queue_stats(tasks_queue_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TASKS_H */