/* =============================================================================
 * main/actuators.c — Controle dos atuadores (LEDs, buzzer, servo)
 * =============================================================================
 *
 * Hardware:
 *   - LED verde    (GPIO 1)  → aprovação
 *   - LED vermelho (GPIO 2)  → reprovação
 *   - Buzzer       (GPIO 3)  → reprovação (ativo, liga/desliga)
 *   - Servo SG90   (GPIO 21) → aprovação (LEDC timer 1, canal 1)
 *
 * ATENÇÃO: o servo NÃO pode usar LEDC_TIMER_0 / LEDC_CHANNEL_0 — esses
 * são ocupados pelo driver da câmera para gerar o XCLK. Ver config.h.
 *
 * Compatível com ESP-IDF 6.1.0 / target esp32s3.
 * ========================================================================== */

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "actuators.h"

static const char *TAG = CONFIG_LOG_TAG_ACTUATOR;

/* -----------------------------------------------------------------------------
 * Estado interno
 * -------------------------------------------------------------------------- */

static bool g_initialized = false;

/* -----------------------------------------------------------------------------
 * Helpers de GPIO
 * -------------------------------------------------------------------------- */

/*
 * Escreve um nível em um pino de saída. Como LEDs e buzzer são "ativos
 * em nível alto" por padrão (CONFIG_LEVEL_*_ON = 1), 'on=true' liga e
 * 'on=false' desliga. Se você reconfigurar para lógica invertida em
 * config.h, basta trocar os níveis lá.
 */
static inline esp_err_t gpio_write(int pin, bool on, int active_level)
{
    return gpio_set_level((gpio_num_t)pin, on ? active_level : !active_level);
}

/* -----------------------------------------------------------------------------
 * Servo — conversão ângulo → duty
 * -------------------------------------------------------------------------- */

/*
 * Converte ângulo (0–180°) em duty cycle do LEDC, interpolando linearmente
 * entre CONFIG_SERVO_DUTY_MIN e CONFIG_SERVO_DUTY_MAX.
 *
 * Não bloqueia. O servo começa a se mover assim que o duty é atualizado;
 * para esperar o movimento terminar, o chamador deve usar vTaskDelay.
 */
static uint32_t angle_to_duty(uint8_t degrees)
{
    if (degrees > 180) {
        degrees = 180;
    }
    uint32_t range = CONFIG_SERVO_DUTY_MAX - CONFIG_SERVO_DUTY_MIN;
    /* Cálculo em 32 bits evita overflow em resoluções maiores. */
    return CONFIG_SERVO_DUTY_MIN + ((uint32_t)degrees * range / 180U);
}

static esp_err_t servo_set_duty(uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE,
                                  CONFIG_SERVO_CHANNEL,
                                  duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, CONFIG_SERVO_CHANNEL);
}

/* -----------------------------------------------------------------------------
 * Init / self-test
 * -------------------------------------------------------------------------- */

esp_err_t actuators_init(void)
{
    if (g_initialized) {
        ESP_LOGW(TAG, "actuators_init() chamada novamente — ignorando");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Inicializando atuadores");

    /* -------------------------------------------------------------------------
     * 1) LEDs e buzzer — saída digital
     * ---------------------------------------------------------------------- */

    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_GPIO_LED_GREEN) |
                        (1ULL << CONFIG_GPIO_LED_RED)   |
                        (1ULL << CONFIG_GPIO_BUZZER),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&io_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(LEDs/buzzer) falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Estado inicial: tudo apagado/desligado. */
    gpio_write(CONFIG_GPIO_LED_GREEN, false, CONFIG_LEVEL_LED_ON);
    gpio_write(CONFIG_GPIO_LED_RED,   false, CONFIG_LEVEL_LED_ON);
    gpio_write(CONFIG_GPIO_BUZZER,    false, CONFIG_LEVEL_BUZZER_ON);

    /* -------------------------------------------------------------------------
     * 2) Servo — timer e canal LEDC
     * ---------------------------------------------------------------------- */

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = CONFIG_SERVO_TIMER_RES,
        .timer_num       = CONFIG_SERVO_TIMER,
        .freq_hz         = CONFIG_SERVO_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config(servo) falhou: %s",
                 esp_err_to_name(err));
        ESP_LOGE(TAG, "Verifique se CONFIG_SERVO_TIMER != LEDC_TIMER_0 "
                      "(câmera usa TIMER_0)");
        return err;
    }

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = CONFIG_GPIO_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = CONFIG_SERVO_CHANNEL,
        .timer_sel  = CONFIG_SERVO_TIMER,
        .duty       = angle_to_duty(0),
        .hpoint     = 0,
        .intr_type  = LEDC_INTR_DISABLE,
    };

    err = ledc_channel_config(&ch_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config(servo) falhou: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Posiciona o servo em HOME (~0°). O SG90 leva ~300 ms para
     * percorrer 60°, então 400 ms é folgado para o boot. */
    err = servo_set_duty(angle_to_duty(0));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "servo_set_duty(HOME) falhou: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(400));

    g_initialized = true;

    ESP_LOGI(TAG, "Atuadores prontos: verde=GPIO%d, vermelho=GPIO%d, "
                  "buzzer=GPIO%d, servo=GPIO%d (timer=%d, canal=%d)",
             CONFIG_GPIO_LED_GREEN, CONFIG_GPIO_LED_RED,
             CONFIG_GPIO_BUZZER, CONFIG_GPIO_SERVO,
             CONFIG_SERVO_TIMER, CONFIG_SERVO_CHANNEL);
    return ESP_OK;
}

