/* =============================================================================
 * main/actuators.h — Controle dos atuadores (LEDs, buzzer, servo)
 * =============================================================================
 *
 * Hardware controlado:
 *   - LED verde    (GPIO 1)  → aprovação
 *   - LED vermelho (GPIO 2)  → reprovação
 *   - Buzzer       (GPIO 3)  → reprovação (ativo: liga/desliga)
 *   - Servo SG90   (GPIO 21) → aprovação (PWM via LEDC timer 1, canal 1)
 *
 * O servo usa LEDC_TIMER_1 / LEDC_CHANNEL_1 porque o driver da câmera
 * já ocupa TIMER_0/CHANNEL_0 para gerar o XCLK. Essa separação é
 * obrigatória (ver config.h).
 *
 * CONTEXTO DE USO:
 *   - actuators_init() / actuators_self_test(): chamados em app_main,
 *     antes das tasks. Sem concorrência.
 *   - actuators_apply_result(): chamado pela actuator_task, uma vez por
 *     comando recebido. Bloqueia ~4 s.
 *   - Setters individuais (set_led_*, set_buzzer, set_servo_angle):
 *     use apenas em self_test ou em diagnóstico dentro da actuator_task.
 *     Não chame de múltiplas tasks — os GPIOs não têm mutex.
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#ifndef ACTUATORS_H
#define ACTUATORS_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -----------------------------------------------------------------------------
 * Tipos públicos
 * -------------------------------------------------------------------------- */

/*
 * Resultado de uma "análise" a ser materializado pelos atuadores.
 * A máquina de estados decide este valor a partir da resposta do servidor
 * MQTT e o envia à actuator_task, que chama actuators_apply_result().
 */
typedef enum {
    ACTUATOR_RESULT_APPROVED = 0,   /* LED verde + servo             */
    ACTUATOR_RESULT_REJECTED = 1,   /* LED vermelho + buzzer         */
} actuator_result_t;

/* -----------------------------------------------------------------------------
 * Ciclo de vida
 * -------------------------------------------------------------------------- */

/**
 * @brief Inicializa os GPIOs dos LEDs e buzzer e o canal LEDC do servo.
 *
 * Deve ser chamada uma única vez, em app_main().
 *
 * Efeitos:
 *   - Configura LED verde, LED vermelho e buzzer como saída digital.
 *   - Deixa todos apagados/desligados.
 *   - Configura LEDC_TIMER_1 a 50 Hz, resolução 13 bits.
 *   - Configura LEDC_CHANNEL_1 ligado a CONFIG_GPIO_SERVO.
 *   - Posiciona o servo em HOME (~0°) e espera o movimento terminar.
 *
 * @return ESP_OK em sucesso.
 *         ESP_FAIL se algum gpio_config ou ledc_config falhar.
 */
esp_err_t actuators_init(void);

/**
 * @brief Executa uma sequência rápida de teste (~1 segundo).
 *
 * Acende o LED verde 200 ms, o vermelho 200 ms, toca o buzzer 100 ms,
 * e faz o servo ir para 0° → 180° → 90° em ~600 ms. Total ~1 s.
 *
 * Serve como sanity-check em hardware logo após actuators_init().
 * Bloqueia a task chamadora durante o teste.
 *
 * @return ESP_OK se todas as operações completaram.
 */
esp_err_t actuators_self_test(void);

/* -----------------------------------------------------------------------------
 * Ações de alto nível
 * -------------------------------------------------------------------------- */

/**
 * @brief Materializa o resultado de uma análise.
 *
 * Bloqueia até a sequência terminar. Deve ser chamada pela actuator_task
 * quando ela recebe um comando de resultado via fila.
 *
 * Sequência APROVADO:
 *   1. LED vermelho off
 *   2. LED verde on
 *   3. Servo vai para CONFIG_SERVO_DUTY_APPROVED (~90°) em
 *      CONFIG_SERVO_RETURN_MS
 *   4. Aguarda CONFIG_SERVO_HOLD_MS
 *   5. Servo volta para o centro (~90°)
 *   6. Aguarda CONFIG_SERVO_RETURN_MS
 *   7. LED verde off
 *
 * Sequência REPROVADO:
 *   1. LED verde off
 *   2. LED vermelho on
 *   3. Buzzer on
 *   4. Aguarda CONFIG_BUZZER_DURATION_MS
 *   5. Buzzer off
 *   6. LED vermelho off
 *
 * Tempo total:
 *   Aprovado:  ~ (RETURN_MS + HOLD_MS + RETURN_MS) ≈ 3.1 s
 *   Reprovado: ~ BUZZER_DURATION_MS ≈ 3.0 s
 *
 * @param result  ACTUATOR_RESULT_APPROVED ou ACTUATOR_RESULT_REJECTED.
 *
 * @return ESP_OK em sucesso.
 *         ESP_ERR_INVALID_ARG se result não for um valor válido.
 *         ESP_ERR_INVALID_STATE se actuators_init() não foi chamada.
 */
esp_err_t actuators_apply_result(actuator_result_t result);

/**
 * @brief Desliga tudo imediatamente.
 *
 * LEDs off, buzzer off, servo em posição neutra. Útil em casos de erro
 * ou em um "botão de pânico". Não bloqueia (exceto pelo tempo de
 * movimento do servo, que é irrelevante aqui porque não esperamos).
 *
 * @return ESP_OK.
 */
esp_err_t actuators_all_off(void);

/* -----------------------------------------------------------------------------
 * Controle individual (diagnóstico)
 * --------------------------------------------------------------------------
 *
 * Estes setters existem para o self-test e para depuração dentro da
 * actuator_task. Não os chame de múltiplas tasks.
 * -------------------------------------------------------------------------- */

/**
 * @brief Liga/desliga o LED verde. Não bloqueia.
 */
esp_err_t actuators_set_led_green(bool on);

/**
 * @brief Liga/desliga o LED vermelho. Não bloqueia.
 */
esp_err_t actuators_set_led_red(bool on);

/**
 * @brief Liga/desliga o buzzer. Não bloqueia.
 */
esp_err_t actuators_set_buzzer(bool on);

/**
 * @brief Move o servo para um ângulo.
 *
 * Converte graus (0–180) em duty cycle do LEDC usando interpolação
 * linear entre CONFIG_SERVO_DUTY_MIN e CONFIG_SERVO_DUTY_MAX.
 *
 * Não bloqueia: o servo começa a se mover e a função retorna
 * imediatamente. Se você precisa esperar o movimento terminar,
 * use vTaskDelay depois.
 *
 * @param degrees  Ângulo em graus, 0 a 180. Valores fora são limitados.
 *
 * @return ESP_OK em sucesso.
 */
esp_err_t actuators_set_servo_angle(uint8_t degrees);

#ifdef __cplusplus
}
#endif

#endif /* ACTUATORS_H */