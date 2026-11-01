/* =============================================================================
 * main/led_status.h — LED de status WS2812 (GPIO 48)
 * =============================================================================
 *
 * Mapeia o estado da máquina de estados para uma cor/padrão no LED RGB
 * onboard. Uma task dedicada (prioridade 1) executa os padrões de blink
 * sem bloquear nenhuma outra task.
 *
 * Uso:
 *   led_status_init();                    // uma vez, em app_main
 *   led_status_set(SM_STATE_IDLE);        // a cada transição da SM
 *   led_status_set_ota_active(true);      // durante OTA
 *
 * Thread-safe: led_status_set() pode ser chamada de qualquer task.
 * O estado interno é protegido por spinlock (operação de nanossegundos).
 * ========================================================================== */

#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>
#include "esp_err.h"
#include "state_machine.h"

/**
 * @brief Inicializa o driver RMT e cria a task de animação.
 *
 * Deve ser chamada UMA vez, em app_main(), antes de tasks_start_all()
 * (ou logo depois — a task de LED é independente).
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_NO_MEM se a task ou o driver não puderem ser criados.
 */
esp_err_t led_status_init(void);

/**
 * @brief Define o estado atual para o LED refletir.
 *
 * Não bloqueia. A task de animação aplica a cor/padrão no próximo ciclo
 * (≤ 50 ms).
 */
void led_status_set(sm_state_t state);

/**
 * @brief Sobrepõe o LED para o padrão de OTA (roxo piscando rápido).
 *
 * Chamado quando uma OTA começa. O padrão de OTA tem prioridade sobre
 * o estado da máquina de estados até ser desativado.
 */
void led_status_set_ota_active(bool active);

/**
 * @brief Força uma cor RGB específica (diagnóstico).
 *
 * Útil para testar o hardware ou para indicar um evento ad-hoc.
 * Substitui o estado da SM até led_status_set() ser chamada de novo.
 */
void led_status_set_raw(uint8_t r, uint8_t g, uint8_t b);

#endif /* LED_STATUS_H */