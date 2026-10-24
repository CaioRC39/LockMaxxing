/* =============================================================================
 * main/state_machine.c — Implementação da máquina de estados
 * =============================================================================
 *
 * Função pura: recebe evento, decide o novo estado, loga a transição.
 * NÃO bloqueia, NÃO aloca, NÃO chama APIs de hardware.
 *
 * Quem executa ações é a comm_task, que:
 *   1. Recebe evento via fila (ou timeout)
 *   2. Chama sm_process_event()
 *   3. Compara sm_get_state() para decidir a próxima ação
 *
 * Thread-safety: chamada apenas pela comm_task. Sem locks por design.
 * Se você adicionar uma segunda chamadora no futuro, precisará de um
 * mutex externo — não adicione dentro deste arquivo.
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <stddef.h>

#include "esp_log.h"

#include "config.h"
#include "state_machine.h"

static const char *TAG = CONFIG_LOG_TAG_STATE;

/* -----------------------------------------------------------------------------
 * Estado atual
 * --------------------------------------------------------------------------
 *
 * Variável estática privada. Acessada apenas por funções deste arquivo.
 * Nenhuma outra translation unit pode lê-la ou escrevê-la diretamente —
 * use sm_get_state() e sm_process_event().
 * -------------------------------------------------------------------------- */

static sm_state_t s_current_state = SM_STATE_INIT;

/* -----------------------------------------------------------------------------
 * Tabelas de nomes (para logging)
 * --------------------------------------------------------------------------
 *
 * Designated initializers garantem que o índice de cada nome coincide
 * com o valor do enum correspondente. Se um enum for reordenado no
 * header, o compilador NÃO vai avisar — por isso é importante manter
 * os dois arquivos (header e .c) sincronizados.
 *
 * Tamanho inferido automaticamente pelo compilador (maior índice + 1).
 * -------------------------------------------------------------------------- */

static const char *const k_state_names[] = {
    [SM_STATE_INIT]      = "INIT",
    [SM_STATE_IDLE]      = "IDLE",
    [SM_STATE_CAPTURING] = "CAPTURING",
    [SM_STATE_SENDING]   = "SENDING",
    [SM_STATE_WAITING]   = "WAITING",
    [SM_STATE_APPLYING]  = "APPLYING",
    [SM_STATE_ERROR]     = "ERROR",
};

static const char *const k_event_names[] = {
    [SM_EVENT_NETWORK_READY]   = "NETWORK_READY",
    [SM_EVENT_NETWORK_DOWN]    = "NETWORK_DOWN",
    [SM_EVENT_TRIGGER_CAPTURE] = "TRIGGER_CAPTURE",
    [SM_EVENT_CAPTURE_OK]      = "CAPTURE_OK",
    [SM_EVENT_CAPTURE_FAILED]  = "CAPTURE_FAILED",
    [SM_EVENT_CAPTURE_TIMEOUT] = "CAPTURE_TIMEOUT",
    [SM_EVENT_SEND_OK]         = "SEND_OK",
    [SM_EVENT_SEND_FAILED]     = "SEND_FAILED",
    [SM_EVENT_RESULT_APPROVED] = "RESULT_APPROVED",
    [SM_EVENT_RESULT_REJECTED] = "RESULT_REJECTED",
    [SM_EVENT_RESULT_TIMEOUT]  = "RESULT_TIMEOUT",
    [SM_EVENT_RESULT_INVALID]  = "RESULT_INVALID",
    [SM_EVENT_APPLY_DONE]      = "APPLY_DONE",
    [SM_EVENT_RESET]           = "RESET",
    [SM_EVENT_ABORT]           = "ABORT",
};

/* -----------------------------------------------------------------------------
 * Nomes para log (helpers)
 * -------------------------------------------------------------------------- */

const char *sm_get_state_name(sm_state_t state)
{
    size_t n = sizeof(k_state_names) / sizeof(k_state_names[0]);
    if ((size_t)state < n && k_state_names[state] != NULL) {
        return k_state_names[state];
    }
    return "UNKNOWN";
}

const char *sm_get_event_name(sm_event_t event)
{
    size_t n = sizeof(k_event_names) / sizeof(k_event_names[0]);
    if ((size_t)event < n && k_event_names[event] != NULL) {
        return k_event_names[event];
    }
    return "UNKNOWN";
}

/* -----------------------------------------------------------------------------
 * Ciclo de vida e consulta
 * -------------------------------------------------------------------------- */

esp_err_t sm_init(void)
{
    s_current_state = SM_STATE_INIT;
    ESP_LOGI(TAG, "Máquina de estados inicializada em %s",
             sm_get_state_name(s_current_state));
    return ESP_OK;
}

sm_state_t sm_get_state(void)
{
    return s_current_state;
}

