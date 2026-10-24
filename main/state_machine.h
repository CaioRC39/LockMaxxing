/* =============================================================================
 * main/state_machine.h — Máquina de estados do fluxo geral
 * =============================================================================
 *
 * Autômato passivo que rastreia onde o sistema está no ciclo:
 *
 *   (boot) → INIT → IDLE → CAPTURING → SENDING → WAITING_RESULT
 *                          ↓                                        ↓
 *                       ERROR ← (falhas) ← APPLYING ← (resultado) ─┘
 *                          ↓
 *                        IDLE (após reset)
 *
 * CARACTERÍSTICAS DO DESIGN:
 *
 *   - Função PURA: recebe evento, devolve estado. Não bloqueia, não
 *     aloca, não chama APIs de hardware, não toca em MQTT.
 *
 *   - Sem timeout interno: quem controla tempo é a comm_task, com
 *     xQueueReceive(timeout). Ao expirar, ela sintetiza o evento de
 *     timeout e passa para sm_process_event().
 *
 *   - Sem concorrência: só deve ser chamada da comm_task. Se você
 *     adicionar uma segunda chamadora no futuro, precisará de um mutex
 *     externo — a máquina de estados não faz locks.
 *
 *   - Sem ações: as AÇÕES (capturar, enviar, acionar atuador) são
 *     executadas pela comm_task, comparando sm_get_state() com o que
 *     acabou de receber. A máquina de estados só decide o estado.
 *
 * PROTOCOLO DE USO (na comm_task):
 *
 *   sm_init();
 *
 *   while (1) {
 *       sm_event_t evt;
 *       if (xQueueReceive(evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
 *           sm_process_event(evt);
 *       }
 *
 *       switch (sm_get_state()) {
 *       case SM_STATE_CAPTURING:
 *           // enfileirar comando para camera_task
 *           break;
 *       case SM_STATE_SENDING:
 *           // chamar comm_send_image()
 *           break;
 *       ...
 *       }
 *   }
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Estados
 * --------------------------------------------------------------------------
 *
 * SM_STATE_INIT     : antes da rede estar pronta; main.c acabou de
 *                     inicializar módulos.
 * SM_STATE_IDLE     : ocioso, aguardando trigger de captura.
 * SM_STATE_CAPTURING: pediu imagem à camera_task, aguardando frame.
 * SM_STATE_SENDING  : enviando chunks ao broker (via comm_task).
 * SM_STATE_WAITING  : enviou tudo, aguardando resposta em /result.
 * SM_STATE_APPLYING : resultado recebido; aguardando actuator_task
 *                     terminar a sequência (LED/servo/buzzer).
 * SM_STATE_ERROR    : erro recuperável; aguardando reset para IDLE.
 *
 * Por que não há estado APPROVED/REJECTED:
 *   Aprovação e rejeição são EVENTOS (SM_EVENT_RESULT_APPROVED /
 *   SM_EVENT_RESULT_REJECTED), não estados. Depois do resultado, o
 *   sistema entra em SM_STATE_APPLYING para executar a sequência
 *   correspondente, e só depois volta a IDLE. Isso evita estados
 *   "terminais" e mantém o autômato com 7 valores em vez de 9.
 * -------------------------------------------------------------------------- */

typedef enum {
    SM_STATE_INIT     = 0,
    SM_STATE_IDLE     = 1,
    SM_STATE_CAPTURING= 2,
    SM_STATE_SENDING  = 3,
    SM_STATE_WAITING  = 4,
    SM_STATE_APPLYING = 5,
    SM_STATE_ERROR    = 6,
} sm_state_t;

/* -----------------------------------------------------------------------------
 * Eventos
 * --------------------------------------------------------------------------
 *
 * Cada transição é disparada por um evento. Eventos inválidos para o
 * estado atual são logados e ignorados (a máquina de estados NÃO
 * "consome" o evento de um jeito que cause transição indesejada).
 * -------------------------------------------------------------------------- */