esp_err_t actuators_self_test(void)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Self-test: LED verde");

    /* LED verde 200 ms */
    actuators_set_led_green(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    actuators_set_led_green(false);

    /* LED vermelho 200 ms */
    ESP_LOGI(TAG, "Self-test: LED vermelho");
    actuators_set_led_red(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    actuators_set_led_red(false);

    /* Buzzer 100 ms */
    ESP_LOGI(TAG, "Self-test: buzzer");
    actuators_set_buzzer(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    actuators_set_buzzer(false);

    /* Servo: 0° → 180° → 0° (~600 ms total) */
    ESP_LOGI(TAG, "Self-test: servo");
    servo_set_duty(angle_to_duty(180));
    vTaskDelay(pdMS_TO_TICKS(300));
    servo_set_duty(angle_to_duty(0));
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Self-test concluído");
    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Sequências de resultado
 * -------------------------------------------------------------------------- */

esp_err_t actuators_apply_result(actuator_result_t result)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (result) {
    case ACTUATOR_RESULT_APPROVED:
        ESP_LOGI(TAG, "Aplicando APROVADO (verde + servo)");

        /* 1. Garante vermelho apagado antes do verde. */
        actuators_set_led_red(false);

        /* 2. Acende verde. */
        actuators_set_led_green(true);

        /* 3. Servo vai para APPROVED (~180°). */
        servo_set_duty(CONFIG_SERVO_DUTY_APPROVED);

        /* 4. Aguarda movimento terminar. */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));

        /* 5. Mantém posição (dá tempo de "ver" o resultado). */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_HOLD_MS));

        /* 6. Volta para HOME (~0°). */
        servo_set_duty(angle_to_duty(0));

        /* 7. Aguarda retorno. */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SERVO_RETURN_MS));

        /* 8. Desliga LED verde. */
        actuators_set_led_green(false);

        ESP_LOGI(TAG, "Aprovação concluída");
        return ESP_OK;

    case ACTUATOR_RESULT_REJECTED:
        ESP_LOGI(TAG, "Aplicando REPROVADO (vermelho + buzzer)");

        /* 1. Garante verde apagado. */
        actuators_set_led_green(false);

        /* 2. Acende vermelho. */
        actuators_set_led_red(true);

        /* 3. Liga buzzer. */
        actuators_set_buzzer(true);

        /* 4. Aguarda duração configurada (bloqueia a task). */
        vTaskDelay(pdMS_TO_TICKS(CONFIG_BUZZER_DURATION_MS));

        /* 5. Desliga buzzer. */
        actuators_set_buzzer(false);

        /* 6. Desliga vermelho. */
        actuators_set_led_red(false);

        ESP_LOGI(TAG, "Reprovação concluída");
        return ESP_OK;

    default:
        ESP_LOGE(TAG, "Resultado inválido: %d", (int)result);
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t actuators_all_off(void)
{
    gpio_write(CONFIG_GPIO_LED_GREEN, false, CONFIG_LEVEL_LED_ON);
    gpio_write(CONFIG_GPIO_LED_RED,   false, CONFIG_LEVEL_LED_ON);
    gpio_write(CONFIG_GPIO_BUZZER,    false, CONFIG_LEVEL_BUZZER_ON);

    /* Servo volta para HOME sem esperar (chamada rara, "botão de pânico"). */
    if (g_initialized) {
        servo_set_duty(angle_to_duty(0));
    }

    return ESP_OK;
}

/* -----------------------------------------------------------------------------
 * Setters individuais (diagnóstico)
 * -------------------------------------------------------------------------- */

esp_err_t actuators_set_led_green(bool on)
{
    return gpio_write(CONFIG_GPIO_LED_GREEN, on, CONFIG_LEVEL_LED_ON);
}

esp_err_t actuators_set_led_red(bool on)
{
    return gpio_write(CONFIG_GPIO_LED_RED, on, CONFIG_LEVEL_LED_ON);
}

esp_err_t actuators_set_buzzer(bool on)
{
    return gpio_write(CONFIG_GPIO_BUZZER, on, CONFIG_LEVEL_BUZZER_ON);
}

esp_err_t actuators_set_servo_angle(uint8_t degrees)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return servo_set_duty(angle_to_duty(degrees));
}