esp_err_t sm_force_idle(void)
{
    sm_state_t old = s_current_state;
    s_current_state = SM_STATE_IDLE;

    if (old != SM_STATE_IDLE) {
        ESP_LOGW(TAG, "FORCE IDLE: %s → IDLE", sm_get_state_name(old));
    }
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Processamento de eventos
 * --------------------------------------------------------------------------
 *
 * Estrutura:
 *   switch (estado_atual) {
 *     case ESTADO_X:
 *       switch (evento) {
 *         case EVENTO_Y: transição; break;
 *         default:       log warning, estado inalterado;
 *       }
 *   }
 *
 * Cada transição é explícita no código — não há matriz pré-computada.
 * Isso mantém o "porquê" de cada transição documentado ao lado do código.
 * -------------------------------------------------------------------------- */

sm_state_t sm_process_event(sm_event_t event)
{
    sm_state_t old_state = s_current_state;
    sm_state_t new_state = old_state;

    switch (old_state) {

    /* =========================================================================
     * INIT — aguardando rede pronta. Não aceita triggers de captura.
     * ====================================================================== */
    case SM_STATE_INIT:
        switch (event) {
        case SM_EVENT_NETWORK_READY:
            new_state = SM_STATE_IDLE;
            break;

        case SM_EVENT_NETWORK_DOWN:
            /* Já estamos esperando rede — não é novidade. */
            ESP_LOGW(TAG, "Rede caiu em INIT — continuando a aguardar");
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em INIT — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * IDLE — ocioso, espera trigger de captura.
     * ====================================================================== */
    case SM_STATE_IDLE:
        switch (event) {
        case SM_EVENT_TRIGGER_CAPTURE:
            new_state = SM_STATE_CAPTURING;
            break;

        case SM_EVENT_NETWORK_DOWN:
            /*
             * IDLE sem rede: a reconexão é feita pelo wifi.c em background.
             * Não aborta o ciclo — apenas o próximo SENDING falhará se a
             * rede não voltar a tempo.
             */
            ESP_LOGW(TAG, "Rede caiu em IDLE — reconexão automática em curso");
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em IDLE — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * CAPTURING — camera_task foi acionado, aguardando frame.
     * ====================================================================== */
    case SM_STATE_CAPTURING:
        switch (event) {
        case SM_EVENT_CAPTURE_OK:
            new_state = SM_STATE_SENDING;
            break;

        case SM_EVENT_CAPTURE_FAILED:
            ESP_LOGW(TAG, "Falha na captura — indo para ERROR");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_CAPTURE_TIMEOUT:
            ESP_LOGW(TAG, "Timeout na captura — indo para ERROR");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            /*
             * NETWORK_DOWN cai aqui por ausência de case: a captura é
             * local, não depende de rede. Quando formos enviar, essa
             * queda será detectada pelo publish.
             */
            ESP_LOGW(TAG, "Evento %s inválido em CAPTURING — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * SENDING — comm_task está publicando os chunks no broker.
     * ====================================================================== */
    case SM_STATE_SENDING:
        switch (event) {
        case SM_EVENT_SEND_OK:
            new_state = SM_STATE_WAITING;
            break;

        case SM_EVENT_SEND_FAILED:
            ESP_LOGW(TAG, "Falha no envio — indo para ERROR");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_NETWORK_DOWN:
            /*
             * A comm_task pode mandar este evento se detectar queda antes
             * do publish falhar. Como SENDING depende inteiramente de rede,
             * abortamos imediatamente em vez de esperar falhas em série.
             */
            ESP_LOGW(TAG, "Rede caiu durante envio — abortando");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em SENDING — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * WAITING — envio concluído, aguardando resposta em /result.
     * ====================================================================== */
    case SM_STATE_WAITING:
        switch (event) {
        case SM_EVENT_RESULT_APPROVED:
            ESP_LOGI(TAG, "Resultado: APROVADO");
            new_state = SM_STATE_APPLYING;
            break;

        case SM_EVENT_RESULT_REJECTED:
            ESP_LOGI(TAG, "Resultado: REPROVADO");
            new_state = SM_STATE_APPLYING;
            break;

        case SM_EVENT_RESULT_TIMEOUT:
            ESP_LOGW(TAG, "Timeout aguardando resultado — indo para ERROR");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_RESULT_INVALID:
            ESP_LOGW(TAG, "Resposta inválida (id diferente ou JSON malformado) "
                          "— indo para ERROR");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_NETWORK_DOWN:
            /* Sem rede, o resultado nunca chegará. */
            ESP_LOGW(TAG, "Rede caiu aguardando resultado — abortando");
            new_state = SM_STATE_ERROR;
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em WAITING — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * APPLYING — actuator_task está executando a sequência (LED/servo/buzzer).
     * ====================================================================== */
    case SM_STATE_APPLYING:
        switch (event) {
        case SM_EVENT_APPLY_DONE:
            new_state = SM_STATE_IDLE;
            break;

        case SM_EVENT_ABORT:
            new_state = SM_STATE_ERROR;
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em APPLYING — ignorando",
                     sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * ERROR — estado transitório; aguarda RESET para voltar a IDLE.
     * ====================================================================== */
    case SM_STATE_ERROR:
        switch (event) {
        case SM_EVENT_RESET:
            new_state = SM_STATE_IDLE;
            break;

        case SM_EVENT_ABORT:
            /* Já em ERROR — idempotente, apenas loga. */
            ESP_LOGW(TAG, "ABORT em ERROR — ignorando (já em ERROR)");
            break;

        default:
            ESP_LOGW(TAG, "Evento %s inválido em ERROR — ignorando "
                          "(precisa de RESET)", sm_get_event_name(event));
            break;
        }
        break;

    /* =========================================================================
     * Estado corrompido — não deveria acontecer; fail-safe.
     * ====================================================================== */
    default:
        ESP_LOGE(TAG, "Estado interno corrompido: %d — forçando ERROR",
                 (int)old_state);
        new_state = SM_STATE_ERROR;
        break;
    }

    /* -------------------------------------------------------------------------
     * Log e commit da transição
     * ---------------------------------------------------------------------- */

    if (new_state != old_state) {
        ESP_LOGI(TAG, "TRANSITION: %s --[%s]--> %s",
                 sm_get_state_name(old_state),
                 sm_get_event_name(event),
                 sm_get_state_name(new_state));
        s_current_state = new_state;
    }

    return s_current_state;
}