typedef enum {
    /* ---- INIT → IDLE ---- */
    SM_EVENT_NETWORK_READY    = 0,   /* Wi-Fi + MQTT conectados */
    SM_EVENT_NETWORK_DOWN     = 1,   /* perda de rede durante idle ou espera */

    /* ---- IDLE → CAPTURING ---- */
    SM_EVENT_TRIGGER_CAPTURE  = 2,   /* timer expirou ou pedido manual */

    /* ---- CAPTURING → SENDING | ERROR ---- */
    SM_EVENT_CAPTURE_OK       = 3,   /* camera_task entregou um frame */
    SM_EVENT_CAPTURE_FAILED   = 4,   /* camera_task falhou */
    SM_EVENT_CAPTURE_TIMEOUT  = 5,   /* timeout aguardando frame */

    /* ---- SENDING → WAITING | ERROR ---- */
    SM_EVENT_SEND_OK          = 6,   /* todos os chunks publicados */
    SM_EVENT_SEND_FAILED      = 7,   /* falha ao publicar (desconexão, etc) */

    /* ---- WAITING → APPLYING | ERROR ---- */
    SM_EVENT_RESULT_APPROVED  = 8,   /* servidor respondeu "aprovado" */
    SM_EVENT_RESULT_REJECTED  = 9,   /* servidor respondeu "reprovado" */
    SM_EVENT_RESULT_TIMEOUT   = 10,  /* sem resposta no prazo */
    SM_EVENT_RESULT_INVALID   = 11,  /* resposta com image_id diferente
                                      * ou JSON rejeitado pelo mqtt.c */

    /* ---- APPLYING → IDLE ---- */
    SM_EVENT_APPLY_DONE       = 12,  /* actuator_task terminou a sequência */

    /* ---- ERROR → IDLE (após delay) ---- */
    SM_EVENT_RESET            = 13,  /* comm_task decidiu que pode voltar */

    /* ---- Fora de fluxo ---- */
    SM_EVENT_ABORT            = 14,  /* desligamento ou reboot iminente */
} sm_event_t;

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa a máquina de estados em SM_STATE_INIT.
 *
 * Deve ser chamada UMA vez, no início de app_main() ou da primeira
 * execução da comm_task — antes de qualquer sm_process_event().
 *
 * Idempotente: chamar de novo reinicializa para SM_STATE_INIT.
 *
 * @return ESP_OK.
 */
esp_err_t sm_init(void);

/* -----------------------------------------------------------------------------
 * Processamento de eventos
 * -------------------------------------------------------------------------- */

/**
 * @brief Processa um evento e retorna o (possivelmente novo) estado.
 *
 * Se o evento não for válido para o estado atual, loga um warning e
 * devolve o estado atual inalterado. Nunca retorna erro para o chamador
 * por evento inválido — o chamador não pode reagir de forma útil a isso,
 * e o log já basta para diagnóstico.
 *
 * @param event  Evento recebido (ver enum sm_event_t).
 *
 * @return O estado APÓS o processamento. Pode ser o mesmo de antes (se
 *         o evento foi ignorado) ou diferente (se houve transição).
 */
sm_state_t sm_process_event(sm_event_t event);

/* -----------------------------------------------------------------------------
 * Consulta de estado
 * -------------------------------------------------------------------------- */

/**
 * @brief Retorna o estado atual. Não bloqueia.
 */
sm_state_t sm_get_state(void);

/**
 * @brief Nome legível do estado (para log).
 *
 * Ex: "IDLE", "CAPTURING". Retorna "UNKNOWN" para valores inválidos.
 */
const char *sm_get_state_name(sm_state_t state);

/**
 * @brief Nome legível do evento (para log).
 *
 * Ex: "TRIGGER_CAPTURE". Retorna "UNKNOWN" para valores inválidos.
 */
const char *sm_get_event_name(sm_event_t event);

/* -----------------------------------------------------------------------------
 * Reset forçado
 * -------------------------------------------------------------------------- */

/**
 * @brief Força o estado para SM_STATE_IDLE.
 *
 * Usado em dois cenários:
 *   - Ao final do tratamento de erro (depois do delay de recovery)
 *   - Em abortos controlados (ex: comando de "parar tudo" via MQTT)
 *
 * Loga a transição, porque forçar reset é evento que deve ser raro.
 *
 * @return ESP_OK.
 */
esp_err_t sm_force_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* STATE_MACHINE_H */